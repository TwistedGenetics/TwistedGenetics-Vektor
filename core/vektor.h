#pragma once
/*
 * Vektor - vector synthesis oscillator (DSP core)
 *
 * Classic vector synthesis, Prophet VS / Wavestation style: four waveforms
 * sit on the corners of a square and an X/Y point mixes between them.
 *
 *        A ---------- B          X: left (A, C)  -> right (B, D)
 *        |            |          Y: bottom (C, D) -> top (A, B)
 *        |     +      |
 *        |            |          weights are bilinear, so every point
 *        C ---------- D          inside the square is a blend of all four
 *
 * Per voice:
 *   four table oscillators (one per corner), each with its own phase and
 *     a small detune spread (DETUNE)
 *   X/Y = knob position + virtual-patch modulation + the VECTOR PATH:
 *     a per-note movement of the X/Y point that restarts on every note
 *     (one-shot paths stop at their end point, looped paths keep moving)
 *   loudness compensation: the mix gets (sum w^2)^-1/4, so the centre of the
 *     square isn't 6 dB quieter than the corners
 *   BRIGHT: two one-pole low-passes with a key-tracked cutoff (2 x f0 at 0,
 *     off at 100), to take the edge off bright waves
 *
 * Wave tables:
 *   16 single-cycle waves x 9 band-limited levels (8192 .. 1024 samples),
 *   built once from harmonic spectra by inverse FFT at load time (~1.3 MB,
 *   nothing stored in the unit
 *   file). Level L holds 255 >> L harmonics; each corner picks the level
 *   that keeps its highest harmonic under 21 kHz, so nothing aliases.
 *   Waves are loudness-matched (RMS, with a peak ceiling), so moving
 *   between corners doesn't jump in level.
 *
 * CPU (the microKORG 2 runs up to 8 voices of this):
 *   - all the slow stuff (path, X/Y, weights, gain, mip level) is worked out
 *     once per buffer per voice; the sample loop is four linear-interpolated
 *     table reads with linearly ramped weights
 *   - a corner whose weight is zero at both ends of the buffer isn't read,
 *     only its phase is advanced
 *   - no divides, libm calls or allocation in the sample loop
 *
 * Plain C++14 (the microKORG 2 SDK builds with -std=gnu++14).
 */

#include <math.h>
#include <stdint.h>

