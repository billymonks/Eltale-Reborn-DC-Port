// Converts Eltale Reborn's ambient sound for the Dreamcast disc. eltale/ambience holds the PC mod's
// clips (its sfx/*.ogg) and the table that lists them (its include/q64/ambience_clips.h: id, file,
// gain in dB, loops). Each clip is resampled to 22050 Hz, normalised and encoded in the AICA's 4-bit
// ADPCM (runtime/mod/ambience_pack.h has the layout).
//
// Stereo clips whose sides are nearly alike become mono, kept as loud per channel as before. A loop's
// resampling wraps around its end, and its ADPCM starts from the decoder state that its own end leaves,
// so it repeats without a click. A one-shot gets a short silence after it.
//
// usage: ambpack <ambience_clips.h> <dir> <out.bin>    write the pack
//        ambpack -i <ambience_clips.h> <dir>           print each clip's statistics

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_vorbis.c"
#include "../../runtime/mod/ambience_pack.h"

#define MONO_SIDE_DB  (-20.0)  // side/mid energy below this: one channel is enough
#define PEAK_DBFS     (-1.0)
#define ONESHOT_PAD   4410     // samples of silence after a one-shot
#define AICA_MAX_LEN  65535    // the AICA's sample length limit (16-bit loop end)
#define ZEROS         16       // the resampler's sinc zero crossings each side
#define KAISER_BETA   8.0

typedef struct {
    char id[32];
    char file[64];
    double gain_db;
    int loops;
} ClipDef;

typedef struct {
    float* ch[2];
    int channels;
    int frames;
    int rate;
} Audio;

static void fail(const char* msg, const char* arg) {
    fprintf(stderr, "ambpack: %s%s\n", msg, arg ? arg : "");
    exit(1);
}

// X(AMB_FIELD, "birds-with-wind-field", 13.9, 1) lines, in the mod's enum order.
static int read_clips(const char* path, ClipDef* defs) {
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        fail("can't open ", path);
    }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char* x = strstr(line, "X(AMB_");
        if (x == NULL) {
            continue;
        }
        ClipDef d;
        if (sscanf(x, "X(%31[^,], \"%63[^\"]\", %lf, %d)", d.id, d.file, &d.gain_db, &d.loops) != 4) {
            fail("can't read the clip line: ", line);
        }
        if (n >= AMB_PACK_MAX) {
            fail("too many clips", NULL);
        }
        defs[n++] = d;
    }
    fclose(f);
    if (n == 0) {
        fail("no clips in ", path);
    }
    return n;
}

static Audio load_ogg(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ogg", dir, name);
    int channels = 0, rate = 0;
    short* pcm = NULL;
    int frames = stb_vorbis_decode_filename(path, &channels, &rate, &pcm);
    if (frames <= 0 || channels < 1) {
        fail("can't decode ", path);
    }
    Audio a = { { NULL, NULL }, channels > 1 ? 2 : 1, frames, rate };
    for (int c = 0; c < a.channels; c++) {
        a.ch[c] = malloc((size_t)frames * sizeof(float));
        for (int i = 0; i < frames; i++) {
            a.ch[c][i] = pcm[(size_t)i * channels + c] / 32768.0f;
        }
    }
    free(pcm);
    return a;
}

static double energy(const float* x, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) {
        s += (double)x[i] * x[i];
    }
    return s / n;
}

static double db(double power) {
    return power > 0 ? 10.0 * log10(power) : -999.0;
}

static double peak(const float* x, int n) {
    double p = 0;
    for (int i = 0; i < n; i++) {
        double v = fabs(x[i]);
        p = v > p ? v : p;
    }
    return p;
}

// The loudest 400 ms, as a power: what the PC mod's gains bring one-shots to.
static double loudest_400ms(const Audio* a) {
    int w = a->rate * 4 / 10;
    double best = 0;
    for (int start = 0; start + w <= a->frames; start += a->rate / 50) {
        double s = 0;
        for (int c = 0; c < a->channels; c++) {
            s += energy(a->ch[c] + start, w);
        }
        best = s / a->channels > best ? s / a->channels : best;
    }
    return best;
}

