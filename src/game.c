#include "common.h"

#define PLAYER_SPEED 4.6f
#define DASH_SPEED 15.0f
#define DASH_TIME 0.16f
#define DASH_COOLDOWN 0.7f
#define SWING_TIME 0.22f
#define SWING_ACTIVE 0.12f
#define SWING_COOLDOWN 0.32f
#define SWING_RANGE 1.25f

/* ---- spawning helpers --------------------------------------------------- */

static Entity *spawn(Game *g, EntityType type, V2 pos)
{
    if (g->ent_count >= MAX_ENTITIES) return NULL;
    Entity *e = &g->ents[g->ent_count++];
    SDL_memset(e, 0, sizeof(*e));
    e->type = type;
    e->pos = pos;
    e->face = v2(0.7071f, 0.7071f);
    e->anim = rng_frange(&g->rng, 0, 10);
    int tier = (g->depth - 1) / 3;
    switch (type) {
    case E_PLAYER: e->radius = 0.28f; e->hp = e->max_hp = 6; break;
    case E_SLIME: e->radius = 0.30f; e->hp = e->max_hp = 2 + tier; break;
    case E_BAT: e->radius = 0.22f; e->hp = e->max_hp = 1 + tier / 2; e->z = 0.7f; break;
    case E_SKELETON:
        e->radius = 0.28f;
        e->hp = e->max_hp = 3 + tier;
        e->cooldown = rng_frange(&g->rng, 0.8f, 2.0f);
        break;
    case E_ARROW: e->radius = 0.08f; e->z = 0.45f; e->timer = 3.0f; break;
    case E_COIN: e->radius = 0.18f; break;
    case E_POTION: e->radius = 0.22f; break;
    default: break;
    }
    return e;
}

static void emit(Game *g, float x, float y, float z, int n, uint8_t r, uint8_t gg, uint8_t b,
                 float speed, float life, float size, bool glow)
{
    for (int i = 0; i < n && g->part_count < MAX_PARTICLES; i++) {
        Particle *p = &g->parts[g->part_count++];
        float a = rng_frange(&g->rng, 0, SDL_PI_F * 2);
        float s = speed * rng_frange(&g->rng, 0.3f, 1.0f);
        p->x = x; p->y = y; p->z = z;
        p->vx = SDL_cosf(a) * s;
        p->vy = SDL_sinf(a) * s;
        p->vz = rng_frange(&g->rng, 0.5f, 1.5f) * speed * 0.8f;
        p->life = p->max_life = life * rng_frange(&g->rng, 0.6f, 1.0f);
        p->size = size * rng_frange(&g->rng, 0.6f, 1.2f);
        p->r = r; p->g = gg; p->b = b;
        p->glow = glow;
    }
}

static void floater(Game *g, V2 pos, float z, const char *text, uint8_t r, uint8_t gg, uint8_t b)
{
    int slot = 0;
    float oldest = 1e9f;
    for (int i = 0; i < MAX_FLOATERS; i++) {
        if (g->floaters[i].life <= 0) { slot = i; break; }
        if (g->floaters[i].life < oldest) { oldest = g->floaters[i].life; slot = i; }
    }
    Floater *f = &g->floaters[slot];
    f->pos = v2_add(pos, v2(rng_frange(&g->rng, -0.15f, 0.15f), rng_frange(&g->rng, -0.15f, 0.15f)));
    f->z = z;
    f->life = 0.9f;
    SDL_strlcpy(f->text, text, sizeof(f->text));
    f->r = r; f->g = gg; f->b = b;
}

static bool is_enemy(EntityType t) { return t == E_SLIME || t == E_BAT || t == E_SKELETON; }

/* ---- level setup -------------------------------------------------------- */

