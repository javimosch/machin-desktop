// mdk host — the native shell under a machin desktop app.
//
// Owns everything that must outlive a hot reload: the SDL3 window, the GL 3.3
// context, the glyph atlas, and the app's state store. The app itself is MFL
// compiled to a shared object (dev) or linked straight in (release); each frame
// the host calls its exported mdk_frame, which builds an element tree, lays it
// out, and paints by calling back into the mdk_* functions below. Painting is
// GPUI-style: every primitive (rounded rect, border, shadow, glyph) is one
// instance in a single buffer, drawn with ONE instanced call through an SDF
// fragment shader.
//
// Dev reload: the host watches the .so's mtime, dlopens a fresh copy under a
// unique name (the old one is never dlclosed — the MFL runtime may hold
// threads/statics) and keeps the state store, so the window, its size and the
// app's state survive every edit. A sibling <app>.err file, written by the
// build when `machin` rejects the edit, is drawn as an overlay over the last
// good frame instead of a blank window.
#define GL_GLEXT_PROTOTYPES 1
#include <SDL3/SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- instances
typedef struct {
    float x, y, w, h;      // logical px
    float u0, v0, u1, v1;  // atlas uv (glyphs)
    uint8_t col[4];        // fill / glyph color (rgba)
    uint8_t bcol[4];       // border color
    float radius, border, kind, blur;  // kind: 0 rect, 1 glyph, 2 shadow
    float cx0, cy0, cx1, cy1;          // clip rect (logical px); pixels outside are discarded
} Inst;

// Clip stack: each instance records the clip in force when it was pushed.
#define CLIPS 64
static float g_clip[CLIPS][4];
static int g_nclip;

static Inst* g_inst;
static int g_wh_px_hint = 1 << 30;  // framebuffer height, for culling text lines
static int g_ninst, g_capinst;

static Inst* push_inst(void) {
    if (g_ninst == g_capinst) {
        g_capinst = g_capinst ? g_capinst * 2 : 4096;
        g_inst = realloc(g_inst, sizeof(Inst) * (size_t)g_capinst);
    }
    Inst* i = &g_inst[g_ninst++];
    memset(i, 0, sizeof *i);
    if (g_nclip > 0) {
        i->cx0 = g_clip[g_nclip - 1][0]; i->cy0 = g_clip[g_nclip - 1][1];
        i->cx1 = g_clip[g_nclip - 1][2]; i->cy1 = g_clip[g_nclip - 1][3];
    } else { i->cx0 = -1e9f; i->cy0 = -1e9f; i->cx1 = 1e9f; i->cy1 = 1e9f; }
    return i;
}

static void set_rgba(uint8_t* d, int64_t c) {
    d[0] = (uint8_t)(c >> 24); d[1] = (uint8_t)(c >> 16);
    d[2] = (uint8_t)(c >> 8);  d[3] = (uint8_t)c;
}

// ------------------------------------------------------------------- text
// Two faces (regular, bold). Glyphs are rasterized once per (face, gid, px)
// into an R8 atlas. Shaping is cached per (string, px, face): a frame that
// shows the same text as the last one — the common case, and all of a resized
// document — only re-breaks lines over cached advances, never re-shapes.
#define ATLAS 1024
typedef struct { uint32_t gid; int px, face, used; float u0, v0, u1, v1; int w, h, bl, bt; } Glyph;
#define GCAP 8192
static Glyph g_glyphs[GCAP];
static uint8_t* g_atlas_px;
static int g_atlas_full;
static int g_ax, g_ay, g_arow, g_atlas_dirty, g_atlas_y0 = ATLAS, g_atlas_y1;
static GLuint g_atlas_tex;
static FT_Library g_ft;
static FT_Face g_faces[2];
static hb_font_t* g_hbs[2];
static int g_cur_px[2] = {-1, -1};
static float g_dpr = 1.0f;
static hb_buffer_t* g_hbbuf;

static void font_px(int face, int px) {
    if (px == g_cur_px[face]) return;
    FT_Set_Pixel_Sizes(g_faces[face], 0, (FT_UInt)px);
    hb_ft_font_changed(g_hbs[face]);
    g_cur_px[face] = px;
}

static void atlas_reset(void) {
    memset(g_glyphs, 0, sizeof g_glyphs);
    memset(g_atlas_px, 0, (size_t)ATLAS * ATLAS);
    g_ax = g_ay = g_arow = 0;
    g_atlas_y0 = 0; g_atlas_y1 = ATLAS; g_atlas_dirty = 1;
}

static Glyph* glyph(int face, uint32_t gid, int px) {
    uint32_t h = (gid * 2654435761u) ^ ((uint32_t)px * 40503u) ^ ((uint32_t)face * 97u);
    for (int n = 0; n < GCAP; n++) {
        Glyph* g = &g_glyphs[(h + (uint32_t)n) & (GCAP - 1)];
        if (g->used && g->gid == gid && g->px == px && g->face == face) return g;
        if (g->used) continue;
        FT_Face f = g_faces[face];
        font_px(face, px);
        if (FT_Load_Glyph(f, gid, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return NULL;
        FT_Bitmap* bm = &f->glyph->bitmap;
        int w = (int)bm->width, hh = (int)bm->rows;
        if (g_ax + w + 1 > ATLAS) { g_ax = 0; g_ay += g_arow + 1; g_arow = 0; }
        if (g_ay + hh + 1 > ATLAS) { g_atlas_full = 1; return NULL; }  // dropped this frame; atlas resets after it
        for (int r = 0; r < hh; r++)
            memcpy(g_atlas_px + (size_t)(g_ay + r) * ATLAS + g_ax, bm->buffer + r * bm->pitch, (size_t)w);
        g->used = 1; g->gid = gid; g->px = px; g->face = face; g->w = w; g->h = hh;
        g->bl = f->glyph->bitmap_left; g->bt = f->glyph->bitmap_top;
        g->u0 = (float)g_ax / ATLAS; g->v0 = (float)g_ay / ATLAS;
        g->u1 = (float)(g_ax + w) / ATLAS; g->v1 = (float)(g_ay + hh) / ATLAS;
        if (g_ay < g_atlas_y0) g_atlas_y0 = g_ay;
        if (g_ay + hh > g_atlas_y1) g_atlas_y1 = g_ay + hh;
        g_ax += w + 1; if (hh > g_arow) g_arow = hh;
        g_atlas_dirty = 1;
        return g;
    }
    return NULL;
}

// A shaped run: per glyph its id, advance and offsets (device px), and
// whether a line may break after it (it shapes a space).
typedef struct { uint32_t gid; float adv, xo, yo; uint8_t brk; } SG;
typedef struct { uint64_t key; int used, n, px, face; float width; SG* g; } Run;
#define RCAP 8192
static Run g_runs[RCAP];
static int g_nruns;

static uint64_t run_key(const char* s, int px, int face) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)px * 1099511628211ull ^ (uint64_t)face;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) h = (h ^ *p) * 1099511628211ull;
    return h ? h : 1;
}

