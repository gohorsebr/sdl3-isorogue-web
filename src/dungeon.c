#include "common.h"

#define DIST_INF 0xFFFF

static bool in_bounds(int x, int y) { return x >= 0 && y >= 0 && x < MAP_W && y < MAP_H; }

bool map_solid(const Map *m, int x, int y)
{
    if (!in_bounds(x, y)) return true;
    uint8_t t = m->tile[y][x];
    return t != T_FLOOR && t != T_STAIRS;
}

static void carve(Map *m, int x, int y)
{
    if (x > 0 && y > 0 && x < MAP_W - 1 && y < MAP_H - 1) m->tile[y][x] = T_FLOOR;
}

static V2 room_center(const Room *r) { return v2(r->x + r->w * 0.5f, r->y + r->h * 0.5f); }

static void carve_corridor(Map *m, uint64_t *rng, const Room *a, const Room *b)
{
    int x0 = a->x + a->w / 2, y0 = a->y + a->h / 2;
    int x1 = b->x + b->w / 2, y1 = b->y + b->h / 2;
    bool wide = rng_range(rng, 0, 99) < 30;
    bool horizontal_first = rng_range(rng, 0, 1) == 0;
    int cx = horizontal_first ? x1 : x0, cy = horizontal_first ? y0 : y1;

    for (int x = SDL_min(x0, x1); x <= SDL_max(x0, x1); x++) {
        carve(m, x, cy);
        if (wide) carve(m, x, cy + 1);
    }
    for (int y = SDL_min(y0, y1); y <= SDL_max(y0, y1); y++) {
        carve(m, cx, y);
        if (wide) carve(m, cx + 1, y);
    }
}

void map_bfs(const Map *m, int sx, int sy, uint16_t out[MAP_H][MAP_W])
{
    static int queue[MAP_W * MAP_H];
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) out[y][x] = DIST_INF;
    if (map_solid(m, sx, sy)) return;

    int head = 0, tail = 0;
    out[sy][sx] = 0;
    queue[tail++] = sy * MAP_W + sx;
    static const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
    while (head < tail) {
        int c = queue[head++];
        int x = c % MAP_W, y = c / MAP_W;
        for (int i = 0; i < 4; i++) {
            int nx = x + dx[i], ny = y + dy[i];
            if (map_solid(m, nx, ny) || out[ny][nx] != DIST_INF) continue;
            out[ny][nx] = out[y][x] + 1;
            queue[tail++] = ny * MAP_W + nx;
        }
    }
}

static void bake_torch_light(Map *m)
{
    const float radius = 6.5f;
    for (int t = 0; t < m->torch_count; t++) {
        V2 lp = m->torches[t].pos;
        int x0 = (int)(lp.x - radius), x1 = (int)(lp.x + radius) + 1;
        int y0 = (int)(lp.y - radius), y1 = (int)(lp.y + radius) + 1;
        for (int y = SDL_max(0, y0); y <= SDL_min(MAP_H, y1); y++) {
            for (int x = SDL_max(0, x0); x <= SDL_min(MAP_W, x1); x++) {
                V2 c = v2((float)x, (float)y);
                float d = v2_dist(c, lp);
                if (d >= radius) continue;
                /* A corner is lit if the light reaches it through any of the open tiles touching it. */
                bool reached = false;
                for (int k = 0; k < 4 && !reached; k++) {
                    int ox = (k & 1) ? 0 : -1, oy = (k & 2) ? 0 : -1;
                    if (map_solid(m, x + ox, y + oy)) continue;
                    V2 probe = v2(x + (ox ? -0.05f : 0.05f), y + (oy ? -0.05f : 0.05f));
                    reached = map_los(m, lp, probe);
                }
                if (!reached) continue;
                float k = 1.0f - d / radius;
                k = k * k * 1.1f;
                m->static_light[y][x][0] += k * 1.00f;
                m->static_light[y][x][1] += k * 0.55f;
                m->static_light[y][x][2] += k * 0.22f;
            }
        }
    }
}

