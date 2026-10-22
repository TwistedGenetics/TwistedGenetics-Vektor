/*
 *  File: unit.cc
 *
 *  Vektor - logue SDK callbacks for the microKORG 2 (polyphonic)
 *
 *  One vk::Voice per synth voice. Idle voices are skipped. Voice events
 *  restart each voice's vector path on every note (that's what makes it a
 *  vector synth rather than a crossfader). Until the first voice event
 *  arrives every voice renders, so nothing goes silent if the host never
 *  sends events. The virtual patch can modulate VectorX, VectorY, PathAmt and
 *  PathRate per voice. Buffer layout, mod data access and voice events
 *  follow Korg's microkorg2/waves example (and Dusty Pads / Rollers).
 */

#include "unit.h"
#include "runtime.h"
#include "unit_osc.h"
#include "utils/io_ops.h"
#include "utils/mk2_utils.h"
#include "vektor.h"

namespace {

enum { kDestX = 0, kDestY, kDestAmount, kDestRate, kNumDest };

// Per-voice output trim: voices are summed by the synth (Korg's waves uses 0.3)
constexpr float kVoiceTrim = 0.5f;

vk::Voice s_voice[kMk2MaxVoices];
vk::Params s_params;
float s_mod[kNumDest][kMk2MaxVoices];
int32_t s_cached[vk::Params::kNumParams];
unit_runtime_desc_t s_desc;
float s_buf[kMk2BufferSize];

bool s_active[kMk2MaxVoices];
bool s_events_seen = false;

inline const unit_runtime_osc_context_t *context() {
  return static_cast<const unit_runtime_osc_context_t *>(s_desc.hooks.runtime_context);
}

inline int voiceCount(const unit_runtime_osc_context_t *ctx) {
  const int n = ctx->voiceLimit;
  return n < 0 ? 0 : (n > kMk2MaxVoices ? kMk2MaxVoices : n);
}

// NaN/inf check on the bits: -ffast-math lets the compiler drop float NaN compares
inline bool finiteBits(float x) {
  union { float f; uint32_t u; } b;
  b.f = x;
  return (b.u & 0x7F800000u) != 0x7F800000u;
}

void writeName(char *dst, const char *name) {
  int i = 0;
  for (; name[i] && i < 8; ++i) dst[i] = name[i];
  dst[i] = 0;
}

}  // namespace

__attribute__((used)) int8_t unit_init(const unit_runtime_desc_t *desc) {
  if (!desc) return k_unit_err_undef;
  if (desc->target != unit_header.target) return k_unit_err_target;
  if (!UNIT_API_IS_COMPAT(desc->api)) return k_unit_err_api_version;
  if (desc->samplerate != 48000) return k_unit_err_samplerate;

  s_desc = *desc;
  vk::tables().build();  // band-limited wave tables, built once at load
  s_events_seen = false;
  for (int id = 0; id < vk::Params::kNumParams; ++id) {
    s_cached[id] = static_cast<int32_t>(unit_header.params[id].init);
    s_params.set(id, s_cached[id]);
  }
  for (int v = 0; v < kMk2MaxVoices; ++v) {
    s_voice[v].init(0x9E3779B9u * (uint32_t)(v + 1));
    s_active[v] = false;
    for (int d = 0; d < kNumDest; ++d) s_mod[d][v] = 0.f;
  }
  return k_unit_err_none;
}

__attribute__((used)) void unit_teardown() {}

__attribute__((used)) void unit_reset() {
  for (int v = 0; v < kMk2MaxVoices; ++v) s_voice[v].noteOn(s_params, true);
}

__attribute__((used)) void unit_resume() {}

__attribute__((used)) void unit_suspend() {}