static void populate(Game *g)
{
    Map *m = &g->map;
    for (int i = 1; i < m->room_count; i++) {
        const Room *r = &m->rooms[i];
        int count = rng_range(&g->rng, 1, SDL_min(5, 2 + g->depth / 2));
        for (int k = 0; k < count; k++) {
            int roll = rng_range(&g->rng, 0, 99);
            EntityType t;
            if (g->depth >= 2 && roll < 25 + SDL_min(g->depth * 2, 15)) t = E_SKELETON;
            else if (roll < 70) t = E_SLIME;
            else t = E_BAT;
            spawn(g, t, map_random_floor_in_room(m, r, &g->rng));
        }
        int coins = rng_range(&g->rng, 0, 3);
        for (int k = 0; k < coins; k++) spawn(g, E_COIN, map_random_floor_in_room(m, r, &g->rng));
        if (rng_range(&g->rng, 0, 99) < 28) spawn(g, E_POTION, map_random_floor_in_room(m, r, &g->rng));
    }
}

static void new_floor(Game *g)
{
    Entity player = g->ents[0];
    map_generate(&g->map, &g->rng, g->depth);
    g->ent_count = 0;
    g->part_count = 0;
    for (int i = 0; i < MAX_FLOATERS; i++) g->floaters[i].life = 0;
    Entity *p = spawn(g, E_PLAYER, g->map.start);
    player.pos = g->map.start;
    player.vel = v2(0, 0);
    *p = player;
    populate(g);
    g->cam = p->pos;
    g->flow_px = (int)p->pos.x;
    g->flow_py = (int)p->pos.y;
    map_update_flow(&g->map, g->flow_px, g->flow_py);
    map_update_fov(&g->map, p->pos);
    g->swing_t = g->dash_t = 0;
}

/* Advance to the next floor (used by the dev depth override). */
void game_start_floor(Game *g)
{
    g->depth++;
    new_floor(g);
}

void game_new_run(Game *g)
{
    g->depth = 1;
    g->gold = 0;
    g->kills = 0;
    g->ent_count = 0;
    Entity *p = spawn(g, E_PLAYER, v2(0, 0));
    p->face = v2(0.7071f, 0.7071f);
    g->last_move = p->face;
    g->invuln = g->swing_cd = g->dash_cd = 0;
    g->flash = g->shake = g->hitstop = 0;
    new_floor(g);
}

void game_init(Game *g)
{
    SDL_memset(g, 0, sizeof(*g));
    g->rng = SDL_GetPerformanceCounter() ^ 0x9E3779B97F4A7C15ULL;
    if (!g->rng) g->rng = 1;
    g->zoom_user = 1.0f;
    g->show_map = true;
    g->state = ST_TITLE;
    game_new_run(g);
}

/* ---- combat ------------------------------------------------------------- */

static void kill_enemy(Game *g, Entity *e)
{
    uint8_t r = 120, gg = 230, b = 120;
    if (e->type == E_BAT) { r = 150; gg = 90; b = 200; }
    if (e->type == E_SKELETON) { r = 230; gg = 225; b = 200; }
    emit(g, e->pos.x, e->pos.y, e->z + 0.3f, 22, r, gg, b, 3.5f, 0.7f, 0.09f, false);
    emit(g, e->pos.x, e->pos.y, e->z + 0.3f, 8, 255, 240, 200, 2.5f, 0.35f, 0.06f, true);
    int coins = e->type == E_SKELETON ? 2 : (rng_range(&g->rng, 0, 99) < 45 ? 1 : 0);
    for (int i = 0; i < coins; i++) {
        Entity *c = spawn(g, E_COIN, e->pos);
        if (c) {
            c->vel = v2(rng_frange(&g->rng, -2, 2), rng_frange(&g->rng, -2, 2));
            c->vz = 5.0f;
        }
    }
    if (rng_range(&g->rng, 0, 99) < 6) {
        Entity *pot = spawn(g, E_POTION, e->pos);
        if (pot) pot->vz = 4.0f;
    }
    e->type = E_NONE;
    g->kills++;
    g->shake += 0.12f;
    audio_play(SFX_KILL);
}