namespace vk {

constexpr float kSR = 48000.f;
constexpr float kInvSR = 1.f / 48000.f;

constexpr int kMaxHarm = 255;
// Table length per level: VK_OVERSAMPLE samples per cycle of the level's top
// harmonic (8192 for 255 harmonics, 4096, 2048, then 1024 for the rest), so
// linear interpolation between table samples stays clean (error ~70 dB down
// or better). v1.0.0 used 512 for everything, which left interpolation error
// only 35-55 dB down: an inharmonic fizz/buzz, worst on low notes.
#ifndef VK_OVERSAMPLE
#define VK_OVERSAMPLE 32
#endif
constexpr int kBaseSize = 256 * VK_OVERSAMPLE;
#ifndef VK_MINSIZE
#define VK_MINSIZE 1024
#endif
constexpr int kMinSize = VK_MINSIZE;
constexpr int kNumLevels = 9;                 // 255, 127, 63, 31, 15, 7, 3, 1, 1 harmonics
constexpr int kNumWaves = 16;
constexpr int kNumCorners = 4;
constexpr float kHarmLimitHz = 21000.f;
constexpr float kOutGain = 0.7f;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline int levelHarmonics(int level) {
  const int h = kMaxHarm >> level;
  return h < 1 ? 1 : h;
}
static inline int levelSize(int level) {
  const int n = kBaseSize >> level;
  return n < kMinSize ? kMinSize : n;
}
static inline int levelOffset(int level) {  // within one wave's tables, +1 guard sample each
  int o = 0;
  for (int L = 0; L < level; ++L) o += levelSize(L) + 1;
  return o;
}
constexpr int wavePoolSize() {
  int o = 0;
  for (int L = 0; L < kNumLevels; ++L) o += ((kBaseSize >> L) < kMinSize ? kMinSize : (kBaseSize >> L)) + 1;
  return o;
}

// ---------------------------------------------------------------------------
// Waves
// ---------------------------------------------------------------------------

enum Wave {
  kSine = 0, kTri, kSaw, kSquare, kPulse, kNarrow, kOrgan, kGlass,
  kVoxA, kVoxO, kBrass, kReed, kComb, kMetal, kSync, kFold
};

static inline const char *waveName(int w) {
  static const char *const n[kNumWaves] = {"Sine", "Tri",   "Saw",  "Square", "Pulse", "Narrow", "Organ", "Glass",
                                           "Vox A", "Vox O", "Brass", "Reed",  "Comb",  "Metal",  "Sync",  "Fold"};
  return n[clampi(w, 0, kNumWaves - 1)];
}

// Formant envelope over frequency (Lorentzian peaks), for the vox waves.
static inline float formant(float f, const float *F, const float *B, const float *G) {
  float a = 0.004f;  // small floor between formants (0.02 in v1.0.0 was fizzy)
  for (int i = 0; i < 3; ++i) {
    const float d = (f - F[i]) / B[i];
    a += G[i] / (1.f + d * d);
  }
  return a;
}

// Fourier coefficients of a wave: x = sum s[h] sin(2 pi h t) + c[h] cos(2 pi h t)
static void waveSpectrum(int wave, float *s, float *c) {
  const float pi = 3.14159265358979f;
  for (int h = 0; h <= kMaxHarm; ++h) s[h] = c[h] = 0.f;

  // Time-domain waves: 4096-point DFT of the naive shape
  if (wave == kSync || wave == kFold) {
    enum { M = 4096 };
    static float x[M], sn[M];
    for (int n = 0; n < M; ++n) {
      const float t = (float)n / M;
      sn[n] = sinf(2.f * pi * t);
      if (wave == kSync) {
        const float u = 3.6f * t;  // hard-synced saw, slave at 3.6x the master
        x[n] = 2.f * (u - floorf(u)) - 1.f;
      } else {
        x[n] = sinf(2.6f * pi * sinf(2.f * pi * t));  // wavefolded sine
      }
    }
    for (int h = 1; h <= kMaxHarm; ++h) {
      double as = 0, ac = 0;
      for (int n = 0; n < M; ++n) {
        const int k = (h * n) & (M - 1);
        as += x[n] * sn[k];
        ac += x[n] * sn[(k + M / 4) & (M - 1)];
      }
      s[h] = (float)(2.0 * as / M);
      c[h] = (float)(2.0 * ac / M);
    }
    return;
  }

  static const float kVoxAF[3] = {730.f, 1090.f, 2440.f}, kVoxAB[3] = {90.f, 110.f, 160.f}, kVoxAG[3] = {1.f, 0.5f, 0.25f};
  static const float kVoxOF[3] = {450.f, 800.f, 2830.f}, kVoxOB[3] = {70.f, 90.f, 160.f}, kVoxOG[3] = {1.f, 0.6f, 0.12f};
  const float kVoxRef = 110.f;  // vox spectra are drawn for A2

  uint32_t rng = 0x2545F491u;  // fixed seed: Metal is the same on every load
  for (int h = 1; h <= kMaxHarm; ++h) {
    const float fh = (float)h;
    const bool odd = (h & 1) != 0;
    float a = 0.f;
    switch (wave) {
      case kSine: a = h == 1 ? 1.f : 0.f; break;
      case kTri: a = odd ? (((h >> 1) & 1) ? -1.f : 1.f) / (fh * fh) : 0.f; break;
      case kSaw: a = 1.f / fh; break;
      case kSquare: a = odd ? 1.f / fh : 0.f; break;
      case kPulse: a = sinf(pi * fh * 0.25f) / fh; break;
      case kNarrow: a = sinf(pi * fh * 0.1f) / fh; break;
      case kOrgan:  // drawbars 8' 4' 2 2/3' 2' 1 3/5' 1'
        a = h == 1 ? 1.f : h == 2 ? 0.8f : h == 3 ? 0.6f : h == 4 ? 0.5f : h == 5 ? 0.25f : h == 8 ? 0.3f : 0.f;
        break;
      case kGlass:  // square-numbered partials: bell-like
        a = h == 1 ? 1.f : h == 4 ? 0.6f : h == 9 ? 0.42f : h == 16 ? 0.28f : h == 25 ? 0.16f : h == 36 ? 0.08f : 0.f;
        break;
      case kVoxA: a = formant(fh * kVoxRef, kVoxAF, kVoxAB, kVoxAG) / sqrtf(fh); break;
      case kVoxO: a = formant(fh * kVoxRef, kVoxOF, kVoxOB, kVoxOG) / sqrtf(fh); break;
      case kBrass: {
        const float d = (fh - 5.f) / 3.f;
        a = (1.f + 2.f * expf(-d * d)) / fh;
        break;
      }
      case kReed: a = odd ? 1.f / powf(fh, 1.2f) : 0.1f / (fh * fh); break;
      case kComb: {
        const float k = cosf(pi * fh / 8.f);
        a = (0.1f + 0.9f * k * k) / fh;
        break;
      }
      case kMetal: {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const float r = (float)(rng >> 8) * (1.f / 16777216.f);
        a = (r * r) / sqrtf(fh) * (h == 1 ? 3.f : 1.f);
        break;
      }
      default: break;
    }
    if (wave == kPulse || wave == kNarrow) {
      c[h] = a;  // pulses are cosine series (the pulse is centred on t = 0)
    } else if (wave == kVoxA || wave == kVoxO || wave == kMetal) {
      // spread the phases: same spectrum, lower crest factor, louder at the same peak
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      const float ph = (float)(rng >> 8) * (2.f * pi / 16777216.f);
      s[h] = a * cosf(ph);
      c[h] = a * sinf(ph);
    } else {
      s[h] = a;
    }
  }
}

// In-place radix-2 complex FFT, sign +1 = inverse (unscaled). Load time only.
static void fft(float *re, float *im, int n, int sign) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    const double ang = 2.0 * 3.14159265358979323846 / len * sign;
    for (int k = 0; k < len / 2; ++k) {
      const float wr = (float)cos(ang * k), wi = (float)sin(ang * k);
      for (int i = k; i < n; i += len) {
        const int j = i + len / 2;
        const float xr = re[j] * wr - im[j] * wi, xi = re[j] * wi + im[j] * wr;
        re[j] = re[i] - xr; im[j] = im[i] - xi;
        re[i] += xr; im[i] += xi;
      }
    }
  }
}

