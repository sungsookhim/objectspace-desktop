/*
 * G0 prototype: how soon after an action can its sound be heard?
 *
 * Design (gui-design.md, Threads): a PipeWire stream stays open, writing
 * silence; sounds are decoded in advance, so an action event only marks a
 * sound to start. The latency from the action to the speaker is:
 *   the wait until the next process callback writes it into a buffer, plus
 *   what is already queued ahead of it (pw_stream_get_time_n: buffered +
 *   queued + the graph's delay).
 *
 * Plays COUNT short, quiet clicks at irregular intervals and prints the
 * statistics as JSON.
 *
 * usage: soundlat [COUNT]
 */
#define _GNU_SOURCE
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000
#define CLICK_FRAMES (RATE / 100)   /* 10 ms */

static struct pw_thread_loop *tloop;
static struct pw_stream *stream;
static float click[CLICK_FRAMES];
static atomic_int pending;          /* an action asked for the sound */
static atomic_llong asked_ns;
static int playing_at = -1;         /* position in the click, or -1 */
static double *wait_ms, *total_ms;
static atomic_int nsamples;
static int count = 30;

static long long mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void on_process(void *data)
{
    (void)data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;
    struct spa_buffer *buf = b->buffer;
    float *dst = buf->datas[0].data;
    if (!dst) { pw_stream_queue_buffer(stream, b); return; }
    uint32_t n = buf->datas[0].maxsize / sizeof(float);
    if (b->requested && b->requested < n) n = (uint32_t)b->requested;
    if (atomic_exchange(&pending, 0)) {
        long long now = mono_ns();
        struct pw_time t;
        double queued_ms = 0;
        if (pw_stream_get_time_n(stream, &t, sizeof t) == 0 && t.rate.denom) {
            /* Frames ahead of this buffer: still in the graph and device. */
            double frames = (double)t.delay * t.rate.num / t.rate.denom * RATE + (double)t.queued + (double)t.buffered;
            queued_ms = frames * 1000.0 / RATE;
            if (getenv("SOUNDLAT_DEBUG")) fprintf(stderr, "delay %lld rate %u/%u queued %llu buffered %llu\n", (long long)t.delay, t.rate.num, t.rate.denom, (unsigned long long)t.queued, (unsigned long long)t.buffered);
        } else if (getenv("SOUNDLAT_DEBUG")) {
            fprintf(stderr, "no time\n");
        }
        int i = atomic_load(&nsamples);
        if (i < count) {
            wait_ms[i] = (double)(now - atomic_load(&asked_ns)) / 1e6;
            total_ms[i] = wait_ms[i] + queued_ms;
            atomic_store(&nsamples, i + 1);
        }
        playing_at = 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        float v = 0;
        if (playing_at >= 0) {
            v = click[playing_at++];
            if (playing_at >= CLICK_FRAMES) playing_at = -1;
        }
        dst[i] = v;
    }
    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = sizeof(float);
    buf->datas[0].chunk->size = n * sizeof(float);
    pw_stream_queue_buffer(stream, b);
}

static const struct pw_stream_events events = { PW_VERSION_STREAM_EVENTS, .process = on_process };

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc > 1) count = atoi(argv[1]);
    if (count < 1 || count > 1000) count = 30;
    wait_ms = calloc((size_t)count, sizeof *wait_ms);
    total_ms = calloc((size_t)count, sizeof *total_ms);
    /* A soft click: a 2 kHz tone with a fast decay, at low volume. */
    for (int i = 0; i < CLICK_FRAMES; i++)
        click[i] = 0.08f * sinf(2 * (float)M_PI * 2000 * i / RATE) * expf(-(float)i / (RATE / 400.0f));

    pw_init(&argc, &argv);
    tloop = pw_thread_loop_new("wps-sound", NULL);
    stream = pw_stream_new_simple(pw_thread_loop_get_loop(tloop), "WPS sounds",
                                  pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                                                    PW_KEY_MEDIA_ROLE, "Event", PW_KEY_NODE_LATENCY, "256/48000", NULL),
                                  &events, NULL);
    uint8_t pb[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pb, sizeof pb);
    struct spa_audio_info_raw info = { .format = SPA_AUDIO_FORMAT_F32, .rate = RATE, .channels = 1 };
    const struct spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
    pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS, params, 1);
    pw_thread_loop_start(tloop);
    sleep(1);   /* the stream is open and running before any action */

    unsigned seed = 7;
    for (int i = 0; i < count; i++) {
        usleep(150000 + (useconds_t)(rand_r(&seed) % 200000));   /* actions at irregular times */
        atomic_store(&asked_ns, mono_ns());
        atomic_store(&pending, 1);
    }
    usleep(300000);
    pw_thread_loop_stop(tloop);
    pw_stream_destroy(stream);
    pw_thread_loop_destroy(tloop);
    pw_deinit();

    int n = atomic_load(&nsamples);
    qsort(wait_ms, (size_t)n, sizeof *wait_ms, cmp_d);
    qsort(total_ms, (size_t)n, sizeof *total_ms, cmp_d);
    #define P(v, p) (n ? (v)[(int)((p) / 100.0 * (n - 1) + 0.5)] : 0)
    printf("{\"sounds\":%d,\"wait_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f},"
           "\"to_speaker_ms\":{\"p50\":%.2f,\"p99\":%.2f,\"max\":%.2f}}\n",
           n, P(wait_ms, 50), P(wait_ms, 99), n ? wait_ms[n - 1] : 0, P(total_ms, 50), P(total_ms, 99),
           n ? total_ms[n - 1] : 0);
    return 0;
}