static void damage_enemy(Game *g, Input *in, Entity *e, V2 dir)
{
    bool crit = rng_range(&g->rng, 0, 99) < 15;
    int dmg = crit ? 2 : 1;
    e->hp -= dmg;
    e->hurt_t = 0.15f;
    e->stun = 0.25f;
    e->vel = v2_scale(dir, e->type == E_BAT ? 9.0f : 6.0f);
    e->hit_id = g->swing_id;
    e->aggro = true;
    e->timer = 0; /* interrupt skeleton wind-up */
    g->hitstop = 0.045f;
    g->shake += 0.15f;

    char buf[12];
    SDL_snprintf(buf, sizeof(buf), crit ? "%d!" : "%d", dmg);
    if (crit) floater(g, e->pos, e->z + 1.0f, buf, 255, 220, 60);
    else floater(g, e->pos, e->z + 1.0f, buf, 255, 255, 255);
    emit(g, e->pos.x, e->pos.y, e->z + 0.35f, 6, 255, 255, 255, 3.0f, 0.25f, 0.05f, true);
    audio_play(SFX_HIT);
    input_rumble(in, 0.3f, 60);
    if (e->hp <= 0) kill_enemy(g, e);
}

static void hurt_player(Game *g, Input *in, int dmg, V2 dir)
{
    Entity *p = &g->ents[0];
    if (g->invuln > 0 || g->dash_t > 0 || g->state != ST_PLAY) return;
    p->hp -= dmg;
    p->hurt_t = 0.2f;
    p->vel = v2_add(p->vel, v2_scale(dir, 7.0f));
    g->invuln = 0.9f;
    g->flash = 0.35f;
    g->shake += 0.45f;
    g->hitstop = 0.08f;
    emit(g, p->pos.x, p->pos.y, 0.5f, 14, 230, 50, 60, 3.0f, 0.5f, 0.08f, false);
    input_rumble(in, 0.8f, 200);
    if (p->hp <= 0) {
        p->hp = 0;
        g->state = ST_DEAD;
        g->state_t = 0;
        if (g->depth > g->best_depth) g->best_depth = g->depth;
        emit(g, p->pos.x, p->pos.y, 0.5f, 60, 230, 50, 60, 5.0f, 1.2f, 0.1f, false);
        audio_play(SFX_DEATH);
    } else {
        audio_play(SFX_HURT);
    }
}

/* Auto-aim: nearest aggressive visible enemy within reach, used when no explicit aim is given. */
static bool auto_target(Game *g, V2 *dir)
{
    Entity *p = &g->ents[0];
    float best = 2.6f;
    bool found = false;
    for (int i = 1; i < g->ent_count; i++) {
        Entity *e = &g->ents[i];
        if (!is_enemy(e->type)) continue;
        if (!g->map.visible[(int)e->pos.y][(int)e->pos.x]) continue;
        float d = v2_dist(e->pos, p->pos);
        if (d < best) { best = d; *dir = v2_norm(v2_sub(e->pos, p->pos)); found = true; }
    }
    return found;
}

/* Screen-space aim vector -> world direction using the true iso inverse (TILE_W = 2*TILE_H). */
static V2 screen_aim_to_world(V2 s) { return v2_norm(v2(s.x + 2.0f * s.y, 2.0f * s.y - s.x)); }

/* ---- per-entity updates ------------------------------------------------- */

static void move_entity(Game *g, Entity *e, float dt)
{
    e->pos = v2_add(e->pos, v2_scale(e->vel, dt));
    map_collide_circle(&g->map, &e->pos, e->radius);
}

static void steer(Entity *e, V2 target_vel, float accel, float dt)
{
    e->vel = v2_lerp(e->vel, target_vel, 1.0f - SDL_expf(-accel * dt));
}