struct Tables {
  float pool[kNumWaves][wavePoolSize()];  // per wave: level 0, 1, ... each with a guard sample
  int off[kNumLevels];
  bool built = false;

  const float *table(int wave, int level) const { return pool[wave] + off[level]; }

  void build() {
    if (built) return;
    for (int L = 0; L < kNumLevels; ++L) off[L] = levelOffset(L);
    static float re[kBaseSize], im[kBaseSize];
    static float s[kMaxHarm + 1], c[kMaxHarm + 1];
    for (int w = 0; w < kNumWaves; ++w) {
      waveSpectrum(w, s, c);
      float scale = 1.f;
      for (int L = 0; L < kNumLevels; ++L) {
        float *tab = pool[w] + off[L];
        const int H = levelHarmonics(L), N = levelSize(L);
        // spectrum -> one cycle by inverse FFT: X[h] = (c - i s) / 2, X[N-h] = conj
        for (int i = 0; i < N; ++i) re[i] = im[i] = 0.f;
        for (int h = 1; h <= H && h < N / 2; ++h) {
          re[h] = 0.5f * c[h]; im[h] = -0.5f * s[h];
          re[N - h] = 0.5f * c[h]; im[N - h] = 0.5f * s[h];
        }
        fft(re, im, N, 1);
        for (int i = 0; i < N; ++i) tab[i] = re[i];
        float peak = 0.f, sq = 0.f;
        for (int i = 0; i < N; ++i) {
          peak = maxf(peak, fabsf(tab[i]));
          sq += tab[i] * tab[i];
        }
        const float rms = sqrtf(sq / (float)N);
        // Level 0 sets the wave's gain: matched loudness, peak ceiling 0.97
        if (L == 0 && peak > 0.f) {
          const float byPeak = 0.97f / peak, byRms = 0.55f / maxf(rms, 1e-6f);
          scale = byPeak < byRms ? byPeak : byRms;
        }
        // Fewer harmonics can mean a higher peak (square -> sine): keep under 1
        float sc = scale;
        if (peak * sc > 0.99f) sc = 0.99f / peak;
        for (int i = 0; i < N; ++i) tab[i] *= sc;
        tab[N] = tab[0];
      }
    }
    built = true;
  }
};

