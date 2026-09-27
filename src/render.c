#include "common.h"

/* Everything is drawn with SDL_RenderGeometry from one batched vertex buffer.
   Lighting is per-vertex: each tile corner gets an RGB light value
   (baked torches + dynamic lights), interpolated across the triangles. */

#define VCAP 32768
#define ICAP 65536
#define MAX_GLOWS 1024
#define MAX_DRAWABLES (MAP_W * MAP_H + MAX_ENTITIES + MAX_PARTICLES + MAX_TORCHES)
#define Z_SCALE 36.0f   /* one world unit of height in px at zoom 1 */
#define LIGHT_GAIN 1.9f
#define ENT_SCALE 1.2f   /* sprite size relative to tiles */
#define DASH_COOLDOWN_UI 0.7f

typedef struct { V2 p; float r; SDL_FColor c; } Glow;
typedef enum { DK_WALL, DK_TORCH, DK_ENT, DK_PART } DrawKind;
typedef struct { float depth; uint16_t kind, a, b; } Drawable;

static SDL_Renderer *R;
static View V;
static SDL_Vertex verts[VCAP];
static int indices[ICAP];
static int nv, ni;
static SDL_BlendMode cur_blend;
static Glow glows[MAX_GLOWS];
static int nglow;
static Drawable drawables[MAX_DRAWABLES];
static float light[MAP_H + 1][MAP_W + 1][3];

/* ---- batching primitives ------------------------------------------------- */

static void flush(void)
{
    if (ni > 0) SDL_RenderGeometry(R, NULL, verts, nv, indices, ni);
    nv = ni = 0;
}

static void set_blend(SDL_BlendMode m)
{
    if (m == cur_blend) return;
    flush();
    SDL_SetRenderDrawBlendMode(R, m);
    cur_blend = m;
}

static void reserve(int v, int i)
{
    if (nv + v > VCAP || ni + i > ICAP) flush();
}

static int vtx(V2 p, SDL_FColor c)
{
    verts[nv].position = (SDL_FPoint){ p.x, p.y };
    verts[nv].color = c;
    verts[nv].tex_coord = (SDL_FPoint){ 0, 0 };
    return nv++;
}

static SDL_FColor rgba(float r, float g, float b, float a) { return (SDL_FColor){ r, g, b, a }; }

static void quad4(V2 a, V2 b, V2 c, V2 d, SDL_FColor ca, SDL_FColor cb, SDL_FColor cc, SDL_FColor cd)
{
    reserve(4, 6);
    int i0 = vtx(a, ca), i1 = vtx(b, cb), i2 = vtx(c, cc), i3 = vtx(d, cd);
    int *ix = &indices[ni];
    ix[0] = i0; ix[1] = i1; ix[2] = i2;
    ix[3] = i0; ix[4] = i2; ix[5] = i3;
    ni += 6;
}

static void quad(V2 a, V2 b, V2 c, V2 d, SDL_FColor col) { quad4(a, b, c, d, col, col, col, col); }

static void tri(V2 a, V2 b, V2 c, SDL_FColor col)
{
    reserve(3, 3);
    indices[ni++] = vtx(a, col);
    indices[ni++] = vtx(b, col);
    indices[ni++] = vtx(c, col);
}

static void rect(float x, float y, float w, float h, SDL_FColor c)
{
    quad(v2(x, y), v2(x + w, y), v2(x + w, y + h), v2(x, y + h), c);
}

static void ellipse2(V2 c, float rx, float ry, SDL_FColor inner, SDL_FColor outer, int seg)
{
    reserve(seg + 1, seg * 3);
    int center = vtx(c, inner);
    int first = nv;
    for (int i = 0; i < seg; i++) {
        float a = (float)i / seg * SDL_PI_F * 2.0f;
        vtx(v2(c.x + SDL_cosf(a) * rx, c.y + SDL_sinf(a) * ry), outer);
    }
    for (int i = 0; i < seg; i++) {
        indices[ni++] = center;
        indices[ni++] = first + i;
        indices[ni++] = first + (i + 1) % seg;
    }
}

static void ellipse(V2 c, float rx, float ry, SDL_FColor col) { ellipse2(c, rx, ry, col, col, 20); }

static void ring(V2 c, float r0, float r1, SDL_FColor col, int seg)
{
    for (int i = 0; i < seg; i++) {
        float a0 = (float)i / seg * SDL_PI_F * 2.0f, a1 = (float)(i + 1) / seg * SDL_PI_F * 2.0f;
        V2 d0 = v2(SDL_cosf(a0), SDL_sinf(a0)), d1 = v2(SDL_cosf(a1), SDL_sinf(a1));
        quad(v2_add(c, v2_scale(d0, r0)), v2_add(c, v2_scale(d0, r1)), v2_add(c, v2_scale(d1, r1)),
             v2_add(c, v2_scale(d1, r0)), col);
    }
}

static void pie(V2 c, float r, float frac, SDL_FColor col, int seg)
{
    int n = (int)(seg * frac + 0.999f);
    for (int i = 0; i < n; i++) {
        float a0 = -SDL_PI_F / 2 + SDL_PI_F * 2.0f * frac * i / n;
        float a1 = -SDL_PI_F / 2 + SDL_PI_F * 2.0f * frac * (i + 1) / n;
        tri(c, v2(c.x + SDL_cosf(a0) * r, c.y + SDL_sinf(a0) * r), v2(c.x + SDL_cosf(a1) * r, c.y + SDL_sinf(a1) * r), col);
    }
}

/* Thick line segment. */
static void line(V2 a, V2 b, float w, SDL_FColor col)
{
    V2 d = v2_norm(v2_sub(b, a));
    V2 n = v2_scale(v2(-d.y, d.x), w * 0.5f);
    quad(v2_add(a, n), v2_add(b, n), v2_sub(b, n), v2_sub(a, n), col);
}

static void glow(V2 p, float r, SDL_FColor c)
{
    if (nglow < MAX_GLOWS) glows[nglow++] = (Glow){ p, r, c };
}

static int text_scale(float s) { return SDL_max(1, (int)SDL_roundf(s)); }