static Run* run_of(const char* s, int px, int face) {
    uint64_t k = run_key(s, px, face);
    if (g_nruns > RCAP * 3 / 4) {  // simple eviction: drop everything, re-shape on demand
        for (int i = 0; i < RCAP; i++) if (g_runs[i].used) free(g_runs[i].g);
        memset(g_runs, 0, sizeof g_runs);
        g_nruns = 0;
    }
    for (int n = 0; n < RCAP; n++) {
        Run* r = &g_runs[(k + (uint64_t)n) & (RCAP - 1)];
        if (r->used && r->key == k) return r;
        if (r->used) continue;
        font_px(face, px);
        hb_buffer_clear_contents(g_hbbuf);
        hb_buffer_add_utf8(g_hbbuf, s, -1, 0, -1);
        hb_buffer_guess_segment_properties(g_hbbuf);
        hb_shape(g_hbs[face], g_hbbuf, NULL, 0);
        unsigned cnt;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(g_hbbuf, &cnt);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(g_hbbuf, &cnt);
        r->used = 1; r->key = k; r->px = px; r->face = face; r->n = (int)cnt;
        r->g = malloc(sizeof(SG) * (cnt ? cnt : 1));
        float w = 0;
        for (unsigned i = 0; i < cnt; i++) {
            SG* g = &r->g[i];
            g->gid = info[i].codepoint;
            g->adv = (float)pos[i].x_advance / 64.0f;
            g->xo = (float)pos[i].x_offset / 64.0f;
            g->yo = (float)pos[i].y_offset / 64.0f;
            g->brk = s[info[i].cluster] == ' ';
            w += g->adv;
        }
        r->width = w;
        g_nruns++;
        return r;
    }
    return NULL;
}

static int text_px(float size) { int px = (int)(size * g_dpr + 0.5f); return px < 1 ? 1 : px; }

// Greedy line breaking at spaces. Fills starts[] with each line's first glyph
// and returns the line count; *maxline gets the widest line (device px).
#define MAXLINES 4096
static int g_starts[MAXLINES + 1];
static int wrap(Run* r, float maxw, float* maxline) {
    int lines = 0, ls = 0, lastbrk = -1;
    float lw = 0, wmax = 0, upto_brk = 0;
    g_starts[lines++] = 0;
    for (int i = 0; i < r->n; i++) {
        lw += r->g[i].adv;
        if (r->g[i].brk) { lastbrk = i; upto_brk = lw - r->g[i].adv; }
        if (maxw > 0 && lw > maxw && lastbrk >= ls && lastbrk < i && lines < MAXLINES) {
            if (upto_brk > wmax) wmax = upto_brk;
            ls = lastbrk + 1;
            g_starts[lines++] = ls;
            lw = 0;
            for (int j = ls; j <= i; j++) lw += r->g[j].adv;
            lastbrk = -1;
        }
    }
    if (lw > wmax) wmax = lw;
    g_starts[lines] = r->n;
    if (maxline) *maxline = wmax;
    return lines;
}

static float line_h_px(int face, int px) {
    font_px(face, px);
    return (float)g_faces[face]->size->metrics.height / 64.0f;
}

static void paint_run(Run* r, float x, float y, float maxw, int64_t rgba) {
    float maxline;
    int lines = wrap(r, maxw * g_dpr, &maxline);
    FT_Face f = g_faces[r->face];
    font_px(r->face, r->px);
    float asc = (float)f->size->metrics.ascender / 64.0f;
    float lh = (float)f->size->metrics.height / 64.0f;
    float clip_y1 = (float)g_wh_px_hint;
    for (int l = 0; l < lines; l++) {
        float ly = y * g_dpr + lh * (float)l;
        if (ly > clip_y1) break;              // below the window: nothing more is visible
        if (ly + lh < 0) continue;            // above it
        float pen = 0;
        for (int i = g_starts[l]; i < g_starts[l + 1]; i++) {
            SG* sg = &r->g[i];
            Glyph* g = glyph(r->face, sg->gid, r->px);
            if (g && g->w > 0) {
                Inst* in = push_inst();
                in->kind = 1;
                in->x = x + (pen + sg->xo + (float)g->bl) / g_dpr;
                in->y = (ly + asc - sg->yo - (float)g->bt) / g_dpr;
                in->w = (float)g->w / g_dpr; in->h = (float)g->h / g_dpr;
                in->u0 = g->u0; in->v0 = g->v0; in->u1 = g->u1; in->v1 = g->v1;
                set_rgba(in->col, rgba);
            }
            pen += sg->adv;
        }
    }
}