static double side_mid_db(const Audio* a) {
    if (a->channels < 2) {
        return -999.0;
    }
    double side = 0, mid = 0;
    for (int i = 0; i < a->frames; i++) {
        double m = 0.5 * (a->ch[0][i] + a->ch[1][i]), s = 0.5 * (a->ch[0][i] - a->ch[1][i]);
        mid += m * m;
        side += s * s;
    }
    return db(side / (mid > 0 ? mid : 1e-30));
}

static double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; k++) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

// Windowed-sinc resampling to `rate`. A loop becomes an even number of samples spanning exactly its
// input, read around its end; a one-shot is read as silence past both ends.
static float* resample(const float* in, int n, int fin, int fout, int loops, int* out_n) {
    int m;
    double step;
    if (loops) {
        m = (int)floor((double)n * fout / fin + 0.5) & ~1;
        step = (double)n / m;
    } else {
        m = (int)ceil((double)n * fout / fin);
        step = (double)fin / fout;
    }
    double fc = 0.45 * fout / fin;  // cycles per input sample
    if (fc > 0.45) {
        fc = 0.45;
    }
    double half = ZEROS / (2.0 * fc);
    int sub = 64;
    int tn = (int)(half * sub) + 2;
    float* table = malloc((size_t)tn * sizeof(float));
    double i0b = bessel_i0(KAISER_BETA);
    for (int i = 0; i < tn; i++) {
        double t = (double)i / sub;
        double u = t / half;
        double w = u < 1.0 ? bessel_i0(KAISER_BETA * sqrt(1.0 - u * u)) / i0b : 0.0;
        double x = 2.0 * fc * t;
        double s = x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x);
        table[i] = (float)(2.0 * fc * s * w);
    }
    int reach = (int)ceil(half);
    float* out = malloc((size_t)m * sizeof(float));
    for (int j = 0; j < m; j++) {
        double t = j * step;
        int k0 = (int)floor(t);
        double acc = 0;
        for (int k = k0 - reach + 1; k <= k0 + reach; k++) {
            double d = fabs(t - k) * sub;
            int di = (int)d;
            if (di + 1 >= tn) {
                continue;
            }
            float h = table[di] + (float)(d - di) * (table[di + 1] - table[di]);
            float x;
            if (loops) {
                int kk = k % n;
                x = in[kk < 0 ? kk + n : kk];
            } else {
                x = k >= 0 && k < n ? in[k] : 0.0f;
            }
            acc += x * h;
        }
        out[j] = (float)acc;
    }
    free(table);
    *out_n = m;
    return out;
}

// The AICA's ADPCM decoder (KallistiOS utils/wav2adpcm's ymz_step), and an encoder that follows it.
typedef struct {
    int hist, step;
} Adpcm;

