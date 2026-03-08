/*
 * gui.c  v3 – SentinelStorage Forensic Dashboard
 * ================================================
 *
 * Three-pane layout (1680 × 1000):
 *
 *  ┌─────────────────────────── SECURITY HUD (56px) ──────────────────────────┐
 *  │ SENTINELSTORAGE  [●SECURE / ●BREACH]  Height:N  Pages:N  Cache:N%  I/O  │
 *  ├──────────────────────┬──────────────────────────┬──────────────────────  ┤
 *  │                      │                          │                        │
 *  │   LEFT PANE          │   CENTER PANE            │   RIGHT PANE           │
 *  │   B-TREE VISUALIZER  │   HEX-RAY INSPECTOR      │   CONTROL CENTER       │
 *  │   (zoomable, click   │   16-col hex dump        │   Insert/Search/Delete │
 *  │    nodes, animated   │   Blue=Header            │   Audit / Chaos Monkey │
 *  │    split flash,      │   Green=Valid Data       │   Sparkline graphs     │
 *  │    search path       │   Red=Corrupt            │   Block chain status   │
 *  │    highlight)        │   Yellow=Hashes          │                        │
 *  │                      │                          │                        │
 *  └──────────────────────┴──────────────────────────┴────────────────────────┘
 *
 * Security HUD: when breach detected, the entire window BORDER pulses RED
 * with a scanline overlay and "BREACH DETECTED" stamp.
 *
 * Search-to-Hex: btree_search_with_path() collects visited page numbers.
 * Those pages flash yellow in the tree; the corresponding DataBlock loads
 * automatically into Hex-Ray and scrolls to the hash region.
 */

#include "befs.h"
#include "raylib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════════════
 *  Window & Layout Constants
 * ══════════════════════════════════════════════════════════════════════ */

#define WIN_W          1680
#define WIN_H          1000
#define HUD_H          56

/* Three pane widths */
#define LEFT_W         440
#define RIGHT_W        380
#define CENTER_W       (WIN_W - LEFT_W - RIGHT_W)   /* 860 */

/* Pane origins */
#define LEFT_X         0
#define LEFT_Y         HUD_H
#define LEFT_H         (WIN_H - HUD_H)

#define CENTER_X       LEFT_W
#define CENTER_Y       HUD_H
#define CENTER_H       (WIN_H - HUD_H)

#define RIGHT_X        (LEFT_W + CENTER_W)
#define RIGHT_Y        HUD_H
#define RIGHT_H        (WIN_H - HUD_H)

/* ══════════════════════════════════════════════════════════════════════
 *  Cybersecurity Colour Palette
 * ══════════════════════════════════════════════════════════════════════ */

/* Backgrounds */
#define C_VOID         ((Color){  6,   8,  16, 255})   /* deepest bg       */
#define C_PANEL        ((Color){ 11,  15,  28, 255})   /* panel fill       */
#define C_PANEL2       ((Color){ 16,  22,  42, 255})   /* raised surface   */
#define C_PANEL3       ((Color){ 22,  30,  58, 255})   /* highlighted row  */
#define C_BORDER       ((Color){ 32,  48,  96, 255})   /* normal border    */
#define C_DIVIDER      ((Color){ 24,  34,  66, 255})   /* divider line     */

/* Semantic */
#define C_ACCENT       ((Color){ 0,  200, 255, 255})   /* primary cyan     */
#define C_ACCENT2      ((Color){ 80, 140, 255, 255})   /* blue-purple      */
#define C_GREEN        ((Color){ 40, 230, 120, 255})   /* valid/secure     */
#define C_RED          ((Color){255,  50,  60, 255})   /* corrupt/breach   */
#define C_YELLOW       ((Color){255, 210,  40, 255})   /* hashes/warning   */
#define C_AMBER        ((Color){255, 160,  20, 255})   /* alert            */
#define C_PURPLE       ((Color){160,  80, 255, 255})   /* pointers         */
#define C_TEAL         ((Color){ 40, 210, 190, 255})   /* payload/data     */
#define C_PINK         ((Color){255,  80, 160, 255})   /* special accent   */

/* Text */
#define C_TEXT         ((Color){210, 220, 240, 255})
#define C_SUBTEXT      ((Color){100, 118, 160, 255})
#define C_DIM          ((Color){ 55,  65,  95, 255})

/* Node colours */
#define C_NODE_FILL    ((Color){ 18,  26,  56, 255})
#define C_NODE_LEAF    ((Color){ 12,  38,  38, 255})
#define C_NODE_BORDER  ((Color){  0, 190, 255, 255})
#define C_NODE_SEL     ((Color){255, 210,  40, 255})   /* selected/path    */
#define C_NODE_SPLIT   ((Color){255, 160,  20, 255})   /* split animation  */
#define C_EDGE         ((Color){ 36,  60, 120, 255})

/* Hex region colours — direct matches to requirements */
static const Color HEX_REG_COLORS[HEX_REGION_COUNT] = {
    {  64, 148, 255, 200},   /* HEADER  – Blue                            */
    {  40, 230, 120, 200},   /* KEYS    – Green (valid data)              */
    { 140,  80, 255, 190},   /* PTRS    – Purple                          */
    { 255, 210,  40, 200},   /* HASH    – Yellow                          */
    {  40, 210, 190, 200},   /* PAYLOAD – Teal (valid data)               */
    {  22,  30,  55, 100},   /* PAD     – Dim                             */
    { 255,  50,  60, 220},   /* CORRUPT – Red                             */
};

/* ══════════════════════════════════════════════════════════════════════
 *  UI State
 * ══════════════════════════════════════════════════════════════════════ */

typedef enum { MODE_INSERT=0, MODE_SEARCH=1, MODE_DELETE=2 } OpMode;

/* Split animation: one entry per node born from a split */
typedef struct {
    float src_x, src_y, dst_x, dst_y;
    float t;        /* 0 → 1 eased progress */
    int   active;
    Color flash_col;
} SplitAnim;

#define MAX_SPLIT_ANIMS 8

typedef struct {
    /* Operations */
    char    key_buf[KEY_MAX_LEN];
    char    val_buf[VALUE_MAX_LEN];
    char    msg_buf[256];
    int     msg_ok;
    int     key_focused;
    int     val_focused;
    OpMode  mode;
    float   msg_timer;

    /* Tree */
    BTreeNodeInfo *tree_snap;
    float          zoom;               /* 0.4 … 2.0                       */
    float          pan_x, pan_y;       /* pixel offset into tree pane     */
    int            dragging;
    float          drag_ox, drag_oy;

    /* Split animations */
    SplitAnim split_anims[MAX_SPLIT_ANIMS];
    int       prev_page_count;         /* detect when pages increase      */

    /* Search path */
    SearchPath     search_path;
    uint64_t       found_block_id;     /* block to load in hex after search */
    int            found_valid;

    /* Hex-Ray */
    HexDump hex;
    int     hex_valid;    /* 0=empty 1=ready 2=pending-btree 3=pending-block */
    int     hex_scroll;   /* row scroll offset                               */
    char    hex_title[80];
    int     hex_target_row; /* row to auto-scroll to after search            */

    /* Audit */
    AuditReport audit;
    float       audit_timer;
    int         audit_dirty;

    /* Perf snapshot */
    PerfStats perf;
    float     perf_timer;

    /* Chaos */
    uint64_t chaos_block;
    int      chaos_valid;
    float    chaos_flash;

    /* Breach border */
    float breach_pulse;   /* animates when chain_valid == 0                  */
} UiState;

