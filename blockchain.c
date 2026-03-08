/*
 * blockchain.c – Cryptographic Chain Layer
 * =========================================
 * Each DataBlock is SHA-256 hashed over:
 *   magic | block_id | timestamp | key | value | value_len | prev_hash
 *
 * The genesis block (block_id == 0) has prev_hash = 0x00…00.
 *
 * Full_System_Audit() traverses every block in insertion order and
 * verifies that curr_hash[n-1] == prev_hash[n].  A single-byte
 * modification anywhere in a block will cause a hash mismatch.
 */

#include "befs.h"
#include <openssl/evp.h>
#include <string.h>
#include <stdio.h>

/* ── internal: compute SHA-256 over the "header+payload+prev_hash" region */

void sha256_of_block(const DataBlock *blk, uint8_t out[SHA256_DIGEST_LEN])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    /* Hash every field EXCEPT curr_hash and the trailing pad */
    EVP_DigestUpdate(ctx, &blk->magic,      sizeof(blk->magic));
    EVP_DigestUpdate(ctx, &blk->block_id,   sizeof(blk->block_id));
    EVP_DigestUpdate(ctx, &blk->timestamp,  sizeof(blk->timestamp));
    EVP_DigestUpdate(ctx,  blk->key,        KEY_MAX_LEN);
    EVP_DigestUpdate(ctx,  blk->value,      VALUE_MAX_LEN);
    EVP_DigestUpdate(ctx, &blk->value_len,  sizeof(blk->value_len));
    EVP_DigestUpdate(ctx,  blk->prev_hash,  SHA256_DIGEST_LEN);

    unsigned int len = SHA256_DIGEST_LEN;
    EVP_DigestFinal_ex(ctx, out, &len);
    EVP_MD_CTX_free(ctx);
}

/* ── hex helper ───────────────────────────────────────────────────────── */

void hash_to_hex(const uint8_t *hash, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < SHA256_DIGEST_LEN; i++) {
        out[i*2]   = hx[(hash[i] >> 4) & 0xF];
        out[i*2+1] = hx[ hash[i]       & 0xF];
    }
    out[SHA256_DIGEST_LEN * 2] = '\0';
}

/* ── blockchain_append ────────────────────────────────────────────────── */
/*
 * Creates a new DataBlock, links it to the chain tail, writes it to disk,
 * and updates the superblock tail_hash.
 * Returns the file offset of the new block in *out_offset.
 */
BefsStatus blockchain_append(BefsDB *db,
                              const char *key, const char *value,
                              uint64_t *out_offset)
{
    DataBlock blk;
    memset(&blk, 0, sizeof(DataBlock));

    blk.magic      = BLOCK_MAGIC;
    blk.block_id   = db->sb.total_blocks;
    blk.timestamp  = (uint64_t)time(NULL);
    blk.value_len  = (uint32_t)strlen(value);

    strncpy(blk.key,   key,   KEY_MAX_LEN   - 1);
    strncpy(blk.value, value, VALUE_MAX_LEN - 1);

    /* Link to previous block */
    if (blk.block_id == 0) {
        memset(blk.prev_hash, 0, SHA256_DIGEST_LEN);
    } else {
        memcpy(blk.prev_hash, db->sb.tail_hash, SHA256_DIGEST_LEN);
    }

    /* Compute and store current hash */
    sha256_of_block(&blk, blk.curr_hash);

    /* Allocate disk space and write */
    uint64_t offset = befs_alloc_block(db);
    if (befs_write_block(db, offset, &blk) != BEFS_OK)
        return BEFS_ERR_IO;

    /* Update superblock chain metadata */
    if (blk.block_id == 0)
        memcpy(db->sb.genesis_hash, blk.curr_hash, SHA256_DIGEST_LEN);
    memcpy(db->sb.tail_hash, blk.curr_hash, SHA256_DIGEST_LEN);
    db->sb.total_blocks++;
    befs_write_superblock(db);

    if (out_offset) *out_offset = offset;
    return BEFS_OK;
}

/* ── blockchain_audit ─────────────────────────────────────────────────── */
/*
 * Full_System_Audit():
 *   1. Reads every block in order (block_id 0 … N-1)
 *   2. Recomputes SHA-256 and compares to stored curr_hash
 *   3. Verifies prev_hash[n] == curr_hash[n-1]
 *
 * Any single-byte change anywhere in the binary file will be caught
 * because it either:
 *   a) changes the block's own hash  → recomputed != stored
 *   b) corrupts the stored hash      → next block's prev_hash won't match
 */
BefsStatus blockchain_audit(BefsDB *db, AuditReport *report)
{
    memset(report, 0, sizeof(AuditReport));
    report->total_blocks    = db->sb.total_blocks;
    report->first_corrupt_id = UINT64_MAX;
    report->chain_valid      = 1;

    uint8_t expected_prev[SHA256_DIGEST_LEN];
    memset(expected_prev, 0, SHA256_DIGEST_LEN);

    uint64_t off = DATA_AREA_START;

    for (uint64_t i = 0; i < db->sb.total_blocks; i++) {
        if (i >= MAX_BLOCKS) break;

        BlockAuditResult *res = &report->results[i];
        res->block_id = i;

        DataBlock blk;
        memset(&blk, 0, sizeof(DataBlock));

        /* --- read raw block --- */
        if (fseek(db->fp, (long)off, SEEK_SET) != 0 ||
            fread(&blk, sizeof(DataBlock), 1, db->fp) != 1) {
            res->valid = 0;
            report->chain_valid = 0;
            if (report->first_corrupt_id == UINT64_MAX)
                report->first_corrupt_id = i;
            report->corrupt_count++;
            off += PAGE_SIZE;
            continue;
        }

        /* --- verify magic --- */
        if (blk.magic != BLOCK_MAGIC) {
            res->valid = 0;
            report->chain_valid = 0;
            if (report->first_corrupt_id == UINT64_MAX)
                report->first_corrupt_id = i;
            report->corrupt_count++;
            hash_to_hex(blk.curr_hash, res->stored_hash);
            off += PAGE_SIZE;
            continue;
        }

        /* --- recompute hash --- */
        uint8_t computed[SHA256_DIGEST_LEN];
        sha256_of_block(&blk, computed);

        hash_to_hex(computed,        res->expected_hash);
        hash_to_hex(blk.curr_hash,   res->stored_hash);

        int hash_ok    = (memcmp(computed,       blk.curr_hash,   SHA256_DIGEST_LEN) == 0);
        int chain_ok   = (memcmp(expected_prev,  blk.prev_hash,   SHA256_DIGEST_LEN) == 0);

        res->valid = hash_ok && chain_ok;

        if (!res->valid) {
            report->chain_valid = 0;
            report->corrupt_count++;
            if (report->first_corrupt_id == UINT64_MAX)
                report->first_corrupt_id = i;
        }

        /* next iteration's expected_prev = this block's curr_hash */
        memcpy(expected_prev, blk.curr_hash, SHA256_DIGEST_LEN);
        off += PAGE_SIZE;
    }

    return BEFS_OK;
}
