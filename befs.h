#ifndef BEFS_H
#define BEFS_H

/*
 * BEFS v3 – Blockchain-Enabled File System
 * "SentinelStorage" – Forensic Dashboard Edition
 * ================================================
 * gui.c upgrade:
 *   - Three-pane layout: Tree Visualizer | Hex-Ray | Control Center
 *   - Animated B-Tree splits with smooth node transitions
 *   - Hex-Ray: Blue=Header, Green=Valid, Red=Corrupt, Yellow=Hashes
 *   - Security HUD with full-border pulsing RED breach alert
 *   - Search-to-Hex: path highlight + auto-scroll to block
 *   - Live sparkline graphs for Cache Hit% and I/O Latency
 *   - Chaos Monkey with real-time audit reaction
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Page & B-Tree constants ─────────────────────────────────────────── */
#define PAGE_SIZE           4096
#define BTREE_ORDER         5
#define BTREE_MAX_KEYS      (BTREE_ORDER * 2 - 1)
#define BTREE_MIN_KEYS      (BTREE_ORDER - 1)
#define BTREE_MAX_CHILDREN  (BTREE_ORDER * 2)
#define KEY_MAX_LEN         64
#define VALUE_MAX_LEN       256

/* ── Disk layout ─────────────────────────────────────────────────────── */
#define SUPERBLOCK_OFFSET   0
#define BTREE_AREA_START    PAGE_SIZE
#define DATA_AREA_START     (PAGE_SIZE * 1024)
#define NULL_PAGE           ((uint64_t)UINT64_MAX)

/* ── Blockchain constants ────────────────────────────────────────────── */
#define SHA256_DIGEST_LEN   32
#define BLOCK_MAGIC         0xBEEFCAFE
#define MAX_BLOCKS          65536

/* ── LRU Buffer Pool ─────────────────────────────────────────────────── */
#define LRU_CAPACITY        10

/* ── Sparkline history depth ─────────────────────────────────────────── */
#define SPARKLINE_LEN       80

/* ── Error codes ─────────────────────────────────────────────────────── */
typedef enum {
    BEFS_OK              =  0,
    BEFS_ERR_IO          = -1,
    BEFS_ERR_CORRUPT     = -2,
    BEFS_ERR_DUPLICATE   = -3,
    BEFS_ERR_NOT_FOUND   = -4,
    BEFS_ERR_FULL        = -5,
    BEFS_ERR_TAMPER      = -6,
    BEFS_ERR_ALLOC       = -7,
} BefsStatus;