// ------------------------------------------------- per-frame string pool
// MFL strings live in the frame arena; the host copies what the tree needs
// so the app never has to keep strings in globals across frames.
static char** g_strs;
static int g_nstr, g_capstr;

int64_t mdk_str(const char* s) {
    if (g_nstr == g_capstr) {
        g_capstr = g_capstr ? g_capstr * 2 : 1024;
        g_strs = realloc(g_strs, sizeof(char*) * (size_t)g_capstr);
    }
    g_strs[g_nstr] = strdup(s ? s : "");
    return g_nstr++;
}
static const char* str_at(int64_t h) { return (h >= 0 && h < g_nstr) ? g_strs[h] : ""; }
static void strs_reset(void) { for (int i = 0; i < g_nstr; i++) free(g_strs[i]); g_nstr = 0; }

// ---------------------------------------------------------- app-facing API
// face: 0 regular, 1 bold. maxw <= 0 means one line.
float mdk_text_w(int64_t h, float size, int64_t face) {
    Run* r = run_of(str_at(h), text_px(size), (int)face & 1);
    return r ? r->width / g_dpr : 0;
}
float mdk_line_h(float size, int64_t face) { return line_h_px((int)face & 1, text_px(size)) / g_dpr; }
// Wrapped size at maxw: returns the height; the widest line goes to g_last_wrap_w.
static float g_last_wrap_w;
float mdk_text_wrap_h(int64_t h, float size, int64_t face, float maxw) {
    int px = text_px(size);
    Run* r = run_of(str_at(h), px, (int)face & 1);
    if (!r) return 0;
    float ml;
    int lines = wrap(r, maxw * g_dpr, &ml);
    g_last_wrap_w = ml / g_dpr;
    return (float)lines * line_h_px((int)face & 1, px) / g_dpr;
}
float mdk_last_wrap_w(void) { return g_last_wrap_w; }
void mdk_text(int64_t h, float x, float y, float size, int64_t rgba, int64_t face, float maxw) {
    Run* r = run_of(str_at(h), text_px(size), (int)face & 1);
    if (r) paint_run(r, x, y, maxw, rgba);
}

void mdk_rect(float x, float y, float w, float h, int64_t rgba, float radius, float border, int64_t brgba) {
    Inst* in = push_inst();
    in->x = x; in->y = y; in->w = w; in->h = h;
    set_rgba(in->col, rgba); set_rgba(in->bcol, brgba);
    in->radius = radius; in->border = border; in->kind = 0;
}
// Push a clip rect, intersected with the current one; pop restores it.
void mdk_clip_push(float x, float y, float w, float h) {
    float a = x, b = y, c = x + w, d = y + h;
    if (g_nclip > 0) {
        float* t = g_clip[g_nclip - 1];
        if (a < t[0]) a = t[0]; if (b < t[1]) b = t[1];
        if (c > t[2]) c = t[2]; if (d > t[3]) d = t[3];
    }
    if (g_nclip < CLIPS) { g_clip[g_nclip][0] = a; g_clip[g_nclip][1] = b; g_clip[g_nclip][2] = c; g_clip[g_nclip][3] = d; }
    g_nclip++;
}
void mdk_clip_pop(void) { if (g_nclip > 0) g_nclip--; }

void mdk_shadow(float x, float y, float w, float h, int64_t rgba, float radius, float blur) {
    Inst* in = push_inst();
    in->x = x - blur; in->y = y - blur; in->w = w + 2 * blur; in->h = h + 2 * blur;
    set_rgba(in->col, rgba);
    in->radius = radius; in->blur = blur; in->kind = 2;
}

static int g_ww = 480, g_wh = 360;
static float g_mx = -1, g_my = -1, g_wheel;
static int g_mdown, g_mpressed, g_mreleased, g_anim;
static int g_keys[512];
static double g_t0;

float mdk_win_w(void) { return (float)g_ww; }
float mdk_win_h(void) { return (float)g_wh; }
float mdk_mouse_x(void) { return g_mx; }
float mdk_mouse_y(void) { return g_my; }
float mdk_wheel(void) { return g_wheel; }
static float g_px = -1, g_py = -1;
float mdk_press_x(void) { return g_px; }
float mdk_press_y(void) { return g_py; }
int64_t mdk_mouse_down(void) { return g_mdown; }
int64_t mdk_mouse_pressed(void) { return g_mpressed; }
int64_t mdk_mouse_released(void) { return g_mreleased; }
int64_t mdk_key_pressed(int64_t sc) { return (sc >= 0 && sc < 512) ? g_keys[sc] : 0; }
double mdk_time(void) { return (double)SDL_GetTicksNS() / 1e9 - g_t0; }
// Wake the frame loop from any thread. A release build sleeps in
// SDL_WaitEventTimeout(-1) while idle and g_anim alone cannot interrupt a
// wait already in progress; pushing a user event makes it return so one
// more frame renders (any event marks the frame dirty). SDL_PushEvent is
// thread-safe; the event type is allocated once at startup.
static Uint32 g_wake_ev;
void mdk_wake(void) {
    if (!g_wake_ev) return;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = (SDL_EventType)g_wake_ev;
    SDL_PushEvent(&e);
}
void mdk_animate(void) { g_anim = 1; mdk_wake(); }