static void update_enemy(Game *g, Input *in, Entity *e, float dt)
{
    Entity *p = &g->ents[0];
    Map *m = &g->map;
    e->anim += dt;
    if (e->hurt_t > 0) e->hurt_t -= dt;

    V2 to = v2_sub(p->pos, e->pos);
    float d = v2_len(to);
    V2 dir_to = v2_norm(to);
    bool visible = m->visible[(int)e->pos.y][(int)e->pos.x];
    if (!e->aggro && visible && d < FOV_RADIUS - 1) e->aggro = true;

    if (e->stun > 0) {
        e->stun -= dt;
        e->vel = v2_scale(e->vel, SDL_expf(-dt * 8.0f));
        move_entity(g, e, dt);
        return;
    }

    bool los = d < 12.0f && map_los(m, e->pos, p->pos);
    V2 chase = los ? dir_to : map_flow_dir(m, e->pos);
    float speed_bonus = SDL_min(g->depth * 0.06f, 0.8f);

    if (!e->aggro) {
        /* idle wander */
        e->timer -= dt;
        if (e->timer <= 0) {
            e->timer = rng_frange(&g->rng, 1.0f, 3.0f);
            float a = rng_frange(&g->rng, 0, SDL_PI_F * 2);
            e->face = rng_range(&g->rng, 0, 2) ? v2(SDL_cosf(a), SDL_sinf(a)) : v2(0, 0);
        }
        steer(e, v2_scale(e->face, 0.6f), 4.0f, dt);
        if (e->type == E_BAT) e->z = 0.7f + SDL_sinf(e->anim * 10.0f) * 0.05f;
        move_entity(g, e, dt);
        return;
    }

    switch (e->type) {
    case E_SLIME: {
        float hop = SDL_max(0.0f, SDL_sinf(e->anim * 6.0f));
        float speed = (1.2f + speed_bonus) * (0.3f + 1.2f * hop);
        steer(e, v2_scale(chase, speed), 8.0f, dt);
        if (d < e->radius + p->radius + 0.05f) hurt_player(g, in, 1, dir_to);
    } break;

    case E_BAT: {
        V2 perp = v2(-chase.y, chase.x);
        V2 dir = v2_norm(v2_add(chase, v2_scale(perp, SDL_sinf(e->anim * 4.0f) * 0.9f)));
        steer(e, v2_scale(dir, 3.1f + speed_bonus), 5.0f, dt);
        e->z = 0.7f + SDL_sinf(e->anim * 10.0f) * 0.05f;
        if (d < e->radius + p->radius + 0.1f) hurt_player(g, in, 1, dir_to);
    } break;

    case E_SKELETON: {
        if (e->timer > 0) { /* winding up a shot: stand still */
            e->timer -= dt;
            steer(e, v2(0, 0), 10.0f, dt);
            e->face = dir_to;
            if (e->timer <= 0 && los) {
                Entity *a = spawn(g, E_ARROW, v2_add(e->pos, v2_scale(dir_to, 0.3f)));
                if (a) {
                    a->vel = v2_scale(dir_to, 7.0f + speed_bonus * 2.0f);
                    a->face = dir_to;
                }
                audio_play(SFX_SHOOT);
            }
            break;
        }
        V2 want;
        if (los && d < 7.5f) {
            if (d < 3.5f) want = v2_scale(dir_to, -1.0f);
            else if (d > 5.5f) want = dir_to;
            else want = v2_scale(v2(-dir_to.y, dir_to.x), SDL_sinf(e->anim * 0.7f) > 0 ? 1.0f : -1.0f);
            e->cooldown -= dt;
            if (e->cooldown <= 0) {
                e->timer = 0.55f;
                e->cooldown = SDL_max(1.0f, 2.0f - g->depth * 0.08f);
            }
        } else {
            want = chase;
        }
        e->face = dir_to;
        steer(e, v2_scale(want, 1.7f + speed_bonus), 6.0f, dt);
    } break;

    default: break;
    }
    move_entity(g, e, dt);
}

