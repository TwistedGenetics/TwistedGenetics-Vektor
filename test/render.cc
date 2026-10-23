// Desktop test harness for Vektor.
// Renders the DSP without any Korg runtime and checks every wave for NaN /
// clipping / DC, loudness balance, tuning, aliasing at high notes, that the
// vector paths go where they should, that knob jumps don't click and that
// the centre of the square isn't a level hole. Then writes WAV demos to
// test/out/.
//
// Build & run:  ./test/run.sh

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "vektor.h"

using namespace vk;
typedef Params P;

static const int kRate = 48000;
static int failures = 0;

static float noteW0(float note) { return 440.f * std::pow(2.f, (note - 69.f) / 12.f) / kRate; }

static void writeWav(const std::string &path, const std::vector<float> &x) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = x.size(), data = n * 2, riff = 36 + data;
  const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  const uint32_t sr = kRate, br = kRate * 2, fmt = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
  for (float v : x) {
    int16_t s = (int16_t)std::lrint(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}

static void result(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(bool ok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buf[256];
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("%-66s %s\n", buf, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

struct Stats { float peak = 0, dc = 0, rms = 0; bool bad = false; };
static Stats stats(const std::vector<float> &x, size_t from = 0) {
  Stats s; double sum = 0, sq = 0;
  for (size_t i = from; i < x.size(); ++i) {
    const float v = x[i];
    if (!std::isfinite(v)) s.bad = true;
    s.peak = std::fmax(s.peak, std::fabs(v));
    sum += v; sq += double(v) * v;
  }
  const double n = double(x.size() - from);
  s.dc = sum / n; s.rms = std::sqrt(sq / n);
  return s;
}
static float db(float r) { return 20.f * std::log10(std::fmax(r, 1e-12f)); }

// ---- settings ----
struct Set { int x, y, path, a, b, c, d, detune, rate, amount; int bright = 100; };
static Params makeParams(const Set &s) {
  Params p;
  const int v[P::kNumParams] = {s.x, s.y, s.path, s.a, s.b, s.c, s.d, s.detune, s.rate, s.amount, s.bright};
  for (int id = 0; id < P::kNumParams; ++id) p.set(id, v[id]);
  return p;
}
// One wave on corner A, X/Y parked on A, no path
static Set cornerA(int wave) { return {0, 1023, kPathOff, wave, kSine, kSine, kSine, 0, 0, 0}; }

static std::vector<float> render(const Params &p, float note, float seconds, uint32_t seed = 1) {
  Voice v;
  v.init(seed);
  v.setPitch(noteW0(note));
  v.noteOn(p, true);
  const int n = int(seconds * kRate);
  std::vector<float> out(n);
  for (int i = 0; i < n; i += 64) v.render(p, out.data() + i, std::min(64, n - i));
  return out;
}

int main() {
  const auto t0 = std::chrono::steady_clock::now();
  tables().build();
  const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("wave tables built in %.1f ms (%zu kB)\n\n", buildMs, sizeof(Tables) / 1024);

  // ---- every wave: sane across the keyboard ----
  printf("== waves ==\n");
  std::vector<float> rmsAt60(kNumWaves);
  for (int w = 0; w < kNumWaves; ++w) {
    for (float note : {24.f, 48.f, 60.f, 84.f, 108.f}) {
      const Params p = makeParams(cornerA(w));
      const auto x = render(p, note, 0.5f);
      const Stats s = stats(x, 2400);
      const bool ok = !s.bad && s.peak <= 1.f && std::fabs(s.dc) < 0.02f && s.peak > 0.05f;
      if (!ok || note == 60.f)
        result(ok, "%-7s note %3.0f  peak %.3f  rms %.3f  dc %+.4f", waveName(w), note, s.peak, s.rms, s.dc);
      if (note == 60.f) rmsAt60[w] = s.rms;
    }
  }
  {
    float lo = 1e9f, hi = 0.f;
    for (float r : rmsAt60) { lo = std::fmin(lo, r); hi = std::fmax(hi, r); }
    result(db(hi / lo) < 7.f, "loudness spread across the 16 waves: %.1f dB", db(hi / lo));
  }

  // ---- tuning ----
  printf("\n== tuning ==\n");
  {
    const auto x = render(makeParams(cornerA(kSine)), 69.f, 1.f);
    int crossings = 0;
    for (size_t i = 1; i < x.size(); ++i) if (x[i - 1] < 0.f && x[i] >= 0.f) ++crossings;
    result(std::abs(crossings - 440) <= 1, "A4 sine: %d cycles in one second", crossings);
  }

  // ---- aliasing: f0 on an exact DFT bin, energy off the harmonics ----
  printf("\n== aliasing (energy between harmonics) ==\n");
  {
    const int N = 8192;
    for (int m : {150, 450, 900}) {  // 879 Hz, 2637 Hz, 5273 Hz
      for (int w : {kSaw, kSquare, kSync, kMetal, kNarrow}) {
        Params p = makeParams(cornerA(w));
        Voice v;
        v.init(7);
        v.setPitch(float(m) / N);
        v.noteOn(p, true);
        std::vector<float> x(N + 4800);
        for (int i = 0; i < (int)x.size(); i += 64) v.render(p, x.data() + i, std::min(64, (int)x.size() - i));
        const float *s = x.data() + 4800;
        double harm = 0, other = 0;
        for (int k = 1; k < N / 2; ++k) {
          double re = 0, im = 0;
          for (int n = 0; n < N; ++n) {
            const double a = 2.0 * M_PI * double((long long)k * n % N) / N;
            re += s[n] * std::cos(a);
            im -= s[n] * std::sin(a);
          }
          const double e = re * re + im * im;
          if (k % m == 0) harm += e; else other += e;
        }
        const float r = db(float(std::sqrt(other / harm)));
        result(r < -50.f, "%-7s %5.0f Hz  aliasing %6.1f dB", waveName(w), m * 48000.f / N, r);
      }
    }
  }

  // ---- interpolation fizz: real note pitches (not locked to the table) ----
  // Blackman-Harris windowed FFT; energy more than 6 bins from any harmonic.
  // v1.0.0 (512-sample tables) measured -35..-55 dB here: audible buzz.
  printf("\n== inharmonic fizz at real pitches (worst note per wave) ==\n");
  {
    const int N = 65536;
    static float re[N], im[N];
    for (int w = 0; w < kNumWaves; ++w) {
      float worst = -300.f, worstNote = 0.f;
      for (float note : {28.f, 35.f, 40.f, 47.f, 52.f, 59.f, 64.f, 71.f, 76.f}) {
        const auto x = render(makeParams(cornerA(w)), note, (N + 4800) / float(kRate));
        for (int n = 0; n < N; ++n) {
          const double t = 2.0 * M_PI * n / N;
          const double win = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
          re[n] = float(x[4800 + n] * win);
          im[n] = 0.f;
        }
        fft(re, im, N, -1);
        const double bins = noteW0(note) * N;  // harmonic spacing in bins
        double harm = 0, other = 0;
        for (int k = 1; k < N / 2; ++k) {
          const double e = double(re[k]) * re[k] + double(im[k]) * im[k];
          const double h = std::round(k / bins);
          if (h >= 1 && std::fabs(k - h * bins) <= 6) harm += e; else other += e;
        }
        const float r = 10.f * std::log10(float(other / harm) + 1e-30f);
        if (r > worst) { worst = r; worstNote = note; }
      }
      result(worst < -65.f, "%-7s worst %6.1f dB (note %.0f)", waveName(w), worst, worstNote);
    }
  }

  // ---- BRIGHT ----
  printf("\n== bright ==\n");
  {
    auto centroid = [](const std::vector<float> &x) {
      const int N = 16384;
      static float re[N], im[N];
      for (int n = 0; n < N; ++n) { re[n] = x[4800 + n]; im[n] = 0.f; }
      fft(re, im, N, -1);
      double num = 0, den = 0;
      for (int k = 1; k < N / 2; ++k) { const double e = double(re[k]) * re[k] + double(im[k]) * im[k]; num += e * k; den += e; }
      return float(num / den * kRate / N);
    };
    float last = 1e9f;
    bool falling = true;
    for (int b : {100, 80, 60, 40, 20, 0}) {
      Set st = cornerA(kSaw);
      st.bright = b;
      const auto x = render(makeParams(st), 48.f, 0.5f);
      const float c = centroid(x);
      const Stats s = stats(x, 4800);
      printf("  bright %3d  centroid %6.0f Hz  rms %.3f\n", b, c, s.rms);
      if (c >= last || s.bad) falling = false;
      last = c;
    }
    result(falling, "saw C3: brightness falls as BRIGHT goes down");
  }

  // ---- paths ----
  printf("\n== vector paths ==\n");
  {
    // A>D at full amount from the centre: starts on A (0, 1), ends on D (1, 0)
    Params p = makeParams({512, 512, kPathAtoD, kSine, kSine, kSine, kSine, 0, 60, 100});
    Voice v;
    v.init(3);
    v.setPitch(noteW0(60));
    v.noteOn(p, true);
    float buf[64];
    v.render(p, buf, 64);
    const float x0 = v.x(), y0 = v.y();
    for (int i = 0; i < 48000 * 3 / 64; ++i) v.render(p, buf, 64);
    result(x0 < 0.05f && y0 > 0.95f && v.x() > 0.95f && v.y() < 0.05f,
           "A>D: start (%.2f, %.2f) -> end (%.2f, %.2f)", x0, y0, v.x(), v.y());

    // looped paths stay in the square and keep covering it
    for (int path : {kPathCircle, kPathFig8, kPathBounce, kPathDiamond, kPathWander}) {
      p = makeParams({512, 512, path, kSaw, kSquare, kTri, kSine, 0, 50, 100});
      v.init(11);
      v.noteOn(p, true);
      float xmin = 1, xmax = 0, ymin = 1, ymax = 0;
      bool inside = true;
      for (int i = 0; i < 48000 * 8 / 64; ++i) {
        v.render(p, buf, 64);
        xmin = std::fmin(xmin, v.x()); xmax = std::fmax(xmax, v.x());
        ymin = std::fmin(ymin, v.y()); ymax = std::fmax(ymax, v.y());
        if (v.x() < 0 || v.x() > 1 || v.y() < 0 || v.y() > 1) inside = false;
      }
      const bool covers = (xmax - xmin) > 0.6f && (path == kPathBounce || (ymax - ymin) > 0.6f);
      result(inside && covers, "%-7s x %.2f..%.2f  y %.2f..%.2f", pathName(path), xmin, xmax, ymin, ymax);
    }

    // RndNote: different notes land on different points
    p = makeParams({512, 512, kPathRandom, kSaw, kSquare, kTri, kSine, 0, 50, 100});
    v.init(5);
    float spread = 0.f, px = -1.f;
    for (int n = 0; n < 8; ++n) {
      v.noteOn(p, true);
      v.render(p, buf, 64);
      if (px >= 0.f) spread = std::fmax(spread, std::fabs(v.x() - px));
      px = v.x();
    }
    result(spread > 0.2f, "RndNote: X moves by up to %.2f between notes", spread);
  }

  // ---- knob jump: no click ----
  printf("\n== clicks and level ==\n");
  {
    Params p = makeParams({0, 1023, kPathOff, kSaw, kSquare, kSine, kSine, 0, 0, 0});
    Voice v;
    v.init(9);
    v.setPitch(noteW0(48));
    v.noteOn(p, true);
    std::vector<float> x(48000);
    for (int i = 0; i < 24000; i += 64) v.render(p, x.data() + i, 64);
    p.set(P::kX, 1023);  // jump A -> B
    for (int i = 24000; i < 48000; i += 64) v.render(p, x.data() + i, 64);
    // compare the jump to the ordinary sample-to-sample movement of a saw/square
    float steady = 0.f, jump = 0.f;
    for (int i = 1000; i < 23000; ++i) steady = std::fmax(steady, std::fabs(x[i] - x[i - 1]));
    for (int i = 23900; i < 25000; ++i) jump = std::fmax(jump, std::fabs(x[i] - x[i - 1]));
    result(jump < steady * 1.2f + 0.02f, "X jump A->B: max step %.3f (steady %.3f)", jump, steady);
  }
  {
    // centre of the square vs the corners, four different waves
    float corner = 0.f;
    const int waves[4] = {kSaw, kVoxA, kGlass, kSync};
    const int cx[4] = {0, 1023, 0, 1023}, cy[4] = {1023, 1023, 0, 0};
    for (int k = 0; k < 4; ++k)
      corner += stats(render(makeParams({cx[k], cy[k], kPathOff, waves[0], waves[1], waves[2], waves[3], 20, 0, 0}), 57, 1.f), 4800).rms / 4;
    const Stats c = stats(render(makeParams({512, 512, kPathOff, waves[0], waves[1], waves[2], waves[3], 20, 0, 0}), 57, 1.f), 4800);
    result(std::fabs(db(c.rms / corner)) < 4.f, "centre vs corners: %+.1f dB", db(c.rms / corner));
    // same wave on all four corners, no detune: centre must not clip
    const Stats same = stats(render(makeParams({512, 512, kPathOff, kSquare, kSquare, kSquare, kSquare, 0, 0, 0}), 57, 1.f), 4800);
    result(same.peak <= 1.f, "one wave x4 at the centre: peak %.3f", same.peak);
  }

  // ---- speed ----
  printf("\n== speed ==\n");
  {
    Params p = makeParams({512, 512, kPathCircle, kSaw, kVoxA, kGlass, kOrgan, 30, 60, 70, 60});
    Voice v[8];
    for (int k = 0; k < 8; ++k) { v[k].init(k + 1); v[k].setPitch(noteW0(48 + 3 * k)); v[k].noteOn(p, true); }
    float buf[64];
    const auto a = std::chrono::steady_clock::now();
    for (int i = 0; i < 48000 * 10 / 64; ++i)
      for (int k = 0; k < 8; ++k) v[k].render(p, buf, 64);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    printf("8 voices, 10 s of audio: %.1f ms on this machine (%.2f%% real time)\n", s * 1000, s * 10);
  }

  // ---- demos ----
  if (system("mkdir -p test/out") != 0) return 1;
  struct Demo { const char *name; Set s; std::vector<float> notes; float secs; };
  const Demo demos[] = {
      {"default_circle", {512, 512, kPathCircle, kSaw, kVoxA, kGlass, kOrgan, 20, 30, 70, 60}, {48, 55, 60, 63}, 6},
      {"a_to_d_pad", {512, 512, kPathAtoD, kVoxO, kGlass, kOrgan, kSaw, 35, 20, 100, 55}, {45, 52, 57, 60, 64}, 6},
      {"tour_digital", {512, 512, kPathTour, kMetal, kSync, kComb, kFold, 10, 45, 100, 75}, {50, 57, 62}, 5},
      {"wander_choir", {512, 512, kPathWander, kVoxA, kVoxO, kReed, kTri, 50, 25, 90, 60}, {48, 55, 60, 64, 67}, 7},
      {"bounce_bass", {512, 300, kPathBounce, kSaw, kSquare, kPulse, kNarrow, 15, 70, 80, 45}, {36}, 4},
      {"fig8_brass", {512, 512, kPathFig8, kBrass, kSaw, kSquare, kOrgan, 25, 40, 75, 65}, {53, 57, 60}, 6},
  };
  for (const Demo &d : demos) {
    const Params p = makeParams(d.s);
    std::vector<float> mix(int(d.secs * kRate), 0.f);
    uint32_t seed = 1;
    for (float n : d.notes) {
      const auto x = render(p, n, d.secs, seed++);
      for (size_t i = 0; i < mix.size(); ++i) mix[i] += x[i] * 0.5f / std::sqrt(float(d.notes.size()));
    }
    // short fades so the files don't click
    for (int i = 0; i < 480; ++i) { mix[i] *= i / 480.f; mix[mix.size() - 1 - i] *= i / 480.f; }
    writeWav(std::string("test/out/") + d.name + ".wav", mix);
  }
  printf("\nWAV demos written to test/out/\n");

  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
