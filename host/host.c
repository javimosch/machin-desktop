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
} Inst;

static Inst* g_inst;
static int g_ninst, g_capinst;

static Inst* push_inst(void) {
    if (g_ninst == g_capinst) {
        g_capinst = g_capinst ? g_capinst * 2 : 4096;
        g_inst = realloc(g_inst, sizeof(Inst) * (size_t)g_capinst);
    }
    Inst* i = &g_inst[g_ninst++];
    memset(i, 0, sizeof *i);
    return i;
}

static void set_rgba(uint8_t* d, int64_t c) {
    d[0] = (uint8_t)(c >> 24); d[1] = (uint8_t)(c >> 16);
    d[2] = (uint8_t)(c >> 8);  d[3] = (uint8_t)c;
}

// ------------------------------------------------------------------- text
#define ATLAS 2048
typedef struct { uint32_t gid; int px; int used; float u0, v0, u1, v1; int w, h, bl, bt; } Glyph;
#define GCAP 8192
static Glyph g_glyphs[GCAP];
static uint8_t* g_atlas_px;
static int g_ax, g_ay, g_arow, g_atlas_dirty;
static GLuint g_atlas_tex;
static FT_Library g_ft;
static FT_Face g_face;
static hb_font_t* g_hb;
static int g_cur_px = -1;
static float g_dpr = 1.0f;
static hb_buffer_t* g_hbbuf;

static void font_px(int px) {
    if (px == g_cur_px) return;
    FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px);
    hb_ft_font_changed(g_hb);
    g_cur_px = px;
}