__attribute__((used)) void unit_render(const float *in, float *out, uint32_t frames) {
  (void)in;
  const unit_runtime_osc_context_t *ctx = context();
  const int n = voiceCount(ctx);

  for (int v = 0; v < n; ++v) {
    // Voice v writes lane v % 4 of its group of four (Korg's waves)
    const uint32_t offset = GetBufferOffset(ctx, v & ~3, frames);
    const uint32_t lane = (uint32_t)(v & 3);
    if (s_events_seen && !s_active[v]) {
      for (uint32_t i = 0; i < frames; ++i) write_oscillator_output_x1(out, 0.f, offset, ctx->outputStride, i, lane);
      continue;
    }
    // ctx->pitch is a fractional MIDI note number per voice
    float note = ctx->pitch[v];
    if (!finiteBits(note) || note < 0.f || note > 140.f) note = 60.f;
    const uint8_t nn = static_cast<uint8_t>(note);
    const float frac = note - static_cast<float>(nn);
    vk::Voice &vo = s_voice[v];
    vo.setPitch(osc_w0f_for_note(nn, static_cast<uint8_t>(frac * 255.f)));
    vo.setMod(s_mod[kDestX][v], s_mod[kDestY][v], s_mod[kDestAmount][v], s_mod[kDestRate][v]);

    uint32_t done = 0;
    while (done < frames) {
      const uint32_t len = (frames - done) < (uint32_t)kMk2BufferSize ? (frames - done) : (uint32_t)kMk2BufferSize;
      vo.render(s_params, s_buf, (int)len);
      for (uint32_t i = 0; i < len; ++i)
        write_oscillator_output_x1(out, s_buf[i] * kVoiceTrim, offset, ctx->outputStride, done + i, lane);
      done += len;
    }
  }
}

__attribute__((used)) void unit_set_param_value(uint8_t id, int32_t value) {
  if (id >= vk::Params::kNumParams) return;
  value = clipminmaxi32(unit_header.params[id].min, value, unit_header.params[id].max);
  s_cached[id] = value;
  s_params.set(id, value);
}

__attribute__((used)) int32_t unit_get_param_value(uint8_t id) {
  if (id >= vk::Params::kNumParams) return 0;
  return s_cached[id];
}

__attribute__((used)) const char *unit_get_param_str_value(uint8_t id, int32_t value) {
  switch (id) {
    case vk::Params::kPath:
      return vk::pathName(value);
    case vk::Params::kWaveA:
    case vk::Params::kWaveB:
    case vk::Params::kWaveC:
    case vk::Params::kWaveD:
      return vk::waveName(value);
    default:
      return nullptr;
  }
}

__attribute__((used)) const uint8_t *unit_get_param_bmp_value(uint8_t id, int32_t value) {
  (void)id;
  (void)value;
  return nullptr;
}

__attribute__((used)) void unit_set_tempo(uint32_t tempo) { (void)tempo; }

// Note events per voice: restart the vector path, track which voices sound
__attribute__((used)) void unit_osc_voice_event(uint8_t event, uint8_t voice, uint8_t note, uint8_t velocity) {
  (void)note;
  (void)velocity;
  if (voice >= kMk2MaxVoices) return;
  s_events_seen = true;
  switch (event) {
    case k_voice_event_allocation:
      s_voice[voice].noteOn(s_params, !s_active[voice]);  // fresh voice: new phase, X/Y snaps to start
      s_active[voice] = true;
      break;
    case k_voice_event_steal:
      s_voice[voice].noteOn(s_params, false);  // keep phase and glide X/Y: no click
      s_active[voice] = true;
      break;
    case k_voice_event_release:
      break;  // keep rendering (and moving) through the amp release
    case k_voice_event_deallocation:
      s_active[voice] = false;
      break;
    default:
      break;
  }
}

// Virtual patch: modulation data and destination names
__attribute__((used)) void unit_platform_exclusive(uint8_t messageId, void *data, uint32_t dataSize) {
  (void)dataSize;
  switch (messageId) {
    case kMk2PlatformExclusiveModData: {
      const unit_runtime_osc_context_t *ctx = context();
      const int n = voiceCount(ctx);
      const int32_t *index = GetModIndex(data);
      const float *depth = GetModDepth(data);
      for (int d = 0; d < kNumDest; ++d)
        for (int v = 0; v < kMk2MaxVoices; ++v) s_mod[d][v] = 0.f;
      for (int src = 0; src < kNumMk2ModSrc; ++src) {
        const int32_t dest = index[src];
        if (dest < 0 || dest >= kNumDest) continue;
        const float *mod = GetModSourceData(data, src, ctx->voiceLimit, 0);
        for (int v = 0; v < n; ++v) s_mod[dest][v] += mod[v] * depth[src];
      }
      break;
    }
    case kMk2PlatformExclusiveModDestName: {
      // data[0] holds the destination index; the name is written from data[1]
      char *name = GetModDestNameData(data);
      switch (name[0]) {
        case kDestX: writeName(name + 1, "VectorX"); break;
        case kDestY: writeName(name + 1, "VectorY"); break;
        case kDestAmount: writeName(name + 1, "PathAmt"); break;
        case kDestRate: writeName(name + 1, "PathRate"); break;
        default: break;
      }
      break;
    }
    default:
      break;
  }
}