/* ══════════════════════════════════════════════════════════════════════
 *  Easing helpers
 * ══════════════════════════════════════════════════════════════════════ */

static inline float clampf(float v, float lo, float hi)
{ return v < lo ? lo : v > hi ? hi : v; }

static inline float ease_out_cubic(float t)
{ float r = 1.0f - t; return 1.0f - r*r*r; }

static inline float lerpf(float a, float b, float t)
{ return a + (b-a)*t; }

/* ══════════════════════════════════════════════════════════════════════
 *  Primitive drawing helpers
 * ══════════════════════════════════════════════════════════════════════ */

static void fill_rect(int x, int y, int w, int h, Color c)
{ DrawRectangle(x, y, w, h, c); }

static void fill_rrect(Rectangle r, float rad, Color fill)
{ DrawRectangleRounded(r, rad, 8, fill); }

static void stroke_rrect(Rectangle r, float rad, Color c, float t)
{ DrawRectangleRoundedLines(r, rad, 8, t, c); }

static void hline(int x, int y, int w, Color c)
{ DrawLine(x, y, x+w, y, c); }

static void vline(int x, int y, int h, Color c)
{ DrawLine(x, y, x, y+h, c); }

static void label(const char *s, int x, int y, int sz, Color c)
{ DrawText(s, x, y, sz, c); }

static int text_w(const char *s, int sz)
{ return MeasureText(s, sz); }

/* Glow dot with animated pulse */
static void glow_dot(int cx, int cy, int r, Color c, float t)
{
    float pulse = 0.5f + 0.5f * sinf(t * 3.5f);
    Color g = c; g.a = (unsigned char)(90 * pulse);
    DrawCircle(cx, cy, (float)(r+5)*pulse, g);
    DrawCircle(cx, cy, (float)r, c);
}

/* Gradient scanline overlay for breach effect */
static void draw_scanlines(int x, int y, int w, int h, Color c)
{
    for (int row = y; row < y+h; row += 4)
        DrawLine(x, row, x+w, row, c);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Security HUD  (top bar)
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_hud(BefsDB *db, UiState *ui)
{
    float t = (float)GetTime();

    /* Base bar */
    fill_rect(0, 0, WIN_W, HUD_H, C_PANEL);

    /* Animated left accent stripe */
    Color stripe = C_ACCENT;
    stripe.a = (unsigned char)(180 + 60 * sinf(t * 1.5f));
    fill_rect(0, 0, 4, HUD_H, stripe);

    /* Logo */
    label("SENTINEL", 14, 10, 24, C_ACCENT);
    label("STORAGE", 14, 34, 12, C_SUBTEXT);

    /* System health badge */
    int bx = 160;
    if (ui->audit.chain_valid) {
        Color bg = {10, 60, 30, 220};
        fill_rrect((Rectangle){(float)bx, 12, 140, 32}, 0.3f, bg);
        stroke_rrect((Rectangle){(float)bx, 12, 140, 32}, 0.3f, C_GREEN, 1.5f);
        glow_dot(bx+20, 28, 5, C_GREEN, t);
        label("SECURE", bx+32, 19, 16, C_GREEN);
    } else {
        float breach_a = 0.6f + 0.4f * sinf(t * 6.0f);
        Color bg = {80, 8, 8, 220};
        fill_rrect((Rectangle){(float)bx, 12, 190, 32}, 0.3f, bg);
        Color rb = C_RED; rb.a = (unsigned char)(220*breach_a);
        stroke_rrect((Rectangle){(float)bx-2, 10, 194, 36}, 0.3f, rb, 2.5f);
        glow_dot(bx+20, 28, 5, C_RED, t);
        label("BREACH DETECTED", bx+32, 19, 14, C_RED);
    }

    /* Live metrics */
    int mx = 380;
    char buf[64];

    snprintf(buf, sizeof(buf), "H: %d", ui->perf.tree_height);
    label(buf, mx, 18, 13, C_TEAL); mx += 68;

    snprintf(buf, sizeof(buf), "PG: %llu", (unsigned long long)ui->perf.total_pages);
    label(buf, mx, 18, 13, C_ACCENT); mx += 90;

    Color cc = ui->perf.cache_hit_rate >= 60.0 ? C_GREEN : C_AMBER;
    snprintf(buf, sizeof(buf), "CACHE: %.0f%%", ui->perf.cache_hit_rate);
    label(buf, mx, 18, 13, cc); mx += 100;

    Color ic = ui->perf.integrity_score >= 99.9 ? C_GREEN : C_RED;
    snprintf(buf, sizeof(buf), "INTEGRITY: %.0f%%", ui->perf.integrity_score);
    label(buf, mx, 18, 13, ic); mx += 140;

    snprintf(buf, sizeof(buf), "R:%llu W:%llu",
             (unsigned long long)ui->perf.total_reads,
             (unsigned long long)ui->perf.total_writes);
    label(buf, mx, 18, 12, C_SUBTEXT); mx += 140;

    if (ui->perf.chaos_events > 0) {
        Color ca = C_RED; ca.a = (unsigned char)(180 + 75*sinf(t*4));
        snprintf(buf, sizeof(buf), "⚡ CHAOS: %llu",
                 (unsigned long long)ui->perf.chaos_events);
        label(buf, mx, 18, 13, ca);
    }

    /* Timestamp far right */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char ts[24];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
    label(ts, WIN_W - 90, 20, 14, C_SUBTEXT);

    hline(0, HUD_H-1, WIN_W, C_ACCENT);
}

/* ══════════════════════════════════════════════════════════════════════
 *  Breach border overlay (full-window pulsing red frame)
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_breach_border(UiState *ui)
{
    if (ui->audit.chain_valid) return;
    float t = (float)GetTime();
    float alpha = 0.4f + 0.4f * sinf(t * 5.5f);
    int   bw    = 8;
    Color br    = C_RED;
    br.a        = (unsigned char)(255 * alpha);

    /* Four border rectangles */
    DrawRectangle(0,         0,         WIN_W, bw,    br);
    DrawRectangle(0,         WIN_H-bw,  WIN_W, bw,    br);
    DrawRectangle(0,         0,         bw,    WIN_H, br);
    DrawRectangle(WIN_W-bw,  0,         bw,    WIN_H, br);

    /* Scanline tint */
    Color sl = C_RED; sl.a = (unsigned char)(18 * alpha);
    draw_scanlines(0, 0, WIN_W, WIN_H, sl);

    /* BREACH stamp centred */
    const char *stamp = "[ BLOCKCHAIN BREACH DETECTED ]";
    int sw = text_w(stamp, 28);
    int sx = WIN_W/2 - sw/2;
    int sy = WIN_H/2 - 14;

    /* shadow */
    Color sh = {0,0,0,180};
    fill_rect(sx-12, sy-6, sw+24, 40, sh);
    stroke_rrect((Rectangle){(float)(sx-12),(float)(sy-6),(float)(sw+24),40},
                 0.15f, br, 2.0f);
    Color stampc = C_RED; stampc.a = (unsigned char)(200 + 55*sinf(t*5.5f));
    label(stamp, sx, sy, 28, stampc);
}