static Glyph* glyph(uint32_t gid, int px) {
    uint32_t h = (gid * 2654435761u) ^ ((uint32_t)px * 40503u);
    for (int n = 0; n < GCAP; n++) {
        Glyph* g = &g_glyphs[(h + (uint32_t)n) & (GCAP - 1)];
        if (g->used && g->gid == gid && g->px == px) return g;
        if (g->used) continue;
        font_px(px);
        if (FT_Load_Glyph(g_face, gid, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return NULL;
        FT_Bitmap* bm = &g_face->glyph->bitmap;
        int w = (int)bm->width, hh = (int)bm->rows;
        if (g_ax + w + 1 > ATLAS) { g_ax = 0; g_ay += g_arow + 1; g_arow = 0; }
        if (g_ay + hh + 1 > ATLAS) return NULL;  // atlas full: drop glyph (M1)
        for (int r = 0; r < hh; r++)
            memcpy(g_atlas_px + (size_t)(g_ay + r) * ATLAS + g_ax, bm->buffer + r * bm->pitch, (size_t)w);
        g->used = 1; g->gid = gid; g->px = px; g->w = w; g->h = hh;
        g->bl = g_face->glyph->bitmap_left; g->bt = g_face->glyph->bitmap_top;
        g->u0 = (float)g_ax / ATLAS; g->v0 = (float)g_ay / ATLAS;
        g->u1 = (float)(g_ax + w) / ATLAS; g->v1 = (float)(g_ay + hh) / ATLAS;
        g_ax += w + 1; if (hh > g_arow) g_arow = hh;
        g_atlas_dirty = 1;
        return g;
    }
    return NULL;
}

// Shape s at size (logical px). Returns advance width in logical px; when
// paint is set, emits one glyph instance per shaped glyph at (x, y_top).
static float shape(const char* s, float size, int paint, float x, float y, int64_t rgba) {
    int px = (int)(size * g_dpr + 0.5f);
    if (px < 1) px = 1;
    font_px(px);
    hb_buffer_clear_contents(g_hbbuf);
    hb_buffer_add_utf8(g_hbbuf, s, -1, 0, -1);
    hb_buffer_guess_segment_properties(g_hbbuf);
    hb_shape(g_hb, g_hbbuf, NULL, 0);
    unsigned n;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(g_hbbuf, &n);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(g_hbbuf, &n);
    float pen = 0, asc = (float)g_face->size->metrics.ascender / 64.0f / g_dpr;
    for (unsigned i = 0; i < n; i++) {
        if (paint) {
            Glyph* g = glyph(info[i].codepoint, px);
            font_px(px);
            if (g && g->w > 0) {
                Inst* in = push_inst();
                in->kind = 1;
                in->x = x + pen + ((float)pos[i].x_offset / 64.0f + (float)g->bl) / g_dpr;
                in->y = y + asc - ((float)pos[i].y_offset / 64.0f + (float)g->bt) / g_dpr;
                in->w = (float)g->w / g_dpr; in->h = (float)g->h / g_dpr;
                in->u0 = g->u0; in->v0 = g->v0; in->u1 = g->u1; in->v1 = g->v1;
                set_rgba(in->col, rgba);
            }
        }
        pen += (float)pos[i].x_advance / 64.0f / g_dpr;
    }
    return pen;
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
float mdk_text_w(int64_t h, float size) { return shape(str_at(h), size, 0, 0, 0, 0); }
float mdk_line_h(float size) {
    font_px((int)(size * g_dpr + 0.5f));
    return (float)g_face->size->metrics.height / 64.0f / g_dpr;
}
void mdk_text(int64_t h, float x, float y, float size, int64_t rgba) { shape(str_at(h), size, 1, x, y, rgba); }

void mdk_rect(float x, float y, float w, float h, int64_t rgba, float radius, float border, int64_t brgba) {
    Inst* in = push_inst();
    in->x = x; in->y = y; in->w = w; in->h = h;
    set_rgba(in->col, rgba); set_rgba(in->bcol, brgba);
    in->radius = radius; in->border = border; in->kind = 0;
}
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
void mdk_animate(void) { g_anim = 1; }

// ------------------------------------------------------- element buffers
// The MFL layout engine keeps its element tree in host-owned raw memory
// (fixed-stride records, peek/poke from MFL): no per-frame MFL allocation, and
// last frame's laid-out rects — what hit-testing reads — survive reloads.
// Two buffers swap every frame; calloc'd pages stay untouched until used.
#define EL_STRIDE 128
#define EL_CAP 131072
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
const char* mdk_get_str(const char* k, const char* d) {
    Slot* s = slot(k);
    if (!s->kind) { s->kind = 3; s->s = strdup(d); }
    return s->s ? s->s : "";
}
void mdk_set_str(const char* k, const char* v) { Slot* s = slot(k); s->kind = 3; free(s->s); s->s = strdup(v); }

// ------------------------------------------------------------------- GL
static const char* VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_rect; layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col; layout(location=3) in vec4 a_bcol;\n"
    "layout(location=4) in vec4 a_par;\n"
    "uniform vec2 u_screen;\n"
    "out vec2 v_local; out vec2 v_size; out vec2 v_uv; out vec4 v_col; out vec4 v_bcol; out vec4 v_par;\n"
    "void main(){\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = a_rect.xy + c * a_rect.zw;\n"
    "  v_local = c * a_rect.zw; v_size = a_rect.zw; v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col; v_bcol = a_bcol; v_par = a_par;\n"
    "  gl_Position = vec4(p / u_screen * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n";
static const char* FS =
    "#version 330 core\n"
    "in vec2 v_local; in vec2 v_size; in vec2 v_uv; in vec4 v_col; in vec4 v_bcol; in vec4 v_par;\n"
    "uniform sampler2D u_atlas; out vec4 o;\n"
    "float sdf(vec2 p, vec2 hs, float r){ vec2 q = abs(p) - hs + r;\n"
    "  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r; }\n"
    "void main(){\n"
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
    for (int a = 0; a < 5; a++) { glEnableVertexAttribArray((GLuint)a); glVertexAttribDivisor((GLuint)a, 1); }
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
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS, ATLAS, GL_RED, GL_UNSIGNED_BYTE, g_atlas_px);
        g_atlas_dirty = 0;
    }
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)g_ww, (float)g_wh);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Inst) * (size_t)g_ninst), g_inst, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, g_ninst);  // the whole frame: one call
}

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
    float lh = mdk_line_h(14);
    float bh = lh * (float)lines + 2 * pad + 22;
    mdk_rect(8, y - bh, (float)g_ww - 16, bh, 0x2A1215F0, 8, 1, 0xE5484DFF);
    float ty = y - bh + pad;
    mdk_text(mdk_str("build failed — showing last good frame"), 20, ty, 14, 0xFF8A8FFF);
    ty += 22;
    char* save = NULL;
    char tmp[4096]; strncpy(tmp, g_err, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
    int k = 0;
    for (char* line = strtok_r(tmp, "\n", &save); line && k < lines; line = strtok_r(NULL, "\n", &save), k++) {
        mdk_text(mdk_str(line), 20, ty, 14, 0xF2F2F5FF);
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

// Test hooks: --click X Y queues real SDL mouse events (one click per two
// frames, through the normal input path); SIGUSR1 saves the next frame to
// $MDK_SHOT (default /tmp/mdk-shot.ppm) without stopping the app.
#include <signal.h>
static volatile sig_atomic_t g_want_shot;
static void on_usr1(int s) { (void)s; g_want_shot = 1; }
static float g_clicks[64][2];
static int g_nclicks, g_click_i, g_click_phase;
static void feed_clicks(void) {
    if (g_click_i >= g_nclicks) return;
    SDL_Event e; memset(&e, 0, sizeof e);
    float x = g_clicks[g_click_i][0], y = g_clicks[g_click_i][1];
    e.type = SDL_EVENT_MOUSE_MOTION; e.motion.x = x; e.motion.y = y; SDL_PushEvent(&e);
    memset(&e, 0, sizeof e);
    e.type = g_click_phase ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = 1; e.button.x = x; e.button.y = y; e.button.down = !g_click_phase;
    SDL_PushEvent(&e);
    if (g_click_phase) g_click_i++;
    g_click_phase = !g_click_phase;
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
    for (size_t k = 0; k < sizeof known / sizeof *known; k++)
        if (access(known[k], R_OK) == 0) return known[k];
    static char path[1024];
    FILE* p = popen("fc-match -f '%{file}' sans-serif 2>/dev/null", "r");
    if (p) { size_t n = fread(path, 1, sizeof path - 1, p); path[n] = 0; pclose(p); if (n) return path; }
    return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
}

int main(int argc, char** argv) {
    const char* app = NULL; const char* title = "machin";
    const char* shot_path = NULL; int shot_frames = 3, stats = 0, bench_frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--title") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { g_ww = atoi(argv[++i]); g_wh = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) shot_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--click") && i + 2 < argc && g_nclicks < 64) {
            g_clicks[g_nclicks][0] = (float)atof(argv[++i]); g_clicks[g_nclicks][1] = (float)atof(argv[++i]); g_nclicks++;
        }
        else app = argv[i];
    }