static void place_torches(Map *m, uint64_t *rng)
{
    /* Torches hang on walls whose +x or +y side faces a floor tile, so they face the camera. */
    for (int i = 0; i < m->room_count && m->torch_count < MAX_TORCHES; i++) {
        const Room *r = &m->rooms[i];
        int want = rng_range(rng, 1, 2);
        for (int tries = 0; tries < 20 && want > 0 && m->torch_count < MAX_TORCHES; tries++) {
            int wx, wy, fx, fy;
            if (rng_range(rng, 0, 1)) { /* wall above the room (at y-1), floor at +y */
                wx = rng_range(rng, r->x, r->x + r->w - 1);
                wy = r->y - 1;
                fx = wx; fy = wy + 1;
            } else { /* wall left of the room (x-1), floor at +x */
                wx = r->x - 1;
                wy = rng_range(rng, r->y, r->y + r->h - 1);
                fx = wx + 1; fy = wy;
            }
            if (!in_bounds(wx, wy) || m->tile[wy][wx] != T_WALL || m->tile[fy][fx] != T_FLOOR) continue;
            bool dup = false;
            for (int k = 0; k < m->torch_count; k++)
                if (SDL_abs(m->torches[k].wx - wx) + SDL_abs(m->torches[k].wy - wy) < 3) dup = true;
            if (dup) continue;
            Torch *t = &m->torches[m->torch_count++];
            t->wx = wx;
            t->wy = wy;
            t->pos = v2(wx + 0.5f + (fx - wx) * 0.56f, wy + 0.5f + (fy - wy) * 0.56f);
            t->phase = rng_frange(rng, 0, 100);
            want--;
        }
    }
}

void map_generate(Map *m, uint64_t *rng, int depth)
{
    (void)depth;
    SDL_memset(m, 0, sizeof(*m));

    for (int attempt = 0; attempt < 400 && m->room_count < MAX_ROOMS; attempt++) {
        Room r;
        r.w = rng_range(rng, 4, 9);
        r.h = rng_range(rng, 4, 9);
        r.x = rng_range(rng, 2, MAP_W - r.w - 3);
        r.y = rng_range(rng, 2, MAP_H - r.h - 3);
        bool ok = true;
        for (int i = 0; i < m->room_count && ok; i++) {
            const Room *o = &m->rooms[i];
            if (r.x - 2 < o->x + o->w && r.x + r.w + 2 > o->x && r.y - 2 < o->y + o->h && r.y + r.h + 2 > o->y)
                ok = false;
        }
        if (ok) m->rooms[m->room_count++] = r;
    }

    for (int i = 0; i < m->room_count; i++) {
        const Room *r = &m->rooms[i];
        for (int y = r->y; y < r->y + r->h; y++)
            for (int x = r->x; x < r->x + r->w; x++) carve(m, x, y);
    }

    /* Connect rooms with a minimum spanning tree over room centers, plus a few loops. */
    bool connected[MAX_ROOMS] = { true };
    for (int n = 1; n < m->room_count; n++) {
        int best_a = -1, best_b = -1;
        float best = 1e9f;
        for (int a = 0; a < m->room_count; a++) {
            if (!connected[a]) continue;
            for (int b = 0; b < m->room_count; b++) {
                if (connected[b]) continue;
                float d = v2_dist(room_center(&m->rooms[a]), room_center(&m->rooms[b]));
                if (d < best) { best = d; best_a = a; best_b = b; }
            }
        }
        if (best_b < 0) break;
        connected[best_b] = true;
        carve_corridor(m, rng, &m->rooms[best_a], &m->rooms[best_b]);
    }
    for (int extra = 0; extra < 3 && m->room_count > 3; extra++) {
        int a = rng_range(rng, 0, m->room_count - 1), b = rng_range(rng, 0, m->room_count - 1);
        if (a != b) carve_corridor(m, rng, &m->rooms[a], &m->rooms[b]);
    }

    /* Surround floors with walls. */
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            if (m->tile[y][x] != T_VOID) continue;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (in_bounds(x + dx, y + dy) && m->tile[y + dy][x + dx] == T_FLOOR) m->tile[y][x] = T_WALL;
        }
    }

    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) m->variant[y][x] = (uint8_t)rng_next(rng);

    /* Player starts in room 0; stairs go in the room farthest away by walking distance. */
    const Room *start = &m->rooms[0];
    m->start = v2(start->x + start->w / 2 + 0.5f, start->y + start->h / 2 + 0.5f);
    map_bfs(m, (int)m->start.x, (int)m->start.y, m->dist);
    int far_room = m->room_count - 1;
    int far_d = -1;
    for (int i = 1; i < m->room_count; i++) {
        const Room *r = &m->rooms[i];
        int d = m->dist[r->y + r->h / 2][r->x + r->w / 2];
        if (d != DIST_INF && d > far_d) { far_d = d; far_room = i; }
    }
    const Room *fr = &m->rooms[far_room];
    m->stairs_x = fr->x + fr->w / 2;
    m->stairs_y = fr->y + fr->h / 2;
    m->tile[m->stairs_y][m->stairs_x] = T_STAIRS;

    place_torches(m, rng);
    bake_torch_light(m);
}

