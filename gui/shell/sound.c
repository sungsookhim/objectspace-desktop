/* Sounds for action events (sound.h). */
#define _GNU_SOURCE
#include "sound.h"

#include <math.h>
#include <pipewire/pipewire.h>
#include <pthread.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_SNDFILE
#include <sndfile.h>
#endif

#define RATE 48000
#define VOICES 8
#define IDLE_MS 2000   /* pause the stream after this long without sound */

struct clip {
    struct clip *next;
    char *path;
    float *frames;     /* interleaved stereo */
    size_t n;          /* frames */
};

struct voice {
    atomic_int state;  /* 0 free, 1 set up by the player (not yet seen), 2 playing */
    const struct clip *c;
    size_t pos;
    float vol;
};

static struct pw_thread_loop *tloop;
static struct pw_stream *stream;
static bool ready;
static struct voice voices[VOICES];
static struct clip *clips;            /* decoded; kept for the process's life */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint played;
static atomic_llong last_sound_ms;
static bool active;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------- decoding */

/* Interleaved frames at `rate` with `ch` channels -> 48 kHz stereo. */
static float *to_stereo48(const float *in, size_t n, int ch, int rate, size_t *out_n)
{
    if (ch < 1 || rate < 1000) return NULL;
    size_t m = (size_t)((double)n * RATE / rate);
    float *o = malloc((m ? m : 1) * 2 * sizeof *o);
    if (!o) return NULL;
    for (size_t i = 0; i < m; i++) {
        double src = (double)i * rate / RATE;
        size_t a = (size_t)src;
        double f = src - (double)a;
        size_t b = a + 1 < n ? a + 1 : a;
        for (int c = 0; c < 2; c++) {
            int k = ch == 1 ? 0 : c;
            o[2 * i + (size_t)c] = (float)((1 - f) * in[a * (size_t)ch + (size_t)k] + f * in[b * (size_t)ch + (size_t)k]);
        }
    }
    *out_n = m;
    return o;
}

#ifdef HAVE_SNDFILE
static float *decode(const char *path, size_t *n)
{
    SF_INFO info = { 0 };
    SNDFILE *f = sf_open(path, SFM_READ, &info);
    if (!f) return NULL;
    float *raw = NULL;
    if (info.frames > 0 && info.frames < (sf_count_t)RATE * 60 && info.channels > 0 && info.channels <= 8)
        raw = malloc((size_t)info.frames * (size_t)info.channels * sizeof *raw);
    sf_count_t got = raw ? sf_readf_float(f, raw, info.frames) : 0;
    sf_close(f);
    float *o = got > 0 ? to_stereo48(raw, (size_t)got, info.channels, info.samplerate, n) : NULL;
    free(raw);
    return o;
}
const char *sound_formats(void) { return "wav ogg flac opus"; }
#else
/* RIFF WAVE, PCM 8/16/24/32-bit or 32-bit float. */
static float *decode(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[12];
    float *o = NULL, *raw = NULL;
    int ch = 0, rate = 0, bits = 0, fmt = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto out;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) goto out;
        uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fm[16];
            if (len < 16 || fread(fm, 1, 16, f) != 16) goto out;
            fmt = fm[0] | fm[1] << 8;
            ch = fm[2] | fm[3] << 8;
            rate = (int)(fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24);
            bits = fm[14] | fm[15] << 8;
            if (fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR)) goto out;
        } else if (!memcmp(ck, "data", 4)) {
            if (!ch || !bits || len > 64u << 20) goto out;
            unsigned char *d = malloc(len ? len : 1);
            if (!d || fread(d, 1, len, f) != len) { free(d); goto out; }
            size_t bps = (size_t)bits / 8, frames = len / (bps * (size_t)ch);
            raw = malloc((frames ? frames : 1) * (size_t)ch * sizeof *raw);
            for (size_t i = 0; raw && i < frames * (size_t)ch; i++) {
                const unsigned char *p = d + i * bps;
                float v = 0;
                if (fmt == 3 && bits == 32) memcpy(&v, p, 4);
                else if (bits == 8) v = (p[0] - 128) / 128.0f;
                else if (bits == 16) v = (int16_t)(p[0] | p[1] << 8) / 32768.0f;
                else if (bits == 24) v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
                else if (bits == 32) v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.0f;
                raw[i] = v;
            }
            free(d);
            if (raw) o = to_stereo48(raw, frames, ch, rate, n);
            goto out;
        } else if (fseek(f, (long)(len + (len & 1)), SEEK_CUR)) {
            goto out;
        }
    }
out:
    free(raw);
    fclose(f);
    return o;
}
const char *sound_formats(void) { return "wav"; }
#endif