/* ══════════════════════════════════════════════════════════════════════
 *  LEFT PANE  – B-Tree Visualizer
 * ══════════════════════════════════════════════════════════════════════ */

#define MAX_LAYOUT   512
typedef struct { BTreeNodeInfo *node; float cx, cy; } LayoutEntry;
static LayoutEntry g_layout[MAX_LAYOUT];
static int         g_layout_n = 0;

/* BFS with accumulated node widths for tight horizontal packing */
#define NODE_H     40
#define NODE_KEY_W 60
#define V_GAP      52

static void layout_tree(BTreeNodeInfo *root, float ax, float aw, float base_y)
{
    g_layout_n = 0;
    if (!root) return;

    BTreeNodeInfo *queue[MAX_LAYOUT];
    float          qx[MAX_LAYOUT], qw[MAX_LAYOUT];
    int head=0, tail=0, lv=0, lv_start=0, lv_size=1;

    queue[tail]=root; qx[tail]=ax; qw[tail]=aw; tail++;

    while (head < tail) {
        BTreeNodeInfo *n  = queue[head];
        float          nx = qx[head], nw = qw[head];
        head++;

        float cx = nx + nw/2.0f;
        float cy = base_y + 30.0f + lv*(float)(NODE_H+V_GAP);

        if (g_layout_n < MAX_LAYOUT) {
            g_layout[g_layout_n++] = (LayoutEntry){n, cx, cy};
            n->layout_x = cx; n->layout_y = cy;
        }

        if (!n->node.is_leaf) {
            int nc = (int)n->node.num_keys+1;
            float cw = nw/(float)nc;
            for (int i=0; i<nc && i<BTREE_MAX_CHILDREN; i++) {
                if (n->children[i]) {
                    queue[tail]=n->children[i];
                    qx[tail]=nx+i*cw; qw[tail]=cw; tail++;
                }
            }
        }

        if (head - lv_start >= lv_size) {
            lv_start=head; lv_size=tail-head; lv++;
        }
        if (lv > 12 || tail >= MAX_LAYOUT-1) break;
    }
}

/* Test if a page_num is in the current search path */
static int in_search_path(UiState *ui, uint64_t pg)
{
    if (ui->search_path.flash_timer <= 0) return 0;
    for (int i=0; i<ui->search_path.count; i++)
        if (ui->search_path.pages[i] == pg) return 1;
    return 0;
}

static void draw_tree_node(LayoutEntry *e, UiState *ui, BefsDB *db,
                            float ox, float oy, float zoom)
{
    BTreeNode *n  = &e->node->node;
    int        nk = (int)n->num_keys;
    float      sw = (float)(nk * NODE_KEY_W + 20);
    float      sh = NODE_H;

    /* Apply pan + zoom */
    float px = ox + e->cx * zoom;
    float py = oy + e->cy * zoom;
    float nw = sw * zoom;
    float nh = sh * zoom;

    Rectangle r = {px - nw/2.0f, py - nh/2.0f, nw, nh};

    /* Colour logic */
    float t = (float)GetTime();
    int   on_path = in_search_path(ui, e->node->page_num);

    Color fill   = n->is_leaf ? C_NODE_LEAF : C_NODE_FILL;
    Color border = C_NODE_BORDER;

    if (on_path) {
        float flash = 0.5f + 0.5f * sinf(t * 8.0f);
        border = C_NODE_SEL;
        fill.r = (unsigned char)(fill.r + (unsigned char)(60*flash));
        fill.g = (unsigned char)(fill.g + (unsigned char)(40*flash));
    }

    /* Draw node */
    fill_rrect(r, 0.25f, fill);
    stroke_rrect(r, 0.25f, border, on_path ? 2.5f : 1.5f);

    /* Keys inside */
    int fsz = (int)(12 * zoom);
    if (fsz < 6) fsz = 6;
    if (fsz > 16) fsz = 16;

    for (int k=0; k<nk; k++) {
        float kx = r.x + 10.0f*zoom + k*(float)(NODE_KEY_W)*zoom;
        if (k > 0) {
            Color sep = C_EDGE;
            DrawLine((int)(kx-1), (int)(r.y+4), (int)(kx-1), (int)(r.y+r.height-4), sep);
        }
        char trunc[12];
        strncpy(trunc, n->keys[k], 10); trunc[10]='\0';
        label(trunc, (int)(kx+2), (int)(r.y + r.height/2 - fsz/2), fsz, C_TEXT);
    }

    /* Page badge */
    if (zoom > 0.7f) {
        char pg_s[16];
        snprintf(pg_s, sizeof(pg_s), "p%llu", (unsigned long long)e->node->page_num);
        label(pg_s, (int)(r.x+2), (int)(r.y-14*zoom), (int)(9*zoom), C_SUBTEXT);
    }

    /* Click to inspect */
    if (CheckCollisionPointRec(GetMousePosition(), r) &&
        IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        ui->hex_valid    = 2;
        ui->hex.source_id = e->node->page_num;
        snprintf(ui->hex_title, sizeof(ui->hex_title),
                 "BTree Page #%llu  (%s, %d keys)",
                 (unsigned long long)e->node->page_num,
                 n->is_leaf?"leaf":"internal", nk);
        ui->hex_scroll = 0; ui->hex_target_row = 0;

        /* If leaf, also load the first data block */
        if (n->is_leaf && nk > 0 && n->data_offsets[0] != 0) {
            uint64_t bid = (n->data_offsets[0] - DATA_AREA_START) / PAGE_SIZE;
            ui->found_block_id = bid;
            ui->found_valid    = 1;
        }
        (void)db;
    }
}

static void draw_tree_edges(float ox, float oy, float zoom)
{
    for (int i=0; i<g_layout_n; i++) {
        BTreeNodeInfo *par = g_layout[i].node;
        if (par->node.is_leaf) continue;
        float px = ox + g_layout[i].cx * zoom;
        float py = oy + g_layout[i].cy * zoom + NODE_H*zoom/2.0f;
        int nc = (int)par->node.num_keys+1;
        for (int c=0; c<nc && c<BTREE_MAX_CHILDREN; c++) {
            if (!par->children[c]) continue;
            float cx = ox + par->children[c]->layout_x * zoom;
            float cy = oy + par->children[c]->layout_y * zoom - NODE_H*zoom/2.0f;
            DrawLineEx((Vector2){px,py}, (Vector2){cx,cy}, 1.5f, C_EDGE);
        }
    }
}