bool map_los(const Map *m, V2 a, V2 b)
{
    V2 d = v2_sub(b, a);
    float len = v2_len(d);
    int steps = (int)(len / 0.12f) + 1;
    int bx = (int)SDL_floorf(b.x), by = (int)SDL_floorf(b.y);
    for (int i = 1; i < steps; i++) {
        float t = (float)i / (float)steps;
        int x = (int)SDL_floorf(a.x + d.x * t), y = (int)SDL_floorf(a.y + d.y * t);
        if (x == bx && y == by) return true;
        if (map_solid(m, x, y)) return false;
    }
    return true;
}

void map_update_fov(Map *m, V2 eye)
{
    SDL_memset(m->visible, 0, sizeof(m->visible));
    int ex = (int)eye.x, ey = (int)eye.y;
    const int R = FOV_RADIUS;
    for (int ty = ey - R; ty <= ey + R; ty++) {
        for (int tx = ex - R; tx <= ex + R; tx++) {
            if (!in_bounds(tx, ty) || m->tile[ty][tx] == T_VOID) continue;
            int dx = tx - ex, dy = ty - ey;
            if (dx * dx + dy * dy > R * R + R) continue;

            bool vis = false;
            if (map_solid(m, tx, ty)) {
                /* Closest point on the wall box, pulled slightly toward the eye. */
                V2 c = v2(clampf(eye.x, tx + 0.001f, tx + 0.999f), clampf(eye.y, ty + 0.001f, ty + 0.999f));
                c = v2_add(c, v2_scale(v2_norm(v2_sub(eye, c)), 0.03f));
                vis = map_los(m, eye, c);
            } else {
                static const float off[5][2] = { { 0.5f, 0.5f }, { 0.15f, 0.15f }, { 0.85f, 0.15f }, { 0.15f, 0.85f }, { 0.85f, 0.85f } };
                for (int k = 0; k < 5 && !vis; k++) vis = map_los(m, eye, v2(tx + off[k][0], ty + off[k][1]));
            }
            if (vis) {
                m->visible[ty][tx] = 1;
                m->seen[ty][tx] = 1;
            }
        }
    }
}

void map_update_flow(Map *m, int px, int py) { map_bfs(m, px, py, m->dist); }

V2 map_flow_dir(const Map *m, V2 from) { return map_flow_dir_in(m, m->dist, from); }

V2 map_flow_dir_in(const Map *m, const uint16_t dist[MAP_H][MAP_W], V2 from)
{
    int x = (int)from.x, y = (int)from.y;
    if (!in_bounds(x, y)) return v2(0, 0);
    int best = dist[y][x];
    int bx = x, by = y;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (map_solid(m, nx, ny)) continue;
            /* no corner cutting */
            if (dx && dy && (map_solid(m, x + dx, y) || map_solid(m, x, y + dy))) continue;
            if (dist[ny][nx] < best) { best = dist[ny][nx]; bx = nx; by = ny; }
        }
    }
    if (bx == x && by == y) return v2(0, 0);
    return v2_norm(v2_sub(v2(bx + 0.5f, by + 0.5f), from));
}

bool map_collide_circle(const Map *m, V2 *p, float r)
{
    bool hit = false;
    for (int iter = 0; iter < 2; iter++) {
        int x0 = (int)SDL_floorf(p->x - r), x1 = (int)SDL_floorf(p->x + r);
        int y0 = (int)SDL_floorf(p->y - r), y1 = (int)SDL_floorf(p->y + r);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                if (!map_solid(m, x, y)) continue;
                V2 c = v2(clampf(p->x, (float)x, x + 1.0f), clampf(p->y, (float)y, y + 1.0f));
                V2 d = v2_sub(*p, c);
                float dist = v2_len(d);
                if (dist >= r) continue;
                hit = true;
                if (dist > 1e-5f) {
                    *p = v2_add(c, v2_scale(d, r / dist));
                } else {
                    /* center inside the tile: push out along the shallowest axis */
                    float l = p->x - x, rr = x + 1 - p->x, t = p->y - y, b = y + 1 - p->y;
                    float mn = SDL_min(SDL_min(l, rr), SDL_min(t, b));
                    if (mn == l) p->x = x - r;
                    else if (mn == rr) p->x = x + 1 + r;
                    else if (mn == t) p->y = y - r;
                    else p->y = y + 1 + r;
                }
            }
        }
    }
    return hit;
}

V2 map_random_floor_in_room(const Map *m, const Room *r, uint64_t *rng)
{
    for (int i = 0; i < 20; i++) {
        int x = rng_range(rng, r->x, r->x + r->w - 1), y = rng_range(rng, r->y, r->y + r->h - 1);
        if (m->tile[y][x] == T_FLOOR) return v2(x + 0.5f, y + 0.5f);
    }
    return room_center(r);
}