/* ══════════════════════════════════════════════════════════════════════
 *  DISK STRUCTURES  (packed, exactly PAGE_SIZE bytes each)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t root_page;
    uint64_t next_free_page;
    uint64_t next_data_block;
    uint64_t total_records;
    uint64_t total_blocks;
    uint8_t  genesis_hash[SHA256_DIGEST_LEN];
    uint8_t  tail_hash[SHA256_DIGEST_LEN];
    uint8_t  _pad[PAGE_SIZE - 4 - 4 - 8 - 8 - 8 - 8 - 8 - SHA256_DIGEST_LEN*2];
} Superblock;

typedef struct __attribute__((packed)) {
    uint8_t  is_leaf;
    uint8_t  _pad0[3];
    uint32_t num_keys;
    char     keys[BTREE_MAX_KEYS][KEY_MAX_LEN];
    uint64_t children[BTREE_MAX_CHILDREN];
    uint64_t data_offsets[BTREE_MAX_KEYS];
    uint8_t  _pad1[PAGE_SIZE
                   - 1 - 3 - 4
                   - BTREE_MAX_KEYS * KEY_MAX_LEN
                   - BTREE_MAX_CHILDREN * 8
                   - BTREE_MAX_KEYS * 8];
} BTreeNode;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint64_t block_id;
    uint64_t timestamp;
    char     key[KEY_MAX_LEN];
    char     value[VALUE_MAX_LEN];
    uint32_t value_len;
    uint8_t  prev_hash[SHA256_DIGEST_LEN];
    uint8_t  curr_hash[SHA256_DIGEST_LEN];
    uint8_t  _pad[PAGE_SIZE
                  - 4 - 8 - 8
                  - KEY_MAX_LEN - VALUE_MAX_LEN - 4
                  - SHA256_DIGEST_LEN * 2];
} DataBlock;

/* ══════════════════════════════════════════════════════════════════════
 *  LRU BUFFER POOL
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t page_num;
    uint8_t  data[PAGE_SIZE];
    uint64_t lru_clock;
    int      dirty;
} CacheSlot;

typedef struct {
    CacheSlot slots[LRU_CAPACITY];
    uint64_t  clock;
    uint64_t  hits;
    uint64_t  misses;
} LRUCache;

/* ══════════════════════════════════════════════════════════════════════
 *  PERFORMANCE STATISTICS + SPARKLINE HISTORY
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    int      tree_height;
    uint64_t total_pages;
    double   cache_hit_rate;
    double   integrity_score;
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t chaos_events;

    /* Sparkline ring buffers */
    float    cache_history[SPARKLINE_LEN];
    float    io_latency_history[SPARKLINE_LEN];
    int      history_head;          /* ring-buffer write pointer        */
    float    last_io_ms;            /* latency of most recent disk op   */
} PerfStats;

/* ══════════════════════════════════════════════════════════════════════
 *  AUDIT STRUCTURES
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t block_id;
    int      valid;
    char     expected_hash[SHA256_DIGEST_LEN * 2 + 1];
    char     stored_hash[SHA256_DIGEST_LEN * 2 + 1];
} BlockAuditResult;

typedef struct {
    uint64_t         total_blocks;
    uint64_t         corrupt_count;
    uint64_t         first_corrupt_id;
    BlockAuditResult results[MAX_BLOCKS];
    int              chain_valid;
} AuditReport;

/* ══════════════════════════════════════════════════════════════════════
 *  HEX INSPECTOR  (v3: corruption-aware regions)
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    HEX_REGION_HEADER  = 0,   /* Blue   – magic, flags, counters        */
    HEX_REGION_KEYS    = 1,   /* Green  – key string data               */
    HEX_REGION_PTRS    = 2,   /* Cyan   – child page numbers/offsets    */
    HEX_REGION_HASH    = 3,   /* Yellow – prev_hash / curr_hash         */
    HEX_REGION_PAYLOAD = 4,   /* Green  – value bytes                   */
    HEX_REGION_PAD     = 5,   /* Dim    – unused padding                */
    HEX_REGION_CORRUPT = 6,   /* Red    – bytes known to be tampered    */
    HEX_REGION_COUNT   = 7
} HexRegionType;

typedef struct {
    int           byte_start;
    int           byte_end;
    HexRegionType type;
    char          label[32];
} HexRegion;

typedef struct {
    uint8_t   raw[PAGE_SIZE];
    uint64_t  file_offset;
    int       is_block;         /* 0 = BTreeNode, 1 = DataBlock          */
    int       is_corrupt;       /* 1 = audit flagged this block          */
    HexRegion regions[32];
    int       region_count;
    uint64_t  source_id;        /* page_num or block_id                  */
} HexDump;

/* ══════════════════════════════════════════════════════════════════════
 *  ANIMATION STATE  (B-Tree node split / search path)
 * ══════════════════════════════════════════════════════════════════════ */

#define ANIM_MAX_NODES  64

/* One animated node: interpolates from (src) to (dst) position */
typedef struct {
    float  src_x, src_y;
    float  dst_x, dst_y;
    float  cur_x, cur_y;
    float  t;            /* 0.0 → 1.0 progress                          */
    int    active;
} NodeAnim;