static void update_arrow(Game *g, Input *in, Entity *e, float dt)
{
    Entity *p = &g->ents[0];
    e->timer -= dt;
    e->pos = v2_add(e->pos, v2_scale(e->vel, dt));
    if (e->timer <= 0 || map_solid(&g->map, (int)e->pos.x, (int)e->pos.y)) {
        emit(g, e->pos.x, e->pos.y, e->z, 5, 200, 170, 120, 1.5f, 0.3f, 0.05f, false);
        e->type = E_NONE;
        return;
    }
    if (v2_dist(e->pos, p->pos) < p->radius + e->radius) {
        hurt_player(g, in, 1, v2_norm(e->vel));
        e->type = E_NONE;
    }
}

static void update_pickup(Game *g, Entity *e, float dt)
{
    Entity *p = &g->ents[0];
    e->anim += dt;
    /* bounce physics for dropped loot */
    e->vz -= 22.0f * dt;
    e->z += e->vz * dt;
    if (e->z < 0) { e->z = 0; e->vz = -e->vz * 0.4f; if (e->vz < 1.0f) e->vz = 0; }
    e->vel = v2_scale(e->vel, SDL_expf(-dt * 4.0f));

    float d = v2_dist(e->pos, p->pos);
    if (e->type == E_COIN && d < 2.0f) /* magnet */
        e->vel = v2_add(e->vel, v2_scale(v2_norm(v2_sub(p->pos, e->pos)), 30.0f * dt * (2.0f - d)));
    move_entity(g, e, dt);

    if (d < p->radius + e->radius + 0.1f) {
        if (e->type == E_COIN) {
            g->gold++;
            floater(g, e->pos, 0.8f, "+1", 255, 215, 70);
            emit(g, e->pos.x, e->pos.y, 0.3f, 6, 255, 215, 70, 2.0f, 0.3f, 0.05f, true);
            audio_play(SFX_COIN);
            e->type = E_NONE;
        } else if (e->type == E_POTION && p->hp < p->max_hp) {
            p->hp = SDL_min(p->max_hp, p->hp + 2);
            floater(g, e->pos, 1.0f, "+2 HP", 110, 255, 120);
            emit(g, p->pos.x, p->pos.y, 0.4f, 16, 110, 255, 120, 2.0f, 0.6f, 0.06f, true);
            audio_play(SFX_POTION);
            e->type = E_NONE;
        }
    }
}

static void separate_enemies(Game *g)
{
    const Entity *p = &g->ents[0];
    for (int i = 1; i < g->ent_count; i++) {
        Entity *a = &g->ents[i];
        if (!is_enemy(a->type)) continue;
        /* enemies never overlap the player; only the enemy gets pushed */
        V2 dp = v2_sub(a->pos, p->pos);
        float min_p = (a->radius + p->radius) * 0.9f, dl = v2_len(dp);
        if (dl < min_p && dl > 1e-5f) {
            a->pos = v2_add(p->pos, v2_scale(dp, min_p / dl));
            map_collide_circle(&g->map, &a->pos, a->radius);
        }
        for (int j = i + 1; j < g->ent_count; j++) {
            Entity *b = &g->ents[j];
            if (!is_enemy(b->type)) continue;
            V2 d = v2_sub(b->pos, a->pos);
            float min_d = a->radius + b->radius;
            float dist2 = v2_dot(d, d);
            if (dist2 >= min_d * min_d || dist2 < 1e-8f) continue;
            float dist = SDL_sqrtf(dist2);
            V2 push = v2_scale(d, (min_d - dist) * 0.5f / dist);
            a->pos = v2_sub(a->pos, push);
            b->pos = v2_add(b->pos, push);
        }
    }
}