/* align: 0 left, 1 center, 2 right */
static void text(float x, float y, int scale, SDL_FColor c, const char *s, int align)
{
    flush();
    float w = (float)SDL_strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    if (align == 1) x -= w * 0.5f;
    else if (align == 2) x -= w;
    SDL_SetRenderScale(R, (float)scale, (float)scale);
    SDL_SetRenderDrawColorFloat(R, 0, 0, 0, c.a * 0.75f);
    SDL_RenderDebugText(R, x / scale + 1, y / scale + 1, s);
    SDL_SetRenderDrawColorFloat(R, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(R, x / scale, y / scale, s);
    SDL_SetRenderScale(R, 1, 1);
}

/* Largest integer text scale <= want that fits max_w pixels. */
static int fit_scale(const char *s, float want, float max_w)
{
    int sc = text_scale(want);
    float len = (float)SDL_strlen(s) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    while (sc > 1 && len * sc > max_w) sc--;
    return sc;
}

/* ---- projection ----------------------------------------------------------- */

View render_make_view(SDL_Renderer *r, const Game *g)
{
    View v;
    int w = 1, h = 1;
    SDL_GetCurrentRenderOutputSize(r, &w, &h);
    v.r = r;
    v.w = (float)w;
    v.h = (float)h;
    float base = SDL_min(v.w / (TILE_W * 12.0f), v.h / (TILE_H * 14.0f));
    if (v.h > v.w) base = SDL_min(v.w / (TILE_W * 8.0f), v.h / (TILE_H * 14.0f)); /* portrait: fill more of the tall screen */
    v.zoom = base * g->zoom_user;
    v.ui = SDL_max(1.0f, SDL_min(v.w, v.h) / 420.0f);
    float sx = SDL_sinf(g->time * 83.0f) * g->shake * 12.0f * v.zoom;
    float sy = SDL_cosf(g->time * 97.0f) * g->shake * 12.0f * v.zoom;
    v.origin.x = v.w * 0.5f - (g->cam.x - g->cam.y) * TILE_W * 0.5f * v.zoom + sx;
    v.origin.y = v.h * 0.5f - (g->cam.x + g->cam.y) * TILE_H * 0.5f * v.zoom + sy + 16.0f * v.zoom;
    return v;
}

V2 render_world_to_screen(const View *v, float wx, float wy, float wz)
{
    return v2((wx - wy) * TILE_W * 0.5f * v->zoom + v->origin.x,
              (wx + wy) * TILE_H * 0.5f * v->zoom - wz * v->zoom + v->origin.y);
}

V2 render_screen_to_world(const View *v, V2 s)
{
    float a = (s.x - v->origin.x) / (TILE_W * 0.5f * v->zoom);
    float b = (s.y - v->origin.y) / (TILE_H * 0.5f * v->zoom);
    return v2((a + b) * 0.5f, (b - a) * 0.5f);
}

static V2 w2s(float x, float y, float zpx) { return render_world_to_screen(&V, x, y, zpx); }

/* World direction -> normalized screen direction. */
static V2 screen_dir(V2 w) { return v2_norm(v2((w.x - w.y) * TILE_W * 0.5f, (w.x + w.y) * TILE_H * 0.5f)); }

/* ---- lighting --------------------------------------------------------------- */

static void add_light(V2 p, float radius, float r, float g, float b)
{
    int x0 = SDL_max(0, (int)(p.x - radius)), x1 = SDL_min(MAP_W, (int)(p.x + radius) + 1);
    int y0 = SDL_max(0, (int)(p.y - radius)), y1 = SDL_min(MAP_H, (int)(p.y + radius) + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float d = v2_dist(v2((float)x, (float)y), p);
            if (d >= radius) continue;
            float k = 1.0f - d / radius;
            k *= k;
            light[y][x][0] += k * r;
            light[y][x][1] += k * g;
            light[y][x][2] += k * b;
        }
    }
}

static void compute_lights(const Game *g)
{
    const Map *m = &g->map;
    float t = g->time;
    float flicker = 0.88f + 0.08f * SDL_sinf(t * 9.1f) + 0.04f * SDL_sinf(t * 23.7f);
    for (int y = 0; y <= MAP_H; y++) {
        for (int x = 0; x <= MAP_W; x++) {
            light[y][x][0] = 0.07f + m->static_light[y][x][0] * flicker;
            light[y][x][1] = 0.07f + m->static_light[y][x][1] * flicker;
            light[y][x][2] = 0.11f + m->static_light[y][x][2] * flicker;
        }
    }
    const Entity *p = &g->ents[0];
    float pf = 0.96f + 0.04f * SDL_sinf(t * 13.0f);
    add_light(p->pos, 7.5f * pf, 1.0f, 0.9f, 0.75f);
    add_light(v2(m->stairs_x + 0.5f, m->stairs_y + 0.5f), 3.2f, 0.25f, 0.45f, 1.0f);
    for (int i = 1; i < g->ent_count; i++) {
        const Entity *e = &g->ents[i];
        if (e->type == E_POTION) add_light(e->pos, 1.8f, 0.7f, 0.15f, 0.2f);
        else if (e->type == E_ARROW) add_light(e->pos, 1.5f, 0.6f, 0.35f, 0.1f);
        else if (e->type == E_SKELETON && e->timer > 0) add_light(e->pos, 2.0f, 0.8f, 0.1f, 0.1f);
    }
}

static const float *corner(int x, int y) { return light[SDL_clamp(y, 0, MAP_H)][SDL_clamp(x, 0, MAP_W)]; }

static void light_at(V2 p, float out[3])
{
    int x = (int)SDL_floorf(p.x), y = (int)SDL_floorf(p.y);
    float fx = p.x - x, fy = p.y - y;
    const float *a = corner(x, y), *b = corner(x + 1, y), *c = corner(x, y + 1), *d = corner(x + 1, y + 1);
    for (int i = 0; i < 3; i++)
        out[i] = lerpf(lerpf(a[i], b[i], fx), lerpf(c[i], d[i], fx), fy);
}

static SDL_FColor lit(SDL_FColor base, const float *l, float k)
{
    return rgba(SDL_min(1.0f, base.r * l[0] * k), SDL_min(1.0f, base.g * l[1] * k), SDL_min(1.0f, base.b * l[2] * k), base.a);
}

/* Entities never go fully dark so they stay readable. */
static SDL_FColor lit_ent(SDL_FColor base, const float l[3])
{
    float m[3] = { SDL_max(l[0], 0.45f), SDL_max(l[1], 0.45f), SDL_max(l[2], 0.5f) };
    return lit(base, m, 1.6f);
}

/* ---- tiles ------------------------------------------------------------------ */

static bool on_screen(V2 s, float margin)
{
    return s.x > -margin && s.x < V.w + margin && s.y > -margin && s.y < V.h + margin * 2;
}

static float wall_height(const Game *g, int tx, int ty)
{
    if (g->state == ST_TITLE) return WALL_H;
    V2 p = g->ents[0].pos;
    float cx = tx + 0.5f, cy = ty + 0.5f;
    float dd = (cx + cy) - (p.x + p.y);
    float sx = (cx - cy) - (p.x - p.y);
    if (dd <= 0.2f || dd > 4.5f) return WALL_H;
    float a = clampf(1.0f - (SDL_fabsf(sx) - 1.0f) / 1.5f, 0, 1);
    float b = clampf((4.5f - dd) / 1.5f, 0, 1);
    return lerpf(WALL_H, WALL_H * 0.2f, a * b);
}

static SDL_FColor floor_base(const Map *m, int tx, int ty)
{
    float c = 0.30f + (m->variant[ty][tx] & 7) * 0.008f;
    if ((tx + ty) & 1) c *= 0.955f;
    return rgba(c + 0.02f, c, c + 0.035f, 1);
}

