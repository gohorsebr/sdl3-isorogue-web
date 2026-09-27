#ifndef ISOROGUE_COMMON_H
#define ISOROGUE_COMMON_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- tunables ---------------------------------------------------------- */

#define MAP_W 64
#define MAP_H 64

#define TILE_W 64.0f /* iso diamond width in px at zoom 1 */
#define TILE_H 32.0f /* iso diamond height in px at zoom 1 */
#define WALL_H 44.0f /* wall block height in px at zoom 1 */

#define FOV_RADIUS 9
#define MAX_ENTITIES 256
#define MAX_PARTICLES 2048
#define MAX_FLOATERS 64
#define MAX_TORCHES 48
#define MAX_ROOMS 16

#define FIXED_DT (1.0f / 60.0f)

/* ---- math -------------------------------------------------------------- */

typedef struct { float x, y; } V2;

static inline V2 v2(float x, float y) { return (V2){ x, y }; }
static inline V2 v2_add(V2 a, V2 b) { return (V2){ a.x + b.x, a.y + b.y }; }
static inline V2 v2_sub(V2 a, V2 b) { return (V2){ a.x - b.x, a.y - b.y }; }
static inline V2 v2_scale(V2 a, float s) { return (V2){ a.x * s, a.y * s }; }
static inline float v2_dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
static inline float v2_len(V2 a) { return SDL_sqrtf(a.x * a.x + a.y * a.y); }
static inline float v2_dist(V2 a, V2 b) { return v2_len(v2_sub(a, b)); }
static inline V2 v2_norm(V2 a)
{
    float l = v2_len(a);
    return l > 1e-6f ? v2_scale(a, 1.0f / l) : v2(0, 0);
}
static inline V2 v2_lerp(V2 a, V2 b, float t) { return v2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* Screen-space input direction -> world direction (45 degree rotation). */
static inline V2 screen_dir_to_world(V2 s) { return v2((s.x + s.y) * 0.70710678f, (s.y - s.x) * 0.70710678f); }
static inline V2 world_dir_to_screen(V2 w) { return v2((w.x - w.y) * 0.70710678f, (w.x + w.y) * 0.70710678f); }

/* ---- rng (xorshift64*) ------------------------------------------------- */

static inline uint32_t rng_next(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return (uint32_t)((x * 0x2545F4914F6CDD1DULL) >> 32);
}
static inline int rng_range(uint64_t *s, int lo, int hi) /* inclusive */
{
    return lo + (int)(rng_next(s) % (uint32_t)(hi - lo + 1));
}
static inline float rng_float(uint64_t *s) { return (rng_next(s) >> 8) * (1.0f / 16777216.0f); }
static inline float rng_frange(uint64_t *s, float lo, float hi) { return lo + (hi - lo) * rng_float(s); }

/* ---- map --------------------------------------------------------------- */

typedef enum { T_VOID = 0, T_FLOOR, T_WALL, T_STAIRS } TileType;

typedef struct { int x, y, w, h; } Room;

typedef struct {
    V2 pos;       /* light position (world) */
    int wx, wy;   /* wall tile it hangs on */
    float phase;
} Torch;

typedef struct {
    uint8_t tile[MAP_H][MAP_W];
    uint8_t seen[MAP_H][MAP_W];
    uint8_t visible[MAP_H][MAP_W];
    uint8_t variant[MAP_H][MAP_W];
    uint16_t dist[MAP_H][MAP_W];        /* BFS distance to player (flow field) */
    float static_light[MAP_H + 1][MAP_W + 1][3]; /* baked torch light per tile corner */
    Room rooms[MAX_ROOMS];
    int room_count;
    Torch torches[MAX_TORCHES];
    int torch_count;
    int stairs_x, stairs_y;
    V2 start;
} Map;

void map_generate(Map *m, uint64_t *rng, int depth);
bool map_solid(const Map *m, int x, int y);
bool map_los(const Map *m, V2 a, V2 b);
void map_update_fov(Map *m, V2 eye);
void map_update_flow(Map *m, int px, int py);
/* Push a circle out of solid tiles. Returns true if it touched a wall. */
bool map_collide_circle(const Map *m, V2 *p, float r);
V2 map_flow_dir(const Map *m, V2 from);
V2 map_flow_dir_in(const Map *m, const uint16_t dist[MAP_H][MAP_W], V2 from);
void map_bfs(const Map *m, int sx, int sy, uint16_t out[MAP_H][MAP_W]);
V2 map_random_floor_in_room(const Map *m, const Room *r, uint64_t *rng);

/* ---- input ------------------------------------------------------------- */

typedef enum { DEV_KEYBOARD, DEV_GAMEPAD, DEV_TOUCH } Device;

typedef struct {
    bool active;
    SDL_FingerID id;
    V2 origin, pos; /* pixels */
} TouchStick;

typedef struct {
    bool down;
    SDL_FingerID id;
} TouchButton;

typedef struct {
    /* abstract controls, consumed by the game */
    V2 move;          /* screen-space, length <= 1 */
    V2 aim;           /* screen-space unit vector when aim_active */
    bool aim_active;
    bool attack, dash, pause, confirm, toggle_map; /* edge-triggered */
    bool attack_held;
    float zoom_delta;

    Device device;

    /* keyboard/mouse */
    bool mouse_aim;
    V2 mouse_px;
    bool mouse_left;

    /* gamepad */
    SDL_Gamepad *pad;

    /* touch */
    float screen_w, screen_h; /* render output in pixels */
    TouchStick stick;
    TouchButton btn_attack, btn_dash;
} Input;

void input_init(Input *in);
void input_handle_event(Input *in, SDL_Renderer *r, const SDL_Event *e);
void input_poll(Input *in); /* sample continuous state once per frame */
void input_end_step(Input *in);
void input_rumble(Input *in, float strength, int ms);
void input_shutdown(Input *in);

/* Touch UI layout, shared by input hit-testing and rendering. */
typedef struct { V2 attack; float attack_r; V2 dash; float dash_r; V2 pause; float pause_r; float stick_r; } TouchLayout;
TouchLayout touch_layout(float w, float h);

/* ---- audio ------------------------------------------------------------- */

typedef enum {
    SFX_SWING, SFX_HIT, SFX_HURT, SFX_COIN, SFX_POTION, SFX_DASH,
    SFX_STAIRS, SFX_DEATH, SFX_SHOOT, SFX_KILL, SFX_COUNT
} Sfx;

void audio_init(void);
void audio_play(Sfx s);
void audio_shutdown(void);

/* ---- entities ---------------------------------------------------------- */

typedef enum {
    E_NONE, E_PLAYER, E_SLIME, E_BAT, E_SKELETON, E_ARROW, E_COIN, E_POTION
} EntityType;

typedef struct {
    EntityType type;
    V2 pos, vel;
    float z, vz;
    float radius;
    int hp, max_hp;
    V2 face;          /* world-space facing */
    float anim;       /* free-running animation clock */
    float timer;      /* generic AI timer */
    float cooldown;
    float hurt_t;     /* flash timer */
    float stun;
    bool aggro;
    uint32_t hit_id;  /* last player swing that hit this entity */
} Entity;

typedef struct {
    float x, y, z, vx, vy, vz;
    float life, max_life, size;
    uint8_t r, g, b;
    bool glow;
} Particle;

typedef struct {
    V2 pos;
    float z, life;
    char text[12];
    uint8_t r, g, b;
} Floater;

typedef enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_DESCEND, ST_DEAD } GameState;