// Text typed this frame (UTF-8, IME-composed by SDL), modifier state,
// clipboard, the pointer's shape, and the window's title/size.
static char g_typed[1024];
static size_t g_ntyped;
const char* mdk_typed(void) { return g_typed; }
int64_t mdk_mods(void) {  // 1 ctrl (or cmd), 2 shift, 4 alt
    SDL_Keymod m = SDL_GetModState();
    return ((m & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) ? 1 : 0) | ((m & SDL_KMOD_SHIFT) ? 2 : 0) | ((m & SDL_KMOD_ALT) ? 4 : 0);
}
static char* g_clipbuf;
const char* mdk_clipboard(void) {
    SDL_free(g_clipbuf);
    g_clipbuf = SDL_GetClipboardText();
    return str_at(mdk_str(g_clipbuf ? g_clipbuf : ""));
}
void mdk_set_clipboard(const char* s) { SDL_SetClipboardText(s); }
static int g_cursor_want, g_cursor_have = -1;
void mdk_cursor(int64_t kind) { g_cursor_want = (int)kind; }  // 0 default, 1 text, 2 pointer
static SDL_Window* g_win;
static char g_title[256];
void mdk_set_title(const char* t) {
    if (strncmp(g_title, t, sizeof g_title - 1)) { snprintf(g_title, sizeof g_title, "%s", t); SDL_SetWindowTitle(g_win, g_title); }
}
static int64_t g_frame_no;
int64_t mdk_frame_no(void) { return g_frame_no; }

// ------------------------------------------------------- element buffers
// The MFL layout engine keeps its element tree in host-owned raw memory
// (fixed-stride records, peek/poke from MFL): no per-frame MFL allocation, and
// last frame's laid-out rects — what hit-testing reads — survive reloads.
// Two buffers swap every frame; calloc'd pages stay untouched until used.
#define EL_STRIDE 256
#define EL_CAP 65536
static uint8_t *g_el_cur, *g_el_prev;
static int64_t g_el_n, g_el_prev_n;
int64_t mdk_el_buf(void) { return (int64_t)(intptr_t)g_el_cur; }
int64_t mdk_prev_buf(void) { return (int64_t)(intptr_t)g_el_prev; }
int64_t mdk_prev_n(void) { return g_el_prev_n; }
int64_t mdk_el_cap(void) { return EL_CAP; }
void mdk_set_n(int64_t n) { g_el_n = n; }
static void el_swap(void) {
    uint8_t* t = g_el_prev; g_el_prev = g_el_cur; g_el_cur = t;
    g_el_prev_n = g_el_n; g_el_n = 0;
}

// ------------------------------------------------------------ state store
// Keyed values owned by the host: they survive every reload of the app .so.
typedef struct { char* k; int kind; int64_t i; double f; char* s; } Slot;
#define SCAP 4096
static Slot g_state[SCAP];
static Slot* slot(const char* k) {
    uint32_t h = 2166136261u;
    for (const char* p = k; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (int n = 0; n < SCAP; n++) {
        Slot* s = &g_state[(h + (uint32_t)n) & (SCAP - 1)];
        if (!s->k) { s->k = strdup(k); return s; }
        if (!strcmp(s->k, k)) return s;
    }
    return &g_state[0];
}
int64_t mdk_get_int(const char* k, int64_t d) { Slot* s = slot(k); if (!s->kind) { s->kind = 1; s->i = d; } return s->i; }
void mdk_set_int(const char* k, int64_t v) { Slot* s = slot(k); s->kind = 1; s->i = v; }
double mdk_get_f(const char* k, double d) { Slot* s = slot(k); if (!s->kind) { s->kind = 2; s->f = d; } return s->f; }
void mdk_set_f(const char* k, double v) { Slot* s = slot(k); s->kind = 2; s->f = v; }
// Strings returned to MFL are per-frame copies (freed with the frame's
// string pool, as the frame's arena frees everything MFL derived from them):
// the slot can be overwritten mid-frame without leaving MFL a dangling
// pointer. set_str copies before freeing, so set_str(k, state_str(k)) is safe.
const char* mdk_get_str(const char* k, const char* d) {
    Slot* s = slot(k);
    if (!s->kind) { s->kind = 3; s->s = strdup(d); }
    return str_at(mdk_str(s->s ? s->s : ""));
}
void mdk_set_str(const char* k, const char* v) {
    Slot* s = slot(k);
    char* nv = strdup(v ? v : "");
    s->kind = 3; free(s->s); s->s = nv;
}

// ------------------------------------------------------------------- GL
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_rect; layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col; layout(location=3) in vec4 a_bcol;\n"
    "layout(location=4) in vec4 a_par; layout(location=5) in vec4 a_clip;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_local; out vec2 v_size; out vec2 v_uv; out vec4 v_col; out vec4 v_bcol; out vec4 v_par;\n"
    "out vec2 v_pos; out vec4 v_clip;\n"
    "void main(){\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = a_rect.xy + c * a_rect.zw;\n"
    "  v_local = c * a_rect.zw; v_size = a_rect.zw; v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col; v_bcol = a_bcol; v_par = a_par; v_pos = p; v_clip = a_clip;\n"
    "  gl_Position = vec4(p / u_screen * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 v_local; in vec2 v_size; in vec2 v_uv; in vec4 v_col; in vec4 v_bcol; in vec4 v_par;\n"
    "in vec2 v_pos; in vec4 v_clip;\n"
    "uniform sampler2D u_atlas; out vec4 o;\n"
    "float sdf(vec2 p, vec2 hs, float r){ vec2 q = abs(p) - hs + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "void main(){\n"
    "  if (v_pos.x < v_clip.x || v_pos.y < v_clip.y || v_pos.x >= v_clip.z || v_pos.y >= v_clip.w) discard;\n"
    "  float kind = v_par.z;\n"
    "  if (kind > 0.5 && kind < 1.5) { o = vec4(v_col.rgb, v_col.a * texture(u_atlas, v_uv).r); return; }\n"
    "  vec2 hs = v_size * 0.5; vec2 p = v_local - hs;\n"
    "  if (kind > 1.5) { float b = max(v_par.w, 0.5);\n"
    "    float d = sdf(p, hs - b, min(v_par.x, min(hs.x, hs.y) - b));\n"
    "    o = vec4(v_col.rgb, v_col.a * (1.0 - smoothstep(-b, b, d))); return; }\n"
    "  float r = min(v_par.x, min(hs.x, hs.y));\n"
    "  float d = sdf(p, hs, r);\n"
    "  float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "  vec4 c = v_col;\n"
    "  if (v_par.y > 0.0) { float inner = clamp(0.5 - (d + v_par.y), 0.0, 1.0); c = mix(v_bcol, v_col, inner); }\n"
    "  o = vec4(c.rgb, c.a * a);\n"
    "}\n";

static GLuint g_prog, g_vao, g_vbo;
static GLint g_u_screen;

static GLuint compile(GLenum t, const char* src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "mdk: shader: %s\n", log); exit(1); }
    return s;
}