static void draw_floor(const Game *g, int tx, int ty)
{
    const Map *m = &g->map;
    bool vis = m->visible[ty][tx];
    const float ins = 0.025f;
    V2 n = w2s(tx + ins, ty + ins, 0), e = w2s(tx + 1 - ins, ty + ins, 0);
    V2 s = w2s(tx + 1 - ins, ty + 1 - ins, 0), w = w2s(tx + ins, ty + 1 - ins, 0);
    SDL_FColor base = floor_base(m, tx, ty);

    if (!vis) {
        SDL_FColor mem = rgba(0.075f + base.r * 0.08f, 0.08f + base.g * 0.08f, 0.12f + base.b * 0.1f, 1);
        quad(n, e, s, w, mem);
    } else {
        quad4(n, e, s, w, lit(base, corner(tx, ty), LIGHT_GAIN), lit(base, corner(tx + 1, ty), LIGHT_GAIN),
              lit(base, corner(tx + 1, ty + 1), LIGHT_GAIN), lit(base, corner(tx, ty + 1), LIGHT_GAIN));
        uint8_t v = m->variant[ty][tx];
        if (m->tile[ty][tx] == T_FLOOR && v % 7 == 0) { /* pebbles */
            float l[3];
            light_at(v2(tx + 0.5f, ty + 0.5f), l);
            SDL_FColor pc = lit(rgba(0.2f, 0.19f, 0.22f, 1), l, LIGHT_GAIN);
            V2 c = w2s(tx + 0.3f + (v >> 5) * 0.05f, ty + 0.35f + ((v >> 3) & 3) * 0.1f, 0);
            ellipse(c, 3.2f * V.zoom, 1.7f * V.zoom, pc);
            ellipse(v2(c.x + 5 * V.zoom, c.y + 2 * V.zoom), 2.0f * V.zoom, 1.1f * V.zoom, pc);
        } else if (m->tile[ty][tx] == T_FLOOR && v % 11 == 1) { /* crack */
            float l[3];
            light_at(v2(tx + 0.5f, ty + 0.5f), l);
            SDL_FColor cc = lit(rgba(0.18f, 0.17f, 0.2f, 1), l, LIGHT_GAIN);
            V2 a = w2s(tx + 0.2f, ty + 0.5f, 0), b = w2s(tx + 0.5f, ty + 0.45f, 0), c = w2s(tx + 0.7f, ty + 0.75f, 0);
            line(a, b, 1.2f * V.zoom, cc);
            line(b, c, 1.2f * V.zoom, cc);
        }
    }

    if (m->tile[ty][tx] == T_STAIRS) {
        /* stairwell: nested diamonds stepping down */
        for (int k = 0; k < 4; k++) {
            float i = 0.12f + k * 0.07f;
            float dz = -(float)k * 5.0f;
            float sh = vis ? 0.22f - k * 0.05f : 0.06f;
            SDL_FColor c = rgba(sh * 0.8f, sh * 0.9f, sh * 1.3f, 1);
            quad(w2s(tx + i, ty + i, dz), w2s(tx + 1 - i, ty + i, dz), w2s(tx + 1 - i, ty + 1 - i, dz), w2s(tx + i, ty + 1 - i, dz), c);
        }
        if (vis || m->seen[ty][tx]) {
            float pulse = 0.5f + 0.5f * SDL_sinf(g->time * 3.0f);
            glow(w2s(tx + 0.5f, ty + 0.5f, 4), 40 * V.zoom, rgba(0.25f, 0.45f, 1.0f, 0.35f + 0.25f * pulse));
        }
    }
}

static void draw_wall(const Game *g, int tx, int ty)
{
    const Map *m = &g->map;
    bool vis = m->visible[ty][tx];
    float h = wall_height(g, tx, ty);

    V2 nb = w2s((float)tx, (float)ty, 0), eb = w2s(tx + 1.0f, (float)ty, 0);
    V2 sb = w2s(tx + 1.0f, ty + 1.0f, 0), wb = w2s((float)tx, ty + 1.0f, 0);
    V2 nt = v2(nb.x, nb.y - h * V.zoom), et = v2(eb.x, eb.y - h * V.zoom);
    V2 st = v2(sb.x, sb.y - h * V.zoom), wt = v2(wb.x, wb.y - h * V.zoom);

    bool left_face = ty + 1 >= MAP_H || m->tile[ty + 1][tx] != T_WALL || wall_height(g, tx, ty + 1) < h - 0.5f;
    bool right_face = tx + 1 >= MAP_W || m->tile[ty][tx + 1] != T_WALL || wall_height(g, tx + 1, ty) < h - 0.5f;

    SDL_FColor top = rgba(0.40f, 0.38f, 0.44f, 1), lf = rgba(0.34f, 0.31f, 0.37f, 1), rf = rgba(0.25f, 0.23f, 0.29f, 1);
    if (!vis) {
        top = rgba(0.15f, 0.16f, 0.23f, 1);
        lf = rgba(0.10f, 0.11f, 0.17f, 1);
        rf = rgba(0.075f, 0.085f, 0.13f, 1);
    }

    const float *ln = corner(tx, ty), *le = corner(tx + 1, ty), *ls = corner(tx + 1, ty + 1), *lw = corner(tx, ty + 1);
    const float one[3] = { 1, 1, 1 };
    float k = vis ? LIGHT_GAIN : 1.0f;
    if (!vis) ln = le = ls = lw = one;
    const float ao = 0.6f; /* darker at the base */

    /* A face is only lit when it faces walkable space; faces toward the void would
       otherwise pick up torch light through corners shared with the room side. */
    const float amb[3] = { 0.07f, 0.07f, 0.11f };
    bool left_open = ty + 1 < MAP_H && !map_solid(m, tx, ty + 1);
    bool right_open = tx + 1 < MAP_W && !map_solid(m, tx + 1, ty);
    if (left_face) {
        const float *a = (vis && !left_open) ? amb : lw, *b = (vis && !left_open) ? amb : ls;
        quad4(wb, sb, st, wt, lit(lf, a, k * ao), lit(lf, b, k * ao), lit(lf, b, k), lit(lf, a, k));
        if (vis && h > WALL_H * 0.5f) { /* mortar lines */
            SDL_FColor mc = rgba(0, 0, 0, 0.18f);
            for (int i = 1; i <= 2; i++) {
                float z = h * i / 3.0f * V.zoom;
                line(v2(wb.x, wb.y - z), v2(sb.x, sb.y - z), 1.0f * V.zoom, mc);
            }
            V2 mid = v2_lerp(wb, sb, 0.5f);
            line(v2(mid.x, mid.y - h * V.zoom / 3.0f), v2(mid.x, mid.y - h * V.zoom * 2.0f / 3.0f), 1.0f * V.zoom, mc);
        }
    }
    if (right_face) {
        const float *a = (vis && !right_open) ? amb : ls, *b = (vis && !right_open) ? amb : le;
        quad4(sb, eb, et, st, lit(rf, a, k * ao), lit(rf, b, k * ao), lit(rf, b, k), lit(rf, a, k));
        if (vis && h > WALL_H * 0.5f) {
            SDL_FColor mc = rgba(0, 0, 0, 0.18f);
            for (int i = 1; i <= 2; i++) {
                float z = h * i / 3.0f * V.zoom;
                line(v2(sb.x, sb.y - z), v2(eb.x, eb.y - z), 1.0f * V.zoom, mc);
            }
            V2 mid = v2_lerp(sb, eb, 0.5f);
            line(v2(mid.x, mid.y), v2(mid.x, mid.y - h * V.zoom / 3.0f), 1.0f * V.zoom, mc);
        }
    }
    quad4(nt, et, st, wt, lit(top, ln, k), lit(top, le, k), lit(top, ls, k), lit(top, lw, k));
}

static void draw_torch(const Game *g, int idx)
{
    const Torch *t = &g->map.torches[idx];
    if (wall_height(g, t->wx, t->wy) < WALL_H * 0.9f) return;
    bool vis = g->map.visible[t->wy][t->wx];
    V2 base = w2s(t->pos.x, t->pos.y, 20);
    V2 top = w2s(t->pos.x, t->pos.y, 28);
    line(base, top, 2.5f * V.zoom, vis ? rgba(0.35f, 0.22f, 0.1f, 1) : rgba(0.1f, 0.08f, 0.08f, 1));
    float f = 0.8f + 0.2f * SDL_sinf(g->time * 17.0f + t->phase) + 0.1f * SDL_sinf(g->time * 29.0f + t->phase * 2);
    V2 fl = v2(top.x, top.y - 4 * V.zoom * f);
    ellipse(fl, 3.2f * V.zoom * f, 5.5f * V.zoom * f, rgba(1.0f, 0.55f, 0.15f, 0.95f));
    ellipse(v2(fl.x, fl.y + 1.5f * V.zoom), 1.8f * V.zoom, 3.0f * V.zoom * f, rgba(1.0f, 0.92f, 0.55f, 1));
    glow(fl, 30 * V.zoom * f, rgba(1.0f, 0.5f, 0.15f, vis ? 0.45f : 0.25f));
}

