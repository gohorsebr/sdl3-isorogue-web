#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include "common.h"

/* SDL's callback entry points drive the loop, so the same code runs natively
   and under Emscripten (where the browser owns the main loop). */

typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    Game game;
    Input input;
    Uint64 last_ns;
    float accumulator;
    float fps_timer;
    int fps_frames;
    /* dev hooks (env vars): ISOROGUE_SEED, ISOROGUE_DEPTH, ISOROGUE_PLAY, ISOROGUE_BOT, ISOROGUE_SHOT + ISOROGUE_FRAMES */
    const char *shot_path;
    int shot_frame, frame;
    bool bot;
} App;

/* Very small autoplay bot for automated screenshots: chase the nearest visible enemy and swing. */
static void bot_drive(App *app)
{
    Game *g = &app->game;
    Input *in = &app->input;
    const Entity *p = &g->ents[0];
    const Entity *best = NULL;
    float best_d = 1e9f;
    for (int i = 1; i < g->ent_count; i++) {
        const Entity *e = &g->ents[i];
        if (e->type != E_SLIME && e->type != E_BAT && e->type != E_SKELETON) continue;
        float d = v2_dist(e->pos, p->pos);
        if (d < best_d) { best_d = d; best = e; }
    }
    V2 target = best ? best->pos : v2(g->map.stairs_x + 0.5f, g->map.stairs_y + 0.5f);
    static uint16_t dist[MAP_H][MAP_W];
    V2 w;
    if (best && best_d < 2.0f) {
        w = v2_norm(v2_sub(target, p->pos));
    } else {
        map_bfs(&g->map, (int)target.x, (int)target.y, dist);
        w = map_flow_dir_in(&g->map, dist, p->pos);
    }
    in->move = world_dir_to_screen(w);
    in->attack_held = best && best_d < 1.3f;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv)
{
    SDL_SetAppMetadata("IsoRogue", "0.1", "dev.isorogue.demo");
    /* We handle touch ourselves; don't let SDL synthesize mouse clicks from it. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    App *app = SDL_calloc(1, sizeof(App));
    if (!app) return SDL_APP_FAILURE;
    *appstate = app;

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef SDL_PLATFORM_EMSCRIPTEN
    /* Ask for GL up front: otherwise the renderer recreates the window, and the
       recreated window no longer sees the page's CSS sizing (it stops tracking
       the viewport on phones/resizes). */
    flags |= SDL_WINDOW_OPENGL;
#endif
    if (!SDL_CreateWindowAndRenderer("IsoRogue", 1280, 720, flags, &app->window, &app->renderer)) {
        SDL_Log("window/renderer failed: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_Log("renderer: %s", SDL_GetRendererName(app->renderer));

    game_init(&app->game);
    const char *seed = SDL_getenv("ISOROGUE_SEED");
    const char *depth = SDL_getenv("ISOROGUE_DEPTH");
    if (seed) app->game.rng = SDL_strtoull(seed, NULL, 10) | 1;
    if (seed || depth) {
        game_new_run(&app->game);
        if (depth) app->game.depth = SDL_max(1, SDL_atoi(depth)) - 1;
        if (depth) game_start_floor(&app->game);
    }
    if (SDL_getenv("ISOROGUE_PLAY")) app->game.state = ST_PLAY;
    app->bot = SDL_getenv("ISOROGUE_BOT") != NULL;
    app->shot_path = SDL_getenv("ISOROGUE_SHOT");
    const char *frames = SDL_getenv("ISOROGUE_FRAMES");
    app->shot_frame = frames ? SDL_atoi(frames) : 60;
    input_init(&app->input);
    audio_init();
    app->last_ns = SDL_GetTicksNS();
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event)
{
    App *app = appstate;
    Game *g = &app->game;

    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.scancode == SDL_SCANCODE_F11 && !event->key.repeat) {
            bool fs = (SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN) != 0;
            SDL_SetWindowFullscreen(app->window, !fs);
        } else if (event->key.scancode == SDL_SCANCODE_F3 && !event->key.repeat) {
            g->show_debug = !g->show_debug;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        if (g->state == ST_PLAY) g->state = ST_PAUSE;
        break;
    default: break;
    }
    input_handle_event(&app->input, app->renderer, event);
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate)
{
    App *app = appstate;
    Uint64 now = SDL_GetTicksNS();
    float dt = (float)((now - app->last_ns) / 1e9);
    app->last_ns = now;
    if (dt > 0.1f) dt = 0.1f;

    int w = 1, h = 1;
    SDL_GetCurrentRenderOutputSize(app->renderer, &w, &h);
    app->input.screen_w = (float)w;
    app->input.screen_h = (float)h;
    input_poll(&app->input);
    if (app->bot && app->game.state == ST_PLAY) bot_drive(app);
    if (app->shot_path) dt = FIXED_DT; /* deterministic stepping for captures */

    /* fixed-timestep simulation; edge-triggered input is consumed by the first step */
    View view = render_make_view(app->renderer, &app->game);
    app->accumulator += dt;
    int steps = 0;
    while (app->accumulator >= FIXED_DT && steps < 6) {
        game_update(&app->game, &app->input, &view, FIXED_DT);
        input_end_step(&app->input);
        app->accumulator -= FIXED_DT;
        steps++;
    }
    if (steps == 6) app->accumulator = 0;

    render_frame(app->renderer, &app->game, &app->input);
    if (app->shot_path && ++app->frame >= app->shot_frame) {
        SDL_Surface *shot = SDL_RenderReadPixels(app->renderer, NULL);
        if (shot) {
            SDL_SaveBMP(shot, app->shot_path);
            SDL_DestroySurface(shot);
        }
        return SDL_APP_SUCCESS;
    }
    SDL_RenderPresent(app->renderer);

    app->fps_frames++;
    app->fps_timer += dt;
    if (app->fps_timer >= 0.5f) {
        app->game.fps = app->fps_frames / app->fps_timer;
        app->fps_frames = 0;
        app->fps_timer = 0;
    }
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result)
{
    App *app = appstate;
    audio_shutdown();
    if (app) {
        input_shutdown(&app->input);
        SDL_free(app);
    }
}