#ifndef MDK_STATIC
    if (!app) { fprintf(stderr, "usage: mdk-host <app.so> [--title T] [--size W H] [--shot out.ppm --frames N] [--bench N] [--stats]\n"); return 2; }
#endif
    double t_start = (double)SDL_GetTicksNS();
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "mdk: SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow(title, g_ww, g_wh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) { fprintf(stderr, "mdk: window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "mdk: gl: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    if (stats) fprintf(stderr, "mdk: t window+gl %.1f ms  renderer=%s\n", ((double)SDL_GetTicksNS() - t_start) / 1e6, (const char*)glGetString(GL_RENDERER));
    g_dpr = SDL_GetWindowDisplayScale(win);
    if (g_dpr <= 0) g_dpr = 1;

    if (FT_Init_FreeType(&g_ft) || FT_New_Face(g_ft, find_font(), 0, &g_face)) { fprintf(stderr, "mdk: font load failed\n"); return 1; }
    FT_Set_Pixel_Sizes(g_face, 0, 16);
    g_hb = hb_ft_font_create_referenced(g_face);
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
    if (shot_path && g_nclicks) shot_frames += 2 * g_nclicks + 2;
    int frames = 0, dirty = 1, running = 1;
    double reload_check = 0, cpu_acc = 0;
    int cpu_n = 0;
    while (running) {
        SDL_Event e;
        if (frames >= 1) feed_clicks();
        if (g_want_shot) dirty = 1;
        int timeout = (dirty || g_anim || shot_path || bench_frames || g_click_i < g_nclicks) ? 0 : 250;
        g_anim = 0;
        int got = timeout ? SDL_WaitEventTimeout(&e, timeout) : SDL_PollEvent(&e);
        while (got) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_MOUSE_MOTION: g_mx = e.motion.x; g_my = e.motion.y; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN: if (e.button.button == 1) { g_mdown = 1; g_mpressed = 1; g_mx = e.button.x; g_my = e.button.y; g_px = g_mx; g_py = g_my; } break;
            case SDL_EVENT_MOUSE_BUTTON_UP: if (e.button.button == 1) { g_mdown = 0; g_mreleased = 1; } break;
            case SDL_EVENT_MOUSE_WHEEL: g_wheel += e.wheel.y; break;
            case SDL_EVENT_KEY_DOWN: if (e.key.scancode < 512) g_keys[e.key.scancode] = 1;
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) running = 0; break;
            case SDL_EVENT_WINDOW_RESIZED: break;
            }
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
        if (!dirty && !g_anim) continue;
        dirty = 0;
        int lw, lh, fbw, fbh;
        SDL_GetWindowSize(win, &lw, &lh);
        SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
        g_ww = lw; g_wh = lh;
        float ndpr = (float)fbw / (float)(lw > 0 ? lw : 1);
        if (ndpr != g_dpr && ndpr > 0) { g_dpr = ndpr; g_cur_px = -1; }

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
        frames++;
        if (shot_path && frames >= shot_frames) { glFinish(); shot(shot_path, fbw, fbh); break; }
        if (g_want_shot) {
            g_want_shot = 0; glFinish();
            const char* sp = getenv("MDK_SHOT");
            shot(sp && *sp ? sp : "/tmp/mdk-shot.ppm", fbw, fbh);
        }
        SDL_GL_SwapWindow(win);
        if (bench_frames) { g_anim = 1; if (frames >= bench_frames) break; }
        if (stats && cpu_n == 120) { fprintf(stderr, "mdk: frame cpu %.2f ms avg, %d instances\n", cpu_acc / cpu_n, g_ninst); cpu_acc = 0; cpu_n = 0; }
    }
    if (bench_frames && cpu_n) fprintf(stderr, "mdk: bench %d frames, cpu %.2f ms/frame, %d instances\n", frames, cpu_acc / cpu_n, g_ninst);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
