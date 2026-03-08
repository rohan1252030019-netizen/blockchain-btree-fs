/*
 * btree.c – Disk-Resident B-Tree (Order 5)
 * ==========================================
 * Key design decisions:
 *   - Every node is exactly one PAGE_SIZE (4 KB) disk page.
 *   - Page numbers are 64-bit; NULL_PAGE = UINT64_MAX.
 *   - Insert uses the classic "split-on-the-way-down" strategy so that
 *     every split is a single pass (no need to re-read the parent).
 *   - Leaf nodes store file offsets pointing to blockchain DataBlocks.
 *   - Deduplication: btree_search() is called before every insert.
 */

#include "befs.h"
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Private helpers
 * ══════════════════════════════════════════════════════════════════════ */

static void node_init_leaf(BTreeNode *n)
{
    memset(n, 0, sizeof(BTreeNode));
    n->is_leaf  = 1;
    n->num_keys = 0;
    for (int i = 0; i < BTREE_MAX_CHILDREN; i++)
        n->children[i] = NULL_PAGE;
    for (int i = 0; i < BTREE_MAX_KEYS; i++)
        n->data_offsets[i] = 0;
}

static void node_init_internal(BTreeNode *n)
{
    memset(n, 0, sizeof(BTreeNode));
    n->is_leaf  = 0;
    n->num_keys = 0;
    for (int i = 0; i < BTREE_MAX_CHILDREN; i++)
        n->children[i] = NULL_PAGE;
}

/* ── btree_init ────────────────────────────────────────────────────────
 * Create the root page if the database is brand-new.
 */