/* ---- entities ------------------------------------------------------------ */

static void draw_shadow(const Entity *e)
{
    float s = V.zoom;
    float rx = e->radius * TILE_W * 0.72f * s;
    float fly = 1.0f / (1.0f + e->z * 1.2f);
    ellipse2(w2s(e->pos.x, e->pos.y, 0), rx * fly, rx * 0.5f * fly, rgba(0, 0, 0, 0.45f), rgba(0, 0, 0, 0.1f), 16);
}

static void draw_sword(V2 hand, V2 dir, float s, SDL_FColor blade)
{
    V2 tip = v2_add(hand, v2_scale(dir, 20 * s));
    V2 perp = v2(-dir.y, dir.x);
    line(hand, tip, 2.8f * s, blade);
    line(v2_add(hand, v2_scale(perp, 4.5f * s)), v2_sub(hand, v2_scale(perp, 4.5f * s)), 2.2f * s, rgba(0.55f, 0.4f, 0.2f, 1));
}

static void draw_player(const Game *g, const Entity *p)
{
    float s = V.zoom * ENT_SCALE;
    float l[3];
    light_at(p->pos, l);
    bool blink = g->invuln > 0 && SDL_fmodf(g->time, 0.12f) < 0.06f && g->state == ST_PLAY;
    float speed = v2_len(p->vel);
    float walk = SDL_min(1.0f, speed / 4.0f);
    float bob = SDL_fabsf(SDL_sinf(p->anim * 8.0f)) * 2.0f * s * walk;
    V2 b = w2s(p->pos.x, p->pos.y, 0);
    V2 fs = screen_dir(p->face);
    V2 perp = v2(-fs.y, fs.x);

    /* sword direction: sweeps across the swing arc */
    V2 sword_w = p->face;
    float swing_k = 0;
    if (g->swing_t > 0) {
        swing_k = 1.0f - g->swing_t / 0.22f;
        float ang = lerpf(-1.3f, 1.3f, SDL_min(1.0f, swing_k * 1.6f));
        float c = SDL_cosf(ang), sn = SDL_sinf(ang);
        sword_w = v2(g->swing_dir.x * c - g->swing_dir.y * sn, g->swing_dir.x * sn + g->swing_dir.y * c);
    }
    V2 sword_s = screen_dir(sword_w);
    V2 hand = v2(b.x + fs.x * 7 * s, b.y - 14 * s - bob + fs.y * 4 * s);
    SDL_FColor steel = lit_ent(rgba(0.85f, 0.88f, 0.95f, 1), l);

    /* swing trail: a flat arc on the ground plane */
    if (g->swing_t > 0) {
        float a_end = lerpf(-1.3f, 1.3f, SDL_min(1.0f, swing_k * 1.6f));
        int seg = 10;
        float alpha = 0.55f * (g->swing_t / 0.22f);
        for (int i = 0; i < seg; i++) {
            float a0 = lerpf(-1.3f, a_end, (float)i / seg), a1 = lerpf(-1.3f, a_end, (float)(i + 1) / seg);
            V2 d0 = v2(g->swing_dir.x * SDL_cosf(a0) - g->swing_dir.y * SDL_sinf(a0), g->swing_dir.x * SDL_sinf(a0) + g->swing_dir.y * SDL_cosf(a0));
            V2 d1 = v2(g->swing_dir.x * SDL_cosf(a1) - g->swing_dir.y * SDL_sinf(a1), g->swing_dir.x * SDL_sinf(a1) + g->swing_dir.y * SDL_cosf(a1));
            float t0 = (float)i / seg, t1 = (float)(i + 1) / seg;
            V2 i0 = w2s(p->pos.x + d0.x * 0.35f, p->pos.y + d0.y * 0.35f, 14), o0 = w2s(p->pos.x + d0.x * 1.2f, p->pos.y + d0.y * 1.2f, 14);
            V2 i1 = w2s(p->pos.x + d1.x * 0.35f, p->pos.y + d1.y * 0.35f, 14), o1 = w2s(p->pos.x + d1.x * 1.2f, p->pos.y + d1.y * 1.2f, 14);
            quad4(i0, o0, o1, i1, rgba(1, 1, 1, 0), rgba(0.9f, 0.95f, 1, alpha * t0), rgba(0.9f, 0.95f, 1, alpha * t1), rgba(1, 1, 1, 0));
        }
    }

    if (blink) return;
    bool sword_behind = sword_s.y < -0.2f;
    if (sword_behind) draw_sword(hand, sword_s, s, steel);

    /* feet */
    SDL_FColor boot = lit_ent(rgba(0.25f, 0.18f, 0.14f, 1), l);
    float step = SDL_sinf(p->anim * 8.0f) * 3.0f * s * walk;
    ellipse(v2(b.x + perp.x * 4 * s + fs.x * step, b.y - 2 * s + perp.y * 2 * s + fs.y * step * 0.5f), 3.2f * s, 2.2f * s, boot);
    ellipse(v2(b.x - perp.x * 4 * s - fs.x * step, b.y - 2 * s - perp.y * 2 * s - fs.y * step * 0.5f), 3.2f * s, 2.2f * s, boot);

    SDL_FColor cloak = lit_ent(rgba(0.24f, 0.44f, 0.86f, 1), l);
    SDL_FColor cloak_dark = lit_ent(rgba(0.16f, 0.30f, 0.62f, 1), l);
    if (p->hurt_t > 0) cloak = cloak_dark = rgba(1, 1, 1, 1);
    V2 body = v2(b.x, b.y - 13 * s - bob);
    ellipse2(body, 9 * s, 11 * s, cloak, cloak_dark, 20);
    /* belt */
    ellipse(v2(body.x, body.y + 3 * s), 8.3f * s, 2.2f * s, lit_ent(rgba(0.4f, 0.28f, 0.15f, 1), l));

    V2 head = v2(b.x, b.y - 27 * s - bob);
    ellipse(head, 6.8f * s, 6.8f * s, lit_ent(rgba(0.95f, 0.78f, 0.62f, 1), l));
    /* helmet */
    ellipse(v2(head.x - fs.x * 1.5f * s, head.y - 2.5f * s - fs.y * 1.0f * s), 7.4f * s, 5.2f * s, lit_ent(rgba(0.72f, 0.74f, 0.8f, 1), l));
    if (fs.y > -0.35f) { /* eyes when facing the camera-ish */
        V2 eye = v2(head.x + fs.x * 3.5f * s, head.y + 1.5f * s + fs.y * 1.5f * s);
        SDL_FColor ec = rgba(0.1f, 0.1f, 0.15f, 1);
        ellipse(v2(eye.x + perp.x * 2.4f * s, eye.y + perp.y * 1.2f * s), 1.2f * s, 1.5f * s, ec);
        ellipse(v2(eye.x - perp.x * 2.4f * s, eye.y - perp.y * 1.2f * s), 1.2f * s, 1.5f * s, ec);
    }
    if (!sword_behind) draw_sword(hand, sword_s, s, steel);
}