static inline Tables &tables() {
  static Tables tb;
  return tb;
}

// ---------------------------------------------------------------------------
// Vector paths
// ---------------------------------------------------------------------------

enum Path {
  kPathOff = 0, kPathAtoD, kPathDtoA, kPathTour, kPathCircle,
  kPathFig8, kPathBounce, kPathDiamond, kPathWander, kPathRandom, kNumPaths
};

static inline const char *pathName(int p) {
  static const char *const n[kNumPaths] = {"Off",  "A>D",    "D>A",     "Tour",   "Circle",
                                           "Fig 8", "Bounce", "Diamond", "Wander", "RndNote"};
  return n[clampi(p, 0, kNumPaths - 1)];
}

// ---------------------------------------------------------------------------
// Parameters (shared by every voice)
// ---------------------------------------------------------------------------

struct Params {
  enum Id {
    kX = 0,   // 0..1023  A/C (left) <-> B/D (right)
    kY,       // 0..1023  C/D (bottom) <-> A/B (top)
    kPath,    // 0..9     vector path
    kWaveA,   // 0..15    corner waves
    kWaveB,
    kWaveC,
    kWaveD,
    kDetune,  // 0..100 % corner detune spread, 0..12 cents
    kRate,    // 0..100   path speed, 4 s .. 20 ms per segment
    kAmount,  // 0..100 % how far the path moves the X/Y point
    kBright,  // 0..100   key-tracked low-pass: 0 = dark, 100 = off
    kNumParams
  };

  float x = 0.5f, y = 0.5f;
  int path = kPathCircle;
  int wave[kNumCorners] = {kSaw, kVoxA, kGlass, kOrgan};
  float detRatio[kNumCorners] = {1.f, 1.f, 1.f, 1.f};
  float segPerSec = 1.f;
  float amount = 0.7f;
  float bright = 0.6f;

  void set(int id, int32_t v) {
    switch (id) {
      case kX: x = clampf((float)v / 1023.f, 0.f, 1.f); break;
      case kY: y = clampf((float)v / 1023.f, 0.f, 1.f); break;
      case kPath: path = clampi(v, 0, kNumPaths - 1); break;
      case kWaveA: case kWaveB: case kWaveC: case kWaveD:
        wave[id - kWaveA] = clampi(v, 0, kNumWaves - 1);
        break;
      case kDetune: {
        // A stays put, B up, C down, D half up: no two corners beat at the same rate
        static const float pattern[kNumCorners] = {0.f, 1.f, -1.f, 0.5f};
        const float cents = clampf((float)v, 0.f, 100.f) * 0.12f;
        for (int k = 0; k < kNumCorners; ++k) detRatio[k] = exp2f(pattern[k] * cents / 1200.f);
        break;
      }
      case kRate:
        // 0.25 .. ~50 segments per second
        segPerSec = 0.25f * exp2f(clampf((float)v, 0.f, 100.f) * 0.0764f);
        break;
      case kAmount: amount = clampf((float)v / 100.f, 0.f, 1.f); break;
      case kBright: bright = clampf((float)v / 100.f, 0.f, 1.f); break;
      default: break;
    }
  }
};

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

class Voice {
 public:
  void init(uint32_t seed) {
    rng_ = seed ? seed : 0x1234567u;
    for (int k = 0; k < kNumCorners; ++k) ph_[k] = 0.f;
    w0_ = 261.6255653f * kInvSR;
    mx_ = my_ = ma_ = mr_ = mb_ = 0.f;
    lp1_ = lp2_ = 0.f;
    restartPath(Params());
    snap_ = true;
  }

  void setPitch(float w0) { w0_ = clampf(w0, 0.f, 0.45f); }
  void setMod(float x, float y, float amount, float rate, float bright = 0.f) {
    mx_ = x;
    my_ = y;
    ma_ = amount;
    mr_ = rate;
    mb_ = bright;
  }

  // fresh: a new voice (new shared phase, X/Y jumps straight to its start).
  // Otherwise (voice steal / legato): phases run on and X/Y glides, no click.
  void noteOn(const Params &p, bool fresh) {
    if (fresh) {
      const float r = rnd01();
      for (int k = 0; k < kNumCorners; ++k) ph_[k] = r;
      snap_ = true;
    }
    restartPath(p);
  }

