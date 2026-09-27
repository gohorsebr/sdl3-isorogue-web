#include "common.h"

#define STICK_DEADZONE 0.18f
#define AIM_DEADZONE 0.35f
#define TRIGGER_THRESHOLD 0.5f

static bool rt_down, lt_down;

void input_init(Input *in)
{
    SDL_memset(in, 0, sizeof(*in));
    in->device = DEV_KEYBOARD;
}

void input_shutdown(Input *in)
{
    if (in->pad) SDL_CloseGamepad(in->pad);
    in->pad = NULL;
}

TouchLayout touch_layout(float w, float h)
{
    float u = SDL_max(1.0f, SDL_min(w, h) / 400.0f);
    TouchLayout l;
    l.attack = v2(w - 95 * u, h - 100 * u);
    l.attack_r = 46 * u;
    l.dash = v2(w - 190 * u, h - 55 * u);
    l.dash_r = 32 * u;
    l.pause = v2(w - 28 * u, 28 * u);
    l.pause_r = 18 * u;
    l.stick_r = 55 * u;
    return l;
}

static void open_first_gamepad(Input *in)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0) in->pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
}

static V2 apply_deadzone(V2 v, float dz)
{
    float l = v2_len(v);
    if (l < dz) return v2(0, 0);
    float scaled = SDL_min(1.0f, (l - dz) / (1.0f - dz));
    return v2_scale(v, scaled / l);
}

static V2 read_stick(SDL_Gamepad *pad, SDL_GamepadAxis ax, SDL_GamepadAxis ay)
{
    return v2(SDL_GetGamepadAxis(pad, ax) / 32767.0f, SDL_GetGamepadAxis(pad, ay) / 32767.0f);
}

static void use_touch(Input *in)
{
    in->device = DEV_TOUCH;
    in->mouse_aim = false;
}

void input_handle_event(Input *in, SDL_Renderer *r, const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!in->pad) in->pad = SDL_OpenGamepad(e->gdevice.which);
        break;

    case SDL_EVENT_GAMEPAD_REMOVED:
        if (in->pad && SDL_GetGamepadID(in->pad) == e->gdevice.which) {
            SDL_CloseGamepad(in->pad);
            in->pad = NULL;
            open_first_gamepad(in);
        }
        break;

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        in->device = DEV_GAMEPAD;
        in->mouse_aim = false;
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_SOUTH: in->attack = in->confirm = true; break;
        case SDL_GAMEPAD_BUTTON_WEST: in->attack = true; break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: in->dash = true; break;
        case SDL_GAMEPAD_BUTTON_START: in->pause = in->confirm = true; break;
        case SDL_GAMEPAD_BUTTON_BACK: in->toggle_map = true; break;
        default: break;
        }
        break;

    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e->gaxis.value / 32767.0f;
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            bool down = v > TRIGGER_THRESHOLD;
            if (down && !rt_down) { in->attack = true; in->device = DEV_GAMEPAD; }
            rt_down = down;
        } else if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            bool down = v > TRIGGER_THRESHOLD;
            if (down && !lt_down) { in->dash = true; in->device = DEV_GAMEPAD; }
            lt_down = down;
        } else if (SDL_fabsf(v) > 0.5f) {
            in->device = DEV_GAMEPAD;
            in->mouse_aim = false;
        }
    } break;

    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        in->device = DEV_KEYBOARD;
        switch (e->key.scancode) {
        case SDL_SCANCODE_SPACE: case SDL_SCANCODE_J: case SDL_SCANCODE_Z:
            in->attack = true;
            if (e->key.scancode == SDL_SCANCODE_SPACE) in->confirm = true;
            break;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_K: case SDL_SCANCODE_X:
            in->dash = true;
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_P: in->pause = true; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: in->confirm = true; break;
        case SDL_SCANCODE_M: case SDL_SCANCODE_TAB: in->toggle_map = true; break;
        case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_KP_PLUS: in->zoom_delta += 1; break;
        case SDL_SCANCODE_MINUS: case SDL_SCANCODE_KP_MINUS: in->zoom_delta -= 1; break;
        default: break;
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e->motion.which == SDL_TOUCH_MOUSEID) break;
        SDL_Event c = *e;
        SDL_ConvertEventToRenderCoordinates(r, &c);
        if (e->type == SDL_EVENT_MOUSE_MOTION) {
            in->mouse_px = v2(c.motion.x, c.motion.y);
            in->mouse_aim = true;
            in->device = DEV_KEYBOARD;
        } else {
            in->mouse_px = v2(c.button.x, c.button.y);
            bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (e->button.button == SDL_BUTTON_LEFT) {
                in->mouse_left = down;
                if (down) { in->attack = in->confirm = true; in->mouse_aim = true; in->device = DEV_KEYBOARD; }
            } else if (e->button.button == SDL_BUTTON_RIGHT && down) {
                in->dash = true;
            }
        }
    } break;

    case SDL_EVENT_MOUSE_WHEEL:
        in->zoom_delta += e->wheel.y;
        break;

    case SDL_EVENT_FINGER_DOWN: {
        use_touch(in);
        V2 p = v2(e->tfinger.x * in->screen_w, e->tfinger.y * in->screen_h);
        TouchLayout l = touch_layout(in->screen_w, in->screen_h);
        if (v2_dist(p, l.pause) < l.pause_r * 1.6f) {
            in->pause = true;
        } else if (v2_dist(p, l.attack) < l.attack_r * 1.25f) {
            in->btn_attack.down = true;
            in->btn_attack.id = e->tfinger.fingerID;
            in->attack = in->confirm = true;
        } else if (v2_dist(p, l.dash) < l.dash_r * 1.35f) {
            in->btn_dash.down = true;
            in->btn_dash.id = e->tfinger.fingerID;
            in->dash = true;
        } else if (p.x < in->screen_w * 0.55f && !in->stick.active) {
            in->stick.active = true;
            in->stick.id = e->tfinger.fingerID;
            in->stick.origin = in->stick.pos = p;
            in->confirm = true;
        } else {
            in->confirm = true;
        }
    } break;

    case SDL_EVENT_FINGER_MOTION:
        if (in->stick.active && e->tfinger.fingerID == in->stick.id) {
            V2 p = v2(e->tfinger.x * in->screen_w, e->tfinger.y * in->screen_h);
            float r = touch_layout(in->screen_w, in->screen_h).stick_r;
            V2 d = v2_sub(p, in->stick.origin);
            /* floating stick: drag the origin along when the finger goes past the rim */
            if (v2_len(d) > r) in->stick.origin = v2_sub(p, v2_scale(v2_norm(d), r));
            in->stick.pos = p;
        }
        break;

    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (in->stick.active && e->tfinger.fingerID == in->stick.id) in->stick.active = false;
        if (in->btn_attack.down && e->tfinger.fingerID == in->btn_attack.id) in->btn_attack.down = false;
        if (in->btn_dash.down && e->tfinger.fingerID == in->btn_dash.id) in->btn_dash.down = false;
        break;

    default: break;
    }
}