static void draw_enemy(const Game *g, const Entity *e)
{
    float s = V.zoom * ENT_SCALE;
    float l[3];
    light_at(e->pos, l);
    V2 b = w2s(e->pos.x, e->pos.y, 0);
    const Entity *p = &g->ents[0];
    V2 look = screen_dir(v2_sub(p->pos, e->pos));
    bool flash = e->hurt_t > 0;

    switch (e->type) {
    case E_SLIME: {
        float sq = SDL_sinf(e->anim * 6.0f);
        float rx = 12.5f * s * (1 + 0.13f * sq), ry = 10 * s * (1 - 0.13f * sq);
        bool tough = e->max_hp > 2;
        SDL_FColor c = tough ? rgba(0.72f, 0.35f, 0.9f, 0.92f) : rgba(0.35f, 0.85f, 0.42f, 0.92f);
        SDL_FColor cd = tough ? rgba(0.45f, 0.18f, 0.6f, 0.92f) : rgba(0.18f, 0.55f, 0.25f, 0.92f);
        c = lit_ent(c, l);
        cd = lit_ent(cd, l);
        if (flash) c = cd = rgba(1, 1, 1, 1);
        V2 center = v2(b.x, b.y - ry * 0.9f);
        ellipse2(v2(center.x, center.y - ry * 0.2f), rx, ry, c, cd, 24);
        ellipse(v2(center.x - rx * 0.35f, center.y - ry * 0.6f), rx * 0.25f, ry * 0.18f, rgba(1, 1, 1, 0.5f));
        V2 eye = v2(center.x + look.x * rx * 0.3f, center.y - ry * 0.2f + look.y * ry * 0.2f);
        ellipse(v2(eye.x - 3.5f * s, eye.y), 1.7f * s, 2.3f * s, rgba(0.05f, 0.05f, 0.08f, 1));
        ellipse(v2(eye.x + 3.5f * s, eye.y), 1.7f * s, 2.3f * s, rgba(0.05f, 0.05f, 0.08f, 1));
    } break;

    case E_BAT: {
        V2 c = v2(b.x, b.y - e->z * Z_SCALE * s);
        float flap = SDL_sinf(e->anim * 25.0f);
        SDL_FColor body = flash ? rgba(1, 1, 1, 1) : lit_ent(rgba(0.38f, 0.24f, 0.5f, 1), l);
        SDL_FColor wing = flash ? rgba(1, 1, 1, 1) : lit_ent(rgba(0.28f, 0.16f, 0.38f, 1), l);
        tri(v2(c.x - 3 * s, c.y), v2(c.x - 17 * s, c.y - 7 * s + flap * 8 * s), v2(c.x - 8 * s, c.y + 4 * s), wing);
        tri(v2(c.x + 3 * s, c.y), v2(c.x + 17 * s, c.y - 7 * s + flap * 8 * s), v2(c.x + 8 * s, c.y + 4 * s), wing);
        tri(v2(c.x - 4 * s, c.y - 3 * s), v2(c.x - 3 * s, c.y - 9 * s), v2(c.x - 1 * s, c.y - 4 * s), body);
        tri(v2(c.x + 4 * s, c.y - 3 * s), v2(c.x + 3 * s, c.y - 9 * s), v2(c.x + 1 * s, c.y - 4 * s), body);
        ellipse(c, 6 * s, 5.5f * s, body);
        ellipse(v2(c.x - 2.2f * s, c.y - 1 * s), 1.2f * s, 1.2f * s, rgba(1, 0.2f, 0.2f, 1));
        ellipse(v2(c.x + 2.2f * s, c.y - 1 * s), 1.2f * s, 1.2f * s, rgba(1, 0.2f, 0.2f, 1));
    } break;

    case E_SKELETON: {
        SDL_FColor bone = flash ? rgba(1, 1, 1, 1) : lit_ent(rgba(0.86f, 0.84f, 0.76f, 1), l);
        SDL_FColor dark = rgba(0.12f, 0.1f, 0.1f, 1);
        V2 fs = screen_dir(e->face);
        float walk = SDL_min(1.0f, v2_len(e->vel) / 1.5f);
        float step = SDL_sinf(e->anim * 9.0f) * 3.0f * s * walk;
        line(v2(b.x - 3 * s, b.y - 10 * s), v2(b.x - 3 * s + step, b.y - 1 * s), 2.2f * s, bone);
        line(v2(b.x + 3 * s, b.y - 10 * s), v2(b.x + 3 * s - step, b.y - 1 * s), 2.2f * s, bone);
        V2 chest = v2(b.x, b.y - 17 * s);
        ellipse(chest, 7 * s, 8 * s, bone);
        for (int i = -1; i <= 1; i++)
            line(v2(chest.x - 5 * s, chest.y + i * 3 * s), v2(chest.x + 5 * s, chest.y + i * 3 * s), 1.1f * s, rgba(0.2f, 0.18f, 0.16f, 0.8f));
        V2 skull = v2(b.x, b.y - 30 * s);
        ellipse(skull, 6.5f * s, 6.2f * s, bone);
        bool windup = e->timer > 0;
        SDL_FColor eyec = windup ? rgba(1, 0.2f, 0.15f, 1) : dark;
        ellipse(v2(skull.x - 2.4f * s + fs.x * 1.5f * s, skull.y + 0.5f * s), 1.6f * s, 1.9f * s, eyec);
        ellipse(v2(skull.x + 2.4f * s + fs.x * 1.5f * s, skull.y + 0.5f * s), 1.6f * s, 1.9f * s, eyec);
        if (windup) {
            glow(skull, 18 * s, rgba(1, 0.15f, 0.1f, 0.5f));
        }
        /* bow */
        V2 bc = v2(chest.x + fs.x * 9 * s, chest.y + fs.y * 5 * s);
        V2 perp = v2(-fs.y, fs.x);
        SDL_FColor wood = lit_ent(rgba(0.5f, 0.32f, 0.15f, 1), l);
        V2 prev = v2_add(bc, v2_add(v2_scale(perp, 9 * s), v2_scale(fs, -3 * s)));
        for (int i = 1; i <= 6; i++) {
            float t = (float)i / 6 * 2 - 1;
            V2 pt = v2_add(bc, v2_add(v2_scale(perp, -t * 9 * s), v2_scale(fs, (1 - t * t) * 3 * s - 3 * s)));
            line(prev, pt, 1.8f * s, wood);
            prev = pt;
        }
    } break;

    default: break;
    }
}