static void adpcm_step(Adpcm* s, int code) {
    static const int scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int mag = code & 7;
    int diff = ((1 + (mag << 1)) * s->step) >> 3;
    if (diff > 32767) {
        diff = 32767;
    }
    int v = (code & 8) ? s->hist - diff : s->hist + diff;
    int ns = (scale[mag] * s->step) >> 8;
    s->step = ns < 127 ? 127 : ns > 24576 ? 24576 : ns;
    s->hist = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

static int adpcm_code(Adpcm* s, int x) {
    int d = x - s->hist;
    int sign = d < 0 ? 8 : 0;
    int best = 0;
    long best_err = -1;
    for (int mag = 0; mag < 8; mag++) {
        Adpcm t = *s;
        adpcm_step(&t, mag | sign);
        long err = labs((long)x - t.hist);
        if (best_err < 0 || err < best_err) {
            best_err = err;
            best = mag;
        }
    }
    adpcm_step(s, best | sign);
    return best | sign;
}

// Encodes x into out (if not NULL). With marks, notes the state before every sample whose index,
// counted from base, starts a chunk.
#define CHUNK_SAMPLES (AMB_PACK_CHUNK * 2)

static Adpcm encode_marking(const int16_t* x, int n, uint8_t* out, Adpcm st, int base, Adpcm* marks) {
    for (int i = 0; i < n; i++) {
        if (marks != NULL && (base + i) % CHUNK_SAMPLES == 0) {
            marks[(base + i) / CHUNK_SAMPLES] = st;
        }
        int code = adpcm_code(&st, x[i]);
        if (out != NULL) {
            if (i & 1) {
                out[i >> 1] |= (uint8_t)(code << 4);
            } else {
                out[i >> 1] = (uint8_t)code;
            }
        }
    }
    return st;
}

static Adpcm encode(const int16_t* x, int n, uint8_t* out, Adpcm st) {
    return encode_marking(x, n, out, st, 0, NULL);
}

// The steps the decoder can reach from its key-on step of 127, by a shortest path: step_prev[s] is the
// step before s and step_mag[s] the code magnitude that got there (any of 0-3 shrinks the step alike).
static int step_prev[24577];
static int step_mag[24577];

static void find_step_paths(void) {
    static const int scale[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    static const int mags[5] = { 0, 4, 5, 6, 7 };
    static int queue[24577];
    int head = 0, tail = 0;
    for (int s = 0; s <= 24576; s++) {
        step_prev[s] = -1;
    }
    step_prev[127] = 127;
    queue[tail++] = 127;
    while (head < tail) {
        int s = queue[head++];
        for (int k = 0; k < 5; k++) {
            int ns = (scale[mags[k]] * s) >> 8;
            ns = ns < 127 ? 127 : ns > 24576 ? 24576 : ns;
            if (step_prev[ns] < 0) {
                step_prev[ns] = s;
                step_mag[ns] = mags[k];
                queue[tail++] = ns;
            }
        }
    }
}

// Codes that take the decoder from its key-on state (0, 127) to `to`, an even number of them; -1 if
// that can't be done in AMB_PACK_PRE. First, at step 127 (which codes of magnitude 0-3 leave there,
// moving the history by 15, 47, 79 or 111), the history goes to where the step's path needs it; then
// the path up to the step, each code's sign chosen (from the end back) to keep the history in range.
// Every code at step 127 moves the history an odd amount, so their count is as odd as the distance;
// when that would make the total odd, the preamble opens with a detour up and back to step 127 of
// three codes that moves it an even amount.
static int preamble(Adpcm to, uint8_t* codes) {
    if (to.step < 127 || to.step > 24576 || step_prev[to.step] < 0) {
        return -1;
    }
    int path[64], len = 0;
    int h = to.hist;
    for (int s = to.step; s != 127; s = step_prev[s]) {
        if (len == 64) {
            return -1;
        }
        int before = step_prev[s], mag = step_mag[s];
        int diff = ((1 + (mag << 1)) * before) >> 3;
        diff = diff > 32767 ? 32767 : diff;
        // This code added diff (or took it away) to arrive at h: before it, h was h - diff (or + diff).
        if (labs((long)h - diff) <= labs((long)h + diff)) {
            path[len++] = mag;
            h -= diff;
        } else {
            path[len++] = mag | 8;
            h += diff;
        }
    }
    int n = 0;
    Adpcm at = { 0, 127 };
    if (((h & 1) ^ (len & 1)) != 0) {
        static const uint8_t detour[3] = { 4, 0, 0 };
        for (int i = 0; i < 3; i++) {
            codes[n++] = detour[i];
            adpcm_step(&at, detour[i]);
        }
        if (at.step != 127 || (at.hist & 1) != 0) {
            return -1;
        }
    }
    int dist = h - at.hist;
    int big = abs(dist) / 111;
    for (int i = 0; i < big; i++) {
        codes[n++] = dist < 0 ? 3 | 8 : 3;
    }
    // The rest with the fewest codes, by a search over small values.
    int rest = dist - (dist < 0 ? -111 : 111) * big;
    int need_odd = rest & 1;
    enum { SPAN = 1024, WIDTH = 2 * SPAN + 1 };
    static int prev_v[WIDTH][2], prev_c[WIDTH][2];
    static int vq[WIDTH * 2], pq[WIDTH * 2];
    for (int v = 0; v < WIDTH; v++) {
        prev_v[v][0] = prev_v[v][1] = INT32_MIN;
    }
    int qh = 0, qt = 0;
    prev_v[SPAN][0] = SPAN;
    vq[qt] = SPAN;
    pq[qt++] = 0;
    static const int amount[4] = { 15, 47, 79, 111 };
    while (qh < qt && prev_v[rest + SPAN][need_odd] == INT32_MIN) {
        int v = vq[qh], p = pq[qh++];
        for (int c = 0; c < 8; c++) {
            int nv = v + ((c & 4) ? -amount[c & 3] : amount[c & 3]);
            if (nv < 0 || nv >= WIDTH || prev_v[nv][p ^ 1] != INT32_MIN) {
                continue;
            }
            prev_v[nv][p ^ 1] = v;
            prev_c[nv][p ^ 1] = (c & 3) | ((c & 4) ? 8 : 0);
            vq[qt] = nv;
            pq[qt++] = p ^ 1;
        }
    }
    if (rest + SPAN < 0 || rest + SPAN >= WIDTH || prev_v[rest + SPAN][need_odd] == INT32_MIN) {
        return -1;
    }
    int extra[64], ne = 0;
    for (int v = rest + SPAN, p = need_odd; !(v == SPAN && p == 0);) {
        if (ne == 64) {
            return -1;
        }
        extra[ne++] = prev_c[v][p];
        int pv = prev_v[v][p];
        v = pv;
        p ^= 1;
    }
    if (n + ne + len > AMB_PACK_PRE) {
        return -1;
    }
    for (int i = ne - 1; i >= 0; i--) {
        codes[n++] = (uint8_t)extra[i];
    }
    for (int i = len - 1; i >= 0; i--) {
        codes[n++] = (uint8_t)path[i];
    }
    Adpcm st = { 0, 127 };
    for (int i = 0; i < n; i++) {
        adpcm_step(&st, codes[i]);
    }
    return st.hist == to.hist && st.step == to.step && (n & 1) == 0 ? n : -1;
}

// A loop's start state: the one its end leaves, found by encoding it over and over from the state
// the last pass ended in. Usually the passes settle on one state; sometimes they go round a cycle of
// a few, and then the loop is stored as that many passes, which end where the first began. 0 laps:
// neither within MAX_ROUNDS.
#define MAX_ROUNDS 64

static Adpcm loop_state(const int16_t* x, int n, int* laps) {
    Adpcm seen[MAX_ROUNDS];
    Adpcm st = { 0, 127 };
    int pre = n < 8192 ? n : 8192;
    st = encode(x + n - pre, pre, NULL, st);
    for (int round = 0; round < MAX_ROUNDS; round++) {
        seen[round] = st;
        st = encode(x, n, NULL, st);
        for (int j = round; j >= 0; j--) {
            if (seen[j].hist == st.hist && seen[j].step == st.step) {
                *laps = round - j + 1;
                return seen[j];
            }
        }
    }
    *laps = 0;
    return st;
}

// The PC mod's muffle filter (miniaudio's second-order low-pass at 700 Hz), run twice over a loop so
// the second pass starts settled. Adds E[x^2], E[w^2] and E[x w] for w = 1.4 lowpass(x).
static void muffle_energy(const float* x, int n, int fs, int loops, double* ex, double* ew, double* exw) {
    double w0 = 2.0 * M_PI * 700.0 / fs, alpha = sin(w0) / (2.0 * sqrt(0.5)), c = cos(w0);
    double a0 = 1 + alpha;
    double b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0, b2 = (1 - c) / 2 / a0;
    double a1 = -2 * c / a0, a2 = (1 - alpha) / a0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int pass = loops ? 0 : 1; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            double y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x[i];
            y2 = y1;
            y1 = y;
            if (pass == 1) {
                double w = 1.4 * y;
                *ex += (double)x[i] * x[i];
                *ew += w * w;
                *exw += x[i] * w;
            }
        }
    }
}