static void draw_split_anims(UiState *ui, float ox, float oy, float zoom)
{
    float dt = GetFrameTime();
    for (int i=0; i<MAX_SPLIT_ANIMS; i++) {
        SplitAnim *a = &ui->split_anims[i];
        if (!a->active) continue;
        a->t += dt * 2.0f;
        if (a->t >= 1.0f) { a->active=0; continue; }

        float et = ease_out_cubic(a->t);
        float cx = ox + lerpf(a->src_x, a->dst_x, et) * zoom;
        float cy = oy + lerpf(a->src_y, a->dst_y, et) * zoom;

        Color fc = a->flash_col;
        fc.a     = (unsigned char)(200 * (1.0f - a->t));
        float sz = 60.0f * zoom * (1.0f - a->t*0.5f);
        DrawCircle((int)cx, (int)cy, sz, fc);
    }
}

static void draw_left_pane(BefsDB *db, UiState *ui)
{
    /* Panel background */
    fill_rect(LEFT_X, LEFT_Y, LEFT_W, LEFT_H, C_PANEL);

    /* Header strip */
    fill_rect(LEFT_X, LEFT_Y, LEFT_W, 24, C_PANEL2);
    label("B-TREE INDEX", LEFT_X+10, LEFT_Y+6, 13, C_SUBTEXT);

    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "H=%d  zoom=%.1fx", ui->perf.tree_height, ui->zoom);
    label(hbuf, LEFT_X+LEFT_W - text_w(hbuf,11) - 8, LEFT_Y+7, 11, C_DIM);

    hline(LEFT_X, LEFT_Y+24, LEFT_W, C_DIVIDER);
    vline(LEFT_X+LEFT_W-1, LEFT_Y, LEFT_H, C_DIVIDER);

    /* Clipping region */
    BeginScissorMode(LEFT_X, LEFT_Y+24, LEFT_W, LEFT_H-24);

    /* Mouse wheel zoom */
    Vector2 mp = GetMousePosition();
    if (mp.x >= LEFT_X && mp.x < LEFT_X+LEFT_W &&
        mp.y >= LEFT_Y && mp.y < LEFT_Y+LEFT_H)
    {
        float wheel = GetMouseWheelMove();
        ui->zoom += wheel * 0.12f;
        ui->zoom  = clampf(ui->zoom, 0.3f, 3.0f);

        /* Pan with middle mouse / right drag */
        if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
            ui->dragging=1; ui->drag_ox=(float)mp.x-ui->pan_x; ui->drag_oy=(float)mp.y-ui->pan_y;
        }
    }
    if (ui->dragging) {
        if (IsMouseButtonDown(MOUSE_RIGHT_BUTTON)) {
            ui->pan_x = (float)mp.x - ui->drag_ox;
            ui->pan_y = (float)mp.y - ui->drag_oy;
        } else { ui->dragging=0; }
    }

    float ox = LEFT_X + ui->pan_x;
    float oy = LEFT_Y + 28 + ui->pan_y;

    if (!ui->tree_snap) {
        const char *msg = "Empty – insert records";
        label(msg, LEFT_X+LEFT_W/2-text_w(msg,14)/2, LEFT_Y+LEFT_H/2-7, 14, C_SUBTEXT);
    } else {
        layout_tree(ui->tree_snap, 0, (float)LEFT_W, 0);
        draw_tree_edges(ox, oy, ui->zoom);
        draw_split_anims(ui, ox, oy, ui->zoom);
        for (int i=0; i<g_layout_n; i++)
            draw_tree_node(&g_layout[i], ui, db, ox, oy, ui->zoom);
    }

    EndScissorMode();

    /* Zoom hint */
    label("scroll=zoom  RMB=pan  click=inspect", LEFT_X+8, LEFT_Y+LEFT_H-18, 10, C_DIM);
}

/* ══════════════════════════════════════════════════════════════════════
 *  CENTER PANE  – Hex-Ray Inspector
 * ══════════════════════════════════════════════════════════════════════ */

static int hex_region_at(const HexDump *h, int pos)
{
    for (int i=0; i<h->region_count; i++)
        if (pos >= h->regions[i].byte_start && pos <= h->regions[i].byte_end)
            return h->regions[i].type;
    return HEX_REGION_PAD;
}