static void draw_entity(const Game *g, const Entity *e)
{
    float s = V.zoom * ENT_SCALE;
    float l[3];
    switch (e->type) {
    case E_PLAYER: draw_player(g, e); break;
    case E_SLIME: case E_BAT: case E_SKELETON: draw_enemy(g, e); break;
    case E_ARROW: {
        V2 c = w2s(e->pos.x, e->pos.y, e->z * Z_SCALE);
        V2 d = screen_dir(e->face);
        line(v2_sub(c, v2_scale(d, 8 * s)), v2_add(c, v2_scale(d, 6 * s)), 2 * s, rgba(0.6f, 0.45f, 0.25f, 1));
        tri(v2_add(c, v2_scale(d, 10 * s)), v2_add(c, v2(-d.y * 3 * s + d.x * 5 * s, d.x * 3 * s + d.y * 5 * s)),
            v2_add(c, v2(d.y * 3 * s + d.x * 5 * s, -d.x * 3 * s + d.y * 5 * s)), rgba(0.9f, 0.9f, 0.95f, 1));
        glow(c, 14 * s, rgba(1.0f, 0.6f, 0.2f, 0.35f));
    } break;
    case E_COIN: {
        light_at(e->pos, l);
        float zb = (0.18f + 0.05f * SDL_sinf(e->anim * 3.0f) + e->z) * Z_SCALE;
        V2 c = w2s(e->pos.x, e->pos.y, zb);
        float spin = SDL_fabsf(SDL_cosf(e->anim * 4.0f));
        ellipse(v2(c.x + 0.8f * s, c.y), 5.2f * s * spin + 1.2f * s, 5.2f * s, rgba(0.6f, 0.42f, 0.1f, 1));
        ellipse(c, 5.2f * s * spin + 0.6f * s, 5.2f * s, lit_ent(rgba(1.0f, 0.84f, 0.3f, 1), l));
        glow(c, 16 * s, rgba(1.0f, 0.8f, 0.3f, 0.35f));
    } break;
    case E_POTION: {
        light_at(e->pos, l);
        float zb = (0.2f + 0.06f * SDL_sinf(e->anim * 2.5f) + e->z) * Z_SCALE;
        V2 c = w2s(e->pos.x, e->pos.y, zb);
        rect(c.x - 2 * s, c.y - 12 * s, 4 * s, 6 * s, rgba(0.75f, 0.82f, 0.9f, 0.9f));
        rect(c.x - 2.5f * s, c.y - 14 * s, 5 * s, 2.5f * s, rgba(0.5f, 0.32f, 0.15f, 1));
        ellipse2(c, 6.5f * s, 6.5f * s, rgba(1.0f, 0.3f, 0.35f, 1), rgba(0.65f, 0.05f, 0.1f, 1), 20);
        ellipse(v2(c.x - 2.2f * s, c.y - 2.2f * s), 1.6f * s, 1.6f * s, rgba(1, 1, 1, 0.7f));
        glow(c, 20 * s, rgba(1.0f, 0.2f, 0.25f, 0.4f));
    } break;
    default: break;
    }
}

static void draw_particle(const Particle *p)
{
    V2 c = w2s(p->x, p->y, p->z * Z_SCALE);
    float k = p->life / p->max_life;
    float sz = p->size * TILE_W * 0.5f * V.zoom * (0.4f + 0.6f * k);
    SDL_FColor col = rgba(p->r / 255.0f, p->g / 255.0f, p->b / 255.0f, SDL_min(1.0f, k * 1.5f));
    quad(v2(c.x, c.y - sz), v2(c.x + sz, c.y), v2(c.x, c.y + sz), v2(c.x - sz, c.y), col);
    if (p->glow) glow(c, sz * 3.0f, rgba(col.r, col.g, col.b, 0.5f * k));
}

/* ---- world pass ------------------------------------------------------------- */

static int cmp_drawable(const void *a, const void *b)
{
    float da = ((const Drawable *)a)->depth, db = ((const Drawable *)b)->depth;
    return (da > db) - (da < db);
}

static void draw_world(const Game *g)
{
    const Map *m = &g->map;
    float margin = TILE_W * V.zoom * 1.5f;
    int nd = 0;

    /* floors first, then shadows, then depth-sorted walls/entities/particles */
    for (int ty = 0; ty < MAP_H; ty++) {
        for (int tx = 0; tx < MAP_W; tx++) {
            uint8_t t = m->tile[ty][tx];
            if (t == T_VOID || !m->seen[ty][tx]) continue;
            if (!on_screen(w2s(tx + 0.5f, ty + 0.5f, 0), margin)) continue;
            if (t == T_WALL) drawables[nd++] = (Drawable){ (float)(tx + ty + 1), DK_WALL, (uint16_t)tx, (uint16_t)ty };
            else draw_floor(g, tx, ty);
        }
    }
    for (int i = 0; i < m->torch_count; i++) {
        const Torch *t = &m->torches[i];
        if (m->seen[t->wy][t->wx]) drawables[nd++] = (Drawable){ (float)(t->wx + t->wy + 1) + 0.01f, DK_TORCH, (uint16_t)i, 0 };
    }
    for (int i = 0; i < g->ent_count; i++) {
        const Entity *e = &g->ents[i];
        if (e->type == E_NONE) continue;
        if (i != 0 && !m->visible[(int)e->pos.y][(int)e->pos.x]) continue;
        if (i == 0 && g->state == ST_DEAD) continue;
        if (!on_screen(w2s(e->pos.x, e->pos.y, 0), margin)) continue;
        if (e->type != E_ARROW) draw_shadow(e);
        drawables[nd++] = (Drawable){ e->pos.x + e->pos.y, DK_ENT, (uint16_t)i, 0 };
    }
    for (int i = 0; i < g->part_count; i++) {
        const Particle *p = &g->parts[i];
        drawables[nd++] = (Drawable){ p->x + p->y, DK_PART, (uint16_t)i, 0 };
    }

    SDL_qsort(drawables, nd, sizeof(Drawable), cmp_drawable);
    for (int i = 0; i < nd; i++) {
        const Drawable *d = &drawables[i];
        switch (d->kind) {
        case DK_WALL: draw_wall(g, d->a, d->b); break;
        case DK_TORCH: draw_torch(g, d->a); break;
        case DK_ENT: draw_entity(g, &g->ents[d->a]); break;
        case DK_PART: draw_particle(&g->parts[d->a]); break;
        }
    }

    /* additive glow pass */
    set_blend(SDL_BLENDMODE_ADD);
    for (int i = 0; i < nglow; i++)
        ellipse2(glows[i].p, glows[i].r, glows[i].r * 0.75f, glows[i].c, rgba(glows[i].c.r, glows[i].c.g, glows[i].c.b, 0), 20);
    set_blend(SDL_BLENDMODE_BLEND);
    nglow = 0;
}

/* ---- HUD / overlays --------------------------------------------------------- */

static void vignette(float strength, SDL_FColor tint)
{
    float w = V.w, h = V.h, ix = w * 0.22f, iy = h * 0.22f;
    SDL_FColor o = rgba(tint.r, tint.g, tint.b, strength), i = rgba(tint.r, tint.g, tint.b, 0);
    V2 o0 = v2(0, 0), o1 = v2(w, 0), o2 = v2(w, h), o3 = v2(0, h);
    V2 i0 = v2(ix, iy), i1 = v2(w - ix, iy), i2 = v2(w - ix, h - iy), i3 = v2(ix, h - iy);
    quad4(o0, o1, i1, i0, o, o, i, i);
    quad4(o1, o2, i2, i1, o, o, i, i);
    quad4(o2, o3, i3, i2, o, o, i, i);
    quad4(o3, o0, i0, i3, o, o, i, i);
}

static void draw_floaters(const Game *g)
{
    int sc = text_scale(V.ui * 0.9f);
    for (int i = 0; i < MAX_FLOATERS; i++) {
        const Floater *f = &g->floaters[i];
        if (f->life <= 0) continue;
        V2 p = w2s(f->pos.x, f->pos.y, f->z * Z_SCALE);
        text(p.x, p.y, sc, rgba(f->r / 255.0f, f->g / 255.0f, f->b / 255.0f, SDL_min(1.0f, f->life * 2.5f)), f->text, 1);
    }
}

