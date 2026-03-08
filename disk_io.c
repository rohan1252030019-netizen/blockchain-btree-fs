/*
 * disk_io.c  v2 – Disk-Resident I/O Layer
 * =========================================
 *
 * New in v2:
 *   [A] LRU Buffer Pool (10-slot, write-through, O(1) hot-path)
 *   [B] Shadow Superblock / Crash Recovery  (*.bak atomic copy)
 *   [C] Chaos Monkey  (single-bit flip in random data-block payload)
 *   [D] Hex Inspector (annotate 4 KB pages with HexRegion descriptors)
 *   [E] Raw read      (bypass cache for inspector / chaos)
 */

#include "befs.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SUPERBLOCK_MAGIC  0xBEF51234
#define SUPERBLOCK_VER    2

/* ══════════════════════════════════════════════════════════════════════
 *  A. LRU Buffer Pool
 * ══════════════════════════════════════════════════════════════════════ */

void lru_init(LRUCache *c)
{
    memset(c, 0, sizeof(LRUCache));
    for (int i = 0; i < LRU_CAPACITY; i++)
        c->slots[i].page_num = NULL_PAGE;
}

static int lru_find(LRUCache *c, uint64_t page_num)
{
    for (int i = 0; i < LRU_CAPACITY; i++)
        if (c->slots[i].page_num == page_num) return i;
    return -1;
}

static int lru_victim(LRUCache *c)
{
    int v = 0;
    for (int i = 1; i < LRU_CAPACITY; i++)
        if (c->slots[i].lru_clock < c->slots[v].lru_clock)
            v = i;
    return v;
}

void lru_invalidate_page(LRUCache *c, uint64_t page_num)
{
    int idx = lru_find(c, page_num);
    if (idx >= 0) {
        c->slots[idx].page_num  = NULL_PAGE;
        c->slots[idx].dirty     = 0;
        c->slots[idx].lru_clock = 0;
    }
}