  // Current X/Y (0..1) after the path and modulation; for tests.
  float x() const { return xs_; }
  float y() const { return ys_; }

  void render(const Params &p, float *out, int len) {
    if (len <= 0) return;
    const float dt = (float)len * kInvSR;

    // ---- vector path ----
    float rate = p.segPerSec * exp2f(clampf(mr_, -1.f, 1.f) * 4.f);
    if (rate > 60.f) rate = 60.f;
    advancePath(p.path, rate * dt);
    float px, py;
    pathPoint(p.path, px, py);
    const float amt = clampf(p.amount + ma_, 0.f, 1.f) * 0.5f;
    const float xt = clampf(p.x + mx_ + amt * px, 0.f, 1.f);
    const float yt = clampf(p.y + my_ + amt * py, 0.f, 1.f);
    if (snap_) {
      xs_ = xt;
      ys_ = yt;
    } else {
      // ~3 ms glide: knob jumps and fast paths don't click
      const float k = clampf(dt * (1.f / 0.003f), 0.f, 1.f);
      xs_ += (xt - xs_) * k;
      ys_ += (yt - ys_) * k;
    }

    // ---- corner weights: bilinear, (sum w^2)^-1/4 loudness compensation ----
    float w[kNumCorners];
    w[0] = (1.f - xs_) * ys_;          // A top-left
    w[1] = xs_ * ys_;                  // B top-right
    w[2] = (1.f - xs_) * (1.f - ys_);  // C bottom-left
    w[3] = xs_ * (1.f - ys_);          // D bottom-right
    const float e = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3];
    const float g = kOutGain / sqrtf(sqrtf(maxf(e, 0.25f)));
    for (int k = 0; k < kNumCorners; ++k) w[k] *= g;
    if (snap_) {
      for (int k = 0; k < kNumCorners; ++k) wz_[k] = w[k];
      snap_ = false;
    }

    // ---- oscillators ----
    for (int i = 0; i < len; ++i) out[i] = 0.f;
    const Tables &tb = tables();
    const float invLen = 1.f / (float)len;
    for (int k = 0; k < kNumCorners; ++k) {
      const float inc = w0_ * p.detRatio[k];
      float ph = ph_[k];
      const float w0 = wz_[k], w1 = w[k];
      wz_[k] = w1;
      if (w0 < 1e-6f && w1 < 1e-6f) {
        ph += inc * (float)len;
        ph_[k] = ph - (float)(int)ph;
        continue;
      }
      // band-limited level: highest harmonic under kHarmLimitHz
      const float f0 = inc * kSR;
      int L = 0;
      while (L < kNumLevels - 1 && f0 * (float)levelHarmonics(L) > kHarmLimitHz) ++L;
      const float *tab = tb.table(p.wave[k], L);
      const int size = levelSize(L), mask = size - 1;
      const float fsize = (float)size;

      float wk = w0;
      const float dw = (w1 - w0) * invLen;
      for (int i = 0; i < len; ++i) {
        const float idx = ph * fsize;
        const int i0 = (int)idx;
        const float f = idx - (float)i0;
        const float a = tab[i0 & mask], b = tab[(i0 & mask) + 1];
        out[i] += wk * (a + f * (b - a));
        wk += dw;
        ph += inc;
        ph -= (float)(int)ph;
      }
      ph_[k] = ph;
    }