static void draw_minimap(const Game *g, const Input *in)
{
    if (!g->show_map) return;
    const Map *m = &g->map;
    const Entity *p = &g->ents[0];
    float u = V.ui;
    float M = SDL_min(120 * u, V.w * 0.36f);
    float H = M * 0.55f;
    float top = 10 * u + (in->device == DEV_TOUCH ? 40 * u : 0);
    SDL_FRect panel = { V.w - M - 10 * u, top, M, H };
    V2 pc = v2(panel.x + M * 0.5f, panel.y + H * 0.5f);
    const int RAD = 20;
    float dw = M / (RAD * 1.41421f);

    rect(panel.x, panel.y, panel.w, panel.h, rgba(0.02f, 0.02f, 0.05f, 0.55f));
    flush();
    SDL_Rect clip = { (int)panel.x, (int)panel.y, (int)panel.w, (int)panel.h };
    SDL_SetRenderClipRect(R, &clip);
    int px = (int)p->pos.x, py = (int)p->pos.y;
    for (int ty = py - RAD; ty <= py + RAD; ty++) {
        for (int tx = px - RAD; tx <= px + RAD; tx++) {
            if (tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H || !m->seen[ty][tx]) continue;
            uint8_t t = m->tile[ty][tx];
            if (t == T_VOID) continue;
            SDL_FColor c;
            if (t == T_WALL) c = rgba(0.55f, 0.55f, 0.65f, 0.9f);
            else if (t == T_STAIRS) c = rgba(0.35f, 0.65f, 1.0f, 1);
            else c = m->visible[ty][tx] ? rgba(0.3f, 0.28f, 0.36f, 0.9f) : rgba(0.17f, 0.17f, 0.24f, 0.9f);
            float rx = tx + 0.5f - p->pos.x, ry = ty + 0.5f - p->pos.y;
            V2 c0 = v2(pc.x + (rx - ry) * dw * 0.5f, pc.y + (rx + ry) * dw * 0.25f);
            float hw = dw * 0.5f, hh = dw * 0.25f;
            quad(v2(c0.x, c0.y - hh), v2(c0.x + hw, c0.y), v2(c0.x, c0.y + hh), v2(c0.x - hw, c0.y), c);
        }
    }
    for (int i = 1; i < g->ent_count; i++) {
        const Entity *e = &g->ents[i];
        if (e->type != E_SLIME && e->type != E_BAT && e->type != E_SKELETON) continue;
        if (!m->visible[(int)e->pos.y][(int)e->pos.x]) continue;
        float rx = e->pos.x - p->pos.x, ry = e->pos.y - p->pos.y;
        ellipse(v2(pc.x + (rx - ry) * dw * 0.5f, pc.y + (rx + ry) * dw * 0.25f), 2 * u, 2 * u, rgba(1, 0.25f, 0.25f, 1));
    }
    ellipse(pc, 2.5f * u, 2.5f * u, rgba(1, 1, 1, 1));
    flush();
    SDL_SetRenderClipRect(R, NULL);
}

static void draw_hud(const Game *g, const Input *in)
{
    const Entity *p = &g->ents[0];
    float u = V.ui;
    float x0 = 14 * u, y0 = 16 * u;
    float pulse = p->hp <= 2 ? 0.75f + 0.25f * SDL_sinf(g->time * 8.0f) : 1.0f;
    for (int i = 0; i < p->max_hp; i++) {
        V2 c = v2(x0 + i * 17 * u + 7 * u, y0);
        float r = 7 * u;
        SDL_FColor col = i < p->hp ? rgba(0.92f * pulse, 0.2f, 0.25f, 1) : rgba(0.18f, 0.16f, 0.2f, 0.9f);
        /* heart = two lobes + triangle */
        ellipse(v2(c.x - r * 0.45f, c.y - r * 0.25f), r * 0.55f, r * 0.55f, col);
        ellipse(v2(c.x + r * 0.45f, c.y - r * 0.25f), r * 0.55f, r * 0.55f, col);
        tri(v2(c.x - r * 0.98f, c.y - r * 0.05f), v2(c.x + r * 0.98f, c.y - r * 0.05f), v2(c.x, c.y + r * 0.85f), col);
    }
    char buf[64];
    int sc = text_scale(u * 1.0f);
    SDL_snprintf(buf, sizeof(buf), "FLOOR %d", g->depth);
    text(x0, y0 + 14 * u, sc, rgba(0.85f, 0.85f, 0.95f, 1), buf, 0);
    SDL_snprintf(buf, sizeof(buf), "GOLD  %d", g->gold);
    text(x0, y0 + 14 * u + 11 * sc, sc, rgba(1.0f, 0.82f, 0.3f, 1), buf, 0);

    /* dash cooldown bar */
    if (in->device != DEV_TOUCH) {
        float bw = 60 * u, bh = 4 * u, by = y0 + 14 * u + 24 * sc;
        float k = 1.0f - clampf(g->dash_cd / DASH_COOLDOWN_UI, 0, 1);
        rect(x0, by, bw, bh, rgba(0.1f, 0.1f, 0.15f, 0.8f));
        rect(x0, by, bw * k, bh, k >= 1 ? rgba(0.45f, 0.7f, 1.0f, 1) : rgba(0.3f, 0.4f, 0.6f, 1));
    }

    if (g->show_debug) {
        SDL_snprintf(buf, sizeof(buf), "%.0f FPS  ents %d  parts %d  zoom %.2f", g->fps, g->ent_count, g->part_count, V.zoom);
        text(8 * u, V.h - 14 * u, text_scale(u * 0.8f), rgba(0.7f, 1, 0.7f, 1), buf, 0);
    }
}

static void draw_touch(const Game *g, const Input *in)
{
    if (in->device != DEV_TOUCH) return;
    TouchLayout L = touch_layout(V.w, V.h);
    float u = V.ui;
    int sc = text_scale(u * 0.8f);

    /* pause button is available in every state */
    ring(L.pause, L.pause_r - 2 * u, L.pause_r, rgba(1, 1, 1, 0.35f), 24);
    rect(L.pause.x - 5 * u, L.pause.y - 6 * u, 3.5f * u, 12 * u, rgba(1, 1, 1, 0.5f));
    rect(L.pause.x + 1.5f * u, L.pause.y - 6 * u, 3.5f * u, 12 * u, rgba(1, 1, 1, 0.5f));

    if (g->state != ST_PLAY) return;

    if (in->stick.active) {
        V2 d = v2_sub(in->stick.pos, in->stick.origin);
        if (v2_len(d) > L.stick_r) d = v2_scale(v2_norm(d), L.stick_r);
        ellipse(in->stick.origin, L.stick_r, L.stick_r, rgba(1, 1, 1, 0.08f));
        ring(in->stick.origin, L.stick_r - 2 * u, L.stick_r, rgba(1, 1, 1, 0.3f), 32);
        ellipse(v2_add(in->stick.origin, d), L.stick_r * 0.42f, L.stick_r * 0.42f, rgba(1, 1, 1, 0.4f));
    } else {
        V2 ghost = v2(L.stick_r + 30 * u, V.h - L.stick_r - 40 * u);
        ring(ghost, L.stick_r - 2 * u, L.stick_r, rgba(1, 1, 1, 0.12f), 32);
        text(ghost.x, ghost.y - 4 * sc, sc, rgba(1, 1, 1, 0.3f), "MOVE", 1);
    }

    SDL_FColor ac = in->btn_attack.down ? rgba(0.95f, 0.35f, 0.3f, 0.55f) : rgba(0.85f, 0.25f, 0.22f, 0.35f);
    ellipse(L.attack, L.attack_r, L.attack_r, ac);
    ring(L.attack, L.attack_r - 2.5f * u, L.attack_r, rgba(1, 0.8f, 0.75f, 0.5f), 40);
    /* sword icon */
    V2 sd = v2(0.7071f, -0.7071f);
    draw_sword(v2_sub(L.attack, v2_scale(sd, 12 * u)), sd, u * 1.2f, rgba(1, 1, 1, 0.8f));

    float k = clampf(g->dash_cd / DASH_COOLDOWN_UI, 0, 1);
    SDL_FColor dc = in->btn_dash.down ? rgba(0.35f, 0.55f, 1.0f, 0.55f) : rgba(0.3f, 0.45f, 0.9f, 0.35f);
    ellipse(L.dash, L.dash_r, L.dash_r, dc);
    if (k > 0) pie(L.dash, L.dash_r, k, rgba(0, 0, 0, 0.45f), 32);
    ring(L.dash, L.dash_r - 2 * u, L.dash_r, rgba(0.75f, 0.85f, 1, 0.5f), 32);
    text(L.dash.x, L.dash.y - 4 * sc, sc, rgba(1, 1, 1, 0.85f), "DASH", 1);
}