static void update_particles(Game *g, float dt)
{
    int n = 0;
    for (int i = 0; i < g->part_count; i++) {
        Particle *p = &g->parts[i];
        p->life -= dt;
        if (p->life <= 0) continue;
        p->vz -= (p->glow ? 3.0f : 14.0f) * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->z += p->vz * dt;
        if (p->z < 0) { p->z = 0; p->vz *= -0.3f; p->vx *= 0.6f; p->vy *= 0.6f; }
        float drag = SDL_expf(-dt * 2.5f);
        p->vx *= drag;
        p->vy *= drag;
        g->parts[n++] = *p;
    }
    g->part_count = n;

    for (int i = 0; i < MAX_FLOATERS; i++) {
        if (g->floaters[i].life <= 0) continue;
        g->floaters[i].life -= dt;
        g->floaters[i].z += dt * 1.2f;
    }
}

static void compact_entities(Game *g)
{
    int n = 1;
    for (int i = 1; i < g->ent_count; i++)
        if (g->ents[i].type != E_NONE) g->ents[n++] = g->ents[i];
    g->ent_count = n;
}

static void update_camera(Game *g, float dt, bool idle)
{
    Entity *p = &g->ents[0];
    V2 target = v2_add(p->pos, v2_scale(p->vel, 0.12f));
    if (idle) target = v2_add(p->pos, v2(SDL_sinf(g->time * 0.3f) * 1.5f, SDL_cosf(g->time * 0.23f) * 1.5f));
    g->cam = v2_lerp(g->cam, target, 1.0f - SDL_expf(-dt * 6.0f));
    g->shake = SDL_min(g->shake, 1.0f) * SDL_expf(-dt * 8.0f);
    if (g->flash > 0) g->flash -= dt;
}

/* ---- main update --------------------------------------------------------- */

static void update_play(Game *g, Input *in, const View *view, float dt)
{
    Entity *p = &g->ents[0];
    Map *m = &g->map;

    /* movement intent */
    V2 mv = screen_dir_to_world(in->move);
    bool moving = v2_len(mv) > 0.1f;
    if (moving) g->last_move = v2_norm(mv);

    /* facing / aim */
    bool explicit_aim = false;
    if (in->aim_active) {
        p->face = screen_aim_to_world(in->aim);
        explicit_aim = true;
    } else if (in->mouse_aim && in->device == DEV_KEYBOARD) {
        V2 mw = render_screen_to_world(view, in->mouse_px);
        V2 d = v2_sub(mw, p->pos);
        if (v2_len(d) > 0.05f) p->face = v2_norm(d);
        explicit_aim = true;
    } else if (moving && g->swing_t <= 0) {
        p->face = g->last_move;
    }

    /* dash */
    if (g->dash_cd > 0) g->dash_cd -= dt;
    if (in->dash && g->dash_cd <= 0) {
        V2 dir = moving ? v2_norm(mv) : p->face;
        g->dash_t = DASH_TIME;
        g->dash_cd = DASH_COOLDOWN;
        p->vel = v2_scale(dir, DASH_SPEED);
        audio_play(SFX_DASH);
        emit(g, p->pos.x, p->pos.y, 0.1f, 10, 180, 200, 255, 2.0f, 0.35f, 0.07f, true);
    }
    if (g->dash_t > 0) {
        g->dash_t -= dt;
        emit(g, p->pos.x, p->pos.y, 0.35f, 2, 140, 170, 255, 0.4f, 0.3f, 0.08f, true);
    } else {
        steer(p, v2_scale(mv, PLAYER_SPEED), 18.0f, dt);
    }

    /* attack */
    if (g->swing_cd > 0) g->swing_cd -= dt;
    if ((in->attack || in->attack_held) && g->swing_cd <= 0) {
        V2 dir = p->face;
        if (!explicit_aim) auto_target(g, &dir);
        p->face = dir;
        g->swing_dir = dir;
        g->swing_t = SWING_TIME;
        g->swing_cd = SWING_COOLDOWN;
        g->swing_id++;
        p->vel = v2_add(p->vel, v2_scale(dir, 2.0f));
        audio_play(SFX_SWING);
    }
    if (g->swing_t > 0) {
        bool active = g->swing_t > SWING_TIME - SWING_ACTIVE;
        g->swing_t -= dt;
        if (active) {
            for (int i = 1; i < g->ent_count; i++) {
                Entity *e = &g->ents[i];
                if (e->type == E_NONE) continue;
                V2 d = v2_sub(e->pos, p->pos);
                float dist = v2_len(d);
                if (dist > SWING_RANGE + e->radius) continue;
                if (dist > 0.45f && v2_dot(v2_norm(d), g->swing_dir) < 0.34f) continue;
                if (is_enemy(e->type) && e->hit_id != g->swing_id) {
                    damage_enemy(g, in, e, g->swing_dir);
                } else if (e->type == E_ARROW) { /* parry */
                    emit(g, e->pos.x, e->pos.y, e->z, 8, 255, 240, 160, 2.5f, 0.3f, 0.05f, true);
                    floater(g, e->pos, 1.0f, "PARRY", 180, 220, 255);
                    e->type = E_NONE;
                }
            }
        }
    }

    move_entity(g, p, dt);
    p->anim += dt * (0.5f + v2_len(p->vel) * 0.4f);
    if (g->invuln > 0) g->invuln -= dt;
    if (p->hurt_t > 0) p->hurt_t -= dt;

    /* world reacts to the player */
    int ptx = (int)p->pos.x, pty = (int)p->pos.y;
    if (ptx != g->flow_px || pty != g->flow_py) {
        g->flow_px = ptx;
        g->flow_py = pty;
        map_update_flow(m, ptx, pty);
    }
    map_update_fov(m, p->pos);

    for (int i = 1; i < g->ent_count; i++) {
        Entity *e = &g->ents[i];
        switch (e->type) {
        case E_SLIME: case E_BAT: case E_SKELETON: update_enemy(g, in, e, dt); break;
        case E_ARROW: update_arrow(g, in, e, dt); break;
        case E_COIN: case E_POTION: update_pickup(g, e, dt); break;
        default: break;
        }
        if (g->state != ST_PLAY) break;
    }
    separate_enemies(g);

    if (g->state == ST_PLAY && ptx == m->stairs_x && pty == m->stairs_y &&
        v2_dist(p->pos, v2(m->stairs_x + 0.5f, m->stairs_y + 0.5f)) < 0.4f) {
        g->state = ST_DESCEND;
        g->state_t = 0;
        audio_play(SFX_STAIRS);
    }
}

