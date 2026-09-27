# IsoRogue — SDL3 isometric roguelike tech demo

Real-time action roguelike in plain C11 + SDL3. All art and sound are generated in code (no assets).
Builds natively (Linux/macOS/Windows) and to WebAssembly, and the web build is playable on phones.

## Features

- Isometric renderer on `SDL_RenderGeometry`: one batched vertex buffer, depth-sorted walls/entities/particles,
  per-vertex lighting (baked torch light + dynamic lights), additive glow pass, wall cutaway around the player
- Procedural dungeons: rooms + MST corridors with loops, stairs placed in the farthest room, torches
- Field of view with fog-of-war memory, BFS flow field for enemy pathing, circle-vs-tile collision
- Combat: sword swing with auto-aim (touch-friendly), dash with i-frames, crits, knockback, hit-stop,
  screen shake, arrow parry; slimes, bats and skeleton archers scaling with depth
- Input: keyboard + mouse, gamepads (with rumble), multitouch virtual stick + buttons
- Procedural SFX synthesized at startup; fixed 60 Hz simulation; HiDPI aware

## Controls

| | Keyboard / mouse | Gamepad | Touch |
|---|---|---|---|
| Move | WASD / arrows | Left stick / D-pad | Left thumb (floating stick) |
| Aim | Mouse | Right stick | Auto-aim |
| Attack | LMB / Space / J | A / X / RT | ATTACK button |
| Dash | RMB / Shift / K | B / LB / LT | DASH button |
| Pause | Esc / P | Start | ❚❚ button |
| Map | M / Tab | Back | — |

Also: `+`/`-` or mouse wheel to zoom, F11 fullscreen, F3 debug overlay.

## Build — native

Needs CMake ≥ 3.20 and SDL3. An installed SDL3 is used if found; otherwise it is fetched (git over SSH) and built statically.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/isorogue
```

## Build — WebAssembly

Needs the Emscripten SDK. SDL3 comes from Emscripten's ports and is downloaded on first build.

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -S . -B build-web -G Ninja
cmake --build build-web
python3 -m http.server 8000 -d build-web   # open http://localhost:8000
```

Deploy by uploading `build-web/index.html`, `index.js` and `index.wasm` to any static host (HTTPS is needed for
phones on your network to allow fullscreen/gamepad reliably). The page's ⛶ button requests fullscreen and landscape lock.

## Layout

```
src/common.h   shared types, tunables, math, rng
src/main.c     SDL callback entry points, fixed-timestep loop, dev hooks
src/dungeon.c  generation, FOV, flow field, collision, baked torch light
src/game.c     entities, combat, AI, game states
src/render.c   iso projection, batching, lighting, HUD, minimap, touch UI
src/input.c    keyboard/mouse, gamepad, touch → abstract controls
src/audio.c    synthesized sound effects
web/shell.html mobile-ready page (full-viewport canvas, no scroll/zoom, fullscreen button)
```

## Dev hooks

Environment variables for reproducible runs and headless screenshots
(`SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy` works without a display):

- `ISOROGUE_SEED=<n>` fixed dungeon seed, `ISOROGUE_DEPTH=<n>` start floor
- `ISOROGUE_PLAY=1` skip the title, `ISOROGUE_BOT=1` autoplay bot
- `ISOROGUE_SHOT=out.bmp ISOROGUE_FRAMES=<n>` save a screenshot after n frames and exit