static void overlay_text_block(const char **lines, int count, float y, float want_scale, SDL_FColor c)
{
    for (int i = 0; i < count; i++) {
        int sc = fit_scale(lines[i], want_scale, V.w * 0.92f);
        text(V.w * 0.5f, y, sc, c, lines[i], 1);
        y += 12 * sc;
    }
}

static void draw_overlays(const Game *g, const Input *in)
{
    float u = V.ui;
    char buf[96];
    bool blink = SDL_fmodf(g->time, 1.0f) < 0.65f;
    const char *prompt = in->device == DEV_TOUCH ? "TAP TO START" : in->device == DEV_GAMEPAD ? "PRESS (A) TO START" : "PRESS ENTER TO START";

    switch (g->state) {
    case ST_TITLE: {
        rect(0, 0, V.w, V.h, rgba(0.02f, 0.02f, 0.05f, 0.45f));
        float y = V.h * 0.22f;
        int big = fit_scale("ISOROGUE", u * 4.5f, V.w * 0.9f);
        text(V.w * 0.5f, y, big, rgba(1.0f, 0.75f, 0.35f, 1), "ISOROGUE", 1);
        y += 12 * big;
        const char *sub[] = { "an SDL3 isometric roguelike tech demo" };
        overlay_text_block(sub, 1, y, u * 1.1f, rgba(0.8f, 0.8f, 0.9f, 1));
        if (blink) {
            int sc = fit_scale(prompt, u * 1.6f, V.w * 0.9f);
            text(V.w * 0.5f, V.h * 0.52f, sc, rgba(1, 1, 1, 1), prompt, 1);
        }
        const char *help[] = {
            "KEYBOARD  WASD move  mouse aim",
            "LMB/SPACE attack  RMB/SHIFT dash",
            "M map  ESC pause  +/- zoom  F11 fullscreen",
            "",
            "GAMEPAD  L-stick move  R-stick aim",
            "A/X/RT attack  B/LB/LT dash  START pause",
            "",
            "TOUCH  left thumb moves",
            "ATTACK auto-aims  DASH dodges",
        };
        overlay_text_block(help, 9, V.h * 0.64f, u * 0.85f, rgba(0.65f, 0.65f, 0.75f, 1));
    } break;

    case ST_PAUSE: {
        rect(0, 0, V.w, V.h, rgba(0.02f, 0.02f, 0.05f, 0.6f));
        int big = fit_scale("PAUSED", u * 3.5f, V.w * 0.9f);
        text(V.w * 0.5f, V.h * 0.35f, big, rgba(1, 1, 1, 1), "PAUSED", 1);
        const char *lines[] = { in->device == DEV_TOUCH ? "tap to resume" : "press ESC / START to resume" };
        overlay_text_block(lines, 1, V.h * 0.35f + 14 * big, u * 1.1f, rgba(0.8f, 0.8f, 0.9f, 1));
    } break;

    case ST_DEAD: {
        float a = clampf(g->state_t / 0.8f, 0, 1);
        rect(0, 0, V.w, V.h, rgba(0.15f, 0.0f, 0.02f, 0.6f * a));
        int big = fit_scale("YOU DIED", u * 4.0f, V.w * 0.9f);
        text(V.w * 0.5f, V.h * 0.3f, big, rgba(1, 0.25f, 0.25f, a), "YOU DIED", 1);
        float y = V.h * 0.3f + 14 * big;
        SDL_snprintf(buf, sizeof(buf), "floor %d   gold %d   kills %d", g->depth, g->gold, g->kills);
        const char *l1[] = { buf };
        overlay_text_block(l1, 1, y, u * 1.2f, rgba(0.9f, 0.85f, 0.85f, a));
        char best[48];
        SDL_snprintf(best, sizeof(best), "best floor %d", g->best_depth);
        const char *l2[] = { best };
        overlay_text_block(l2, 1, y + 16 * text_scale(u * 1.2f), u * 1.0f, rgba(0.7f, 0.7f, 0.8f, a));
        if (g->state_t > 1.0f && blink) {
            const char *r = in->device == DEV_TOUCH ? "TAP TO TRY AGAIN" : in->device == DEV_GAMEPAD ? "PRESS (A) TO TRY AGAIN" : "PRESS ENTER TO TRY AGAIN";
            int sc = fit_scale(r, u * 1.4f, V.w * 0.9f);
            text(V.w * 0.5f, V.h * 0.62f, sc, rgba(1, 1, 1, 1), r, 1);
        }
    } break;

    case ST_DESCEND: {
        float t = g->state_t;
        float a = t < 0.6f ? t / 0.6f : 1.0f - (t - 0.6f) / 0.6f;
        rect(0, 0, V.w, V.h, rgba(0, 0, 0, clampf(a, 0, 1)));
        SDL_snprintf(buf, sizeof(buf), "FLOOR %d", t < 0.6f ? g->depth + 1 : g->depth);
        int sc = fit_scale(buf, u * 3.0f, V.w * 0.9f);
        text(V.w * 0.5f, V.h * 0.45f, sc, rgba(0.8f, 0.85f, 1, clampf(a * 1.5f, 0, 1)), buf, 1);
    } break;

    default: break;
    }
}

void render_frame(SDL_Renderer *r, Game *g, Input *in)
{
    R = r;
    V = render_make_view(r, g);
    cur_blend = SDL_BLENDMODE_BLEND;
    SDL_SetRenderDrawBlendMode(R, cur_blend);
    SDL_SetRenderDrawColor(R, 9, 8, 13, 255);
    SDL_RenderClear(R);

    compute_lights(g);
    draw_world(g);
    draw_floaters(g);

    const Entity *p = &g->ents[0];
    vignette(0.55f, rgba(0, 0, 0, 1));
    if (p->hp <= 2 && g->state == ST_PLAY) vignette(0.25f + 0.15f * SDL_sinf(g->time * 6.0f), rgba(0.6f, 0, 0.05f, 1));
    if (g->flash > 0) rect(0, 0, V.w, V.h, rgba(0.8f, 0.05f, 0.05f, g->flash * 0.5f));

    if (g->state != ST_TITLE) {
        draw_minimap(g, in);
        draw_hud(g, in);
    }
    draw_touch(g, in);
    draw_overlays(g, in);
    flush();
}
