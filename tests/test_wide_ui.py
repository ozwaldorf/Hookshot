"""Ultrawide menus' Extended moves a 2D layout's side panes out to the screen's edges, and only while it is chosen.

nx2/oot_wide.py calls host/hos_wide_ui.c from f_8154cab0, where the game makes a pane's final transform: the parent it is
made from, then the transform stored (the translation at +0x4c). A pane at one side of the 3840-wide canvas whose ancestors
were not moved has its x moved out by the screen's extra half width; its children keep the move they were made with; a
pane spanning the canvas is drawn the screen's width instead. Changing the choice marks every pane to remake its transform.

python -I -B tests/test_wide_ui.py [--cc gcc]
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_wide  # noqa: E402

FIXTURE_HEAD = r'''#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t x[32]; uint64_t vreg[32][2]; } Ctx;
enum { HOS_OPT_WIDE_MENUS = 53 };
#define BASE 0x10000000ull
static unsigned char mem[0x40000];
static int menu = 2;
volatile float hos_wide_squeeze;
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; if (a < BASE || a >= BASE + sizeof mem) return 0; return mem[a - BASE]; }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; if (a < BASE || a + 4 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 4); return v; }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; if (a < BASE || a + 8 > BASE + sizeof mem) return 0; memcpy(&v, mem + (a - BASE), 8); return v; }
static void recomp_store32(Ctx *c, uint64_t a, uint32_t v) { (void)c; memcpy(mem + (a - BASE), &v, 4); }
static void recomp_store64(Ctx *c, uint64_t a, uint64_t v) { (void)c; memcpy(mem + (a - BASE), &v, 8); }
typedef void (*A64Fn)(Ctx *);
A64Fn a64_lookup(uint64_t addr);
int rx_option(int id, int otherwise) { return id == HOS_OPT_WIDE_MENUS ? menu : otherwise; }
static void hos_log(const char *f, ...) { (void)f; }
'''

FIXTURE_MAIN = r'''
static void putf(uint64_t a, float v) { memcpy(mem + (a - BASE), &v, 4); }
static void put32(uint64_t a, uint32_t v) { memcpy(mem + (a - BASE), &v, 4); }
static void put64(uint64_t a, uint64_t v) { memcpy(mem + (a - BASE), &v, 8); }
static float getf(uint64_t a) { float v; memcpy(&v, mem + (a - BASE), 4); return v; }
static int near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }
static uint64_t get64(uint64_t a) { uint64_t v; memcpy(&v, mem + (a - BASE), 8); return v; }
static Ctx c;
/* the game's f_8154f1c0 (a picture's record packed: what each piece was, a made-up address for its constants) and f_81550720
   (the drawer drawn: the constants it was drawn with; the site at its start runs again in it) */
static int packs, draws;
static float packed[4][7];   /* (translation x, y, width, top left u, top right u, bottom left u, bottom right u) */
static uint64_t packed_args[2], drawn_with[4];
static void fake_pack(Ctx *k) {
    const uint64_t rec = k->x[0];
    if (packs < 4) {
        const float row[7] = {getf(rec + 0x4c), getf(rec + 0x5c), getf(rec + 0xb0), getf(rec + 0xc0), getf(rec + 0xc8), getf(rec + 0xd0), getf(rec + 0xd8)};
        memcpy(packed[packs], row, sizeof row);
    }
    packed_args[0] = k->x[1], packed_args[1] = k->x[2];
    put64(k->x[8], 0x1000 + (uint64_t)packs), put64(k->x[8] + 8, 0x2000 + (uint64_t)packs), put64(k->x[8] + 16, 0x3000 + (uint64_t)packs);
    packs++;
    k->x[8] = k->x[9] = k->x[19] = 0;   /* (what a call may leave in the registers) */
}
static void fake_draw(Ctx *k) {
    hos_wide_ui_bg_draw(k);
    if (draws < 4) drawn_with[draws] = get64(k->x[0] + 0x48);
    draws++;
}
A64Fn a64_lookup(uint64_t addr) { return addr == 0x8154f1c0ull ? fake_pack : addr == 0x81550720ull ? fake_draw : NULL; }
#define LAYOUT (BASE + 0x100)
#define WORLD (BASE + 0x300)
#define STATUS (BASE + 0x500)
#define PANES (BASE + 0x1000)
#define NAMES (BASE + 0x8000)
#define STRINGS (BASE + 0x9000)
static uint64_t text_at = STRINGS;
static uint64_t text(const char *s) { const uint64_t a = text_at; memcpy(mem + (a - BASE), s, strlen(s) + 1); text_at += 64; return a; }
/* a layout object and its UIL header: its name, the pane names (one table for every test layout) */
static void layout_named(uint64_t l, const char *name) {
    const uint64_t res = l + 0x180;
    memcpy(mem + (l + 0x100 - BASE), &(uint64_t){PANES}, 8), memcpy(mem + (l + 0x13c - BASE), &(uint32_t){64}, 4);
    memcpy(mem + (l + 0xf8 - BASE), &res, 8);
    memcpy(mem + (res + 0x18 - BASE), &(uint64_t){text(name)}, 8), memcpy(mem + (res + 0x20 - BASE), &(uint64_t){NAMES}, 8);
}
/* pane i of the layout: x scale 1, its x and width; origin across 0 left, 1 centre; kind 0 none, 1 picture, 6 part */
static uint64_t pane_in(uint64_t layout, int i, const char *name, float x, float w, unsigned origin, unsigned type) {
    const uint64_t p = PANES + (uint64_t)i * 0xb0;
    put64(p + 0x18, layout), put64(NAMES + (uint64_t)i * 8, text(name));
    putf(p + 0x28, 1.0f), putf(p + 0x4c, x), putf(p + 0x58, w);
    put32(p + 0xa8, ((origin + 3) << 11) | (type << 22) | 1);
    return p;
}
static uint64_t pane(int i, float x, float w, unsigned origin, unsigned type) { return pane_in(LAYOUT, i, "P_Test_00", x, w, origin, type); }
/* the game making p's transform from parent (0: none); x as the game stores it (its parent's move in it) */
static float place(uint64_t p, uint64_t parent, float x) {
    putf(p + 0x4c, x);
    hos_wide_ui_parent(parent);
    hos_wide_ui_placed(&c, p);
    return getf(p + 0x4c);
}
static float drawn_width(uint64_t p) {
    const float w = getf(p + 0x58);
    memcpy(c.vreg[0], &w, 4);
    hos_wide_ui_size(&c, p);
    float out;
    memcpy(&out, c.vreg[0], 4);
    return out;
}
int main(void) {
    layout_named(LAYOUT, "Test_00"), layout_named(WORLD, "Pointer_00"), layout_named(STATUS, "PlayerStatus_00");
    hos_wide_squeeze = (16.0f / 9.0f) / (3440.0f / 1440.0f);   /* (3440x1440) */
    const float extra = 1920.0f * (1.0f / hos_wide_squeeze - 1.0f);
    const uint64_t root = pane(0, 0, 0, 1, 0), all = pane(1, 0, 200, 1, 0), left = pane(2, -1920, 60, 1, 0),
                   heart = pane(3, -1140, 1300, 1, 6), right = pane(4, 1626, 240, 1, 1), back = pane(5, 0, 3840, 1, 1),
                   page = pane(6, 3840, 0, 1, 0), cell = pane(7, 3840, 200, 1, 1), grid = pane(10, 0, 0, 1, 0),
                   col0 = pane(11, -1750, 200, 1, 1), col1 = pane(12, -900, 200, 1, 1), preview = pane(13, 700, 900, 1, 1),
                   edge = pane(14, -1920, 400, 0, 1);
    /* the HUD by name: the left group stays (the pause menu places part of it), its gauges move; a pointer placed from the
       world stays wherever it is */
    const uint64_t sroot = pane_in(STATUS, 20, "RootPane", 0, 0, 1, 0), scale = pane_in(STATUS, 21, "N_ScaleL_00", -1920, 60, 1, 0),
                   gauge = pane_in(STATUS, 22, "Pa_HeartGauge_00", -1140, 1300, 1, 6),
                   step = pane_in(STATUS, 23, "N_ScaleL_01", 6, 60, 1, 0), mp = pane_in(STATUS, 24, "Pa_MpGauge_00", -1560, 1256, 1, 6),
                   wroot = pane_in(WORLD, 25, "RootPane", 0, 0, 1, 0), arrow = pane_in(WORLD, 26, "P_Arrow_00", -1800, 100, 1, 1);
    /* the game's frame: parents before children; dx the move the plan gave each parent */
    float moved_left = 0, moved_grid0 = 0, moved_grid1 = 0;
#define FRAME() do { \
        place(root, 0, 0); place(all, root, 0); moved_left = place(left, all, -1920) + 1920; \
        place(heart, left, -1140 + moved_left); place(right, all, 1626); place(back, root, 0); \
        place(page, root, 3840); place(cell, page, 3840); place(grid, root, 0); \
        moved_grid0 = place(col0, grid, -1750) + 1750; moved_grid1 = place(col1, grid, -900) + 900; \
        place(preview, grid, 700); place(edge, root, -1920); \
        place(sroot, 0, 0); place(scale, sroot, -1920); place(gauge, scale, -1140); place(step, scale, 6); \
        place(mp, step, -1560); place(wroot, 0, 0); place(arrow, wroot, -1800); \
    } while (0)

    hos_wide_ui_tick();   /* Centred, blurred sides: nothing moves */
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    menu = 1;   /* Extended: noted on the first frame, planned at the present, moved on the next */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0);
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);   /* (the plan changed: every pane placed again) */
    FRAME();
    CHECK(near(moved_left, -extra));                       /* (the HUD's left side out, at the left edge) */
    CHECK(near(getf(heart + 0x4c), -1140 - extra));        /* (made from it: moved once) */
    CHECK(near(getf(right + 0x4c), 1626 + extra));         /* (far out on the right: out) */
    CHECK(near(drawn_width(back), 3840 / hos_wide_squeeze) && drawn_width(right) == 240);   /* (spans: wider) */
    CHECK(getf(page + 0x4c) == 3840 && getf(cell + 0x4c) == 3840);   /* (off the canvas: left alone) */
    CHECK(near(moved_grid0, -extra) && near(moved_grid1, -extra));   /* (a grid's columns out together) */
    CHECK(getf(preview + 0x4c) == 700);                    /* (its other side stays) */
    CHECK(near(getf(edge + 0x4c), -1920 - extra));         /* (left origin, at the left edge) */
    CHECK(getf(scale + 0x4c) == -1920 && getf(step + 0x4c) == 6);   /* (the HUD's left group: kept) */
    CHECK(near(getf(gauge + 0x4c), -1140 - extra) && near(getf(mp + 0x4c), -1560 - extra));   /* (its gauges: out) */
    CHECK(getf(arrow + 0x4c) == -1800);                    /* (placed from the world: stays) */
    for (int i = 0; i < 4; i++) { hos_wide_ui_tick(); FRAME(); }   /* (stable: the plan no longer changes) */
    CHECK(hos_wide_ui_force == 0 && near(moved_left, -extra) && near(getf(heart + 0x4c), -1140 - extra));

    /* the System page: its button column out to the left; the controls sheet (a part right of the centre, its backdrop and
       capture to the right edge, labels either side of the controller) kept whole */
    {
#define SYSMENU (BASE + 0x700)
#define SYSCTRL (BASE + 0x900)
        layout_named(SYSMENU, "PaSystemMenu_00"), layout_named(SYSCTRL, "PaSystemController_00");
        /* (its root made from the pause menu's page pane, another layout's: here one no plan walks) */
        const uint64_t mroot = pane_in(SYSMENU, 30, "RootPane", 0, 0, 1, 0), inout = pane_in(SYSMENU, 31, "N_InOut_00", 0, 60, 1, 0),
                       button = pane_in(SYSMENU, 32, "Pa_Button_00", -1330, 720, 1, 6),
                       sheet = pane_in(SYSMENU, 33, "Pa_SystemController_00", 540, 2600, 1, 6),
                       croot = pane_in(SYSCTRL, 34, "RootPane", 540, 0, 1, 0), call = pane_in(SYSCTRL, 35, "N_All_00", 540, 2240, 1, 0),
                       capture = pane_in(SYSCTRL, 36, "C_Base_00", 540, 2560, 1, 5), base = pane_in(SYSCTRL, 37, "P_Base_00", 540, 2560, 1, 1),
                       basic = pane_in(SYSCTRL, 38, "N_Basic_00", 540, 90, 1, 0), zl = pane_in(SYSCTRL, 39, "N_ZL_00", -40, 60, 1, 0),
                       guide_l = pane_in(SYSCTRL, 40, "T_GuideL_00", -140, 442, 2, 2), zr = pane_in(SYSCTRL, 41, "N_ZR_00", 1120, 60, 1, 0),
                       guide_r = pane_in(SYSCTRL, 42, "T_GuideR_00", 1218, 442, 0, 2);
        float o = 0;   /* (where the strip has slid the page to) */
#define SYS_FRAME() do { \
            place(mroot, wroot, o); place(inout, mroot, o); place(button, inout, o - 1330); place(sheet, inout, o + 540); \
            place(croot, sheet, o + 540); place(call, croot, o + 540); place(capture, call, o + 540); place(base, call, o + 540); \
            place(basic, call, o + 540); place(zl, basic, o - 40); place(guide_l, zl, o - 140); place(zr, basic, o + 1120); \
            place(guide_r, zr, o + 1218); \
        } while (0)
        o = 1500;   /* (sliding in: the column at its place on the page, moving with it) */
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); SYS_FRAME(); }
        CHECK(near(getf(button + 0x4c), o - 1330 - extra) && getf(sheet + 0x4c) == o + 540);
        /* (the plan reading a frame the game's threads are placing: the page's root as it was, its panes moved on) */
        o = 1950;
        SYS_FRAME();
        node_find(mroot)->ox = 1500;
        hos_wide_ui_tick();
        SYS_FRAME();
        CHECK(near(getf(button + 0x4c), o - 1330 - extra));
        o = 0;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); SYS_FRAME(); }
        CHECK(near(getf(button + 0x4c), -1330 - extra));
        CHECK(getf(sheet + 0x4c) == 540 && getf(capture + 0x4c) == 540 && getf(base + 0x4c) == 540);
        CHECK(getf(guide_l + 0x4c) == -140 && getf(guide_r + 0x4c) == 1218);
        CHECK(drawn_width(base) == 2560);
    }

    /* the pause menu: its pages spaced 2 x the extra further apart along their strips, a strip slid by the wider spacing; the
       background drawn in pieces, its halves apart and its centre column across between */
    {
#define PLUS (BASE + 0xb00)
        layout_named(PLUS, "PlusMenu_00");
        const uint64_t proot = pane_in(PLUS, 43, "RootPane", 0, 0, 1, 0), strip = pane_in(PLUS, 44, "N_MenuChange_00", 0, 200, 1, 0),
                       info1 = pane_in(PLUS, 45, "P_InfoBg_01", 4186, 1600, 1, 1), amiibo = pane_in(PLUS, 46, "P_MenuBgAmiibo_00", 2932, 520, 1, 1),
                       bg = pane_in(PLUS, 47, "P_MenuBg_00", 4280, 9200, 1, 1), strip1 = pane_in(PLUS, 48, "N_MenuChange_01", 0, 200, 1, 0),
                       dream = pane_in(PLUS, 49, "N_DreamInfo_00", 0, 200, 1, 0), pos = pane_in(PLUS, 50, "N_Pos_01", 8560, 200, 1, 0),
                       equip = pane_in(PLUS, 51, "Pa_Equip_00", 4280, 200, 1, 6),
                       fade = pane_in(PLUS, 8, "C_3DFade_00", 1080, 1680, 1, 5), faded = pane_in(PLUS, 9, "P_3DFade_02", 1080, 1680, 1, 1);
        const float slide = -4280, stride = 4280 + 2 * extra;   /* (the Equipment page shown) */
#define PM_FRAME() do { \
            place(proot, 0, 0); const float s0 = place(strip, proot, slide); place(info1, strip, s0 + 4186); \
            place(amiibo, strip, s0 + 2932); place(bg, strip, s0 + 4280); \
            const float s1 = place(strip1, proot, slide), d = place(dream, strip1, s1); \
            place(pos, dream, d + 8560); place(equip, strip1, s1 + 4280); \
            putf(fade + 0x28, 1.0f); place(fade, proot, 1080); place(faded, proot, 1080); \
        } while (0)
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PM_FRAME(); }
        CHECK(near(getf(strip + 0x4c), -stride) && near(getf(strip1 + 0x4c), -stride));   /* (slid a wider page) */
        CHECK(near(getf(info1 + 0x4c), 4186 - 4280));             /* (on the page shown: where it was on screen) */
        CHECK(near(getf(amiibo + 0x4c), 2932 - 4280 - extra));    /* (and its own side move) */
        CHECK(near(getf(bg + 0x4c), 0) && near(getf(equip + 0x4c), 0));
        CHECK(near(getf(pos + 0x4c), stride));                    /* (two pages on, in a pane of the strip's) */
        /* (the game view's fade as a page changes: its capture and the picture drawing it as the game makes them) */
        CHECK(getf(fade + 0x4c) == 1080 && getf(fade + 0x28) == 1.0f && getf(faded + 0x4c) == 1080 && drawn_width(faded) == 1680);

        /* the background's draw: its record as f_815505b0 filled it (scale 1, at 0 across and 7 down, one texture over all
           of it), packed three times */
        const uint64_t drawer = BASE + 0x2e000, info = BASE + 0x2f000, sp = BASE + 0x30000, rec = sp + 0x54;
        Ctx k;
        const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
#define PM_RECORD() do { \
            memset(mem + (rec - BASE), 0, 0x640); \
            putf(rec + 0x40, 1), putf(rec + 0x54, 1), putf(rec + 0x5c, 7), putf(rec + 0x68, 1); \
            putf(rec + 0xb0, 9200), putf(rec + 0xb4, 2200), put32(rec + 0x630, 1); \
            for (int j = 0; j < 8; j++) putf(rec + 0xc0 + 4 * (uint64_t)j, uv[j]); \
            memset(&k, 0, sizeof k); \
            k.x[19] = drawer, k.x[20] = bg, k.x[23] = info, k.x[31] = sp, k.x[8] = k.x[9] = 0xdead; \
            packs = draws = 0; \
        } while (0)
        put64(info + 0x18, 0xa1), put64(info + 0x20, 0xa2);
        PM_RECORD();
        hos_wide_ui_bg(&k);
        CHECK(packs == 3 && packed_args[0] == 0xa1 && packed_args[1] == 0xa2);
        const float half_r[7] = {extra + 2300, 7, 4600, 0.5f, 1, 0.5f, 1}, mid[7] = {0, 7, 2 * extra, 0.5f, 0.5f, 0.5f, 0.5f},
                    half_l[7] = {-extra - 2300, 7, 4600, 0, 0.5f, 0, 0.5f};
        for (int j = 0; j < 7; j++) CHECK(near(packed[0][j], half_r[j]) && near(packed[1][j], mid[j]) && near(packed[2][j], half_l[j]));
        CHECK(k.x[8] == 0x1002 && k.x[9] == 0x2002 && k.x[19] == drawer);   /* (the drawer keeps the left half's) */
        put64(drawer + 0x48, k.x[8]), put64(drawer + 0x50, k.x[9]), put64(drawer + 0x58, get64(sp + 0x18));
        Ctx dk;
        memset(&dk, 0, sizeof dk);
        dk.x[0] = drawer, dk.x[1] = 0x77;
        hos_wide_ui_bg_draw(&dk);
        CHECK(draws == 2 && drawn_with[0] == 0x1000 && drawn_with[1] == 0x1001);   /* (the right half, the column) */
        CHECK(get64(drawer + 0x48) == 0x1002 && get64(drawer + 0x58) == 0x3002 && dk.x[0] == drawer && dk.x[1] == 0x77);

        menu = 2;   /* off: the pages as the game spaces them, the background drawn whole */
        hos_wide_ui_tick();
        FRAME(); PM_FRAME();
        CHECK(getf(strip + 0x4c) == -4280 && getf(info1 + 0x4c) == 4186 - 4280 && getf(pos + 0x4c) == 4280);
        CHECK(getf(amiibo + 0x4c) == 2932 - 4280 && getf(bg + 0x4c) == 0);
        PM_RECORD();
        hos_wide_ui_bg(&k);
        hos_wide_ui_bg_draw(&dk);
        CHECK(packs == 0 && draws == 0 && k.x[8] == 0xdead);
        menu = 1;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PM_FRAME(); }
        CHECK(near(getf(strip + 0x4c), -stride) && near(getf(pos + 0x4c), stride));
    }

    /* the Threads of Time page sliding in by 80: its vignette as wide as at rest. The Map page: its title, its icons (placed by
       code, one by the right edge) and the world map (wider than the screen) kept over the map, which stays 16:9. The same
       layout is the HUD's minimap: there its icons are not at the origin and move out with it, and the title is planned */
    {
#define CHALLENGE (BASE + 0x4000)
#define MAPMENU (BASE + 0x4200)
#define MAPICON (BASE + 0x4400)
#define MAPWORLD (BASE + 0x4600)
        layout_named(CHALLENGE, "ChallengeMenu_01"), layout_named(MAPMENU, "MapMenu_00");
        layout_named(MAPICON, "PaMapIcon_00"), layout_named(MAPWORLD, "PaMapWorld_00");
        const uint64_t troot = pane_in(CHALLENGE, 52, "RootPane", 0, 0, 1, 0), slide_in = pane_in(CHALLENGE, 53, "N_In_00", 0, 60, 1, 0),
                       vgroup = pane_in(CHALLENGE, 54, "N__Vignette_00", 0, 200, 1, 0),
                       vignette = pane_in(CHALLENGE, 55, "W_Vignette_00", 0, 4080, 1, 1),
                       mroot2 = pane_in(MAPMENU, 56, "RootPane", 0, 0, 1, 0), title = pane_in(MAPMENU, 57, "T_MapTitle_00", -1702, 1030, 0, 1),
                       icons = pane_in(MAPMENU, 58, "N_MapIcon_00", 0, 60, 1, 0), world = pane_in(MAPMENU, 59, "Pa_WorldMap_00", 0, 9920, 1, 6),
                       iroot = pane_in(MAPICON, 60, "RootPane", 0, 0, 1, 0), icon = pane_in(MAPICON, 61, "P_Icon_00", 1800, 60, 1, 1),
                       wmroot = pane_in(MAPWORLD, 62, "RootPane", 0, 0, 1, 0), wmap = pane_in(MAPWORLD, 63, "P_WorldMap_00", 0, 8448, 1, 1);
        float d = 80, ix = 0;
#define PAGES_FRAME() do { \
            place(troot, 0, 0); place(slide_in, troot, d); place(vgroup, slide_in, d); place(vignette, vgroup, d); \
            place(mroot2, 0, 0); place(title, mroot2, -1702); place(icons, mroot2, ix); \
            place(iroot, icons, getf(icons + 0x4c)); place(icon, iroot, getf(icons + 0x4c) + 1800 - ix); place(world, mroot2, 0); place(wmroot, world, 0); place(wmap, wmroot, 0); \
        } while (0)
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(drawn_width(vignette), 4080 / hos_wide_squeeze) && getf(vignette + 0x4c) == 80);
        d = 0;
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(drawn_width(vignette), 4080 / hos_wide_squeeze));
        CHECK(getf(title + 0x4c) == -1702);
        CHECK(getf(icon + 0x4c) == 1800 && getf(wmap + 0x4c) == 0 && drawn_width(wmap) == 8448);
        ix = 1490;   /* the minimap: its icons over it, by the right edge */
        for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); PAGES_FRAME(); }
        CHECK(near(getf(icons + 0x4c), 1490 + extra));
        CHECK(near(getf(title + 0x4c), -1702 - extra));
    }

    /* a layout let go: its panes are not planned with the ones that take their addresses */
    put32(LAYOUT + 0x13c, 12);
    hos_wide_ui_tick();
    FRAME();
    CHECK(getf(edge + 0x4c) == -1920);
    put32(LAYOUT + 0x13c, 64);

    menu = 2;   /* back: every pane remade as the game makes it */
    hos_wide_ui_tick();
    CHECK(hos_wide_ui_force > 0);
    FRAME();
    CHECK(moved_left == 0 && getf(right + 0x4c) == 1626 && drawn_width(back) == 3840);

    hos_wide_squeeze = 1.0f;   /* a 16:9 screen: Extended does nothing */
    menu = 1;
    for (int i = 0; i < 3; i++) { hos_wide_ui_tick(); FRAME(); }
    CHECK(moved_left == 0);

    /* the force site: bit 6 of the dirty bits set while forced */
    {
        Ctx *const site = &c;
        site->x[8] = 0x1;
        hos_wide_ui_force = 2;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x41);
        hos_wide_ui_force = 0;
        site->x[8] = 0x1;
        { Ctx *c = site; FORCE_SITE }
        CHECK(site->x[8] == 0x1);
    }
    (void)moved_grid0, (void)moved_grid1;
    printf("PASS: Extended plans each layout's side panes out to the screen's edges, and back\n");
    return 0;
}
'''


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    host = (ROOT / "host" / "hos_wide_ui.c").read_text(encoding="utf-8")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    framegen = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    option = replay[replay.index("t.option = "):]
    option = option[:option.index("};") + 2]

    check("enum { HOS_OPT_WIDE_MENUS = 53 };" in api, "no option id for Ultrawide menus")
    check("if (id == HOS_OPT_WIDE_MENUS) return opts.wide_menus;" in option, "the host cannot see the Ultrawide menus row")
    check('"Ultrawide menus", &opts.wide_menus, 0, 3, 1, tx_wide_menus' in menu, "the row does not reach Extended")
    check('"Extended"' in menu, "the row has no Extended value")
    check("opts.wide_menus == 2 || opts.wide_menus == 3" in framegen, "Extended would get side bars over its side panes")
    check("w.side = opts.wide_menus - 1" in framegen, "the centred choices do not reach their sides")
    check("if (version < 7) opts.wide_menus" in menu and 'options_version=7' in menu, "an older options file's centred choice would become Extended")
    check("hos_wide_ui_tick();" in backend, "the host is not told the choice each present")
    check("rx_option(HOS_OPT_WIDE_MENUS, 0)" in host and 'getenv("HOS_WIDE_UI")' in host, "the host does not read the choice")

    words = {off: word for off, word, _ in oot_wide.UI_SITES}
    check(words == {0x152a730: 0xb9400008, 0x152caa0: 0xb9400008, 0x152cd98: 0xad4207e0, 0x152cdd4: 0xb9403008, 0x152ce64: 0xfc04c263,
                    0x152ac40: 0x2d4b0400, 0x15306f4: 0xa940a7e8, 0x1530740: 0xaa0003f6, 0x1525ca0: 0x91004260},
          "the pane transform sites moved")
    check(all(site in oot_wide.SITES for site in oot_wide.UI_SITES), "the pane sites are not installed")
    for name in ("hos_wide_ui_parent(uint64_t parent)", "hos_wide_ui_placed(Ctx *c, uint64_t pane)",
                 "hos_wide_ui_size(Ctx *c, uint64_t pane)", "hos_wide_ui_bg(Ctx *c)", "hos_wide_ui_bg_draw(Ctx *c)"):
        check(("void " + name) in host, "the host's %s has another signature" % name.split("(")[0])

    body = "".join(line for line in host.splitlines(keepends=True) if not line.startswith("#include"))
    for decl in ("int rx_option(int id, int otherwise);\n", "extern volatile float hos_wide_squeeze;\n"):
        check(decl in body, "the host file no longer declares " + decl.strip())
        body = body.replace(decl, "", 1)
    src = FIXTURE_HEAD + body + FIXTURE_MAIN.replace("FORCE_SITE", oot_wide.UI_FORCE)
    with tempfile.TemporaryDirectory(prefix="wide-ui-") as tmp:
        c = Path(tmp) / "fixture.c"
        exe = Path(tmp) / "fixture"
        c.write_text(src, encoding="utf-8")
        cc = subprocess.run([a.cc, "-std=c11", "-O1", "-Wall", "-Wno-unused-function", "-o", str(exe), str(c), "-lm"],
                            capture_output=True, text=True)
        check(cc.returncode == 0, "fixture did not compile:\n" + cc.stdout + cc.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        check(run.returncode == 0, "fixture failed:\n" + run.stdout + run.stderr)
        print(run.stdout.strip())


if __name__ == "__main__":
    main()