static void lru_update_rate(BefsDB *db)
{
    uint64_t total = db->cache.hits + db->cache.misses;
    db->stats.cache_hit_rate = total
        ? (double)db->cache.hits / (double)total * 100.0 : 0.0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Offset helpers
 * ══════════════════════════════════════════════════════════════════════ */

static inline uint64_t page_file_offset(uint64_t page_num)
{
    return BTREE_AREA_START + page_num * PAGE_SIZE;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Open / Close
 * ══════════════════════════════════════════════════════════════════════ */

BefsDB *befs_open(const char *path)
{
    BefsDB *db = (BefsDB *)calloc(1, sizeof(BefsDB));
    if (!db) return NULL;

    strncpy(db->path,     path, sizeof(db->path) - 1);
    snprintf(db->bak_path, sizeof(db->bak_path), "%s.bak", path);
    lru_init(&db->cache);

    db->fp = fopen(path, "r+b");
    if (!db->fp) {
        db->fp = fopen(path, "w+b");
        if (!db->fp) { free(db); return NULL; }

        memset(&db->sb, 0, sizeof(Superblock));
        db->sb.magic           = SUPERBLOCK_MAGIC;
        db->sb.version         = SUPERBLOCK_VER;
        db->sb.root_page       = NULL_PAGE;
        db->sb.next_free_page  = 0;
        db->sb.next_data_block = DATA_AREA_START;

        if (befs_write_superblock(db) != BEFS_OK) {
            fclose(db->fp); free(db); return NULL;
        }
        befs_shadow_save(db);
        return db;
    }

    if (befs_read_superblock(db) != BEFS_OK ||
        db->sb.magic != SUPERBLOCK_MAGIC)
    {
        fprintf(stderr, "[disk_io] Primary corrupt – attempting shadow restore\n");
        fclose(db->fp); db->fp = NULL;
        if (befs_shadow_restore(db) == BEFS_OK) {
            db->fp = fopen(path, "r+b");
            if (db->fp && befs_read_superblock(db) == BEFS_OK &&
                db->sb.magic == SUPERBLOCK_MAGIC)
                goto loaded;
        }
        if (db->fp) fclose(db->fp);
        free(db); return NULL;
    }

loaded:
    db->stats.total_pages = db->sb.next_free_page;
    return db;
}

void befs_close(BefsDB *db)
{
    if (!db) return;
    if (db->fp) {
        for (int i = 0; i < LRU_CAPACITY; i++) {
            CacheSlot *s = &db->cache.slots[i];
            if (s->dirty && s->page_num != NULL_PAGE) {
                long off = (long)page_file_offset(s->page_num);
                if (fseek(db->fp, off, SEEK_SET) == 0)
                    fwrite(s->data, PAGE_SIZE, 1, db->fp);
                s->dirty = 0;
            }
        }
        befs_write_superblock(db);
        befs_shadow_save(db);
        fclose(db->fp);
    }
    free(db);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Superblock
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus befs_read_superblock(BefsDB *db)
{
    if (fseek(db->fp, SUPERBLOCK_OFFSET, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fread(&db->sb, sizeof(Superblock), 1, db->fp)  != 1) return BEFS_ERR_IO;
    return BEFS_OK;
}

BefsStatus befs_write_superblock(BefsDB *db)
{
    if (fseek(db->fp, SUPERBLOCK_OFFSET, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fwrite(&db->sb, sizeof(Superblock), 1, db->fp) != 1) return BEFS_ERR_IO;
    fflush(db->fp);
    return BEFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  B. Shadow Superblock / Crash Recovery
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus befs_shadow_save(BefsDB *db)
{
    char tmp[524];
    snprintf(tmp, sizeof(tmp), "%s.tmp", db->bak_path);

    if (db->fp) fflush(db->fp);

    FILE *src = fopen(db->path, "rb");
    if (!src) return BEFS_ERR_IO;
    FILE *dst = fopen(tmp, "wb");
    if (!dst) { fclose(src); return BEFS_ERR_IO; }

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src); fclose(dst);

    remove(db->bak_path);
    if (rename(tmp, db->bak_path) != 0) {
        remove(tmp); return BEFS_ERR_IO;
    }
    return BEFS_OK;
}

BefsStatus befs_shadow_restore(BefsDB *db)
{
    FILE *src = fopen(db->bak_path, "rb");
    if (!src) return BEFS_ERR_IO;
    FILE *dst = fopen(db->path, "wb");
    if (!dst) { fclose(src); return BEFS_ERR_IO; }

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
        fwrite(buf, 1, n, dst);
    fclose(src); fclose(dst);

    lru_init(&db->cache);
    fprintf(stderr, "[disk_io] Shadow restore complete from %s\n", db->bak_path);
    return BEFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  B-Tree page I/O  (LRU cache-aware)
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus befs_read_page(BefsDB *db, uint64_t page_num, BTreeNode *out)
{
    if (page_num == NULL_PAGE) return BEFS_ERR_IO;
    db->stats.total_reads++;

    int idx = lru_find(&db->cache, page_num);
    if (idx >= 0) {
        /* ── Cache HIT ── */
        db->cache.hits++;
        db->cache.slots[idx].lru_clock = ++db->cache.clock;
        memcpy(out, db->cache.slots[idx].data, sizeof(BTreeNode));
        lru_update_rate(db);
        return BEFS_OK;
    }

    /* ── Cache MISS: read from disk ── */
    db->cache.misses++;
    long off = (long)page_file_offset(page_num);
    if (fseek(db->fp, off, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fread(out, sizeof(BTreeNode), 1, db->fp) != 1) return BEFS_ERR_IO;

    /* Evict LRU victim with write-back */
    int v = lru_victim(&db->cache);
    CacheSlot *slot = &db->cache.slots[v];
    if (slot->dirty && slot->page_num != NULL_PAGE) {
        long voff = (long)page_file_offset(slot->page_num);
        if (fseek(db->fp, voff, SEEK_SET) == 0)
            fwrite(slot->data, PAGE_SIZE, 1, db->fp);
        slot->dirty = 0;
    }

    slot->page_num  = page_num;
    slot->lru_clock = ++db->cache.clock;
    slot->dirty     = 0;
    memcpy(slot->data, out, PAGE_SIZE);

    lru_update_rate(db);
    return BEFS_OK;
}

BefsStatus befs_write_page(BefsDB *db, uint64_t page_num, const BTreeNode *node)
{
    db->stats.total_writes++;
    long off = (long)page_file_offset(page_num);
    if (fseek(db->fp, off, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fwrite(node, sizeof(BTreeNode), 1, db->fp) != 1) return BEFS_ERR_IO;
    fflush(db->fp);

    /* Write-through: update cache slot if present */
    int idx = lru_find(&db->cache, page_num);
    if (idx >= 0) {
        memcpy(db->cache.slots[idx].data, node, PAGE_SIZE);
        db->cache.slots[idx].dirty     = 0;
        db->cache.slots[idx].lru_clock = ++db->cache.clock;
    }
    return BEFS_OK;
}

uint64_t befs_alloc_page(BefsDB *db)
{
    uint64_t pg = db->sb.next_free_page++;
    db->stats.total_pages = db->sb.next_free_page;
    befs_write_superblock(db);
    return pg;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Blockchain block I/O
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus befs_read_block(BefsDB *db, uint64_t offset, DataBlock *out)
{
    if (fseek(db->fp, (long)offset, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fread(out, sizeof(DataBlock), 1, db->fp)   != 1) return BEFS_ERR_IO;
    if (out->magic != BLOCK_MAGIC) return BEFS_ERR_CORRUPT;
    return BEFS_OK;
}

BefsStatus befs_write_block(BefsDB *db, uint64_t offset, const DataBlock *blk)
{
    if (fseek(db->fp, (long)offset, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fwrite(blk, sizeof(DataBlock), 1, db->fp)  != 1) return BEFS_ERR_IO;
    fflush(db->fp);
    return BEFS_OK;
}

uint64_t befs_alloc_block(BefsDB *db)
{
    uint64_t off = db->sb.next_data_block;
    db->sb.next_data_block += PAGE_SIZE;
    befs_write_superblock(db);
    return off;
}

/* ══════════════════════════════════════════════════════════════════════
 *  E. Raw read (cache-bypass)
 * ══════════════════════════════════════════════════════════════════════ */

BefsStatus befs_read_raw(BefsDB *db, uint64_t file_offset,
                          uint8_t *buf, size_t len)
{
    if (fseek(db->fp, (long)file_offset, SEEK_SET) != 0) return BEFS_ERR_IO;
    if (fread(buf, 1, len, db->fp) != len) return BEFS_ERR_IO;
    return BEFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  C. Chaos Monkey
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * Flips exactly one bit in the payload (value field) of a randomly chosen
 * DataBlock.  Opens the file independently with "r+b" so the flip is
 * immediately durable on disk, bypassing any OS buffering in db->fp.
 *
 * DataBlock payload region (packed):
 *   offset  0 –   3  : magic        (4)
 *   offset  4 –  11  : block_id     (8)
 *   offset 12 –  19  : timestamp    (8)
 *   offset 20 –  83  : key         (64)
 *   offset 84 – 339  : value       (256)   <-- we target this
 *   offset 340 – 343 : value_len    (4)
 *   offset 344 – 375 : prev_hash   (32)
 *   offset 376 – 407 : curr_hash   (32)
 *   offset 408 – 4095: padding
 */
BefsStatus chaos_monkey_corrupt(BefsDB *db, uint64_t *out_block_id)
{
    if (db->sb.total_blocks == 0) return BEFS_ERR_NOT_FOUND;

    srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)db);

    uint64_t bid = (uint64_t)((uint32_t)rand() % (uint32_t)db->sb.total_blocks);
    if (out_block_id) *out_block_id = bid;

    /* Corrupt a byte inside the value field */
    int payload_start = 4 + 8 + 8 + KEY_MAX_LEN;          /* 84  */
    int corrupt_off   = payload_start + rand() % VALUE_MAX_LEN;
    uint64_t target   = DATA_AREA_START + bid * PAGE_SIZE
                        + (uint64_t)corrupt_off;

    FILE *fp = fopen(db->path, "r+b");
    if (!fp) return BEFS_ERR_IO;

    if (fseek(fp, (long)target, SEEK_SET) != 0) { fclose(fp); return BEFS_ERR_IO; }
    uint8_t byte_val = 0;
    if (fread(&byte_val, 1, 1, fp) != 1)          { fclose(fp); return BEFS_ERR_IO; }

    byte_val ^= 0x01;   /* single-bit flip */

    if (fseek(fp, (long)target, SEEK_SET) != 0)   { fclose(fp); return BEFS_ERR_IO; }
    if (fwrite(&byte_val, 1, 1, fp) != 1)          { fclose(fp); return BEFS_ERR_IO; }
    fflush(fp);
    fclose(fp);

    db->stats.chaos_events++;
    return BEFS_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 *  D. Hex Inspector  – region annotation
 * ══════════════════════════════════════════════════════════════════════ */

static void add_region(HexDump *h, int start, int end,
                        HexRegionType type, const char *label)
{
    if (h->region_count >= 32 || start > end) return;
    HexRegion *r = &h->regions[h->region_count++];
    r->byte_start = start;
    r->byte_end   = end;
    r->type       = type;
    strncpy(r->label, label, sizeof(r->label) - 1);
    r->label[sizeof(r->label) - 1] = '\0';
}

/*
 * BTreeNode packed layout:
 *   [0]     is_leaf   (1)
 *   [1..3]  _pad0     (3)
 *   [4..7]  num_keys  (4)
 *   [8..583]  keys[9][64]     (576)
 *   [584..663] children[10]×8 (80)
 *   [664..735] data_offsets[9]×8 (72)
 *   [736..4095] _pad1          (3360)
 */
BefsStatus hex_dump_btree_page(BefsDB *db, uint64_t page_num, HexDump *out)
{
    memset(out, 0, sizeof(HexDump));
    out->file_offset = BTREE_AREA_START + page_num * PAGE_SIZE;
    out->is_block    = 0;
    out->is_corrupt  = 0;
    out->source_id   = page_num;

    if (befs_read_raw(db, out->file_offset, out->raw, PAGE_SIZE) != BEFS_OK)
        return BEFS_ERR_IO;

    add_region(out,   0,   7, HEX_REGION_HEADER,  "is_leaf / num_keys");
    add_region(out,   8, 583, HEX_REGION_KEYS,    "keys[9][64]");
    add_region(out, 584, 663, HEX_REGION_PTRS,    "children[10]");
    add_region(out, 664, 735, HEX_REGION_PTRS,    "data_offsets[9]");
    add_region(out, 736, PAGE_SIZE - 1, HEX_REGION_PAD, "Padding");

    return BEFS_OK;
}

/*
 * DataBlock packed layout:
 *   [0..3]   magic      (4)
 *   [4..11]  block_id   (8)
 *   [12..19] timestamp  (8)
 *   [20..83]  key       (64)
 *   [84..339] value     (256)
 *   [340..343] value_len (4)
 *   [344..375] prev_hash (32)
 *   [376..407] curr_hash (32)
 *   [408..4095] _pad    (3688)
 */
BefsStatus hex_dump_data_block(BefsDB *db, uint64_t block_id,
                                HexDump *out, int mark_corrupt)
{
    memset(out, 0, sizeof(HexDump));
    out->file_offset = DATA_AREA_START + block_id * PAGE_SIZE;
    out->is_block    = 1;
    out->is_corrupt  = mark_corrupt;
    out->source_id   = block_id;

    if (befs_read_raw(db, out->file_offset, out->raw, PAGE_SIZE) != BEFS_OK)
        return BEFS_ERR_IO;

    add_region(out,   0,  19, HEX_REGION_HEADER,  "magic / block_id / ts");
    add_region(out,  20,  83, HEX_REGION_KEYS,    "key[64]");

    /* If corrupt, mark the payload region red instead of teal */
    if (mark_corrupt) {
        add_region(out,  84, 339, HEX_REGION_CORRUPT, "value[256] *** TAMPERED");
    } else {
        add_region(out,  84, 339, HEX_REGION_PAYLOAD, "value[256]");
    }
    add_region(out, 340, 343, HEX_REGION_HEADER,  "value_len");
    add_region(out, 344, 375, HEX_REGION_HASH,    "prev_hash[32]");
    add_region(out, 376, 407, HEX_REGION_HASH,    "curr_hash[32]");
    add_region(out, 408, PAGE_SIZE - 1, HEX_REGION_PAD, "Padding");

    return BEFS_OK;
}
