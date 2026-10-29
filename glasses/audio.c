/*
 * audio.c - I2S microphone capture via ALSA.
 *
 * The voiceHAT driver only does 48 kHz / 32-bit / stereo, so each 80 ms
 * chunk is averaged down to 16 kHz / 16-bit / mono (the format the iOS
 * wake-word detector wants). All buffers are fixed-size and allocated once.
 */
#include "audio.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ALSA_DEVICE  "plughw:CARD=sndrpigooglevoi,DEV=0"
#define HW_RATE      48000
#define HW_CHANNELS  2
#define DECIMATE     (HW_RATE / AUDIO_RATE)     /* 3 */
#define GAIN         10.0f      /* raise if too quiet, lower if CLIPPING shows in logs */
#define LOG_SECONDS  5
#define RING_SAMPLES (AUDIO_BUFFER_SECONDS * AUDIO_RATE)
#define MAX_SUBS     8
#define SUB_QUEUE    50         /* chunks per subscriber before we drop (never block the mic) */

struct audio_sub {
    int16_t chunks[SUB_QUEUE][AUDIO_CHUNK];
    int head, count;
    pthread_cond_t cond;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t ring[RING_SAMPLES];
static size_t ring_pos, ring_count;
static audio_sub_t *subs[MAX_SUBS];
static int n_subs;

/* S32_LE stereo 48 kHz -> int16 mono 16 kHz: average 3 frames x 2 channels */
static void to_16k_mono(const int32_t *raw, int16_t *out)
{
    for (int i = 0; i < AUDIO_CHUNK; i++) {
        double sum = 0;
        for (int k = 0; k < DECIMATE * HW_CHANNELS; k++)
            sum += raw[i * DECIMATE * HW_CHANNELS + k];
        float s = (float)(sum / (DECIMATE * HW_CHANNELS) / 65536.0) * GAIN;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        out[i] = (int16_t)s;
    }
}

static void publish(const int16_t *pcm)
{
    pthread_mutex_lock(&lock);

    for (int i = 0; i < AUDIO_CHUNK; i++) {
        ring[ring_pos] = pcm[i];
        ring_pos = (ring_pos + 1) % RING_SAMPLES;
    }
    ring_count = ring_count + AUDIO_CHUNK > RING_SAMPLES ? RING_SAMPLES : ring_count + AUDIO_CHUNK;

    for (int i = 0; i < MAX_SUBS; i++) {
        audio_sub_t *s = subs[i];
        if (!s || s->count == SUB_QUEUE)
            continue;   /* slow client: drop this chunk */
        memcpy(s->chunks[(s->head + s->count) % SUB_QUEUE], pcm, sizeof(s->chunks[0]));
        s->count++;
        pthread_cond_signal(&s->cond);
    }

    pthread_mutex_unlock(&lock);
}

static void *audio_thread(void *arg)
{
    snd_pcm_t *pcm = arg;
    static int32_t raw[AUDIO_CHUNK * DECIMATE * HW_CHANNELS];
    int16_t out[AUDIO_CHUNK];
    double sumsq = 0;
    int peak = 0;
    long logged = 0;

    printf("Microphone active...\n");
    for (;;) {
        snd_pcm_uframes_t got = 0, want = AUDIO_CHUNK * DECIMATE;
        while (got < want) {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, raw + got * HW_CHANNELS, want - got);
            if (n < 0 && snd_pcm_recover(pcm, n, 1) < 0) {
                /* Mic is gone: exit so systemd restarts us */
                printf("[audio] capture failed: %s -> exiting\n", snd_strerror(n));
                _exit(1);
            }
            if (n > 0)
                got += n;
        }

        to_16k_mono(raw, out);
        publish(out);

        /* Log every 5 seconds */
        for (int i = 0; i < AUDIO_CHUNK; i++) {
            sumsq += (double)out[i] * out[i];
            int a = abs(out[i]);
            if (a > peak)
                peak = a;
        }
        logged += AUDIO_CHUNK;
        if (logged >= LOG_SECONDS * AUDIO_RATE) {
            char ts[16];
            time_t now = time(NULL);
            strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));
            pthread_mutex_lock(&lock);
            int clients = n_subs;
            pthread_mutex_unlock(&lock);
            printf("[audio] %s %.1fs rms=%.0f peak=%d%s stream_clients=%d\n",
                   ts, (double)logged / AUDIO_RATE, sqrt(sumsq / logged), peak,
                   peak >= 32767 ? " CLIPPING" : "", clients);
            sumsq = 0;
            peak = 0;
            logged = 0;
        }
    }
    return NULL;
}

int audio_start(void)
{
    snd_pcm_t *pcm;
    pthread_t th;
    int err;

    if ((err = snd_pcm_open(&pcm, ALSA_DEVICE, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "[audio] cannot open %s: %s\n", ALSA_DEVICE, snd_strerror(err));
        return -1;
    }
    if ((err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  HW_CHANNELS, HW_RATE, 1, 500000)) < 0) {
        fprintf(stderr, "[audio] cannot set params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return -1;
    }
    /* set_params uses a playback-style start threshold (buffer almost full),
     * which never starts a capture stream: start as soon as we ask for data */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0) {
        fprintf(stderr, "[audio] cannot set sw params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return -1;
    }
    if (pthread_create(&th, NULL, audio_thread, pcm)) {
        snd_pcm_close(pcm);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

size_t audio_get_recent(int16_t *dst, size_t n)
{
    pthread_mutex_lock(&lock);
    if (n > ring_count)
        n = ring_count;
    size_t start = (ring_pos + RING_SAMPLES - n) % RING_SAMPLES;
    size_t first = n < RING_SAMPLES - start ? n : RING_SAMPLES - start;
    memcpy(dst, ring + start, first * sizeof(int16_t));
    memcpy(dst + first, ring, (n - first) * sizeof(int16_t));
    pthread_mutex_unlock(&lock);
    return n;
}

audio_sub_t *audio_subscribe(void)
{
    audio_sub_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    pthread_cond_init(&s->cond, NULL);

    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!subs[i]) {
            subs[i] = s;
            n_subs++;
            pthread_mutex_unlock(&lock);
            return s;
        }
    }
    pthread_mutex_unlock(&lock);
    pthread_cond_destroy(&s->cond);
    free(s);
    return NULL;
}

int audio_sub_read(audio_sub_t *s, int16_t chunk[AUDIO_CHUNK])
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;

    pthread_mutex_lock(&lock);
    while (s->count == 0) {
        if (pthread_cond_timedwait(&s->cond, &lock, &deadline)) {
            pthread_mutex_unlock(&lock);
            return 0;
        }
    }
    memcpy(chunk, s->chunks[s->head], sizeof(s->chunks[0]));
    s->head = (s->head + 1) % SUB_QUEUE;
    s->count--;
    pthread_mutex_unlock(&lock);
    return 1;
}

void audio_unsubscribe(audio_sub_t *s)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i] == s) {
            subs[i] = NULL;
            n_subs--;
        }
    }
    pthread_mutex_unlock(&lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}