    // ---- BRIGHT: two one-pole low-passes, cutoff key-tracked ----
    // cutoff = 2 x f0 (0) .. 512 x f0 (~100); 100 = off. 12 dB/octave.
    const float br = clampf(p.bright + mb_, 0.f, 1.f);
    if (br < 0.999f) {
      float fc = w0_ * exp2f(1.f + br * 8.f);  // cycles per sample
      if (fc > 0.45f) fc = 0.45f;
      const float a = 1.f - expf(-6.2831853f * fc);
      float y1 = lp1_, y2 = lp2_;
      for (int i = 0; i < len; ++i) {
        y1 += a * (out[i] - y1);
        y2 += a * (y1 - y2);
        out[i] = y2;
      }
      lp1_ = y1;
      lp2_ = y2;
    } else {
      lp1_ = lp2_ = out[len - 1];  // ready to fade in without a jump
    }
  }

 private:
  float rnd01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return (float)(rng_ >> 8) * (1.f / 16777216.f);
  }
  float rnd11() { return rnd01() * 2.f - 1.f; }

  void restartPath(const Params &p) {
    pos_ = 0.f;
    wax_ = rnd11(); way_ = rnd11();
    wbx_ = rnd11(); wby_ = rnd11();
    rx_ = rnd11(); ry_ = rnd11();
    (void)p;
  }

  void advancePath(int path, float d) {
    pos_ += d;
    switch (path) {
      case kPathCircle: case kPathFig8: case kPathDiamond:
        while (pos_ >= 4.f) pos_ -= 4.f;
        break;
      case kPathBounce:
        while (pos_ >= 2.f) pos_ -= 2.f;
        break;
      case kPathWander:
        while (pos_ >= 1.f) {  // next random target
          pos_ -= 1.f;
          wax_ = wbx_; way_ = wby_;
          wbx_ = rnd11(); wby_ = rnd11();
        }
        break;
      default:  // one-shot paths hold their end point
        if (pos_ > 8.f) pos_ = 8.f;
        break;
    }
  }

  // Path offset in [-1, 1]^2 (x right, y up) at the current position
  void pathPoint(int path, float &x, float &y) const {
    static const float kAtoD[] = {-1, 1, 1, -1};
    static const float kDtoA[] = {1, -1, -1, 1};
    static const float kTour[] = {-1, 1, 1, 1, 1, -1, -1, -1, 0, 0};  // A B D C centre
    static const float kDiamond[] = {0, 1, 1, 0, 0, -1, -1, 0};
    static const float kBounce[] = {-1, 0, 1, 0};
    const float halfPi = 1.57079632679f;
    switch (path) {
      case kPathAtoD: lineThrough(kAtoD, 2, false, x, y); break;
      case kPathDtoA: lineThrough(kDtoA, 2, false, x, y); break;
      case kPathTour: lineThrough(kTour, 5, false, x, y); break;
      case kPathDiamond: lineThrough(kDiamond, 4, true, x, y); break;
      case kPathBounce: lineThrough(kBounce, 2, true, x, y); break;
      case kPathCircle: {  // starts at the top, clockwise, 4 segments per turn
        const float a = pos_ * halfPi;
        x = sinf(a);
        y = cosf(a);
        break;
      }
      case kPathFig8: {
        const float a = pos_ * halfPi;
        x = sinf(a);
        y = sinf(2.f * a);
        break;
      }
      case kPathWander: {
        const float t = pos_ * pos_ * (3.f - 2.f * pos_);  // smoothstep between targets
        x = wax_ + (wbx_ - wax_) * t;
        y = way_ + (wby_ - way_) * t;
        break;
      }
      case kPathRandom: x = rx_; y = ry_; break;
      default: x = y = 0.f; break;
    }
  }

  void lineThrough(const float *pts, int n, bool loop, float &x, float &y) const {
    const int segs = loop ? n : n - 1;
    float pos = pos_;
    if (!loop && pos >= (float)segs) {
      x = pts[2 * (n - 1)];
      y = pts[2 * (n - 1) + 1];
      return;
    }
    int s = (int)pos;
    if (s >= segs) s = segs - 1;
    const float f = pos - (float)s;
    const int a = s, b = (s + 1) % n;
    x = pts[2 * a] + (pts[2 * b] - pts[2 * a]) * f;
    y = pts[2 * a + 1] + (pts[2 * b + 1] - pts[2 * a + 1]) * f;
  }

  float ph_[kNumCorners];
  float wz_[kNumCorners] = {0, 0, 0, 0};
  float w0_ = 0.f;
  float mx_ = 0.f, my_ = 0.f, ma_ = 0.f, mr_ = 0.f, mb_ = 0.f;
  float lp1_ = 0.f, lp2_ = 0.f;
  float pos_ = 0.f;
  float wax_ = 0.f, way_ = 0.f, wbx_ = 0.f, wby_ = 0.f, rx_ = 0.f, ry_ = 0.f;
  float xs_ = 0.5f, ys_ = 0.5f;
  bool snap_ = true;
  uint32_t rng_ = 1;
};

}  // namespace vk
