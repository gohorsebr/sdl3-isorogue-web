#include "common.h"

/* Tiny procedural SFX: every sound is synthesized into a float buffer at startup
   and played on one of a few voices (audio streams bound to the device). */

#define SAMPLE_RATE 44100
#define VOICES 8

static SDL_AudioDeviceID device;
static SDL_AudioStream *voices[VOICES];
static int next_voice;
static float *buffers[SFX_COUNT];
static int lengths[SFX_COUNT];
static uint64_t noise_state = 0x1234567ULL;

static float noise(void) { return rng_float(&noise_state) * 2.0f - 1.0f; }
static float square(float phase) { return SDL_fmodf(phase, 1.0f) < 0.5f ? 1.0f : -1.0f; }
static float saw(float phase) { return SDL_fmodf(phase, 1.0f) * 2.0f - 1.0f; }
static float sine(float phase) { return SDL_sinf(phase * 2.0f * SDL_PI_F); }

typedef float (*SynthFn)(float t, float dur, float *phase, float *lp);

static float s_swing(float t, float dur, float *ph, float *lp)
{
    float cutoff = lerpf(5000, 600, t / dur);
    *lp += (1.0f - SDL_expf(-2.0f * SDL_PI_F * cutoff / SAMPLE_RATE)) * (noise() - *lp);
    float env = SDL_sinf(SDL_PI_F * t / dur);
    return *lp * env * 0.9f;
}

static float s_hit(float t, float dur, float *ph, float *lp)
{
    *ph += lerpf(190, 90, t / dur) / SAMPLE_RATE;
    float env = 1.0f - t / dur;
    return (square(*ph) * 0.5f + noise() * 0.4f) * env * env;
}

static float s_hurt(float t, float dur, float *ph, float *lp)
{
    *ph += lerpf(240, 70, t / dur) / SAMPLE_RATE;
    return saw(*ph) * (1.0f - t / dur) * 0.7f;
}

static float s_coin(float t, float dur, float *ph, float *lp)
{
    *ph += (t < 0.05f ? 988.0f : 1319.0f) / SAMPLE_RATE;
    return square(*ph) * (1.0f - t / dur) * 0.3f;
}

static float s_potion(float t, float dur, float *ph, float *lp)
{
    static const float notes[4] = { 523.25f, 659.25f, 783.99f, 1046.5f };
    int n = SDL_min(3, (int)(t / 0.08f));
    *ph += notes[n] / SAMPLE_RATE;
    return (sine(*ph) * 0.7f + square(*ph) * 0.15f) * (1.0f - t / dur);
}

static float s_dash(float t, float dur, float *ph, float *lp)
{
    float cutoff = lerpf(800, 3000, t / dur);
    *lp += (1.0f - SDL_expf(-2.0f * SDL_PI_F * cutoff / SAMPLE_RATE)) * (noise() - *lp);
    return *lp * SDL_sinf(SDL_PI_F * t / dur) * 0.9f;
}

static float s_stairs(float t, float dur, float *ph, float *lp)
{
    *ph += lerpf(660, 110, t / dur) / SAMPLE_RATE;
    float trem = 0.7f + 0.3f * SDL_sinf(t * 40.0f);
    return sine(*ph) * trem * (1.0f - t / dur) * 0.8f;
}

static float s_death(float t, float dur, float *ph, float *lp)
{
    float vib = 1.0f + 0.05f * SDL_sinf(t * 50.0f);
    *ph += lerpf(300, 40, t / dur) * vib / SAMPLE_RATE;
    return square(*ph) * (1.0f - t / dur) * 0.5f;
}

static float s_shoot(float t, float dur, float *ph, float *lp)
{
    *ph += lerpf(1200, 400, t / dur) / SAMPLE_RATE;
    return square(*ph) * (1.0f - t / dur) * 0.25f;
}

static float s_kill(float t, float dur, float *ph, float *lp)
{
    *ph += lerpf(130, 40, t / dur) / SAMPLE_RATE;
    float env = 1.0f - t / dur;
    return (square(*ph) * 0.5f + noise() * 0.5f) * env * env;
}

static void synth(Sfx id, SynthFn fn, float dur, float volume)
{
    int n = (int)(dur * SAMPLE_RATE);
    float *buf = SDL_malloc(sizeof(float) * n);
    if (!buf) return;
    float phase = 0, lp = 0;
    for (int i = 0; i < n; i++) {
        float t = (float)i / SAMPLE_RATE;
        float s = fn(t, dur, &phase, &lp) * volume;
        /* short fade in/out to avoid clicks */
        float edge = SDL_min(1.0f, SDL_min(i, n - i) / 64.0f);
        buf[i] = clampf(s * edge, -1.0f, 1.0f);
    }
    buffers[id] = buf;
    lengths[id] = n;
}

void audio_init(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio disabled: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, SAMPLE_RATE };
    device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!device) {
        SDL_Log("audio disabled: %s", SDL_GetError());
        return;
    }
    for (int i = 0; i < VOICES; i++) {
        voices[i] = SDL_CreateAudioStream(&spec, NULL);
        if (voices[i]) SDL_BindAudioStream(device, voices[i]);
    }

    synth(SFX_SWING, s_swing, 0.12f, 0.35f);
    synth(SFX_HIT, s_hit, 0.12f, 0.40f);
    synth(SFX_HURT, s_hurt, 0.30f, 0.40f);
    synth(SFX_COIN, s_coin, 0.14f, 0.35f);
    synth(SFX_POTION, s_potion, 0.36f, 0.35f);
    synth(SFX_DASH, s_dash, 0.18f, 0.35f);
    synth(SFX_STAIRS, s_stairs, 0.70f, 0.35f);
    synth(SFX_DEATH, s_death, 1.00f, 0.35f);
    synth(SFX_SHOOT, s_shoot, 0.08f, 0.40f);
    synth(SFX_KILL, s_kill, 0.22f, 0.40f);
}

void audio_play(Sfx s)
{
    if (!device || !buffers[s]) return;
    /* prefer an idle voice, otherwise steal round-robin */
    int v = -1;
    for (int i = 0; i < VOICES; i++) {
        if (voices[i] && SDL_GetAudioStreamQueued(voices[i]) == 0) { v = i; break; }
    }
    if (v < 0) {
        v = next_voice;
        next_voice = (next_voice + 1) % VOICES;
    }
    if (!voices[v]) return;
    SDL_ClearAudioStream(voices[v]);
    SDL_PutAudioStreamData(voices[v], buffers[s], lengths[s] * (int)sizeof(float));
}

void audio_shutdown(void)
{
    for (int i = 0; i < VOICES; i++) {
        if (voices[i]) SDL_DestroyAudioStream(voices[i]);
        voices[i] = NULL;
    }
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    for (int i = 0; i < SFX_COUNT; i++) {
        SDL_free(buffers[i]);
        buffers[i] = NULL;
    }
}