static void gl_init(void) {
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g_prog);
    g_u_screen = glGetUniformLocation(g_prog, "u_screen");
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    GLsizei st = sizeof(Inst);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof(Inst, x));
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof(Inst, u0));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)offsetof(Inst, col));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)offsetof(Inst, bcol));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof(Inst, radius));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof(Inst, cx0));
    for (int a = 0; a < 6; a++) { glEnableVertexAttribArray((GLuint)a); glVertexAttribDivisor((GLuint)a, 1); }
    glGenTextures(1, &g_atlas_tex); glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS, ATLAS, 0, GL_RED, GL_UNSIGNED_BYTE, g_atlas_px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

static void gl_draw(int fbw, int fbh, uint32_t clear) {
    glViewport(0, 0, fbw, fbh);
    glClearColor((float)(clear >> 24 & 255) / 255.f, (float)(clear >> 16 & 255) / 255.f,
                 (float)(clear >> 8 & 255) / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g_atlas_dirty) {
        glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
        if (g_atlas_y1 > g_atlas_y0)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, g_atlas_y0, ATLAS, g_atlas_y1 - g_atlas_y0, GL_RED, GL_UNSIGNED_BYTE,
                            g_atlas_px + (size_t)g_atlas_y0 * ATLAS);
        g_atlas_dirty = 0; g_atlas_y0 = ATLAS; g_atlas_y1 = 0;
    }
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)g_ww, (float)g_wh);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Inst) * (size_t)g_ninst), g_inst, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, g_ninst);  // the whole frame: one call
}

#ifndef MDK_DEFAULT_TITLE
#define MDK_DEFAULT_TITLE "machin"
#endif

// ------------------------------------------------------------- app module
typedef void (*frame_fn)(void);
static frame_fn g_frame;
static uint32_t g_clear = 0x16161AFFu;
void mdk_clear(int64_t rgba) { g_clear = (uint32_t)rgba; }

#ifdef MDK_STATIC
void mfl_mdk_frame_0(void);
static int load_app(const char* p) { (void)p; g_frame = mfl_mdk_frame_0; return 1; }
static int app_changed(const char* p) { (void)p; return 0; }
#else
static time_t g_app_mtime;
static long g_app_mtime_ns;
static int g_gen;
static int load_app(const char* path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "/tmp/mdk-%d-%d.so", (int)getpid(), g_gen++);
    FILE *in = fopen(path, "rb"), *out = fopen(tmp, "wb");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return 0; }
    char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    void* h = dlopen(tmp, RTLD_NOW | RTLD_LOCAL);
    unlink(tmp);
    if (!h) { fprintf(stderr, "mdk: dlopen: %s\n", dlerror()); return 0; }
    frame_fn f = (frame_fn)dlsym(h, "mfl_mdk_frame_0");
    if (!f) { fprintf(stderr, "mdk: no mdk_frame export in %s\n", path); return 0; }
    g_frame = f;  // old module stays mapped on purpose
    return 1;
}
static int app_changed(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (st.st_mtim.tv_sec == g_app_mtime && st.st_mtim.tv_nsec == g_app_mtime_ns) return 0;
    g_app_mtime = st.st_mtim.tv_sec; g_app_mtime_ns = st.st_mtim.tv_nsec;
    return 1;
}
#endif