static void to_int16(const float* x, int n, double scale, int16_t* out) {
    for (int i = 0; i < n; i++) {
        double v = x[i] * scale * 32767.0;
        long r = lround(v);
        out[i] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

static void info(const ClipDef* d, const Audio* a) {
    double e[2] = { energy(a->ch[0], a->frames), a->channels > 1 ? energy(a->ch[1], a->frames) : 0 };
    double rms = a->channels > 1 ? (e[0] + e[1]) / 2 : e[0];
    double pk = peak(a->ch[0], a->frames);
    if (a->channels > 1) {
        double p1 = peak(a->ch[1], a->frames);
        pk = p1 > pk ? p1 : pk;
    }
    printf("%-15s %6d Hz %d ch %7.2f s  peak %6.1f  rms %6.1f dBFS  side/mid %6.1f dB  gain %+5.1f -> rms %6.1f",
           d->id, a->rate, a->channels, (double)a->frames / a->rate, db(pk * pk), db(rms), side_mid_db(a), d->gain_db,
           db(rms) + d->gain_db);
    if (!d->loops) {
        printf("  loudest 400 ms %6.1f -> %6.1f", db(loudest_400ms(a)), db(loudest_400ms(a)) + d->gain_db);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    int info_only = argc == 4 && strcmp(argv[1], "-i") == 0;
    if (!info_only && argc != 4) {
        fprintf(stderr, "usage: ambpack <ambience_clips.h> <dir> <out.bin>\n       ambpack -i <ambience_clips.h> <dir>\n");
        return 1;
    }
    const char* clips_h = info_only ? argv[2] : argv[1];
    const char* dir = info_only ? argv[3] : argv[2];
    ClipDef defs[AMB_PACK_MAX];
    int count = read_clips(clips_h, defs);

    if (info_only) {
        for (int i = 0; i < count; i++) {
            Audio a = load_ogg(dir, defs[i].file);
            info(&defs[i], &a);
            for (int c = 0; c < a.channels; c++) {
                free(a.ch[c]);
            }
        }
        return 0;
    }

    FILE* out = fopen(argv[3], "wb");
    if (out == NULL) {
        fail("can't write ", argv[3]);
    }
    find_step_paths();
    AmbPackHeader hdr = { AMB_PACK_MAGIC, (uint32_t)count, AMB_PACK_RATE, AMB_PACK_CHUNK, 0, { 0 } };
    AmbPackClip clips[AMB_PACK_MAX];
    memset(clips, 0, sizeof(clips));
    size_t starts_size = (size_t)count * AMB_PACK_STARTS * sizeof(AmbPackStart);
    AmbPackStart* starts_all = calloc(1, starts_size);
    uint32_t pos = (uint32_t)(sizeof(hdr) + sizeof(clips) + starts_size);
    pos = (pos + AMB_PACK_ALIGN - 1) & ~(uint32_t)(AMB_PACK_ALIGN - 1);
    hdr.data = pos;
    fseek(out, pos, SEEK_SET);

    for (int i = 0; i < count; i++) {
        const ClipDef* d = &defs[i];
        Audio a = load_ogg(dir, d->file);
        info(d, &a);
        for (int c = 0; c < a.channels; c++) {
            double mean = 0;
            for (int k = 0; k < a.frames; k++) {
                mean += a.ch[c][k];
            }
            mean /= a.frames;
            for (int k = 0; k < a.frames; k++) {
                a.ch[c][k] -= (float)mean;
            }
        }

        // One channel when the sides are nearly alike, scaled to keep each speaker's level.
        int channels = a.channels;
        if (channels == 2 && side_mid_db(&a) < MONO_SIDE_DB) {
            double target = (energy(a.ch[0], a.frames) + energy(a.ch[1], a.frames)) / 2;
            for (int k = 0; k < a.frames; k++) {
                a.ch[0][k] = 0.5f * (a.ch[0][k] + a.ch[1][k]);
            }
            double now = energy(a.ch[0], a.frames);
            float k_mono = (float)sqrt(target / (now > 0 ? now : 1e-30));
            for (int k = 0; k < a.frames; k++) {
                a.ch[0][k] *= k_mono;
            }
            channels = 1;
        }

        float* res[2] = { NULL, NULL };
        int m = 0;
        for (int c = 0; c < channels; c++) {
            res[c] = resample(a.ch[c], a.frames, a.rate, AMB_PACK_RATE, d->loops, &m);
        }
        double pk = 0;
        for (int c = 0; c < channels; c++) {
            double p = peak(res[c], m);
            pk = p > pk ? p : pk;
        }
        double scale = pk > 0 ? pow(10.0, PEAK_DBFS / 20.0) / pk : 1.0;
        double ex = 0, ew = 0, exw = 0;
        for (int c = 0; c < channels; c++) {
            muffle_energy(res[c], m, AMB_PACK_RATE, d->loops, &ex, &ew, &exw);
        }

        // A loop's channels each find their start state, and the clip is as many passes as their cycles
        // need together.
        int16_t* pcm[2] = { NULL, NULL };
        Adpcm start[2] = { { 0, 127 }, { 0, 127 } };
        int laps = 1;
        for (int c = 0; c < channels; c++) {
            pcm[c] = calloc((size_t)m + ONESHOT_PAD + 2, sizeof(int16_t));
            to_int16(res[c], m, scale, pcm[c]);
            if (d->loops) {
                int cycle = 0;
                start[c] = loop_state(pcm[c], m, &cycle);
                if (cycle == 0) {
                    fprintf(stderr, "ambpack: %s's ADPCM doesn't come back to its start state\n", d->id);
                    return 1;
                }
                int a = laps, b = cycle;
                while (b != 0) {
                    int t = a % b;
                    a = b;
                    b = t;
                }
                laps = laps / a * cycle;
            }
        }
        int audible = m * laps;
        int total = d->loops ? m * laps : (m + ONESHOT_PAD + 1) & ~1;
        if (!d->loops && total > AICA_MAX_LEN) {
            fprintf(stderr, "ambpack: %s is %d samples with its padding, over the AICA's %d\n", d->id, total,
                    AICA_MAX_LEN);
            return 1;
        }
        int bytes = total / 2;
        int chunks = (bytes + AMB_PACK_CHUNK - 1) / AMB_PACK_CHUNK;
        uint8_t* data = calloc((size_t)chunks * AMB_PACK_CHUNK * channels, 1);
        uint8_t* adpcm = calloc((size_t)chunks * AMB_PACK_CHUNK, 1);
        Adpcm* marks = calloc((size_t)chunks + 1, sizeof(Adpcm));
        static uint8_t pre[AMB_PACK_STARTS][2][AMB_PACK_PRE];
        int pre_n[AMB_PACK_STARTS][2] = { { 0 } };
        AmbPackStart* starts = starts_all + (size_t)i * AMB_PACK_STARTS;
        for (int c = 0; c < channels; c++) {
            memset(adpcm, 0, (size_t)chunks * AMB_PACK_CHUNK);
            if (d->loops) {
                Adpcm st = start[c];
                for (int lap = 0; lap < laps; lap++) {
                    // Each pass is m samples (even), so it starts on a whole byte.
                    st = encode_marking(pcm[c], m, adpcm + (size_t)lap * m / 2, st, lap * m, marks);
                }
                if (st.hist != start[c].hist || st.step != start[c].step) {
                    fprintf(stderr, "ambpack: %s's %d passes don't end where they began\n", d->id, laps);
                    return 1;
                }
                for (int j = 0; j < AMB_PACK_STARTS; j++) {
                    int ck = j * chunks / AMB_PACK_STARTS;
                    starts[j].sample = (uint32_t)ck * CHUNK_SAMPLES;
                    pre_n[j][c] = preamble(marks[ck], pre[j][c]);
                    if (pre_n[j][c] < 0) {
                        fprintf(stderr, "ambpack: no preamble reaches %s's state (%d, %d) at sample %d\n", d->id,
                                marks[ck].hist, marks[ck].step, ck * CHUNK_SAMPLES);
                        return 1;
                    }
                }
            } else {
                encode(pcm[c], total, adpcm, start[c]);
            }
            for (int k = 0; k < chunks; k++) {
                memcpy(data + ((size_t)k * channels + c) * AMB_PACK_CHUNK, adpcm + (size_t)k * AMB_PACK_CHUNK,
                       AMB_PACK_CHUNK);
            }
        }
        size_t size = (size_t)chunks * AMB_PACK_CHUNK * channels;
        fwrite(data, 1, size, out);

        // Both channels' preambles as long as the longer one, and a multiple of 8 codes so the data
        // after them starts on a 4-byte boundary in sound RAM: the shorter ones start with pairs of
        // codes that go up 15 and back.
        int longest_pre = 0;
        for (int j = 0; d->loops && j < AMB_PACK_STARTS; j++) {
            int len = pre_n[j][0] > pre_n[j][1] ? pre_n[j][0] : pre_n[j][1];
            len = (len + 7) & ~7;
            if (len > AMB_PACK_PRE) {
                fprintf(stderr, "ambpack: %s's preamble at start %d is too long\n", d->id, j);
                return 1;
            }
            for (int c = 0; c < channels; c++) {
                uint8_t codes[AMB_PACK_PRE];
                int pad = len - pre_n[j][c];
                for (int k = 0; k < pad; k++) {
                    codes[k] = (k & 1) ? 8 : 0;
                }
                memcpy(codes + pad, pre[j][c], (size_t)pre_n[j][c]);
                for (int k = 0; k < len; k++) {
                    starts[j].pre[c][k >> 1] |= (uint8_t)(codes[k] << ((k & 1) * 4));
                }
            }
            starts[j].codes = (uint16_t)len;
            longest_pre = len > longest_pre ? len : longest_pre;
        }
        free(marks);

        AmbPackClip* clip = &clips[i];
        clip->offset = pos;
        clip->samples = (uint32_t)total;
        clip->audible = (uint32_t)audible;
        clip->gain_mdb = (int32_t)lround((d->gain_db - 20.0 * log10(scale)) * 1000.0);
        clip->wet = ex > 0 ? (float)(ew / ex) : 0.0f;
        clip->cross = ex > 0 ? (float)(exw / ex) : 0.0f;
        clip->channels = (uint8_t)channels;
        clip->loops = (uint8_t)d->loops;
        snprintf(clip->name, sizeof(clip->name), "%s", d->file);
        printf("  -> %d ch, %d samples (%.2f s, %d pass%s), %zu KB, gain %+.2f dB, muffled %+.1f dB", channels,
               total, (double)total / AMB_PACK_RATE, laps, laps > 1 ? "es for its ADPCM" : "", size / 1024,
               clip->gain_mdb / 1000.0, db(clip->wet));
        if (d->loops) {
            printf(", preambles up to %d codes", longest_pre);
        }
        printf("\n");
        pos += (uint32_t)size;
        uint32_t aligned = (pos + AMB_PACK_ALIGN - 1) & ~(uint32_t)(AMB_PACK_ALIGN - 1);
        static const uint8_t zero[AMB_PACK_ALIGN];
        fwrite(zero, 1, aligned - pos, out);
        pos = aligned;

        free(data);
        free(adpcm);
        for (int c = 0; c < 2; c++) {
            free(pcm[c]);
            free(res[c]);
            free(a.ch[c]);
        }
    }
    fseek(out, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, out);
    fwrite(clips, sizeof(clips), 1, out);
    fwrite(starts_all, 1, starts_size, out);
    fclose(out);
    free(starts_all);
    printf("ambpack: %d clips, %u KB\n", count, (unsigned)(pos / 1024));
    return 0;
}