typedef struct {
    Map map;
    Entity ents[MAX_ENTITIES]; /* ents[0] is always the player */
    int ent_count;
    Particle parts[MAX_PARTICLES];
    int part_count;
    Floater floaters[MAX_FLOATERS];

    GameState state;
    float state_t;
    float time;
    int depth, gold, kills, best_depth;
    uint64_t rng;

    /* player combat */
    float swing_t, swing_cd, dash_t, dash_cd, invuln;
    V2 swing_dir;
    uint32_t swing_id;
    V2 last_move;

    /* camera */
    V2 cam;
    float zoom_user;
    float shake;
    float hitstop;
    float flash;       /* red screen flash when hurt */
    bool show_map;
    bool show_debug;
    float fps;
    int flow_px, flow_py;
} Game;

void game_init(Game *g);
void game_new_run(Game *g);
void game_start_floor(Game *g);
struct View;
void game_update(Game *g, Input *in, const struct View *v, float dt);

/* ---- render ------------------------------------------------------------ */

typedef struct View {
    SDL_Renderer *r;
    float w, h;        /* output size in pixels */
    float zoom;
    float ui;          /* UI scale factor */
    V2 origin;         /* screen offset so that the camera is centered */
} View;

void render_frame(SDL_Renderer *r, Game *g, Input *in);
V2 render_world_to_screen(const View *v, float wx, float wy, float wz);
V2 render_screen_to_world(const View *v, V2 s);
View render_make_view(SDL_Renderer *r, const Game *g);

#endif