static const struct clip *clip_for(const char *path)
{
    pthread_mutex_lock(&mu);
    for (struct clip *c = clips; c; c = c->next)
        if (!strcmp(c->path, path)) { pthread_mutex_unlock(&mu); return c->frames ? c : NULL; }
    pthread_mutex_unlock(&mu);
    size_t n = 0;
    float *fr = decode(path, &n);
    struct clip *c = calloc(1, sizeof *c);
    if (!c) { free(fr); return NULL; }
    c->path = strdup(path);
    c->frames = fr;   /* NULL: unreadable, remembered so it is not tried again */
    c->n = n;
    pthread_mutex_lock(&mu);
    c->next = clips;
    clips = c;
    pthread_mutex_unlock(&mu);
    return fr ? c : NULL;
}

/* -------------------------------------------------------------- playing */

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(stream, b); return; }
    uint32_t n = buf->datas[0].maxsize / (2 * sizeof(float));
    if (b->requested && b->requested < n) n = (uint32_t)b->requested;
    memset(dst, 0, (size_t)n * 2 * sizeof(float));
    for (int k = 0; k < VOICES; k++) {
        struct voice *v = &voices[k];
        int st = atomic_load(&v->state);
        if (!st) continue;
        if (st == 1) atomic_store(&v->state, 2);
        size_t left = v->c->n - v->pos, m = left < n ? left : n;
        const float *src = v->c->frames + 2 * v->pos;
        for (size_t i = 0; i < 2 * m; i++) dst[i] += src[i] * v->vol;
        v->pos += m;
        if (v->pos >= v->c->n) atomic_store(&v->state, 0);
    }
    for (size_t i = 0; i < (size_t)n * 2; i++) dst[i] = dst[i] > 1 ? 1 : dst[i] < -1 ? -1 : dst[i];
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = 2 * sizeof(float);
    buf->datas[0].chunk->size = n * 2 * sizeof(float);
    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events stream_events = { PW_VERSION_STREAM_EVENTS, .process = on_process };

/* Pause the stream when nothing has played for a while (no wake-ups every
 * 5 ms for silence). Runs on the PipeWire loop. */
static void idle_check(void *data, uint64_t expirations)
{
    (void)data; (void)expirations;
    bool any = false;
    for (int k = 0; k < VOICES; k++) any |= atomic_load(&voices[k].state) != 0;
    if (active && !any && now_ms() - atomic_load(&last_sound_ms) > IDLE_MS) {
        pw_stream_set_active(stream, false);
        active = false;
    }
}

bool sound_init(void)
{
    if (ready) return true;
    if (getenv("WPS_NO_SOUND")) return false;
    pw_init(NULL, NULL);
    tloop = pw_thread_loop_new("wps-sound", NULL);
    if (!tloop) return false;
    stream = pw_stream_new_simple(pw_thread_loop_get_loop(tloop), "WPS sounds",
                                  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                                    PW_KEY_MEDIA_ROLE, "Event", PW_KEY_NODE_LATENCY, "256/48000",
                                                    PW_KEY_APP_NAME, "Workplace Shell", NULL),
                                  &stream_events, NULL);
    if (!stream) { pw_thread_loop_destroy(tloop); tloop = NULL; return false; }
    uint8_t pb[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pb, sizeof pb);
    struct spa_audio_info_raw info = { .format = SPA_AUDIO_FORMAT_F32, .rate = RATE, .channels = 2,
                                       .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR } };
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
    if (pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                              PW_STREAM_FLAG_INACTIVE, params, 1) < 0) {
        pw_stream_destroy(stream);
        pw_thread_loop_destroy(tloop);
        stream = NULL;
        tloop = NULL;
        return false;
    }
    struct spa_source *timer = pw_loop_add_timer(pw_thread_loop_get_loop(tloop), idle_check, NULL);
    struct timespec one = { 1, 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(tloop), timer, &one, &one, false);
    pw_thread_loop_start(tloop);
    ready = true;
    return true;
}

void sound_shutdown(void)
{
    if (!ready) return;
    pw_thread_loop_stop(tloop);
    pw_stream_destroy(stream);
    pw_thread_loop_destroy(tloop);
    /* No pw_deinit: the process is ending, and unloading PipeWire's
     * plug-ins first would hide whose allocations they are from the leak
     * checker (tests/gui/lsan.supp names them). */
    ready = false;
    while (clips) {
        struct clip *c = clips;
        clips = c->next;
        free(c->path);
        free(c->frames);
        free(c);
    }
}

bool sound_play(const char *path, float volume)
{
    if (!ready || !path || !*path || volume <= 0) return false;
    const struct clip *c = clip_for(path);
    if (!c || !c->n) return false;
    for (int k = 0; k < VOICES; k++) {
        struct voice *v = &voices[k];
        if (atomic_load(&v->state)) continue;
        v->c = c;
        v->pos = 0;
        v->vol = volume > 1 ? 1 : volume;
        atomic_store(&v->state, 1);
        atomic_store(&last_sound_ms, now_ms());
        atomic_fetch_add(&played, 1);
        pw_thread_loop_lock(tloop);
        if (!active) { pw_stream_set_active(stream, true); active = true; }
        pw_thread_loop_unlock(tloop);
        return true;
    }
    return false;   /* eight at once: this one is dropped */
}

unsigned sound_played(void) { return atomic_load(&played); }
bool sound_ready(void) { return ready; }
