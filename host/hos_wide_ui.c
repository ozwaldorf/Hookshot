/* (hookshot) Ultrawide menus: Extended. The HUD and menus keep their 16:9 size (hos_wide_squeeze, nx2/oot_wide.py HUD) and
   what sits at a side of the 16:9 canvas moves out to the same side of the wider screen; panes spanning the canvas are
   drawn the screen's width.

   The game's 2D layouts keep their panes in an array (the layout's +0x100, 0xb0 bytes each). f_8154cab0 makes a pane's
   final 3x4 transform (+0x28, the translation at +0x4c) from its own position, scale and rotation and its parent's
   transform, only when the pane is marked for it (dirty bit 6, f_8154caa0). nx2/oot_wide.py calls in here:

   - hos_wide_ui_parent: the parent f_8154cab0 multiplies by (0 for none), on the thread that is making the transform
   - hos_wide_ui_placed: the transform just stored. The pane is noted (its parent, kind, and its place across the canvas as
     the game made it, without the moves); if the plan moves it, its translation is moved, and its children follow, since
     theirs are made from it
   - hos_wide_ui_size: a pane's size as drawn (f_8154ac30): a pane the plan widens is drawn the screen's width
   - hos_wide_ui_built: a layout's panes just built (f_81545d60): a list kept at the screen's width widened before it opens
   - hos_wide_ui_force (read in f_8154caa0): while set, every pane remakes its transform: after a change of the setting, of
     the screen's shape, or of the plan

   The plan (hos_wide_ui_tick, each present) walks the noted panes as a tree, parts' own layouts included, with how far
   each one's contents reach across (as the ultrawide UI romfs mod's rules.py does from the layout files): a pane spanning
   the canvas is widened; one touching an edge, or the panes on one side of their parent together when they reach an edge
   or sit far out, move out to that side; a container across the centre is looked into; what is off the canvas (a page
   waiting to slide in) or wider than it (a scrolling strip) is left alone.

   The pause menu's pages sit along two strips (PlusMenu_00:N_MenuChange_00/01) every 4280 units, the strip slid to the page
   shown. On a wider screen the next page would show at the edges: as the mod's menu.py, the pages are spaced 4280 + 2 x the
   extra apart (a strip's pane moves by its page times 2 x the extra, as well as by its own side move) and the strip's own x
   is scaled by the new spacing over 4280. Their background, P_MenuBg_00, one picture behind all the pages centred on the
   Equipment page, is drawn in three pieces there instead of a wider texture: its left half the extra further left, its
   right half the extra further right, the column at its centre stretched across the gap (hos_wide_ui_bg).

   HOS_WIDE_UI=0 or 1 stands over the Insert menu's choice; HOS_WIDE_UI_LOG=<n> logs the first n changes of the plan. */
#include "hos.h"
#include "../gpu/vk/vk_api.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int rx_option(int id, int otherwise);
extern volatile float hos_wide_squeeze;

enum {
    LAYOUT_RES = 0xf8, RES_NAME = 0x18, RES_PANE_NAMES = 0x20,   /* (the layout file's UIL header: its name, its panes') */
    LAYOUT_PANES = 0x100, LAYOUT_COUNT = 0x13c, PANE_SIZE = 0xb0,
    PANE_LAYOUT = 0x18, PANE_XX = 0x28, PANE_X = 0x4c, PANE_W = 0x58, PANE_FLAGS = 0xa8,
    PANE_TYPE_NULL = 0, PANE_TYPE_SCISSOR = 4, PANE_TYPE_CAPTURE = 5, PANE_TYPE_PART = 6, PANE_TYPE_HOST = 7,   /* (7: holds other layouts, a list) */
    PANE_DIRTY_RECT = 4,
    WIDE_MENUS_EXTENDED = 1,
};
static const float CANVAS_HALF = 1920.0f;   /* (every layout's canvas is 3840 x 2160) */
static const float EDGE_NEAR = 150.0f;      /* (a pane within this of an edge is anchored to it) */
static const float EDGE_TOL = 40.0f;        /* (a pane within this of both edges spans the canvas) */
static const float SIDE = 1100.0f;          /* (a group centred further out than this sits at a side) */
static const float CROSS = 0.25f;           /* (a pane with less than this share across the centre belongs to one side) */
static const float SIZE_CAP = 960.0f;       /* (wider panes count by their origin only: glows, hit boxes, backdrops) */
static const float PAGE_STRIDE = 4280.0f;   /* (the pause menu's pages along their strips) */

volatile uint32_t hos_wide_ui_force;   /* (read by nx2/oot_wide.py's site in f_8154caa0) */
static volatile int ui_on;
static volatile float ui_extra;        /* (the canvas units a side pane moves out: the screen's extra half width) */
static volatile float ui_widen = 1.0f; /* (a spanning pane's width factor) */

/* A pane seen, by its guest address: its parent, kind, and place across the canvas without the moves (x0, x1 its rectangle,
   ox its origin); plan, how far the plan moves it (its own move); space, how far its page moves along a pause menu strip;
   widen, the plan draws it the screen's width; moved, how far its stored transform was moved, its own and its ancestors'
   together, pushed the side moves among them. Open addressing; emptied when three quarters full, every pane then placed
   again. The game's threads write their panes; the plan reads them all */
typedef struct {
    uint64_t key, parent, layout, lname, pname;   /* (lname, pname: its layout's name and its own, guest strings) */
    float x0, x1, ox, plan, space, moved, pushed;
    float wide;            /* (its drawn width times this: 0 or 1, as the game draws it) */
    float from;            /* (its parent's ox when it was placed, NAN if its parent was not noted: its place in it) */
    float size_game, size_set;   /* (a list's own width: the game's, and the wider one written over it) */
    uint32_t type;
    uint16_t lkind, rule;  /* (its layout's kind and its own rule, by name: LK_ and RULE_ below) */
    uint32_t dead;         /* (its layout let it go: its slot is taken by the next new pane) */
} Node;
enum { NODES = 1 << 16 };
static Node nodes[NODES];
static volatile int nodes_used;