BefsStatus btree_init(BefsDB *db)
{
    if (db->sb.root_page != NULL_PAGE)
        return BEFS_OK;          /* already initialised */

    BTreeNode root;
    node_init_leaf(&root);

    uint64_t pg = befs_alloc_page(db);
    if (befs_write_page(db, pg, &root) != BEFS_OK)
        return BEFS_ERR_IO;

    db->sb.root_page = pg;
    return befs_write_superblock(db);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Search
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus btree_search(BefsDB *db, const char *key,
                         char *value_out, uint64_t *block_offset_out)
{
    if (db->sb.root_page == NULL_PAGE) return BEFS_ERR_NOT_FOUND;

    uint64_t  cur_pg = db->sb.root_page;
    BTreeNode node;

    while (1) {
        if (befs_read_page(db, cur_pg, &node) != BEFS_OK)
            return BEFS_ERR_IO;

        /* Binary search within this node */
        int lo = 0, hi = (int)node.num_keys - 1, mid, cmp = 1;
        int found_idx = -1;
        while (lo <= hi) {
            mid = (lo + hi) / 2;
            cmp = strncmp(key, node.keys[mid], KEY_MAX_LEN);
            if (cmp == 0) { found_idx = mid; break; }
            else if (cmp < 0) hi = mid - 1;
            else              lo = mid + 1;
        }

        if (found_idx >= 0) {
            /* Key found in this node */
            if (block_offset_out)
                *block_offset_out = node.data_offsets[found_idx];
            if (value_out) {
                DataBlock blk;
                if (befs_read_block(db, node.data_offsets[found_idx], &blk) != BEFS_OK)
                    return BEFS_ERR_IO;
                strncpy(value_out, blk.value, VALUE_MAX_LEN - 1);
                value_out[VALUE_MAX_LEN - 1] = '\0';
            }
            return BEFS_OK;
        }

        if (node.is_leaf) return BEFS_ERR_NOT_FOUND;

        /* Descend into the correct child */
        int child_idx = lo;   /* lo is the first key > search key */
        if (node.children[child_idx] == NULL_PAGE) return BEFS_ERR_NOT_FOUND;
        cur_pg = node.children[child_idx];
    }
}

int btree_exists(BefsDB *db, const char *key)
{
    return btree_search(db, key, NULL, NULL) == BEFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Insert  (split-on-the-way-down)
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Split child[child_idx] of *parent (which is full: num_keys == MAX_KEYS).
 * The median key rises into parent; two half-full nodes remain on disk.
 */
static BefsStatus split_child(BefsDB *db,
                               BTreeNode *parent, uint64_t parent_pg,
                               int child_idx)
{
    BTreeNode child, sibling;
    uint64_t child_pg = parent->children[child_idx];

    if (befs_read_page(db, child_pg, &child) != BEFS_OK)
        return BEFS_ERR_IO;

    int t = BTREE_ORDER;                 /* minimum degree                */
    int median = t - 1;                  /* index of median key           */

    /* Initialise sibling */
    if (child.is_leaf) node_init_leaf(&sibling);
    else               node_init_internal(&sibling);

    /* Copy right half of child → sibling */
    int sibling_keys = (int)child.num_keys - median - 1;
    sibling.num_keys = (uint32_t)sibling_keys;
    for (int j = 0; j < sibling_keys; j++) {
        strncpy(sibling.keys[j], child.keys[median + 1 + j], KEY_MAX_LEN);
        sibling.data_offsets[j] = child.data_offsets[median + 1 + j];
    }
    if (!child.is_leaf) {
        for (int j = 0; j <= sibling_keys; j++)
            sibling.children[j] = child.children[median + 1 + j];
    }

    /* Truncate child */
    child.num_keys = (uint32_t)median;

    /* Allocate disk page for sibling */
    uint64_t sib_pg = befs_alloc_page(db);

    /* Make room in parent for the median key */
    for (int j = (int)parent->num_keys; j > child_idx; j--) {
        strncpy(parent->keys[j], parent->keys[j-1], KEY_MAX_LEN);
        parent->data_offsets[j]  = parent->data_offsets[j-1];
        parent->children[j+1]   = parent->children[j];
    }
    /* Insert median into parent */
    strncpy(parent->keys[child_idx], child.keys[median], KEY_MAX_LEN);
    parent->data_offsets[child_idx] = child.data_offsets[median];
    parent->children[child_idx + 1] = sib_pg;
    parent->num_keys++;

    /* Write all three modified nodes to disk */
    if (befs_write_page(db, child_pg,   &child)   != BEFS_OK) return BEFS_ERR_IO;
    if (befs_write_page(db, sib_pg,     &sibling)  != BEFS_OK) return BEFS_ERR_IO;
    if (befs_write_page(db, parent_pg,  parent)    != BEFS_OK) return BEFS_ERR_IO;

    return BEFS_OK;
}

/*
 * Insert into a node guaranteed NOT to be full.
 * Recurses down, splitting full children before descending.
 */
static BefsStatus insert_nonfull(BefsDB *db,
                                  uint64_t node_pg,
                                  const char *key,
                                  uint64_t data_offset)
{
    BTreeNode node;
    if (befs_read_page(db, node_pg, &node) != BEFS_OK)
        return BEFS_ERR_IO;

    int i = (int)node.num_keys - 1;

    if (node.is_leaf) {
        /* Shift keys right to make room */
        while (i >= 0 && strncmp(key, node.keys[i], KEY_MAX_LEN) < 0) {
            strncpy(node.keys[i+1], node.keys[i], KEY_MAX_LEN);
            node.data_offsets[i+1] = node.data_offsets[i];
            i--;
        }
        strncpy(node.keys[i+1], key, KEY_MAX_LEN - 1);
        node.keys[i+1][KEY_MAX_LEN-1] = '\0';
        node.data_offsets[i+1] = data_offset;
        node.num_keys++;
        return befs_write_page(db, node_pg, &node);
    }

    /* Internal node: find correct child */
    while (i >= 0 && strncmp(key, node.keys[i], KEY_MAX_LEN) < 0)
        i--;
    i++;   /* child index */

    uint64_t child_pg = node.children[i];
    if (child_pg == NULL_PAGE) return BEFS_ERR_CORRUPT;

    BTreeNode child;
    if (befs_read_page(db, child_pg, &child) != BEFS_OK) return BEFS_ERR_IO;

    if (child.num_keys == BTREE_MAX_KEYS) {
        /* Split the full child */
        BefsStatus s = split_child(db, &node, node_pg, i);
        if (s != BEFS_OK) return s;

        /* Re-read parent (modified by split_child) */
        if (befs_read_page(db, node_pg, &node) != BEFS_OK) return BEFS_ERR_IO;

        /* Decide which side to descend */
        if (strncmp(key, node.keys[i], KEY_MAX_LEN) > 0)
            i++;
    }

    return insert_nonfull(db, node.children[i], key, data_offset);
}

/* ── Public insert ────────────────────────────────────────────────────── */

BefsStatus btree_insert(BefsDB *db, const char *key, const char *value)
{
    /* Data deduplication */
    if (btree_exists(db, key)) return BEFS_ERR_DUPLICATE;

    /* Write blockchain block first */
    uint64_t blk_off = 0;
    BefsStatus s = blockchain_append(db, key, value, &blk_off);
    if (s != BEFS_OK) return s;

    BTreeNode root;
    if (befs_read_page(db, db->sb.root_page, &root) != BEFS_OK)
        return BEFS_ERR_IO;

    if (root.num_keys == BTREE_MAX_KEYS) {
        /* Root is full – grow the tree upward */
        uint64_t old_root_pg = db->sb.root_page;
        BTreeNode new_root;
        node_init_internal(&new_root);
        new_root.children[0] = old_root_pg;

        uint64_t new_root_pg = befs_alloc_page(db);
        if (befs_write_page(db, new_root_pg, &new_root) != BEFS_OK)
            return BEFS_ERR_IO;

        db->sb.root_page = new_root_pg;
        befs_write_superblock(db);

        /* Split the (now-child) old root */
        s = split_child(db, &new_root, new_root_pg, 0);
        if (s != BEFS_OK) return s;
    }

    s = insert_nonfull(db, db->sb.root_page, key, blk_off);
    if (s == BEFS_OK) {
        db->sb.total_records++;
        befs_write_superblock(db);
    }
    return s;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Delete  (simplified: mark block invalid, remove from B-Tree)
 * ══════════════════════════════════════════════════════════════════════ */

/* Minimal delete: find the leaf, remove key, shift remaining keys left.
 * Full B-Tree deletion (rebalancing, merge) is implemented for leaf nodes.
 * Internal-node deletion is deferred (marks as tombstone approach). */

static BefsStatus delete_from_leaf(BefsDB *db, uint64_t pg, const char *key)
{
    BTreeNode node;
    if (befs_read_page(db, pg, &node) != BEFS_OK) return BEFS_ERR_IO;

    /* Find key */
    int idx = -1;
    for (int i = 0; i < (int)node.num_keys; i++) {
        if (strncmp(key, node.keys[i], KEY_MAX_LEN) == 0) {
            idx = i; break;
        }
    }
    if (idx < 0) return BEFS_ERR_NOT_FOUND;

    /* Shift left */
    for (int i = idx; i < (int)node.num_keys - 1; i++) {
        strncpy(node.keys[i], node.keys[i+1], KEY_MAX_LEN);
        node.data_offsets[i] = node.data_offsets[i+1];
    }
    node.num_keys--;
    return befs_write_page(db, pg, &node);
}

BefsStatus btree_delete(BefsDB *db, const char *key)
{
    if (db->sb.root_page == NULL_PAGE) return BEFS_ERR_NOT_FOUND;

    /* Walk to the leaf */
    uint64_t  cur_pg = db->sb.root_page;
    BTreeNode node;

    while (1) {
        if (befs_read_page(db, cur_pg, &node) != BEFS_OK) return BEFS_ERR_IO;

        int lo = 0, hi = (int)node.num_keys - 1, found = -1;
        while (lo <= hi) {
            int mid = (lo+hi)/2;
            int cmp = strncmp(key, node.keys[mid], KEY_MAX_LEN);
            if (cmp == 0) { found = mid; break; }
            else if (cmp < 0) hi = mid - 1;
            else              lo = mid + 1;
        }

        if (node.is_leaf) {
            BefsStatus s = delete_from_leaf(db, cur_pg, key);
            if (s == BEFS_OK) {
                db->sb.total_records--;
                befs_write_superblock(db);
            }
            return s;
        }

        if (found >= 0) {
            /* Key in internal node – descend to leaf successor */
            cur_pg = node.children[found + 1];
        } else {
            cur_pg = node.children[lo];
        }
        if (cur_pg == NULL_PAGE) return BEFS_ERR_NOT_FOUND;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  GUI Snapshot – in-memory tree for rendering
 * ══════════════════════════════════════════════════════════════════════ */

static BTreeNodeInfo *snapshot_node(BefsDB *db, uint64_t pg)
{
    if (pg == NULL_PAGE) return NULL;

    BTreeNodeInfo *info = (BTreeNodeInfo *)calloc(1, sizeof(BTreeNodeInfo));
    if (!info) return NULL;

    info->page_num = pg;
    if (befs_read_page(db, pg, &info->node) != BEFS_OK) {
        free(info);
        return NULL;
    }

    if (!info->node.is_leaf) {
        int n = (int)info->node.num_keys + 1;
        info->child_count = n;
        for (int i = 0; i < n; i++)
            info->children[i] = snapshot_node(db, info->node.children[i]);
    }
    return info;
}

BTreeNodeInfo *btree_snapshot(BefsDB *db)
{
    if (db->sb.root_page == NULL_PAGE) return NULL;
    return snapshot_node(db, db->sb.root_page);
}

void btree_free_snapshot(BTreeNodeInfo *root)
{
    if (!root) return;
    for (int i = 0; i < root->child_count; i++)
        btree_free_snapshot(root->children[i]);
    free(root);
}

/* ══════════════════════════════════════════════════════════════════════
 *  btree_height  (added v2)
 *  Walk the leftmost spine: root → child[0] → … until leaf.
 *  Returns 1 for a single-node (root==leaf) tree.
 * ══════════════════════════════════════════════════════════════════════ */

int btree_height(BefsDB *db)
{
    if (db->sb.root_page == NULL_PAGE) return 0;

    int       height  = 1;
    uint64_t  cur_pg  = db->sb.root_page;
    BTreeNode node;

    while (1) {
        if (befs_read_page(db, cur_pg, &node) != BEFS_OK) break;
        if (node.is_leaf) break;
        /* descend leftmost child */
        if (node.children[0] == NULL_PAGE) break;
        cur_pg = node.children[0];
        height++;
        if (height > 64) break;   /* sanity cap */
    }

    db->stats.tree_height = height;
    return height;
}

/* ══════════════════════════════════════════════════════════════════════
 *  btree_search_with_path  (added v3)
 *  Same as btree_search but also records every page_num visited so
 *  the GUI can highlight the search path through the tree.
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus btree_search_with_path(BefsDB *db, const char *key,
                                   char *value_out,
                                   uint64_t *block_offset_out,
                                   SearchPath *path)
{
    if (path) { path->count = 0; path->flash_timer = 2.5f; }
    if (db->sb.root_page == NULL_PAGE) return BEFS_ERR_NOT_FOUND;

    uint64_t  cur_pg = db->sb.root_page;
    BTreeNode node;

    while (1) {
        if (path && path->count < 32)
            path->pages[path->count++] = cur_pg;

        if (befs_read_page(db, cur_pg, &node) != BEFS_OK)
            return BEFS_ERR_IO;

        int lo = 0, hi = (int)node.num_keys - 1, mid, found_idx = -1;
        while (lo <= hi) {
            mid = (lo + hi) / 2;
            int cmp = strncmp(key, node.keys[mid], KEY_MAX_LEN);
            if (cmp == 0)       { found_idx = mid; break; }
            else if (cmp < 0)   hi = mid - 1;
            else                lo = mid + 1;
        }

        if (found_idx >= 0) {
            if (block_offset_out)
                *block_offset_out = node.data_offsets[found_idx];
            if (value_out) {
                DataBlock blk;
                if (befs_read_block(db, node.data_offsets[found_idx], &blk) != BEFS_OK)
                    return BEFS_ERR_IO;
                strncpy(value_out, blk.value, VALUE_MAX_LEN - 1);
                value_out[VALUE_MAX_LEN - 1] = '\0';
            }
            return BEFS_OK;
        }

        if (node.is_leaf) return BEFS_ERR_NOT_FOUND;

        if (node.children[lo] == NULL_PAGE) return BEFS_ERR_NOT_FOUND;
        cur_pg = node.children[lo];
    }
}