// Build error overlay: <app>.err, written by `mdk dev` when machin rejects an
// edit. Drawn over the last good frame — never a blank window.
static char g_err[4096];
static void read_err(const char* app) {
    char p[512]; snprintf(p, sizeof p, "%s.err", app);
    FILE* f = fopen(p, "rb");
    if (!f) { g_err[0] = 0; return; }
    size_t n = fread(g_err, 1, sizeof g_err - 1, f);
    g_err[n] = 0;
    fclose(f);
}
static void draw_err(void) {
    if (!g_err[0]) return;
    float pad = 12, y = (float)g_wh - 16;
    int lines = 1;
    for (char* p = g_err; *p; p++) if (*p == '\n' && p[1]) lines++;
    if (lines > 8) lines = 8;
    float lh = mdk_line_h(14, 0);
    float bh = lh * (float)lines + 2 * pad + 22;
    mdk_rect(8, y - bh, (float)g_ww - 16, bh, 0x2A1215F0, 8, 1, 0xE5484DFF);
    float ty = y - bh + pad;
    mdk_text(mdk_str("build failed — showing last good frame"), 20, ty, 14, 0xFF8A8FFF, 1, 0);
    ty += 22;
    char* save = NULL;
    char tmp[4096]; strncpy(tmp, g_err, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    int k = 0;
    for (char* line = strtok_r(tmp, "\n", &save); line && k < lines; line = strtok_r(NULL, "\n", &save), k++) {
        mdk_text(mdk_str(line), 20, ty, 14, 0xF2F2F5FF, 0, (float)g_ww - 40);
        ty += lh;
    }
}

// --------------------------------------------------------- screenshot (PPM)
static void shot(const char* path, int fbw, int fbh) {
    uint8_t* px = malloc((size_t)fbw * (size_t)fbh * 4);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(path, "wb");
    if (!f) { free(px); return; }
    fprintf(f, "P6\n%d %d\n255\n", fbw, fbh);
    for (int y = fbh - 1; y >= 0; y--)
        for (int x = 0; x < fbw; x++) fwrite(px + ((size_t)y * (size_t)fbw + (size_t)x) * 4, 1, 3, f);
    fclose(f);
    free(px);
}

// Test hooks: scripted input through the real SDL event path, in order —
//   --click X Y      press + release (two frames)
//   --type "text"    an SDL text-input event
//   --key NAME       a key press, e.g. Return, Backspace, Left, ctrl+s, shift+Home
//   --wheel DY       a wheel event at the pointer
// SIGUSR1 saves the next frame to $MDK_SHOT (default /tmp/mdk-shot.ppm).
#include <signal.h>
static volatile sig_atomic_t g_want_shot;
static void on_usr1(int s) { (void)s; g_want_shot = 1; }
typedef struct { int kind; float x, y; char str[256]; } Action;  // 0 click, 1 type, 2 key, 3 wheel
static Action g_acts[256];
static int g_nacts, g_act_i, g_click_phase;
static SDL_Keymod g_fake_mod;
static int g_fake_mod_frame, g_frames_now;
static void feed_actions(void) {
    if (g_act_i >= g_nacts) return;
    Action* a = &g_acts[g_act_i];
    SDL_Event e; memset(&e, 0, sizeof e);
    if (a->kind == 0) {
        e.type = SDL_EVENT_MOUSE_MOTION; e.motion.x = a->x; e.motion.y = a->y; SDL_PushEvent(&e);
        memset(&e, 0, sizeof e);
        e.type = g_click_phase ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = 1; e.button.x = a->x; e.button.y = a->y; e.button.down = !g_click_phase;
        SDL_PushEvent(&e);
        if (!g_click_phase) { g_click_phase = 1; return; }
        g_click_phase = 0;
    } else if (a->kind == 1) {
        e.type = SDL_EVENT_TEXT_INPUT; e.text.text = SDL_strdup(a->str);  // leaked: test-only
        SDL_PushEvent(&e);
    } else if (a->kind == 2) {
        char name[256]; snprintf(name, sizeof name, "%s", a->str);
        SDL_Keymod mod = 0; char* k = name;
        for (char* plus; (plus = strchr(k, '+')); k = plus + 1) {
            *plus = 0;
            if (!SDL_strcasecmp(k, "ctrl")) mod |= SDL_KMOD_LCTRL;
            else if (!SDL_strcasecmp(k, "shift")) mod |= SDL_KMOD_LSHIFT;
            else if (!SDL_strcasecmp(k, "alt")) mod |= SDL_KMOD_LALT;
        }
        SDL_Scancode sc = SDL_GetScancodeFromName(k);
        if (sc == SDL_SCANCODE_UNKNOWN && strlen(k) == 1) sc = SDL_GetScancodeFromKey((SDL_Keycode)(unsigned char)SDL_tolower(k[0]), NULL);
        g_fake_mod = mod; g_fake_mod_frame = g_frames_now;
        SDL_SetModState(mod);
        e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = sc; e.key.down = true; e.key.mod = mod;
        SDL_PushEvent(&e);
    } else if (a->kind == 3) {
        e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.y = a->x; SDL_PushEvent(&e);
    }
    g_act_i++;
}

// Benchmark recorder, the protocol of MyGo's benchmarks/: with MDK_BENCH=1
// the app animates every frame; after BENCH_WARMUP frames it says "start",
// and after BENCH_FRAMES more "done {frames,wallUs,p50Us,p99Us,maxUs}",
// timing the intervals between frame starts. Then it stops animating.
static int rec_on, rec_warm = 120, rec_n = 600, rec_seen;
static double rec_last, rec_t0, *rec_iv;
static void rec_init(void) {
    const char* b = getenv("MDK_BENCH");
    if (!b || strcmp(b, "1")) return;
    rec_on = 1;
    if (getenv("BENCH_WARMUP")) rec_warm = atoi(getenv("BENCH_WARMUP"));
    if (getenv("BENCH_FRAMES")) rec_n = atoi(getenv("BENCH_FRAMES"));
    rec_iv = calloc((size_t)rec_n + 1, sizeof(double));
}
static int cmp_d(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return (x > y) - (x < y); }
static void rec_tick(void) {
    double now = (double)SDL_GetTicksNS() / 1e3;  // us
    rec_seen++;
    if (rec_seen == rec_warm) { printf("start\n"); fflush(stdout); rec_t0 = now; }
    else if (rec_seen > rec_warm && rec_seen <= rec_warm + rec_n) {
        int k = rec_seen - rec_warm - 1;
        rec_iv[k] = now - rec_last;
        if (k == rec_n - 1) {
            qsort(rec_iv, (size_t)rec_n, sizeof(double), cmp_d);
            printf("done {\"frames\":%d,\"wallUs\":%.0f,\"p50Us\":%.0f,\"p99Us\":%.0f,\"maxUs\":%.0f}\n",
                   rec_n, now - rec_t0, rec_iv[rec_n / 2], rec_iv[rec_n * 99 / 100], rec_iv[rec_n - 1]);
            fflush(stdout);
        }
    }
    rec_last = now;
    if (rec_seen <= rec_warm + rec_n) g_anim = 1;
}

static const char* find_bold(const char* reg) {
    const char* env = getenv("MDK_FONT_BOLD");
    if (env && *env) return env;
    static char b[1024];
    const char* pats[][2] = {{"-R.ttf", "-B.ttf"}, {"-Regular.ttf", "-Bold.ttf"}, {".ttf", "-Bold.ttf"}};
    for (int k = 0; k < 3; k++) {
        size_t n = strlen(reg), m = strlen(pats[k][0]);
        if (n > m && !strcmp(reg + n - m, pats[k][0]) && n - m + strlen(pats[k][1]) < sizeof b) {
            memcpy(b, reg, n - m); strcpy(b + n - m, pats[k][1]);
            if (access(b, R_OK) == 0) return b;
        }
    }
    return reg;
}

static const char* find_font(void) {
    const char* env = getenv("MDK_FONT");
    if (env && *env) return env;
    // Known paths first: spawning fc-match costs ~35 ms of startup.
    static const char* known[] = {
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "C:\\Windows\\Fonts\\segoeui.ttf",
    };
    const size_t nk = sizeof known / sizeof *known;
    const char* first = NULL;
    for (size_t k = 0; k < nk; k++)
        if (access(known[k], R_OK) == 0) { first = known[k]; break; }
    /* Coverage probe: the first candidate wins only if its cmap has the
       punctuation real documents use (⇒ — …). Ubuntu's font lacks them and
       every such char renders as a tofu rectangle; DejaVu covers them. If no
       candidate covers the set, fall back to `first` (today's behaviour). */
    FT_Library probe = NULL;
    if (FT_Init_FreeType(&probe) == 0) {
        for (size_t k = 0; k < nk; k++) {
            if (access(known[k], R_OK) != 0) continue;
            FT_Face tf = NULL;
            if (FT_New_Face(probe, known[k], 0, &tf)) continue;
            int ok = FT_Get_Char_Index(tf, 0x21D2) && FT_Get_Char_Index(tf, 0x2014) && FT_Get_Char_Index(tf, 0x2026);
            FT_Done_Face(tf);
            if (ok) { FT_Done_FreeType(probe); return known[k]; }
        }
        FT_Done_FreeType(probe);
    }
    if (first) return first;
    static char path[1024];
    FILE* p = popen("fc-match -f '%{file}' sans-serif 2>/dev/null", "r");
    if (p) { size_t n = fread(path, 1, sizeof path - 1, p); path[n] = 0; pclose(p); if (n) return path; }
    return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
}

int main(int argc, char** argv) {
    const char* app = NULL; const char* title = getenv("MDK_TITLE") ? getenv("MDK_TITLE") : MDK_DEFAULT_TITLE;
    if (getenv("MDK_SIZE")) sscanf(getenv("MDK_SIZE"), "%dx%d", &g_ww, &g_wh);
#ifdef MDK_DEFAULT_W
    else { g_ww = MDK_DEFAULT_W; g_wh = MDK_DEFAULT_H; }
#endif
    const char* shot_path = NULL; int shot_frames = 3, stats = 0, bench_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { g_ww = atoi(argv[++i]); g_wh = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shot_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--click") && i + 2 < argc && g_nacts < 256) {
            g_acts[g_nacts].kind = 0; g_acts[g_nacts].x = (float)atof(argv[++i]); g_acts[g_nacts].y = (float)atof(argv[++i]); g_nacts++;
        } else if ((!strcmp(argv[i], "--type") || !strcmp(argv[i], "--key")) && i + 1 < argc && g_nacts < 256) {
            g_acts[g_nacts].kind = argv[i][2] == 't' ? 1 : 2;
            snprintf(g_acts[g_nacts].str, sizeof g_acts[g_nacts].str, "%s", argv[i + 1]); i++; g_nacts++;
        } else if (!strcmp(argv[i], "--wheel") && i + 1 < argc && g_nacts < 256) {
            g_acts[g_nacts].kind = 3; g_acts[g_nacts].x = (float)atof(argv[++i]); g_nacts++;
        }
        else app = argv[i];
    }
#ifdef MDK_STATIC
    if (app) setenv("MDK_SCENE", app, 1);  // release: first positional arg selects a scene
#else
    if (!app) { fprintf(stderr, "usage: mdk-host <app.so> [--title T] [--size W H] [--shot out.ppm --frames N] [--bench N] [--stats]\n"); return 2; }
#endif
    double t_start = (double)SDL_GetTicksNS();
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "mdk: SDL_Init: %s\n", SDL_GetError()); return 1; }
    g_wake_ev = SDL_RegisterEvents(1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow(title, g_ww, g_wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { fprintf(stderr, "mdk: window: %s\n", SDL_GetError()); return 1; }
    g_win = win;
    SDL_StartTextInput(win);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "mdk: gl: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    if (stats) fprintf(stderr, "mdk: t window+gl %.1f ms  renderer=%s\n", ((double)SDL_GetTicksNS() - t_start) / 1e6, (const char*)glGetString(GL_RENDERER));
    g_dpr = SDL_GetWindowDisplayScale(win);
    if (g_dpr <= 0) g_dpr = 1;

    const char* fp = find_font();
    if (FT_Init_FreeType(&g_ft) || FT_New_Face(g_ft, fp, 0, &g_faces[0])) { fprintf(stderr, "mdk: font load failed: %s\n", fp); return 1; }
    if (FT_New_Face(g_ft, find_bold(fp), 0, &g_faces[1])) FT_New_Face(g_ft, fp, 0, &g_faces[1]);
    for (int f = 0; f < 2; f++) { FT_Set_Pixel_Sizes(g_faces[f], 0, 16); g_hbs[f] = hb_ft_font_create_referenced(g_faces[f]); }
    g_hbbuf = hb_buffer_create();
    g_atlas_px = calloc(ATLAS, ATLAS);
    g_el_cur = calloc(EL_CAP, EL_STRIDE);
    g_el_prev = calloc(EL_CAP, EL_STRIDE);
    if (stats) fprintf(stderr, "mdk: t font %.1f ms\n", ((double)SDL_GetTicksNS() - t_start) / 1e6);
    gl_init();
    g_t0 = (double)SDL_GetTicksNS() / 1e9;

    if (!load_app(app)) return 1;
#ifndef MDK_STATIC
    app_changed(app);
    read_err(app);
#endif

    signal(SIGUSR1, on_usr1);
    rec_init();
    if (shot_path && g_nacts) shot_frames += 2 * g_nacts + 3;
    int debug_events = getenv("MDK_DEBUG_EVENTS") != NULL;
    int frames = 0, dirty = 1, running = 1;
    double reload_check = 0, cpu_acc = 0;
    int cpu_n = 0;
    while (running) {
        SDL_Event e;
        if (g_fake_mod && frames > g_fake_mod_frame) { g_fake_mod = 0; SDL_SetModState(0); }  // a scripted chord lasts one frame
        if (frames >= 1) feed_actions();
        if (g_want_shot) dirty = 1;
        // Idle: a release build sleeps until an event (no wake-ups at all);
        // dev wakes 5x a second to look for a rebuilt module.
#ifdef MDK_STATIC
        int idle_ms = -1;
#else
        int idle_ms = 200;
#endif
        int timeout = (dirty || g_anim || shot_path || bench_frames || g_act_i < g_nacts) ? 0 : idle_ms;
        int anim = g_anim || shot_path != NULL;  /* a screenshot run renders every frame */
        g_anim = 0;
        int got = timeout ? SDL_WaitEventTimeout(&e, timeout) : SDL_PollEvent(&e);  /* -1 = wait forever */
        while (got) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_MOUSE_MOTION: g_mx = e.motion.x; g_my = e.motion.y; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: if (e.button.button == 1) { g_mdown = 1; g_mpressed = 1; g_mx = e.button.x; g_my = e.button.y; g_px = g_mx; g_py = g_my; } break;
            case SDL_EVENT_MOUSE_BUTTON_UP: if (e.button.button == 1) { g_mdown = 0; g_mreleased = 1; } break;
            case SDL_EVENT_MOUSE_WHEEL: g_wheel += e.wheel.y; break;
            case SDL_EVENT_TEXT_INPUT: {
                size_t n = strlen(e.text.text);
                if (g_ntyped + n < sizeof g_typed) { memcpy(g_typed + g_ntyped, e.text.text, n); g_ntyped += n; g_typed[g_ntyped] = 0; }
                break;
            }
            case SDL_EVENT_KEY_DOWN: if (e.key.scancode < 512) g_keys[e.key.scancode] = 1;
                break;
            case SDL_EVENT_WINDOW_RESIZED: break;
            }
            if (debug_events) fprintf(stderr, "mdk: event 0x%x\n", e.type);  // MDK_DEBUG_EVENTS=1
            dirty = 1;
            got = SDL_PollEvent(&e);
        }
#ifndef MDK_STATIC
        double now = mdk_time();
        if (now - reload_check > 0.2) {
            reload_check = now;
            if (app_changed(app)) {
                double t0 = (double)SDL_GetTicksNS();
                if (load_app(app)) fprintf(stderr, "mdk: reloaded %s in %.1f ms (state kept)\n", app, ((double)SDL_GetTicksNS() - t0) / 1e6);
                dirty = 1;
            }
            char before = g_err[0];
            read_err(app);
            if (before != g_err[0]) dirty = 1;
        }
#endif
        if (!dirty && !anim) continue;
        dirty = 0;
        int lw, lh, fbw, fbh;
        SDL_GetWindowSize(win, &lw, &lh);
        SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
        g_ww = lw; g_wh = lh;
        float ndpr = (float)fbw / (float)(lw > 0 ? lw : 1);
        if (ndpr != g_dpr && ndpr > 0) { g_dpr = ndpr; g_cur_px[0] = g_cur_px[1] = -1; }
        g_wh_px_hint = fbh;

        double c0 = (double)SDL_GetTicksNS();
        g_ninst = 0;
        strs_reset();
        el_swap();
        g_frame();
        draw_err();
        gl_draw(fbw, fbh, g_clear);
        double c1 = (double)SDL_GetTicksNS();
        cpu_acc += (c1 - c0) / 1e6; cpu_n++;
        if (frames == 0 && stats) fprintf(stderr, "mdk: first frame %.1f ms after start, %d instances\n", (c1 - t_start) / 1e6, g_ninst);
        g_mpressed = g_mreleased = 0; g_wheel = 0;
        memset(g_keys, 0, sizeof g_keys);
        g_ntyped = 0; g_typed[0] = 0;
        if (g_cursor_want != g_cursor_have) {
            static SDL_Cursor* cur[3];
            SDL_SystemCursor ids[3] = {SDL_SYSTEM_CURSOR_DEFAULT, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_POINTER};
            int k = g_cursor_want >= 0 && g_cursor_want < 3 ? g_cursor_want : 0;
            if (!cur[k]) cur[k] = SDL_CreateSystemCursor(ids[k]);
            SDL_SetCursor(cur[k]);
            g_cursor_have = g_cursor_want;
        }
        g_cursor_want = 0;
        frames++;
        g_frame_no++;
        g_frames_now = frames;
        if (g_atlas_full) { g_atlas_full = 0; atlas_reset(); dirty = 1; }
        if (shot_path && frames >= shot_frames) { glFinish(); shot(shot_path, fbw, fbh); break; }
        if (g_want_shot) {
            g_want_shot = 0; glFinish();
            const char* sp = getenv("MDK_SHOT");
            shot(sp && *sp ? sp : "/tmp/mdk-shot.ppm", fbw, fbh);
        }
        SDL_GL_SwapWindow(win);
        if (frames == 1) { printf("ready\n"); fflush(stdout); }
        if (rec_on) rec_tick();
        if (bench_frames) { g_anim = 1; if (frames >= bench_frames) break; }
        if (stats && cpu_n == 120) { fprintf(stderr, "mdk: frame cpu %.2f ms avg, %d instances\n", cpu_acc / cpu_n, g_ninst); cpu_acc = 0; cpu_n = 0; }
    }
    if (bench_frames && cpu_n) fprintf(stderr, "mdk: bench %d frames, cpu %.2f ms/frame, %d instances\n", frames, cpu_acc / cpu_n, g_ninst);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