static uint32_t node_slot(uint64_t key) { return (uint32_t)((key >> 4) * 0x9E3779B1u) & (NODES - 1); }

static Node *node_find(uint64_t key) {
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        const uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &nodes[i];
        if (!k) return NULL;
    }
    return NULL;
}

static void nodes_clear(void) {
    memset(nodes, 0, sizeof nodes);
    __atomic_store_n(&nodes_used, 0, __ATOMIC_RELAXED);
    hos_wide_ui_force = 3;
}

static void node_fresh(Node *n) { memset((char *)n + sizeof n->key, 0, sizeof *n - sizeof n->key); }

/* The pane's node: its own, else an empty slot or one a let-go pane had (a menu opened again puts its panes at new
   addresses: the old ones' slots are used again, so the table does not fill with them) */
static Node *node_take(uint64_t key) {
    Node *own = node_find(key);
    if (own) {
        if (own->dead) node_fresh(own);
        return own;
    }
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (!k && __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (__atomic_add_fetch(&nodes_used, 1, __ATOMIC_RELAXED) > NODES * 3 / 4) {
                hos_log("wide ui: %d panes noted: noted afresh", NODES * 3 / 4);
                nodes_clear();
                return NULL;
            }
            return &nodes[i];
        }
        if (k && __atomic_load_n(&nodes[i].dead, __ATOMIC_ACQUIRE) &&
            __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            node_fresh(&nodes[i]);
            return &nodes[i];
        }
    }
    static int told;
    if (!told) told = 1, hos_log("wide ui: no slot for pane %llx (64 taken in a row)", (unsigned long long)key);
    return NULL;
}

/* ---- names: the layouts and panes the ultrawide UI romfs mod's rules.py and menu.py treat by hand ---- */

enum { LK_PLAIN, LK_WORLD, LK_PAGE, LK_CANVAS, LK_WHOLE };   /* (a layout placed by code from the world; a pause page: by hand;
                                                                its own canvas; on a screen kept whole) */
enum { RULE_NONE, RULE_KEEP, RULE_KEEP_LOOK, RULE_KEEP_WIDE, RULE_WIDEN, RULE_GROW, RULE_LEFT, RULE_RIGHT, RULE_SLIDE,
       RULE_KEEP_ORIGIN, RULE_KEEP_TAB, RULE_STRIP, RULE_PAGES, RULE_SPLIT };   /* (a pane the game slides in from beside its parent, planned where it comes
                                                  to rest; a strip of pages; a pane in one whose children are its pages'
                                                  too; the pages' background, drawn in pieces) */

static const char *const WORLD_LAYOUTS[] = {"PlayerStatus3D_00", "Pointer_00", "BalloonMessage_00"};
static const char *const PAGE_LAYOUTS[] = {"PaCollectMenu_00", "PaItemMenu_00", "PaEquipmentMenu_00"};
static const char *const CANVAS_LAYOUTS[] = {"PaSystemMenu_00"};
/* (layouts of screens nx2/oot_wide.py keeps whole (KEEP_VTABLES): their canvas already spans the screen, so nothing in
   them moves or widens. The item wheel's background picture widened to 5160 units mapped its capture over 3840 / 5160
   of the screen, black at the sides) */