void game_update(Game *g, Input *in, const View *view, float dt)
{
    g->time += dt;
    if (in->zoom_delta != 0) g->zoom_user = clampf(g->zoom_user * SDL_powf(1.1f, in->zoom_delta), 0.55f, 1.8f);
    if (in->toggle_map) g->show_map = !g->show_map;

    switch (g->state) {
    case ST_TITLE:
        if (in->confirm || in->attack) {
            g->state = ST_PLAY;
            audio_play(SFX_POTION);
        }
        update_particles(g, dt);
        update_camera(g, dt, true);
        break;

    case ST_PAUSE:
        if (in->pause || in->confirm) g->state = ST_PLAY;
        break;

    case ST_DEAD:
        g->state_t += dt;
        update_particles(g, dt);
        update_camera(g, dt, false);
        if (g->state_t > 1.0f && (in->confirm || in->attack)) {
            game_new_run(g);
            g->state = ST_PLAY;
        }
        break;

    case ST_DESCEND: {
        float before = g->state_t;
        g->state_t += dt;
        if (before < 0.6f && g->state_t >= 0.6f) {
            g->depth++;
            new_floor(g);
        }
        if (g->state_t >= 1.2f) g->state = ST_PLAY;
        update_particles(g, dt);
        update_camera(g, dt, false);
    } break;

    case ST_PLAY:
        if (in->pause) { g->state = ST_PAUSE; break; }
        if (g->hitstop > 0) { g->hitstop -= dt; break; }
        update_play(g, in, view, dt);
        update_particles(g, dt);
        update_camera(g, dt, false);
        compact_entities(g);
        break;
    }
}