void input_poll(Input *in)
{
    const bool *keys = SDL_GetKeyboardState(NULL);
    V2 kb = v2(0, 0);
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) kb.y -= 1;
    if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) kb.y += 1;
    if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) kb.x -= 1;
    if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) kb.x += 1;
    kb = v2_norm(kb);

    V2 pad = v2(0, 0), aim = v2(0, 0);
    bool pad_attack = false;
    if (in->pad) {
        pad = apply_deadzone(read_stick(in->pad, SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY), STICK_DEADZONE);
        V2 dp = v2(0, 0);
        if (SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) dp.y -= 1;
        if (SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dp.y += 1;
        if (SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dp.x -= 1;
        if (SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dp.x += 1;
        if (v2_len(dp) > 0) pad = v2_norm(dp);
        aim = read_stick(in->pad, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY);
        pad_attack = SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_SOUTH) ||
                     SDL_GetGamepadButton(in->pad, SDL_GAMEPAD_BUTTON_WEST) || rt_down;
    }

    V2 touch = v2(0, 0);
    if (in->stick.active) {
        float r = touch_layout(in->screen_w, in->screen_h).stick_r;
        touch = apply_deadzone(v2_scale(v2_sub(in->stick.pos, in->stick.origin), 1.0f / r), 0.12f);
    }

    /* take whichever device is pushing hardest */
    in->move = kb;
    if (v2_len(pad) > v2_len(in->move)) in->move = pad;
    if (v2_len(touch) > v2_len(in->move)) in->move = touch;

    in->aim_active = v2_len(aim) > AIM_DEADZONE;
    in->aim = in->aim_active ? v2_norm(aim) : v2(0, 0);
    if (in->aim_active) { in->device = DEV_GAMEPAD; in->mouse_aim = false; }

    in->attack_held = in->mouse_left || pad_attack || in->btn_attack.down ||
                      keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_J] || keys[SDL_SCANCODE_Z];
}

void input_end_step(Input *in)
{
    in->attack = in->dash = in->pause = in->confirm = in->toggle_map = false;
    in->zoom_delta = 0;
}

void input_rumble(Input *in, float strength, int ms)
{
    if (!in->pad) return;
    Uint16 lo = (Uint16)(clampf(strength, 0, 1) * 0xFFFF);
    Uint16 hi = (Uint16)(clampf(strength * 0.6f, 0, 1) * 0xFFFF);
    SDL_RumbleGamepad(in->pad, lo, hi, (Uint32)ms);
}