static const char *const WHOLE_LAYOUTS[] = {"ShortCutBG_00", "EventFade_00"};
typedef struct { const char *layout, *pane; uint16_t rule; } PaneRule;
static const PaneRule RULES[] = {
    /* (the HUD: the left gauges are moved by name, their group stays where the pause menu places part of it) */
    {"PlayerStatus_00", "N_ScaleL_00", RULE_KEEP_LOOK}, {"PlayerStatus_00", "N_ScaleL_01", RULE_KEEP_LOOK},
    {"PlayerStatus_00", "Pa_HeartGauge_00", RULE_LEFT}, {"PlayerStatus_00", "Pa_MpGauge_00", RULE_LEFT},
    {"PlayerStatus_00", "Pa_AgonyStone_00", RULE_LEFT},
    /* (the pause menu: the page strips and the item preview's panels by hand. The game view's fades while a page changes,
       a capture each and the picture drawing it, kept at their size: a capture's wider transform does not widen what it
       captures, so its picture grown drew the same view 1.8 times as wide) */
    {"PlusMenu_00", "N_MenuChange_00", RULE_STRIP}, {"PlusMenu_00", "N_MenuChange_01", RULE_STRIP},
    {"PlusMenu_00", "N_DreamInfo_00", RULE_PAGES}, {"PlusMenu_00", "P_MenuBg_00", RULE_SPLIT},
    {"PlusMenu_00", "N_3DFade_00", RULE_KEEP}, {"PlusMenu_00", "C_3DFade_00", RULE_KEEP},
    {"PlusMenu_00", "C_3DFade_01", RULE_KEEP}, {"PlusMenu_00", "P_3DFade_01", RULE_KEEP},
    {"PlusMenu_00", "P_3DFade_02", RULE_KEEP}, {"PlusMenu_00", "P_InfoBg_00", RULE_GROW},
    {"PlusMenu_00", "P_InfoBg_01", RULE_GROW}, {"PlusMenu_00", "P_InfoBg_02", RULE_GROW},
    {"PlusMenu_00", "P_InfoBg_03", RULE_RIGHT}, {"PlusMenu_00", "P_MenuBgAmiibo_00", RULE_LEFT},
    {"PlusMenu_00", "P_MenuBgArrow_00", RULE_LEFT}, {"PlusMenu_00", "P_SystemBg_00", RULE_LEFT},
    {"PaCollectMenu_00", "N_ItemAll_00", RULE_RIGHT}, {"PaCollectMenu_00", "N_Arrow_00", RULE_RIGHT},
    {"PaItemMenu_00", "N_ItemAll_00", RULE_LEFT}, {"PaItemMenu_00", "N_Arrow_00", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_HeadEquip_00", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_HeadEquip_01", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_HeadEquip_02", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_HeadEquip_03", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_FootEquip_00", RULE_LEFT}, {"PaEquipmentMenu_00", "Pa_FootEquip_01", RULE_LEFT},
    {"PaEquipmentMenu_00", "P_AmiiboIcon_00", RULE_LEFT}, {"PaEquipmentMenu_00", "N_Arrow_00", RULE_LEFT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_00", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_01", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_02", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_03", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_04", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_05", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_06", RULE_RIGHT}, {"PaEquipmentMenu_00", "Pa_Epuipment_07", RULE_RIGHT},
    {"PaEquipmentMenu_00", "Pa_Epuipment_08", RULE_RIGHT}, {"PaEquipmentMenu_00", "N_Arrow_01", RULE_RIGHT},
    /* (the System page's controls sheet, right of the centre: labels on both sides of the controller, its backdrop to the
       canvas's right edge. The mod never looks into a part off the origin: kept whole) */
    {"PaSystemMenu_00", "Pa_SystemController_00", RULE_KEEP},
    /* (the item quick select's carousel: its items scroll through the whole canvas, kept together) */
    {"PaShortCutSlideList_00", "N_SlideList_00", RULE_KEEP_WIDE},   /* (its own width: where its items are cut off) */
    /* (an illustrated board, kept whole; a backdrop and the area name's frame corners to the screen's width) */
    {"ImportantMenu_01", "W_Window_00", RULE_KEEP}, {"ChallengeMenu_01", "P_TapestryMask_00", RULE_WIDEN},
    /* (the Threads of Time and Map pages slide 20 to 80 units as they change: planned at rest, else their vignettes span
       the canvas, and are widened, only at rest) */
    {"ChallengeMenu_01", "N_In_00", RULE_SLIDE}, {"MapMenu_00", "N_MenuIn_00", RULE_SLIDE},
    {"MapMenu_00", "N_InChallenge_00", RULE_SLIDE},
    /* (the map's icons, placed by code over the map, and the world map, wider than the screen with its location panels on
       it: kept whole with the map under them) */
    /* (MapMenu_00 is also the HUD's minimap: there the icons' group sits over the minimap and moves out with it; kept only at
       the canvas's origin, where the map page puts it. While it does, the area title and the world map card stay on the map,
       which stays 16:9. The icons' part itself is never looked into: its icons move as one with their group) */
    {"MapMenu_00", "N_MapIcon_00", RULE_KEEP_ORIGIN}, {"MapMenu_00", "Pa_MapIcon_00", RULE_KEEP},
    {"MapMenu_00", "Pa_WorldMap_00", RULE_KEEP},
    {"MapMenu_00", "T_MapTitle_00", RULE_KEEP_TAB}, {"MapMenu_00", "P_PicField_00", RULE_KEEP_TAB},
    {"MapMenu_00", "N_Field_01", RULE_KEEP_TAB}, {"MapMenu_00", "Pa_Guide_01", RULE_KEEP_TAB},
    {"Location_00", "P_DecoFrameLT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameLB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLBSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRBSh_00", RULE_WIDEN},
};

/* A guest string is s (64 characters at most) */
static int guest_is(uint64_t g, const char *s) {
    if (!g) return 0;
    for (int i = 0; i < 64; i++) {
        const char ch = (char)recomp_load8(NULL, g + (uint64_t)i);
        if (ch != s[i]) return 0;
        if (!ch) return 1;
    }
    return 0;
}

static int guest_in(uint64_t g, const char *const *list, int n) {
    for (int i = 0; i < n; i++)
        if (guest_is(g, list[i])) return 1;
    return 0;
}

/* A pane's layout kind and rule, from its layout's name and its own (the layout file's UIL header) */
static void classify(Node *n, uint64_t pane, uint64_t layout) {
    const uint64_t res = layout ? recomp_load64(NULL, layout + LAYOUT_RES) : 0;
    const uint64_t lname = res ? recomp_load64(NULL, res + RES_NAME) : 0;
    const uint64_t names = res ? recomp_load64(NULL, res + RES_PANE_NAMES) : 0;
    const uint64_t base = layout ? recomp_load64(NULL, layout + LAYOUT_PANES) : 0;
    const uint64_t pname = names && pane >= base ? recomp_load64(NULL, names + (pane - base) / PANE_SIZE * 8) : 0;
#define COUNT(a) (int)(sizeof a / sizeof a[0])
    n->lkind = guest_in(lname, WORLD_LAYOUTS, COUNT(WORLD_LAYOUTS)) ? LK_WORLD
             : guest_in(lname, PAGE_LAYOUTS, COUNT(PAGE_LAYOUTS)) ? LK_PAGE
             : guest_in(lname, CANVAS_LAYOUTS, COUNT(CANVAS_LAYOUTS)) ? LK_CANVAS
             : guest_in(lname, WHOLE_LAYOUTS, COUNT(WHOLE_LAYOUTS)) ? LK_WHOLE : LK_PLAIN;
    n->lname = lname, n->pname = pname, n->rule = RULE_NONE;
    for (int i = 0; i < COUNT(RULES) && pname; i++)
        if (guest_is(lname, RULES[i].layout) && guest_is(pname, RULES[i].pane)) n->rule = RULES[i].rule;
#undef COUNT
}

/* HOS_WIDE_UI_LOG=<n>: the first n changes of the plan after each change of the choice, to tell what a pane that moves
   wrongly is */
static int log_each, log_left;

static void guest_copy(char *out, int cap, uint64_t g) {
    int i = 0;
    for (; g && i < cap - 1; i++)
        if (!(out[i] = (char)recomp_load8(NULL, g + (uint64_t)i))) break;
    out[i] = 0;
}

static void decided(const Node *n, float plan, float space, float widen) {
    if (log_left <= 0) return;
    log_left--;
    char ln[40], pn[40];
    guest_copy(ln, sizeof ln, n->lname), guest_copy(pn, sizeof pn, n->pname);
    hos_log("wide ui: %s:%s (pane %llx parent %llx) type %u x %.1f [%.1f %.1f] rule %u: %s, page move %.1f", ln, pn,
            (unsigned long long)n->key, (unsigned long long)n->parent, n->type, (double)n->ox, (double)n->x0, (double)n->x1, n->rule,
            widen > 1.0f ? "wider" : plan < 0.0f ? "left" : plan > 0.0f ? "right" : "stays", (double)space);
}

/* ---- the plan: the presenting thread, once a present ---- */

static int32_t first_child[NODES], next_sibling[NODES], parent_of[NODES];
static float ext_lo[NODES], ext_hi[NODES], plan_new[NODES], space_new[NODES], wide_new[NODES];
static float rest[NODES];   /* (how far a pane is from where the plan sees it: its slide, or its page's along a strip) */
static uint8_t ext_done[NODES], rest_done[NODES];

static int drawn(const Node *n) { return n->type != PANE_TYPE_NULL && n->type != PANE_TYPE_PART; }

/* A pane noted is still one of its layout's: a layout let go (a menu closed) leaves its panes noted, and their addresses
   are used again by others */
static int alive(const Node *n) {
    if (!n->layout || recomp_load64(NULL, n->key + PANE_LAYOUT) != n->layout) return 0;
    const uint64_t base = recomp_load64(NULL, n->layout + LAYOUT_PANES);
    const uint32_t count = (uint32_t)recomp_load32(NULL, n->layout + LAYOUT_COUNT) & 0xffff;
    return n->key >= base && (n->key - base) % PANE_SIZE == 0 && (n->key - base) / PANE_SIZE < count;
}

/* A page's own canvas: its root, the pane whose parent is in another layout */
static int canvas_root(int i) {
    const Node *n = &nodes[i];
    return n->lkind == LK_CANVAS && parent_of[i] >= 0 && nodes[parent_of[i]].layout != n->layout;
}

/* How far a pane is from its place at rest: a page's canvas root at the canvas's origin (the strip sliding it in, or a
   page away), a sliding pane where its parent is, what is in them with them. The plan is the same throughout a slide.
   Each pane counts from its parent as they were when it was placed: the game's threads place a frame's panes while the
   plan reads them, and a page root from one frame with its panes from the last put them a frame's slide apart */
static float rest_of(int i, int depth) {
    if (rest_done[i] || depth > 64) return rest[i];
    const Node *n = &nodes[i];
    const int p = parent_of[i];
    float at = n->ox;   /* (its place at rest) */
    if (canvas_root(i)) at = 0.0f;
    else if (p >= 0) {
        const float up = nodes[p].ox - rest_of(p, depth + 1);
        at = n->rule == RULE_SLIDE ? up : up + n->ox - (isnan(n->from) ? nodes[p].ox : n->from);
    }
    rest[i] = n->ox - at;
    rest_done[i] = 1;
    return rest[i];
}

/* How far a pane and what is in it reach across: its origin, its own rectangle when it is drawn and not wider than
   SIZE_CAP, and its children's (as rules.py Extents) */
static void extents(int i, int depth) {
    if (ext_done[i]) return;
    const Node *n = &nodes[i];
    const float r = rest[i];
    float lo = n->ox - r, hi = n->ox - r;
    if (drawn(n) && n->x1 - n->x0 < SIZE_CAP) lo = fminf(lo, n->x0 - r), hi = fmaxf(hi, n->x1 - r);
    if (depth < 48)
        for (int c = first_child[i]; c >= 0; c = next_sibling[c]) {
            extents(c, depth + 1);
            lo = fminf(lo, ext_lo[c]), hi = fmaxf(hi, ext_hi[c]);
        }
    ext_lo[i] = lo, ext_hi[i] = hi, ext_done[i] = 1;
}

static void visit(int p, int depth);

/* The layouts showing their map page: one of their RULE_KEEP_ORIGIN panes at the canvas's origin */
enum { TAB_LAYOUTS = 16 };
static uint64_t tab_layouts[TAB_LAYOUTS];
static int tab_count;

static int in_tab(uint64_t layout) {
    for (int i = 0; i < tab_count; i++)
        if (tab_layouts[i] == layout) return 1;
    return 0;
}

static void push(int i, int side) { plan_new[i] = (float)side * ui_extra; }

/* A pane spanning the canvas: drawn the screen's width; what is in it is placed as usual */
static void widen(int i, int depth) {
    const float mid = (nodes[i].x0 + nodes[i].x1) * 0.5f - rest[i];
    if (drawn(&nodes[i]) && mid * mid < 1.0f) wide_new[i] = ui_widen;
    visit(i, depth + 1);
}

/* rules.py plan_layout's visit: each child of p by how far it and its contents reach */
static void visit(int p, int depth) {
    if (depth > 48) return;
    const float E = CANVAS_HALF;
    int group[2][64], count[2] = {0, 0};
    float glo[2] = {INFINITY, INFINITY}, ghi[2] = {-INFINITY, -INFINITY};
    for (int c = first_child[p]; c >= 0; c = next_sibling[c]) {
        const Node *n = &nodes[c];
        const float ox = n->ox - rest[c], x0 = n->x0 - rest[c], x1 = n->x1 - rest[c];
        if (n->rule == RULE_KEEP_WIDE) wide_new[c] = ui_widen;
        if ((n->rule == RULE_KEEP_ORIGIN && fabsf(ox) < 1.0f) || (n->rule == RULE_KEEP_TAB && in_tab(n->layout)))
            continue;   /* (the map page's: kept over the map) */
        if (n->lkind == LK_WORLD || n->lkind == LK_PAGE || n->lkind == LK_WHOLE || n->rule == RULE_KEEP || n->rule == RULE_KEEP_WIDE || n->rule == RULE_GROW ||
            n->rule == RULE_LEFT || n->rule == RULE_RIGHT || n->rule >= RULE_STRIP)
            continue;   /* (placed by code, by hand, or by name: below) */
        if (n->rule == RULE_KEEP_LOOK) {
            visit(c, depth + 1);
            continue;
        }
        if (n->rule == RULE_WIDEN) {
            widen(c, depth);
            continue;
        }
        if (n->type == PANE_TYPE_PART && fabsf(ox) < 1.0f) {   /* (a part at the canvas's origin: its own layout's panes) */
            visit(c, depth + 1);
            continue;
        }
        extents(c, 0);
        float lo = ext_lo[c], hi = ext_hi[c];
        if (drawn(n)) {
            if (x0 <= -E + EDGE_TOL && x1 >= E - EDGE_TOL) {
                widen(c, depth);
                continue;
            }
            if (first_child[c] < 0) lo = fminf(lo, x0), hi = fmaxf(hi, x1);
        }
        if (lo >= E || hi <= -E || (hi - lo > 2.5f * E && fabsf(ox) > 1.0f)) continue;   /* (off the canvas, a strip) */
        lo = fmaxf(lo, -E - EDGE_NEAR), hi = fminf(hi, E + EDGE_NEAR);
        if (lo <= -E + EDGE_TOL && hi >= E - EDGE_TOL) {
            visit(c, depth + 1);
            continue;
        }
        if (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > CROSS * (hi - lo)) {
            const int near_hi = hi >= E - EDGE_NEAR, near_lo = lo <= -E + EDGE_NEAR;
            if (near_hi != near_lo && (first_child[c] < 0 || hi - lo < E)) push(c, near_hi ? 1 : -1);
            else visit(c, depth + 1);
            continue;
        }
        if (lo == 0.0f || hi == 0.0f ||
            (lo < 0.0f && hi > 0.0f && (fabsf(lo + hi) * 0.5f < EDGE_NEAR || (first_child[c] >= 0 && fabsf(ox) < 1.0f)))) {
            visit(c, depth + 1);   /* (a centred container) */
            continue;
        }
        const int s = lo + hi >= 0.0f;
        if (count[s] < 64) group[s][count[s]++] = c;
        glo[s] = fminf(glo[s], lo), ghi[s] = fmaxf(ghi[s], hi);
    }
    for (int s = 0; s < 2; s++) {
        if (!count[s]) continue;
        const float outer = s ? ghi[s] : -glo[s];
        const int out = outer >= E - EDGE_NEAR || fabsf(glo[s] + ghi[s]) * 0.5f >= SIDE;
        for (int k = 0; k < count[s]; k++) {
            if (out) push(group[s][k], s ? 1 : -1);
            else visit(group[s][k], depth + 1);
        }
    }
}

static void plan(void) {
    const int on = ui_on;
    for (int i = 0; i < NODES; i++) first_child[i] = next_sibling[i] = parent_of[i] = -1;
    memset(ext_done, 0, sizeof ext_done);
    memset(rest_done, 0, sizeof rest_done);
    memset(rest, 0, sizeof rest);
    memset(plan_new, 0, sizeof plan_new);
    memset(space_new, 0, sizeof space_new);
    for (int i = 0; i < NODES; i++) wide_new[i] = 1.0f;
    if (on) {
        for (int i = 0; i < NODES; i++) {
            if (!__atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE)) continue;
            if (!alive(&nodes[i])) {
                nodes[i].layout = 0;   /* (gone: not part of any tree; its slot free for another pane) */
                __atomic_store_n(&nodes[i].dead, 1, __ATOMIC_RELEASE);
                continue;
            }
            const Node *pn = nodes[i].parent ? node_find(nodes[i].parent) : NULL;
            const int p = pn && pn->layout ? (int)(pn - nodes) : -1;
            if (p < 0 || p == i) continue;
            parent_of[i] = p, next_sibling[i] = first_child[p], first_child[p] = i;
        }
        for (int i = 0; i < NODES; i++)
            if (nodes[i].key && nodes[i].layout) rest_of(i, 0);
        tab_count = 0;
        for (int i = 0; i < NODES; i++)
            if (nodes[i].key && nodes[i].layout && nodes[i].rule == RULE_KEEP_ORIGIN && fabsf(nodes[i].ox - rest[i]) < 1.0f &&
                !in_tab(nodes[i].layout) && tab_count < TAB_LAYOUTS)
                tab_layouts[tab_count++] = nodes[i].layout;
        /* (the roots: a layout's root pane, or a pane whose parent was never seen; each walked as the canvas's root) */
        for (int i = 0; i < NODES; i++)
            if (nodes[i].key && nodes[i].layout && parent_of[i] < 0 && nodes[i].lkind != LK_WORLD &&
                nodes[i].lkind != LK_WHOLE)
                visit(i, 0);
        /* (a page's own canvas, from its root) */
        for (int i = 0; i < NODES; i++)
            if (nodes[i].key && nodes[i].layout && canvas_root(i)) visit(i, 0);
        /* (by name: moved out, unless an ancestor already is; preview panels grown by the screen's extra width) */
        for (int i = 0; i < NODES; i++) {
            const Node *n = &nodes[i];
            if (!n->key || !n->layout) continue;
            if (n->rule == RULE_GROW && n->x1 > n->x0) wide_new[i] = (n->x1 - n->x0 + 2.0f * ui_extra) / (n->x1 - n->x0);
            if (n->rule != RULE_LEFT && n->rule != RULE_RIGHT) continue;
            int moved = 0;
            for (int a = parent_of[i], d = 0; a >= 0 && d < 64 && !moved; a = parent_of[a], d++) moved = plan_new[a] != 0.0f;
            if (!moved) push(i, n->rule == RULE_LEFT ? -1 : 1);
        }
        /* (the pause menu's pages spaced wider: a pane along a strip, or in a pane of one whose children are pages' too, by
           its page, the nearest to its place along the strip, as menu.py plan_plus_menu) */
        for (int i = 0; i < NODES; i++) {
            const int p = parent_of[i];
            if (!nodes[i].key || !nodes[i].layout || p < 0) continue;
            const int s = nodes[p].rule == RULE_STRIP ? p
                        : nodes[p].rule == RULE_PAGES && parent_of[p] >= 0 && nodes[parent_of[p]].rule == RULE_STRIP ? parent_of[p] : -1;
            if (s >= 0) space_new[i] = roundf((nodes[i].ox - nodes[s].ox) / PAGE_STRIDE) * 2.0f * ui_extra;
        }
    }
    int changed = 0;
    for (int i = 0; i < NODES; i++) {
        Node *n = &nodes[i];
        if (!n->key || (n->plan == plan_new[i] && n->space == space_new[i] && n->wide == wide_new[i])) continue;
        decided(n, plan_new[i], space_new[i], wide_new[i]);
        n->plan = plan_new[i], n->space = space_new[i], n->wide = wide_new[i];
        changed = 1;
    }
    if (changed && hos_wide_ui_force < 2) hos_wide_ui_force = 2;
}

/* Each present (host/rx_backend.c host_wide_menus): whether Extended is on, how far a side moves, and the plan */
void hos_wide_ui_tick(void) {
    static const char *env;
    static int looked, last = -1;
    static float last_extra;
    if (!looked) {
        looked = 1, env = getenv("HOS_WIDE_UI");
        const char *e = getenv("HOS_WIDE_UI_LOG");
        log_each = e ? atoi(e) : 0;
    }
    const float q = hos_wide_squeeze;
    const int want = env && *env ? atoi(env) != 0 : rx_option(HOS_OPT_WIDE_MENUS, 0) == WIDE_MENUS_EXTENDED;
    const int on = want && q > 0.0f && q < 0.999f;
    const float extra = on ? CANVAS_HALF * (1.0f / q - 1.0f) : 0.0f;
    if (on != last || extra != last_extra) {
        ui_extra = extra, ui_widen = on ? 1.0f / q : 1.0f, ui_on = on;
        for (int i = 0; i < NODES; i++) {   /* (the lists' own widths back to the game's) */
            const Node *n = &nodes[i];
            if (!n->key || !n->size_set || !alive(n)) continue;
            uint32_t u;
            memcpy(&u, &n->size_set, 4);
            if ((uint32_t)recomp_load32(NULL, n->key + PANE_W) != u) continue;
            memcpy(&u, &n->size_game, 4);
            recomp_store32(NULL, n->key + PANE_W, u);
            recomp_store32(NULL, n->key, (uint32_t)recomp_load32(NULL, n->key) | PANE_DIRTY_RECT);
        }
        nodes_clear();   /* (every pane placed again, noted afresh) */
        hos_wide_ui_force = 3;   /* (a few frames: the layouts not drawn this frame catch up when they are) */
        log_left = log_each;
        if (last >= 0) hos_log("wide ui: side panes %s (%.0f canvas units)", on ? "at the screen's edges" : "in the middle 16:9", (double)extra);
        last = on, last_extra = extra;
    } else if (hos_wide_ui_force) {
        hos_wide_ui_force--;
    }
    if (on) plan();
}

/* ---- the game's threads ---- */

static _Thread_local uint64_t ui_parent;

void hos_wide_ui_parent(uint64_t parent) { ui_parent = parent; }

static float load_f(Ctx *c, uint64_t a) {
    const uint32_t u = (uint32_t)recomp_load32(c, a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void store_f(Ctx *c, uint64_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    recomp_store32(c, a, u);
}

void hos_wide_ui_placed(Ctx *c, uint64_t pane) {
    const uint64_t parent = ui_parent;
    ui_parent = 0;
    if (!ui_on) return;   /* (off: the transforms are the game's) */
    Node *n = node_take(pane);
    if (!n) return;
    const Node *pn = parent ? node_find(parent) : NULL;
    const float inherited = pn ? pn->moved : 0.0f, pushed = pn ? pn->pushed : 0.0f;
    const uint32_t flags = (uint32_t)recomp_load32(c, pane + PANE_FLAGS);
    const float x = load_f(c, pane + PANE_X) - inherited;
    const float w = load_f(c, pane + PANE_W) * fabsf(load_f(c, pane + PANE_XX));
    const uint32_t ox = ((flags >> 11) & 0x7f) % 3;   /* (0 left, 1 centre, 2 right: f_8154a820) */
    const uint64_t layout = recomp_load64(c, pane + PANE_LAYOUT);
    if (n->layout != layout) classify(n, pane, layout);
    n->parent = parent, n->layout = layout, n->type = (flags >> 22) & 0xf, n->ox = x, n->from = pn ? pn->ox : NAN;
    n->x0 = x - (ox == 1 ? w * 0.5f : ox == 2 ? w : 0.0f), n->x1 = n->x0 + w;
    const float side = pushed != 0.0f ? 0.0f : n->plan;   /* (made from a parent moved out: already moved with it) */
    float own = side + n->space;
    if (n->rule == RULE_STRIP && parent)   /* (a strip: the page it shows, or between, at the wider spacing) */
        own += (x + inherited - load_f(c, parent + PANE_X)) * (2.0f * ui_extra / PAGE_STRIDE);
    n->moved = inherited + own, n->pushed = pushed + side;
    if (own != 0.0f) store_f(c, pane + PANE_X, x + inherited + own);
    if (n->wide > 1.0f && (n->type == PANE_TYPE_HOST || n->type == PANE_TYPE_SCISSOR)) {   /* (a list or a scissor: its own width,
                                                                                         which what is in it is cut off at) */
        const float now = load_f(c, pane + PANE_W);
        if (now != n->size_set) n->size_game = now;
        n->size_set = n->size_game * n->wide;
        if (now != n->size_set) {
            store_f(c, pane + PANE_W, n->size_set);
            recomp_store32(c, pane, (uint32_t)recomp_load32(c, pane) | PANE_DIRTY_RECT);
        }
    }
    if (n->wide > 1.0f && n->type == PANE_TYPE_CAPTURE) {   /* (a capture is not drawn by f_8154ac30: its across axis, wider) */
        for (int k = 0; k < 3; k++) store_f(c, pane + PANE_XX + 4 * (uint64_t)k, load_f(c, pane + PANE_XX + 4 * (uint64_t)k) * n->wide);
    }
}

/* The drawn size of a pane, its width in the low lane of v0 (f_8154ac30) */
void hos_wide_ui_size(Ctx *c, uint64_t pane) {
    if (!ui_on) return;
    const Node *n = node_find(pane);
    if (!n || n->wide <= 1.0f || n->type == PANE_TYPE_HOST || n->type == PANE_TYPE_SCISSOR || n->type == PANE_TYPE_CAPTURE)
        return;   /* (those widened above) */
    float w;
    memcpy(&w, c->vreg[0], 4);
    w *= n->wide;
    memcpy(c->vreg[0], &w, 4);
}

/* A layout's panes just built (f_81545d60 returned, the layout x19): a list kept at the screen's width is widened now. It
   lays out its items by its width as it opens, before it is first placed: widened only then, its items stayed where the
   16:9 width put them, one item off the selection, until the selection moved */
void hos_wide_ui_built(Ctx *c, uint64_t layout) {
    if (!ui_on) return;
    const uint64_t base = recomp_load64(c, layout + LAYOUT_PANES);
    const uint32_t count = (uint32_t)recomp_load32(c, layout + LAYOUT_COUNT) & 0xffff;
    for (uint32_t i = 0; base && i < count; i++) {
        const uint64_t pane = base + (uint64_t)i * PANE_SIZE;
        if ((((uint32_t)recomp_load32(c, pane + PANE_FLAGS) >> 22) & 0xf) != PANE_TYPE_HOST) continue;
        Node probe = {0};
        classify(&probe, pane, layout);
        if (probe.rule != RULE_KEEP_WIDE) continue;
        Node *n = node_take(pane);
        if (!n) return;
        classify(n, pane, layout);
        n->layout = layout, n->type = PANE_TYPE_HOST, n->wide = ui_widen;
        n->size_game = load_f(c, pane + PANE_W), n->size_set = n->size_game * n->wide;
        store_f(c, pane + PANE_W, n->size_set);
    }
}

/* ---- the pause menu's background in pieces ----

   A picture's draw is made in two steps. f_815505b0 fills a record on its stack (sp + 0x54: its 3x4 matrix by rows at +0x40,
   the translation the last of each row; its size at +0xb0; each texture's corner coordinates, top left, top right, bottom
   left, bottom right, at REC_UV, their count in the low bits of +0x630) and packs it into the frame's constants
   (f_8154f1c0(record, *(info + 0x18), *(info + 0x20)), returning {address, ...} through x8, at sp + 8), which the drawer
   keeps at +0x48. f_81550720 draws the drawer with them later, its quad from -0.5 to 0.5 of the size across.

   For P_MenuBg_00 the record is packed three times, each with a piece of the picture: its left half (texture and quad) the
   extra further left, the column at its centre over the 2 x extra between, its right half the extra further right. The
   drawer keeps the left half's; the other two are drawn just before it, with the drawer's +0x48 set to theirs for each */
enum { REC_AT = 0x54, REC_OUT = 8, REC_MTX = 0x40, REC_SIZE = 0xb0, REC_TEXTURES = 0x630, SPLITS = 8 };
static const uint16_t REC_UV[6] = {0xc0, 0x110, 0x150, 0x1a0, 0x1e0, 0x230};   /* (f_8154f510's table at 0x83e80630) */
static const uint64_t SPLIT_PACK = 0x8154f1c0ull, SPLIT_DRAW = 0x81550720ull;
static const float QUAD_LEFT = -0.5f;   /* (the quad's left side, in widths from the translation) */
static const float SPLIT_AT = 0.5f;     /* (the cut, in widths from the picture's left: its centre, the Equipment page's) */

/* A drawer drawn in pieces: the packed constants of the two drawn before its own (seq odd while they are written) */
typedef struct { uint64_t drawer; uint32_t seq; uint64_t out[2][3]; } Split;
static Split splits[SPLITS];

static Split *split_at(uint64_t drawer, int take) {
    for (int i = 0; i < SPLITS; i++)
        if (__atomic_load_n(&splits[i].drawer, __ATOMIC_ACQUIRE) == drawer) return &splits[i];
    for (int i = 0; i < SPLITS && take; i++) {
        uint64_t z = 0;
        if (__atomic_compare_exchange_n(&splits[i].drawer, &z, drawer, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return &splits[i];
    }
    return NULL;
}

/* The record as the game filled it: what a piece changes */
typedef struct { float w, m0[3], t[3], uv[6][8]; int count; } Whole;

/* The piece from t0 to t1 of the picture's width, drawn wide (local units) and moved d across the canvas */
static void split_piece(Ctx *c, uint64_t rec, const Whole *o, float t0, float t1, float wide, float d) {
    const float at = QUAD_LEFT * (o->w - wide) + t0 * o->w;   /* (its left side where t0 was) */
    for (int r = 0; r < 3; r++) store_f(c, rec + REC_MTX + 16 * (uint64_t)r + 12, o->t[r] + o->m0[r] * at + (r ? 0.0f : d));
    store_f(c, rec + REC_SIZE, wide);
    for (int i = 0; i < o->count; i++) {
        const float *q = o->uv[i];   /* (top left, top right, bottom left, bottom right; u, v each) */
        for (int k = 0; k < 2; k++) {
            const float *a = q + 4 * k, *b = a + 2;
            for (int j = 0; j < 2; j++) {
                store_f(c, rec + REC_UV[i] + 16 * (uint64_t)k + 4 * (uint64_t)j, a[j] + (b[j] - a[j]) * t0);
                store_f(c, rec + REC_UV[i] + 16 * (uint64_t)k + 8 + 4 * (uint64_t)j, a[j] + (b[j] - a[j]) * t1);
            }
        }
    }
}

/* f_8154f1c0 on the record into out, on the game's thread */
static int split_pack(Ctx *c, uint64_t rec, uint64_t info, uint64_t out) {
    const A64Fn fn = a64_lookup(SPLIT_PACK);
    if (!fn) return 0;
    const Ctx saved = *c;
    c->x[0] = rec, c->x[1] = recomp_load64(c, info + 0x18), c->x[2] = recomp_load64(c, info + 0x20), c->x[8] = out, c->x[30] = 0;
    fn(c);
    *c = saved;
    return 1;
}

static void split_drop(Split *s) {
    if (!s) return;
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s->drawer, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
}

/* After f_815505b0 packed a picture's record (x19 the drawer, x20 the pane, x23 what it draws with), before the drawer keeps
   what it returned (in x8, x9 and at sp + 0x18) */
void hos_wide_ui_bg(Ctx *c) {
    const uint64_t drawer = c->x[19], rec = c->x[31] + REC_AT, out = c->x[31] + REC_OUT, info = c->x[23];
    const Node *n = ui_on ? node_find(c->x[20]) : NULL;
    if (!n || n->rule != RULE_SPLIT || n->space <= 0.0f || ui_extra <= 0.0f) {   /* (spaced as a page: the pages are apart) */
        split_drop(split_at(drawer, 0));
        return;
    }
    Whole o;
    o.w = load_f(c, rec + REC_SIZE);
    for (int r = 0; r < 3; r++)
        o.m0[r] = load_f(c, rec + REC_MTX + 16 * (uint64_t)r), o.t[r] = load_f(c, rec + REC_MTX + 16 * (uint64_t)r + 12);
    o.count = (int)((uint32_t)recomp_load32(c, rec + REC_TEXTURES) & 7);
    if (o.count > 6) o.count = 6;
    for (int i = 0; i < o.count; i++)
        for (int k = 0; k < 8; k++) o.uv[i][k] = load_f(c, rec + REC_UV[i] + 4 * (uint64_t)k);
    Split *s = split_at(drawer, 1);
    if (!s || !(o.w > 1.0f) || !(o.m0[0] > 1e-3f)) {
        split_drop(s);
        return;
    }
    const float e = ui_extra, half = o.w * SPLIT_AT, gap = 2.0f * e / o.m0[0];
    uint64_t made[2][3];
    split_piece(c, rec, &o, SPLIT_AT, 1.0f, o.w - half, e);   /* (the right half) */
    if (!split_pack(c, rec, info, out)) {
        split_drop(s);
        return;
    }
    for (int k = 0; k < 3; k++) made[0][k] = recomp_load64(c, out + 8 * (uint64_t)k);
    split_piece(c, rec, &o, SPLIT_AT, SPLIT_AT, gap, -e);    /* (the centre column, between) */
    split_pack(c, rec, info, out);
    for (int k = 0; k < 3; k++) made[1][k] = recomp_load64(c, out + 8 * (uint64_t)k);
    split_piece(c, rec, &o, 0.0f, SPLIT_AT, half, -e);       /* (the left half, the drawer's own) */
    split_pack(c, rec, info, out);
    c->x[8] = recomp_load64(c, out), c->x[9] = recomp_load64(c, out + 8);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) __atomic_store_n(&s->out[p][k], made[p][k], __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->seq, 1, __ATOMIC_ACQ_REL);
    static int told;
    if (!told) told = 1, hos_log("wide ui: the pause menu's background drawn in three pieces, %.0f units apart", (double)(2.0f * e));
}

/* At the start of f_81550720 (x0 the drawer, x1 what it draws with): the pieces before the drawer's own */
void hos_wide_ui_bg_draw(Ctx *c) {
    static _Thread_local int inside;
    const uint64_t drawer = c->x[0];
    Split *s = inside || !ui_on ? NULL : split_at(drawer, 0);
    if (!s) return;
    uint64_t made[2][3];
    const uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
    for (int p = 0; p < 2; p++)
        for (int k = 0; k < 3; k++) made[p][k] = __atomic_load_n(&s->out[p][k], __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if ((seq & 1) || __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE) != seq || __atomic_load_n(&s->drawer, __ATOMIC_ACQUIRE) != drawer)
        return;
    const A64Fn fn = a64_lookup(SPLIT_DRAW);
    if (!fn) return;
    uint64_t own[3];
    for (int k = 0; k < 3; k++) own[k] = recomp_load64(c, drawer + 0x48 + 8 * (uint64_t)k);
    inside = 1;
    for (int p = 0; p < 2; p++) {
        for (int k = 0; k < 3; k++) recomp_store64(c, drawer + 0x48 + 8 * (uint64_t)k, made[p][k]);
        const Ctx saved = *c;
        c->x[0] = drawer, c->x[1] = saved.x[1], c->x[30] = 0;
        fn(c);
        *c = saved;
    }
    inside = 0;
    for (int k = 0; k < 3; k++) recomp_store64(c, drawer + 0x48 + 8 * (uint64_t)k, own[k]);
}
