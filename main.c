/*
 * main.c  v2 – BEFS Entry Point
 * ================================
 * Usage:
 *   ./befs [dbfile]          → GUI mode (default: befs.db)
 *   ./befs --cli [dbfile]    → headless CLI
 *   ./befs --audit [dbfile]  → run audit and exit
 *   ./befs --restore [dbfile]→ restore from shadow backup
 */

#include "befs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Audit printer
 * ══════════════════════════════════════════════════════════════════════ */

static void print_audit(BefsDB *db)
{
    AuditReport rep;
    memset(&rep, 0, sizeof(rep));
    blockchain_audit(db, &rep);

    double valid_pct = rep.total_blocks
        ? (double)(rep.total_blocks - rep.corrupt_count)
          / (double)rep.total_blocks * 100.0
        : 100.0;

    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║         BEFS v2 – Full System Audit Report          ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  Total blocks  : %-6llu                              ║\n",
           (unsigned long long)rep.total_blocks);
    printf("║  Corrupt       : %-6llu                              ║\n",
           (unsigned long long)rep.corrupt_count);
    printf("║  Integrity     : %5.1f%%                             ║\n", valid_pct);
    printf("║  Chain valid   : %-6s                              ║\n",
           rep.chain_valid ? "YES ✓" : "NO  ✗");
    if (!rep.chain_valid)
        printf("║  First corrupt : #%llu\n",
               (unsigned long long)rep.first_corrupt_id);
    printf("╠══════════════════════════════════════════════════════╣\n");
    for (uint64_t i = 0; i < rep.total_blocks && i < MAX_BLOCKS; i++) {
        BlockAuditResult *r = &rep.results[i];
        printf("║  [%s] Block %4llu  %.20s…\n",
               r->valid ? " OK " : "ERR!",
               (unsigned long long)r->block_id,
               r->stored_hash);
    }
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

/* ══════════════════════════════════════════════════════════════════════
 *  CLI loop
 * ══════════════════════════════════════════════════════════════════════ */

static void cli_loop(BefsDB *db)
{
    printf("BEFS v2 CLI  (type 'help')\n");
    char line[512];
    while (1) {
        printf("befs> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = '\0';

        if (!strcmp(line,"exit") || !strcmp(line,"quit")) break;

        if (!strcmp(line,"help")) {
            printf("  insert <key> <value>   – insert record\n");
            printf("  search <key>           – search\n");
            printf("  delete <key>           – delete\n");
            printf("  audit                  – blockchain audit\n");
            printf("  chaos                  – simulate corruption\n");
            printf("  shadow-save            – save shadow backup\n");
            printf("  shadow-restore         – restore from shadow\n");
            printf("  height                 – print tree height\n");
            printf("  stats                  – perf stats\n");
            printf("  exit                   – quit\n");
            continue;
        }

        if (!strcmp(line,"audit"))  { print_audit(db); continue; }
        if (!strcmp(line,"height")) {
            printf("  Tree height: %d\n", btree_height(db)); continue;
        }
        if (!strcmp(line,"shadow-save")) {
            printf("  %s\n", befs_shadow_save(db)==BEFS_OK ? "Saved." : "Failed."); continue;
        }
        if (!strcmp(line,"shadow-restore")) {
            printf("  %s\n", befs_shadow_restore(db)==BEFS_OK ? "Restored." : "No backup."); continue;
        }
        if (!strcmp(line,"chaos")) {
            uint64_t bid = 0;
            BefsStatus s = chaos_monkey_corrupt(db, &bid);
            if (s == BEFS_OK)
                printf("  ⚡ Bit-flip injected into block #%llu\n", (unsigned long long)bid);
            else
                printf("  Chaos monkey: no blocks to corrupt.\n");
            continue;
        }
        if (!strcmp(line,"stats")) {
            btree_height(db);
            printf("  Tree height   : %d\n",   db->stats.tree_height);
            printf("  Total pages   : %llu\n", (unsigned long long)db->stats.total_pages);
            printf("  Cache hits    : %llu\n", (unsigned long long)db->cache.hits);
            printf("  Cache misses  : %llu\n", (unsigned long long)db->cache.misses);
            printf("  Cache hit %%   : %.1f%%\n", db->stats.cache_hit_rate);
            printf("  Total reads   : %llu\n", (unsigned long long)db->stats.total_reads);
            printf("  Total writes  : %llu\n", (unsigned long long)db->stats.total_writes);
            printf("  Chaos events  : %llu\n", (unsigned long long)db->stats.chaos_events);
            continue;
        }

        char cmd[16], key[KEY_MAX_LEN], val[VALUE_MAX_LEN];
        cmd[0]=key[0]=val[0]='\0';
        int n = sscanf(line, "%15s %63s %255[^\n]", cmd, key, val);
        if (n < 1) continue;

        if (!strcmp(cmd,"insert") && n>=3) {
            BefsStatus s = btree_insert(db, key, val);
            if (s==BEFS_OK)               printf("  Inserted: %s → %s\n",key,val);
            else if (s==BEFS_ERR_DUPLICATE) printf("  Duplicate – not inserted.\n");
            else                           printf("  Error: %d\n",s);

        } else if (!strcmp(cmd,"search") && n>=2) {
            char vout[VALUE_MAX_LEN]={0};
            if (btree_search(db,key,vout,NULL)==BEFS_OK)
                printf("  Found: %s → %s\n",key,vout);
            else printf("  Not found.\n");

        } else if (!strcmp(cmd,"delete") && n>=2) {
            BefsStatus s = btree_delete(db, key);
            printf("  %s\n", s==BEFS_OK ? "Deleted." : "Not found.");

        } else {
            printf("  Unknown command. Type 'help'.\n");
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  main
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    int         cli_mode    = 0;
    int         audit_only  = 0;
    int         do_restore  = 0;
    const char *dbpath      = "befs.db";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"--cli"))     { cli_mode   = 1; continue; }
        if (!strcmp(argv[i],"--audit"))   { audit_only = 1; continue; }
        if (!strcmp(argv[i],"--restore")) { do_restore = 1; continue; }
        dbpath = argv[i];
    }

    printf("BEFS v2 – Blockchain-Enabled File System\n");
    printf("Database: %s\n\n", dbpath);

    BefsDB *db = befs_open(dbpath);
    if (!db) {
        fprintf(stderr, "Failed to open database: %s\n", dbpath);
        return 1;
    }

    if (do_restore) {
        BefsStatus s = befs_shadow_restore(db);
        printf("Shadow restore: %s\n", s==BEFS_OK ? "OK" : "FAILED");
        befs_close(db);
        return s==BEFS_OK ? 0 : 1;
    }

    if (btree_init(db) != BEFS_OK) {
        fprintf(stderr, "B-Tree init failed\n");
        befs_close(db); return 1;
    }

    if (audit_only) {
        print_audit(db);
        befs_close(db);
        return 0;
    }

    if (cli_mode) cli_loop(db);
    else          gui_run(db);

    befs_close(db);
    printf("Closed. Goodbye.\n");
    return 0;
}