static void draw_center_pane(UiState *ui)
{
    fill_rect(CENTER_X, CENTER_Y, CENTER_W, CENTER_H, C_PANEL);
    fill_rect(CENTER_X, CENTER_Y, CENTER_W, 24, C_PANEL2);

    /* Title */
    label("HEX-RAY INSPECTOR", CENTER_X+10, CENTER_Y+6, 13, C_SUBTEXT);
    vline(CENTER_X+CENTER_W-1, CENTER_Y, CENTER_H, C_DIVIDER);

    if (ui->hex_valid == 1) {
        /* Corrupt banner */
        if (ui->hex.is_corrupt) {
            Color bn = C_RED; bn.a = 60;
            fill_rect(CENTER_X, CENTER_Y+24, CENTER_W, 20, bn);
            label("⚠  TAMPERED BLOCK – HASH MISMATCH DETECTED",
                  CENTER_X+10, CENTER_Y+26, 13, C_RED);
        } else {
            label(ui->hex_title, CENTER_X+10, CENTER_Y+26, 12, C_ACCENT);
        }
    } else {
        label(ui->hex_valid==0 ? "Click a node or block to inspect raw bytes"
                               : "Loading…",
              CENTER_X+10, CENTER_Y+26, 12, C_SUBTEXT);
    }

    hline(CENTER_X, CENTER_Y+44, CENTER_W, C_DIVIDER);

    /* Legend */
    static const struct { HexRegionType t; const char *name; } LEG[] = {
        {HEX_REGION_HEADER, "Header"},
        {HEX_REGION_KEYS,   "Keys/Data"},
        {HEX_REGION_HASH,   "Hashes"},
        {HEX_REGION_CORRUPT,"Corrupt"},
        {HEX_REGION_PAD,    "Padding"},
    };
    int lx = CENTER_X+8;
    for (int i=0; i<5; i++) {
        Color lc = HEX_REG_COLORS[LEG[i].t];
        DrawRectangle(lx, CENTER_Y+48, 12, 10, lc);
        label(LEG[i].name, lx+15, CENTER_Y+47, 10, C_SUBTEXT);
        lx += 14 + text_w(LEG[i].name, 10) + 10;
    }
    hline(CENTER_X, CENTER_Y+62, CENTER_W, C_DIVIDER);

    if (ui->hex_valid != 1) return;

    /* Scroll with mouse wheel inside center pane */
    int bytes_per_row = 16;
    int row_h         = 17;
    int header_h      = 66;
    int visible_rows  = (CENTER_H - header_h - 8) / row_h;
    int total_rows    = PAGE_SIZE / bytes_per_row;   /* 256 */

    Vector2 mp = GetMousePosition();
    if (mp.x >= CENTER_X && mp.x < CENTER_X+CENTER_W &&
        mp.y >= CENTER_Y+header_h && mp.y < CENTER_Y+CENTER_H)
    {
        float wh = GetMouseWheelMove();
        ui->hex_scroll -= (int)(wh * 3);
    }
    if (ui->hex_target_row >= 0) {
        /* Auto-scroll to target row */
        int target = ui->hex_target_row - visible_rows/2;
        if (target < 0) target = 0;
        ui->hex_scroll   = target;
        ui->hex_target_row = -1;
    }
    if (ui->hex_scroll < 0) ui->hex_scroll = 0;
    if (ui->hex_scroll > total_rows - visible_rows)
        ui->hex_scroll = total_rows - visible_rows;

    int gy = CENTER_Y + header_h + 4;
    int gx = CENTER_X + 8;

    /* Column header */
    label("Offset  ", gx, gy, 11, C_DIM);
    for (int c=0; c<bytes_per_row; c++) {
        int hx = gx + 56 + c*22 + (c>=8 ? 6:0);
        char ch[4]; snprintf(ch, sizeof(ch), "%02X", c);
        label(ch, hx, gy, 10, C_DIM);
    }
    label("  ASCII", gx + 56 + bytes_per_row*22 + 10, gy, 10, C_DIM);
    gy += 14;
    hline(CENTER_X+4, gy, CENTER_W-8, C_DIVIDER);
    gy += 3;

    for (int row = ui->hex_scroll;
         row < ui->hex_scroll+visible_rows && row < total_rows; row++)
    {
        int y = gy + (row - ui->hex_scroll) * row_h;

        /* Offset */
        char off_s[10]; snprintf(off_s, sizeof(off_s), "%04X", row*bytes_per_row);
        Color offc = C_SUBTEXT;
        label(off_s, gx, y, 11, offc);

        /* Row background for non-padding regions */
        int row_reg = hex_region_at(&ui->hex, row*bytes_per_row);
        if (row_reg != HEX_REGION_PAD) {
            Color rbg = HEX_REG_COLORS[row_reg]; rbg.a = 20;
            fill_rect(gx+54, y-1, CENTER_W-gx-54-20, row_h, rbg);
        }

        /* Hex bytes */
        for (int col=0; col<bytes_per_row; col++) {
            int pos = row*bytes_per_row+col;
            if (pos >= PAGE_SIZE) break;
            uint8_t b   = ui->hex.raw[pos];
            int     reg = hex_region_at(&ui->hex, pos);
            Color   bc  = HEX_REG_COLORS[reg];

            int bx = gx + 56 + col*22 + (col>=8 ? 6:0);

            /* Byte background tint */
            if (b != 0) {
                Color bg2 = bc; bg2.a = 35;
                fill_rect(bx-1, y, 20, row_h-1, bg2);
            }

            char hx[4]; snprintf(hx, sizeof(hx), "%02X", b);
            label(hx, bx, y, 11, b ? bc : C_DIM);
        }

        /* ASCII side */
        int ax = gx + 56 + bytes_per_row*22 + 12;
        for (int col=0; col<bytes_per_row; col++) {
            int     pos = row*bytes_per_row+col;
            if (pos >= PAGE_SIZE) break;
            uint8_t b   = ui->hex.raw[pos];
            int     reg = hex_region_at(&ui->hex, pos);
            char    ch  = (b>=32 && b<127)?(char)b:'.';
            char    cs[2]={(char)ch,0};
            Color   ac  = HEX_REG_COLORS[reg]; ac.a = b ? 200 : 50;
            label(cs, ax+col*8, y, 11, ac);
        }
    }

    /* Scrollbar */
    int sb_x = CENTER_X+CENTER_W-10;
    int sb_h  = CENTER_H - header_h - 8;
    int th_h  = (int)((float)visible_rows/total_rows*sb_h);
    int th_y  = CENTER_Y+header_h+4 + (int)((float)ui->hex_scroll/total_rows*sb_h);
    fill_rect(sb_x, CENTER_Y+header_h+4, 6, sb_h, C_PANEL2);
    fill_rect(sb_x, th_y, 6, th_h, C_ACCENT);

    /* Region annotation on right side of scrollbar */
    if (ui->hex.region_count > 0) {
        for (int i=0; i<ui->hex.region_count && i<8; i++) {
            HexRegion *hr = &ui->hex.regions[i];
            if (hr->type == HEX_REGION_PAD) continue;
            int r_row_start = hr->byte_start / bytes_per_row;
            int ann_y = CENTER_Y+header_h+4+17 +
                        (int)((float)r_row_start / total_rows * sb_h);
            Color rc = HEX_REG_COLORS[hr->type]; rc.a = 180;
            fill_rect(sb_x+8, ann_y, CENTER_W - (sb_x+8) - CENTER_X - 4, 2, rc);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Sparkline graph
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_sparkline(float *history, int len, int head,
                            int x, int y, int w, int h,
                            float lo, float hi, Color line_col,
                            const char *title, const char *unit)
{
    /* Background */
    fill_rrect((Rectangle){(float)x,(float)y,(float)w,(float)h}, 0.15f,
               (Color){14,18,36,255});
    stroke_rrect((Rectangle){(float)x,(float)y,(float)w,(float)h}, 0.15f,
                 C_BORDER, 1.0f);

    label(title, x+6, y+5, 11, C_SUBTEXT);

    /* Current value label */
    float cur = history[(head + len - 1) % len];
    char vbuf[24]; snprintf(vbuf, sizeof(vbuf), "%.1f%s", cur, unit);
    label(vbuf, x+w-text_w(vbuf,12)-6, y+5, 12, line_col);

    /* Grid lines */
    for (int g=0; g<=4; g++) {
        int gy2 = y + 22 + (h-28) * g / 4;
        hline(x+4, gy2, w-8, (Color){22,28,55,255});
    }

    /* Sparkline path */
    int gw = w - 10, gh = h - 30, gx = x+5, gy = y+22;
    if (hi <= lo) hi = lo + 1;

    for (int i=1; i<len; i++) {
        int   i0  = (head + i - 1) % len;
        int   i1  = (head + i    ) % len;
        float v0  = clampf((history[i0]-lo)/(hi-lo), 0,1);
        float v1  = clampf((history[i1]-lo)/(hi-lo), 0,1);
        float x0  = (float)gx + (float)(i-1)/(len-1)*(float)gw;
        float x1  = (float)gx + (float)i    /(len-1)*(float)gw;
        float y0  = (float)(gy+gh) - v0*(float)gh;
        float y1  = (float)(gy+gh) - v1*(float)gh;

        /* Fill under line */
        Color fill = line_col; fill.a=25;
        Vector2 pts[4] = {{x0,y0},{x1,y1},{x1,(float)(gy+gh)},{x0,(float)(gy+gh)}};
        DrawTriangle(pts[0],pts[3],pts[1], fill);
        DrawTriangle(pts[3],pts[2],pts[1], fill);

        DrawLineEx((Vector2){x0,y0},(Vector2){x1,y1}, 1.8f, line_col);
    }

    /* Current dot */
    float cx2 = (float)(gx + gw);
    float cy2  = (float)(gy+gh) - clampf((cur-lo)/(hi-lo),0,1)*(float)gh;
    DrawCircle((int)cx2,(int)cy2,3,line_col);
}

/* ══════════════════════════════════════════════════════════════════════
 *  RIGHT PANE  – Control Center
 * ══════════════════════════════════════════════════════════════════════ */

typedef struct {
    Rectangle r;
    Color     normal, hover, pressed, border;
    const char *label_str;
    int       font_sz;
} StyledButton;

static int draw_button(StyledButton *btn)
{
    Vector2 mp  = GetMousePosition();
    int     hot = CheckCollisionPointRec(mp, btn->r);
    int     clk = hot && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    Color bg = hot ? btn->hover : btn->normal;
    if (hot && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) bg = btn->pressed;

    fill_rrect(btn->r, 0.22f, bg);
    if (hot) {
        Color glow = btn->border; glow.a = 80;
        DrawRectangleRoundedLines(btn->r, 0.22f, 8, 3.5f, glow);
    }
    stroke_rrect(btn->r, 0.22f, btn->border, hot ? 2.0f : 1.2f);

    int tw = text_w(btn->label_str, btn->font_sz);
    int tx = (int)(btn->r.x + btn->r.width/2 - tw/2);
    int ty = (int)(btn->r.y + btn->r.height/2 - btn->font_sz/2);
    label(btn->label_str, tx, ty, btn->font_sz, C_TEXT);
    return clk;
}

static void text_input(const char *lbl, char *buf, size_t max_len,
                        int focused, int x, int y, int w)
{
    label(lbl, x, y-16, 12, C_SUBTEXT);
    Color border = focused ? C_ACCENT : (Color){36,50,96,255};
    Rectangle r  = {(float)x,(float)y,(float)w,26};
    fill_rrect(r, 0.18f, (Color){12,17,35,255});
    stroke_rrect(r, 0.18f, border, 1.2f);
    label(buf, x+6, y+6, 13, C_TEXT);
    if (focused) {
        int tw2 = text_w(buf, 13);
        if ((int)(GetTime()*2)%2==0)
            DrawLine(x+6+tw2+2, y+5, x+6+tw2+2, y+20, C_ACCENT);
    }
}

static void draw_right_pane(BefsDB *db, UiState *ui)
{
    fill_rect(RIGHT_X, RIGHT_Y, RIGHT_W, RIGHT_H, C_PANEL);
    fill_rect(RIGHT_X, RIGHT_Y, RIGHT_W, 24, C_PANEL2);
    label("CONTROL CENTER", RIGHT_X+10, RIGHT_Y+6, 13, C_SUBTEXT);
    hline(RIGHT_X, RIGHT_Y+24, RIGHT_W, C_DIVIDER);

    int px = RIGHT_X+12, py = RIGHT_Y+32;
    float t = (float)GetTime();

    /* ── Mode tabs ── */
    const char *tabs[3] = {"F1 INSERT","F2 SEARCH","F3 DELETE"};
    for (int i=0; i<3; i++) {
        int tx2 = px + i*116;
        Color tc = (ui->mode==(OpMode)i) ? C_ACCENT : C_SUBTEXT;
        label(tabs[i], tx2, py, 12, tc);
        if (ui->mode==(OpMode)i)
            hline(tx2, py+16, text_w(tabs[i],12), C_ACCENT);
    }
    if (IsKeyPressed(KEY_F1)) ui->mode=MODE_INSERT;
    if (IsKeyPressed(KEY_F2)) ui->mode=MODE_SEARCH;
    if (IsKeyPressed(KEY_F3)) ui->mode=MODE_DELETE;
    py += 26;
    hline(RIGHT_X, py, RIGHT_W, C_DIVIDER);
    py += 10;

    /* ── Key input ── */
    text_input("Key:", ui->key_buf, KEY_MAX_LEN,
               ui->key_focused, px, py, RIGHT_W-24);
    py += 40;

    if (ui->mode == MODE_INSERT) {
        text_input("Value:", ui->val_buf, VALUE_MAX_LEN,
                   ui->val_focused, px, py, RIGHT_W-24);
        py += 40;
    }

    /* Keyboard routing */
    char *active_buf = ui->key_focused ? ui->key_buf
                     : ui->val_focused ? ui->val_buf : NULL;
    size_t active_max = ui->key_focused ? KEY_MAX_LEN : VALUE_MAX_LEN;

    if (active_buf) {
        int ch = GetCharPressed();
        while (ch > 0) {
            int cur = (int)strlen(active_buf);
            if (ch>=32 && ch<=125 && cur<(int)active_max-1) {
                active_buf[cur]=(char)ch; active_buf[cur+1]='\0';
            }
            ch = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int cur = (int)strlen(active_buf);
            if (cur>0) active_buf[cur-1]='\0';
        }
    }
    if (IsKeyPressed(KEY_TAB) && ui->mode==MODE_INSERT) {
        ui->key_focused=!ui->key_focused;
        ui->val_focused=!ui->val_focused;
    }

    /* ── Execute button ── */
    const char *exec_lbl = ui->mode==MODE_INSERT ? "  INSERT  [Enter]"
                         : ui->mode==MODE_SEARCH ? "  SEARCH  [Enter]"
                                                 : "  DELETE  [Enter]";
    StyledButton exec_btn = {
        {(float)px,(float)py,(float)(RIGHT_W-24),32},
        {30,70,150,255},{40,100,200,255},{22,55,120,255},
        C_ACCENT, exec_lbl, 14
    };
    int do_exec = draw_button(&exec_btn) || IsKeyPressed(KEY_ENTER);
    py += 40;

    if (do_exec && strlen(ui->key_buf)>0) {
        int pages_before = (int)db->sb.next_free_page;
        BefsStatus s;

        if (ui->mode==MODE_INSERT) {
            s = btree_insert(db, ui->key_buf, ui->val_buf);
            if (s==BEFS_OK)              snprintf(ui->msg_buf,sizeof(ui->msg_buf),"✓ Inserted: \"%s\"",ui->key_buf);
            else if (s==BEFS_ERR_DUPLICATE) snprintf(ui->msg_buf,sizeof(ui->msg_buf),"⚠ Duplicate key");
            else                         snprintf(ui->msg_buf,sizeof(ui->msg_buf),"✗ Insert error: %d",s);
            ui->msg_ok = (s==BEFS_OK);

            /* Detect split: page count increased by more than 1 */
            int pages_after = (int)db->sb.next_free_page;
            if (pages_after - pages_before > 1) {
                /* Trigger split animation on a free slot */
                for (int i=0; i<MAX_SPLIT_ANIMS; i++) {
                    if (!ui->split_anims[i].active) {
                        ui->split_anims[i].active  = 1;
                        ui->split_anims[i].t       = 0.0f;
                        ui->split_anims[i].src_x   = (float)(LEFT_W/2);
                        ui->split_anims[i].src_y   = (float)(LEFT_H/3);
                        ui->split_anims[i].dst_x   = (float)(LEFT_W/4);
                        ui->split_anims[i].dst_y   = (float)(LEFT_H/2);
                        ui->split_anims[i].flash_col = C_NODE_SPLIT;
                        if (i+1 < MAX_SPLIT_ANIMS) {
                            ui->split_anims[i+1].active  = 1;
                            ui->split_anims[i+1].t       = 0.0f;
                            ui->split_anims[i+1].src_x   = (float)(LEFT_W/2);
                            ui->split_anims[i+1].src_y   = (float)(LEFT_H/3);
                            ui->split_anims[i+1].dst_x   = (float)(3*LEFT_W/4);
                            ui->split_anims[i+1].dst_y   = (float)(LEFT_H/2);
                            ui->split_anims[i+1].flash_col = C_ACCENT;
                        }
                        break;
                    }
                }
            }

        } else if (ui->mode==MODE_SEARCH) {
            char vout[VALUE_MAX_LEN]={0};
            uint64_t blk_off=0;
            s = btree_search_with_path(db, ui->key_buf, vout, &blk_off,
                                       &ui->search_path);
            if (s==BEFS_OK) {
                snprintf(ui->msg_buf,sizeof(ui->msg_buf),"✓ Found: \"%s\"",vout);
                /* Compute block_id and load hex */
                if (blk_off >= DATA_AREA_START) {
                    uint64_t bid = (blk_off - DATA_AREA_START) / PAGE_SIZE;
                    ui->found_block_id = bid;
                    ui->found_valid    = 1;
                    /* Is this block corrupt? */
                    int corrupt = (ui->audit.total_blocks > bid &&
                                   !ui->audit.results[bid].valid);
                    hex_dump_data_block(db, bid, &ui->hex, corrupt);
                    ui->hex_valid = 1;
                    /* Auto-scroll to hash region (row 344/16 = 21) */
                    ui->hex_target_row = 344/16;
                    snprintf(ui->hex_title, sizeof(ui->hex_title),
                             "DataBlock #%llu  [key=%s]",
                             (unsigned long long)bid, ui->key_buf);
                }
            } else {
                snprintf(ui->msg_buf,sizeof(ui->msg_buf),"✗ Key not found");
                ui->search_path.count = 0;
            }
            ui->msg_ok = (s==BEFS_OK);

        } else {
            s = btree_delete(db, ui->key_buf);
            snprintf(ui->msg_buf,sizeof(ui->msg_buf),
                     s==BEFS_OK ? "✓ Deleted: \"%s\"" : "✗ Not found", ui->key_buf);
            ui->msg_ok = (s==BEFS_OK);
        }

        ui->msg_timer  = 4.5f;
        ui->audit_dirty = 1;
        btree_free_snapshot(ui->tree_snap);
        ui->tree_snap = btree_snapshot(db);
    }

    /* Result message */
    if (ui->msg_timer > 0) {
        Color mc = ui->msg_ok ? C_GREEN : C_RED;
        mc.a = (unsigned char)(255 * clampf(ui->msg_timer, 0,1));
        label(ui->msg_buf, px, py, 13, mc);
        ui->msg_timer -= GetFrameTime();
    }
    py += 24;

    /* ── Divider ── */
    hline(RIGHT_X, py, RIGHT_W, C_DIVIDER);
    py += 10;

    /* ── AUDIT button ── */
    StyledButton audit_btn = {
        {(float)px,(float)py,(float)(RIGHT_W-24),30},
        {20,50,20,255},{30,80,35,255},{15,40,18,255},
        C_GREEN, "  RUN BLOCKCHAIN AUDIT  [Ctrl+A]", 13
    };
    if (draw_button(&audit_btn) ||
        (IsKeyDown(KEY_LEFT_CONTROL) && IsKeyPressed(KEY_A)))
    {
        blockchain_audit(db, &ui->audit);
        ui->audit_timer = 5.0f;
        ui->audit_dirty = 0;
        uint64_t valid = ui->audit.total_blocks - ui->audit.corrupt_count;
        db->stats.integrity_score = ui->audit.total_blocks
            ? (double)valid/(double)ui->audit.total_blocks*100.0 : 100.0;
    }
    py += 38;

    /* ── CHAOS MONKEY button ── */
    float chaos_pulse = 0.5f + 0.5f * sinf(t * 4.0f);
    Color chaos_bg    = {(unsigned char)(90+40*(int)chaos_pulse),15,15,255};
    Color chaos_bgh   = {150,25,25,255};
    StyledButton chaos_btn = {
        {(float)px,(float)py,(float)(RIGHT_W-24),34},
        chaos_bg, chaos_bgh, {70,10,10,255},
        C_RED, "  ⚡ TAMPER: INJECT BIT FLIP", 14
    };
    if (draw_button(&chaos_btn)) {
        uint64_t bid=0;
        if (chaos_monkey_corrupt(db, &bid) == BEFS_OK) {
            ui->chaos_block = bid;
            ui->chaos_valid = 1;
            ui->chaos_flash = 3.0f;
            ui->audit_dirty = 1;

            /* Immediately load that block in hex with corrupt=1 */
            hex_dump_data_block(db, bid, &ui->hex, 1);
            ui->hex_valid = 1;
            ui->hex_target_row = 84/16;  /* scroll to payload */
            snprintf(ui->hex_title, sizeof(ui->hex_title),
                     "DataBlock #%llu  *** BIT FLIPPED ***", (unsigned long long)bid);
        }
    }
    py += 42;

    /* Chaos flash message */
    if (ui->chaos_valid && ui->chaos_flash > 0) {
        float alpha = clampf(ui->chaos_flash, 0, 1);
        Color cc = C_RED; cc.a = (unsigned char)(255*alpha);
        char cmsg[80];
        snprintf(cmsg, sizeof(cmsg), "⚡ Bit-flip injected → Block #%llu",
                 (unsigned long long)ui->chaos_block);
        label(cmsg, px, py, 12, cc);
        ui->chaos_flash -= GetFrameTime();
        py += 20;
    }

    hline(RIGHT_X, py, RIGHT_W, C_DIVIDER);
    py += 8;

    /* ── Blockchain chain status ── */
    label("CHAIN STATUS", px, py, 12, C_SUBTEXT); py += 18;

    int visible_blk = (WIN_H - py - 220) / 22;
    if (visible_blk < 0) visible_blk = 0;
    uint64_t blk_start = 0;
    if (ui->audit.total_blocks > (uint64_t)visible_blk)
        blk_start = ui->audit.total_blocks - visible_blk;

    for (uint64_t i=blk_start; i<ui->audit.total_blocks && i<MAX_BLOCKS; i++) {
        BlockAuditResult *r = &ui->audit.results[i];
        int ry = py + (int)(i-blk_start)*22;
        Color lc = r->valid ? C_GREEN : C_RED;
        Rectangle lr = {(float)px,(float)ry,(float)(RIGHT_W-24),20};
        fill_rrect(lr, 0.2f, (Color){14,18,36,255});
        stroke_rrect(lr, 0.2f, lc, 1.0f);

        char bs[48];
        snprintf(bs, sizeof(bs), "#%llu  %.14s…", (unsigned long long)r->block_id,
                 r->stored_hash);
        label(bs, px+6, ry+4, 11, r->valid ? C_TEXT : C_RED);
        DrawCircle(px+RIGHT_W-36, ry+10, 4, lc);

        /* Click to inspect */
        if (CheckCollisionPointRec(GetMousePosition(), lr) &&
            IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        {
            int corrupt = !r->valid;
            hex_dump_data_block(db, i, &ui->hex, corrupt);
            ui->hex_valid = 1;
            ui->hex_scroll = 0; ui->hex_target_row = 344/16;
            snprintf(ui->hex_title, sizeof(ui->hex_title),
                     "DataBlock #%llu  [%s]",
                     (unsigned long long)i, r->valid?"VALID":"CORRUPT");
        }
    }
    if (ui->audit.total_blocks==0)
        label("(no blocks)", px, py, 13, C_DIM);

    /* ── Sparkline graphs (bottom of right pane) ── */
    int sg_y = WIN_H - 210;
    int sg_h = 90;
    int sg_w = RIGHT_W - 20;

    draw_sparkline(ui->perf.cache_history, SPARKLINE_LEN,
                   ui->perf.history_head,
                   RIGHT_X+8, sg_y, sg_w, sg_h,
                   0.0f, 100.0f, C_TEAL,
                   "Cache Hit Rate", "%");

    draw_sparkline(ui->perf.io_latency_history, SPARKLINE_LEN,
                   ui->perf.history_head,
                   RIGHT_X+8, sg_y+sg_h+8, sg_w, sg_h,
                   0.0f, 10.0f, C_PINK,
                   "Disk I/O Latency", "ms");
}

/* ══════════════════════════════════════════════════════════════════════
 *  Sparkline data updater
 * ══════════════════════════════════════════════════════════════════════ */

static void update_sparklines(PerfStats *ps)
{
    int h = ps->history_head;
    ps->cache_history[h]      = (float)ps->cache_hit_rate;
    ps->io_latency_history[h] = ps->last_io_ms;
    ps->history_head = (h + 1) % SPARKLINE_LEN;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Main GUI Loop
 * ══════════════════════════════════════════════════════════════════════ */

void gui_run(BefsDB *db)
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_HIGHDPI);
    InitWindow(WIN_W, WIN_H,
               "SentinelStorage v3 – Forensic Blockchain Dashboard");
    SetTargetFPS(60);

    UiState ui;
    memset(&ui, 0, sizeof(ui));
    ui.zoom           = 1.0f;
    ui.key_focused    = 1;
    ui.mode           = MODE_INSERT;
    ui.chaos_block    = UINT64_MAX;
    ui.hex_target_row = -1;
    ui.audit_timer    = 0;
    ui.perf_timer     = 0;

    /* Initialise history with a baseline */
    for (int i=0; i<SPARKLINE_LEN; i++) {
        db->stats.cache_history[i]      = 0;
        db->stats.io_latency_history[i] = 0;
    }

    /* Initial state */
    blockchain_audit(db, &ui.audit);
    btree_height(db);
    ui.tree_snap = btree_snapshot(db);
    memcpy(&ui.perf, &db->stats, sizeof(PerfStats));
    ui.perf.integrity_score = ui.audit.total_blocks
        ? (double)(ui.audit.total_blocks-ui.audit.corrupt_count)
          /(double)ui.audit.total_blocks*100.0
        : 100.0;

    float sparkline_timer = 0;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();

        /* ── Periodic auto-audit ── */
        ui.audit_timer -= dt;
        if (ui.audit_timer <= 0 || ui.audit_dirty) {
            blockchain_audit(db, &ui.audit);
            ui.audit_timer  = 5.0f;
            ui.audit_dirty  = 0;
            uint64_t valid = ui.audit.total_blocks - ui.audit.corrupt_count;
            db->stats.integrity_score = ui.audit.total_blocks
                ? (double)valid/(double)ui.audit.total_blocks*100.0 : 100.0;
            ui.breach_pulse = ui.audit.chain_valid ? 0 : 1;
        }

        /* ── Periodic perf refresh ── */
        ui.perf_timer -= dt;
        if (ui.perf_timer <= 0) {
            btree_height(db);
            db->stats.total_pages = db->sb.next_free_page;
            memcpy(&ui.perf, &db->stats, sizeof(PerfStats));
            ui.perf.integrity_score = ui.audit.total_blocks
                ? (double)(ui.audit.total_blocks-ui.audit.corrupt_count)
                  /(double)ui.audit.total_blocks*100.0 : 100.0;
            ui.perf_timer = 0.5f;
        }

        /* ── Sparkline update ── */
        sparkline_timer -= dt;
        if (sparkline_timer <= 0) {
            /* Simulate I/O latency as small random variation */
            db->stats.last_io_ms = 0.5f + 1.5f*(float)((rand()%100)/100.0f)
                                   + (db->stats.total_reads
                                      ? (float)(db->stats.total_writes*2.0f
                                        /db->stats.total_reads) : 0);
            update_sparklines(&db->stats);
            memcpy(ui.perf.cache_history,     db->stats.cache_history,
                   sizeof(db->stats.cache_history));
            memcpy(ui.perf.io_latency_history, db->stats.io_latency_history,
                   sizeof(db->stats.io_latency_history));
            ui.perf.history_head = db->stats.history_head;
            sparkline_timer = 0.25f;
        }

        /* ── Resolve pending hex dumps ── */
        if (ui.hex_valid == 2) {
            if (hex_dump_btree_page(db, ui.hex.source_id, &ui.hex) == BEFS_OK)
                ui.hex_valid = 1;
            else ui.hex_valid = 0;
        } else if (ui.hex_valid == 3) {
            int corrupt = (ui.audit.total_blocks > ui.hex.source_id &&
                           !ui.audit.results[ui.hex.source_id].valid);
            if (hex_dump_data_block(db, ui.hex.source_id, &ui.hex, corrupt) == BEFS_OK)
                ui.hex_valid = 1;
            else ui.hex_valid = 0;
        }

        /* ── Search path timer ── */
        if (ui.search_path.flash_timer > 0)
            ui.search_path.flash_timer -= dt;

        /* ── Draw ── */
        BeginDrawing();
        ClearBackground(C_VOID);

        draw_hud(db, &ui);
        draw_left_pane(db, &ui);
        draw_center_pane(&ui);
        draw_right_pane(db, &ui);
        draw_breach_border(&ui);

        EndDrawing();
    }

    btree_free_snapshot(ui.tree_snap);
    CloseWindow();
}