/* Search path highlight: list of page numbers on the path to a key */
typedef struct {
    uint64_t pages[32];
    int      count;
    float    flash_timer;   /* counts down for visual flash              */
} SearchPath;

/* ══════════════════════════════════════════════════════════════════════
 *  GUI SNAPSHOT TREE  (with layout coordinates)
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct BTreeNodeInfo {
    uint64_t             page_num;
    BTreeNode            node;
    struct BTreeNodeInfo *children[BTREE_MAX_CHILDREN];
    int                  child_count;
    float                layout_x;   /* centre X assigned by layout pass */
    float                layout_y;   /* centre Y assigned by layout pass */
} BTreeNodeInfo;

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN DATABASE HANDLE
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    FILE       *fp;
    char        path[512];
    char        bak_path[512];
    Superblock  sb;
    LRUCache    cache;
    PerfStats   stats;
} BefsDB;

/* ══════════════════════════════════════════════════════════════════════
 *  FUNCTION DECLARATIONS
 * ══════════════════════════════════════════════════════════════════════ */

/* ── disk_io.c ───────────────────────────────────────────────────────── */
BefsDB    *befs_open(const char *path);
void       befs_close(BefsDB *db);
BefsStatus befs_read_superblock(BefsDB *db);
BefsStatus befs_write_superblock(BefsDB *db);
BefsStatus befs_read_page(BefsDB *db, uint64_t page_num, BTreeNode *out);
BefsStatus befs_write_page(BefsDB *db, uint64_t page_num, const BTreeNode *node);
uint64_t   befs_alloc_page(BefsDB *db);
BefsStatus befs_read_block(BefsDB *db, uint64_t offset, DataBlock *out);
BefsStatus befs_write_block(BefsDB *db, uint64_t offset, const DataBlock *blk);
uint64_t   befs_alloc_block(BefsDB *db);
BefsStatus befs_read_raw(BefsDB *db, uint64_t file_offset,
                          uint8_t *buf, size_t len);
BefsStatus befs_shadow_save(BefsDB *db);
BefsStatus befs_shadow_restore(BefsDB *db);
BefsStatus chaos_monkey_corrupt(BefsDB *db, uint64_t *out_block_id);
BefsStatus hex_dump_btree_page(BefsDB *db, uint64_t page_num, HexDump *out);
BefsStatus hex_dump_data_block(BefsDB *db, uint64_t block_id,
                                HexDump *out, int mark_corrupt);
void       lru_init(LRUCache *c);
void       lru_invalidate_page(LRUCache *c, uint64_t page_num);

/* ── blockchain.c ───────────────────────────────────────────────────── */
void       sha256_of_block(const DataBlock *blk, uint8_t out[SHA256_DIGEST_LEN]);
BefsStatus blockchain_append(BefsDB *db, const char *key, const char *value,
                              uint64_t *out_offset);
BefsStatus blockchain_audit(BefsDB *db, AuditReport *report);
void       hash_to_hex(const uint8_t *hash, char *out);

/* ── btree.c ────────────────────────────────────────────────────────── */
BefsStatus     btree_init(BefsDB *db);
BefsStatus     btree_insert(BefsDB *db, const char *key, const char *value);
BefsStatus     btree_search(BefsDB *db, const char *key,
                             char *value_out, uint64_t *block_offset_out);
BefsStatus     btree_delete(BefsDB *db, const char *key);
int            btree_exists(BefsDB *db, const char *key);
int            btree_height(BefsDB *db);
BTreeNodeInfo *btree_snapshot(BefsDB *db);
void           btree_free_snapshot(BTreeNodeInfo *root);

/* Search returning the path of visited page_nums (for highlight) */
BefsStatus     btree_search_with_path(BefsDB *db, const char *key,
                                       char *value_out,
                                       uint64_t *block_offset_out,
                                       SearchPath *path);

/* ── gui.c ──────────────────────────────────────────────────────────── */
void gui_run(BefsDB *db);

#endif /* BEFS_H */
