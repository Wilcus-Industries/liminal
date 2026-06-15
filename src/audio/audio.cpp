// Audio.cpp — the room-tone drone and its dropouts.
//
// miniaudio integration notes (0.11.x, split miniaudio.c/miniaudio.h build):
//   - The repo links the CMake target "miniaudio", which compiles miniaudio.c
//     for us. We must NOT define MINIAUDIO_IMPLEMENTATION here — doing so would
//     pull a second copy of every miniaudio symbol into this TU and the link
//     would explode with duplicate definitions. Just include the header.
//   - miniaudio drives audio with a *pull* model: CoreAudio (on macOS) wakes a
//     high-priority realtime thread and asks us to fill a buffer. That thread
//     is not ours; it must never block, allocate, lock, or touch anything the
//     game thread might be mutating non-atomically. Hence: the callback owns
//     all DSP state outright, and the game pokes it only through the atomics
//     in AudioParams.
//   - We ask for f32 / 2ch / 48kHz. miniaudio inserts its own converter
//     between us and whatever the hardware actually runs at, so the callback
//     can assume exactly the format it requested. That guarantee is why all
//     the per-sample constants below can be compile-time.

#include <liminal/audio/audio.hpp>

#include "miniaudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace liminal {

namespace {

// The format we request — and, per miniaudio's conversion guarantee, the
// format the callback actually receives.
constexpr ma_uint32 kSampleRate = 48000;
constexpr ma_uint32 kChannels = 2;

constexpr float kTwoPi = 6.283185307179586f;

// The drone: four slow, slightly-off low sines. The frequencies are chosen to
// beat against each other (52.0 vs 54.7 Hz beats at ~2.7 Hz) so the tone never
// quite settles — it sounds like a building, not a note.
constexpr int kNumOsc = 4;
constexpr float kOscFreqHz[kNumOsc] = {52.0f, 54.7f, 78.3f, 103.1f};
constexpr float kOscGain[kNumOsc] = {1.0f, 0.55f, 0.30f, 0.18f};
// Coherence decay spreads the oscillators apart: at decay=1 the 52/54.7 pair
// beats at ~4.7 Hz instead of 2.7 and the upper partials pull sharp/flat in
// opposite directions — the room tone itself unravels on the dream's clock.
constexpr float kOscDetune[kNumOsc] = {0.0f, 0.018f, -0.022f, 0.030f};

// Normalize the worst case (all four peaks aligned, sum of gains = 2.03) down
// to ~0.25 peak before the master gain is applied. Headroom matters more than
// loudness here; the tanh clip at the end is a seatbelt, not an effect.
constexpr float kDroneNorm = 0.25f / (kOscGain[0] + kOscGain[1] + kOscGain[2] + kOscGain[3]);

// Very slow amplitude wobble so the drone breathes instead of sustaining.
constexpr float kLfoFreqHz = 0.06f;
constexpr float kLfoDepth = 0.20f; // +/-20%

// One-pole envelope time constant. ~0.4s means a dropout fades in over about
// a second rather than truncating the waveform — silence ARRIVES, no click.
constexpr float kEnvTauSeconds = 0.4f;

// The dread throb: a low carrier pulsed at heart rate. 38 Hz is felt as much
// as heard; 1.15 Hz is ~69 bpm, an at-rest heart that is not yours. The pulse
// shape is a positive half-sine squared — a thump with space between beats,
// not a wobble.
constexpr float kThrobCarrierHz = 38.0f;
constexpr float kThrobPulseHz = 1.15f;
constexpr float kThrobGain = 0.20f;
// The throb's own envelope is slower than the drone's so it swells like a
// presence arriving, never snaps on.
constexpr float kThrobTauSeconds = 1.2f;

// Footsteps: a dull low-sine knock with a downward pitch droop plus a short
// lowpassed noise scuff. The knock base pitch rises with vigor (how hard the
// foot lands) and gets a random jitter that widens with decay — the steps are
// never quite the same twice, and less so as the dream rots.
constexpr float kStepKnockBaseHz = 58.0f;
constexpr float kStepKnockVigorHz = 22.0f;     // added at full vigor
constexpr float kStepKnockTauSeconds = 0.09f;  // knock decay
constexpr float kStepNoiseTauSeconds = 0.03f;  // scuff decay
constexpr float kStepNoiseLpHz = 900.0f;       // scuff lowpass cutoff
constexpr float kStepPan = 0.25f;              // alternating L/R offset

// Jump takeoff: the footstep voice's vocabulary — a dull low-sine knock plus
// a lowpassed noise scuff — pitched a little above a step and with the pitch
// bending gently UP instead of drooping (feet leaving the ground, not
// landing). Same pitch-jitter-with-decay treatment as the steps, and like
// them it rides outside the dropout windows: your own body keeps sounding in
// the silence.
constexpr float kJumpKnockBaseHz = 74.0f;      // a step's knock, raised a touch
constexpr float kJumpSweepRate = 2.5f;         // gentle upward bend, /s
constexpr float kJumpKnockTauSeconds = 0.11f;  // knock decay
constexpr float kJumpNoiseTauSeconds = 0.05f;  // scuff decay — a push-off drag
constexpr float kJumpAmp = 0.16f;

// NPC mumble: a short burst of syllable-shaped, heavily lowpassed noise —
// speech with all the words removed. The syllable rate slows and the "voice"
// darkens as the dream decays.
constexpr float kMumbleTauSeconds = 0.55f;   // overall burst decay
constexpr float kMumbleSyllHz = 4.2f;        // syllable cadence
constexpr float kMumbleAmp = 0.10f;

// Lamp hum: mains buzz an octave too low — a 90 Hz sine plus a faint third
// harmonic, faded by proximity. Detunes a touch with decay like everything.
constexpr float kHumHz = 90.0f;
constexpr float kHumAmp = 0.030f;
constexpr float kHumTauSeconds = 0.20f;

// Door creak: a stick-slip squeal — a sine that glides down ~a third over
// the burst with a fast tremolo roughing it up. Pitch jitters per fire (and
// wider with decay, like the steps).
constexpr float kCreakTauSeconds = 0.7f;
constexpr float kCreakBaseHz = 340.0f;
constexpr float kCreakTremHz = 13.0f;
constexpr float kCreakAmp = 0.06f;

// Echo footsteps: deep in the decay, roughly one step in eight repeats once
// 0.3-0.6 s later — a touch flat, quieter, and on the WRONG side. The voice
// is the same knock+scuff; only the bookkeeping differs. Not yours.
constexpr float kEchoDecayGate = 0.5f;   // decay below this -> never echoes
constexpr float kEchoOdds = 1.0f / 8.0f;
constexpr float kEchoDelayMinS = 0.3f;
constexpr float kEchoDelayMaxS = 0.6f;
constexpr float kEchoPitch = 0.92f;      // slightly flat of the real step
constexpr float kEchoAmp = 0.7f;         // and slightly behind it in level

// The breath: lowpassed noise under a slow asymmetric inhale/exhale cycle,
// swelling with figure proximity. Rides outside the dropout windows like the
// throb. The game zeroes breathGain while the player stares the figure down;
// the slow envelope turns that cut into a held breath, not a click.
constexpr float kBreathCycleHz = 0.22f;  // one full breath ~4.5 s
constexpr float kBreathLpHz = 500.0f;    // chest, not hiss
constexpr float kBreathGain = 0.05f;
constexpr float kBreathTauSeconds = 1.5f;

// --- v3 ambient loop bank ----------------------------------------------------
// Eight short mono loops synthesized once at startup, deliberately PS1-grade:
// 22050 Hz, 8-bit quantized, 2-6 s long with hard loop points you can hear,
// played back by nearest-sample lookup (no interpolation). The callback runs
// three voices over the bank: two zone ambiences crossfaded by zoneBlend and
// a positional water voice.
constexpr int kNumAmbientLoops = 8;   // mirrors the game's AmbientTag order
constexpr float kAmbientRate = 22050.0f;
constexpr float kAmbientStep = kAmbientRate / static_cast<float>(kSampleRate);
constexpr float kAmbTauSeconds = 0.30f; // voice gain smoothing / tag swap fade

// Read an atomic float with relaxed ordering. The callback only needs *a*
// recent value, not a synchronized one — the game thread publishes parameter
// changes whenever; a callback or two of staleness is inaudible.
inline float loadf(const std::atomic<float>& a) {
    return a.load(std::memory_order_relaxed);
}

// uniform(lo..hi) with degenerate/reversed ranges tolerated, because the
// bounds come from independently-settable atomics and uniform_real_distribution
// with b < a is undefined behavior.
inline double uniformSeconds(std::minstd_rand& rng, float a, float b) {
    const double lo = static_cast<double>(std::min(a, b));
    const double hi = static_cast<double>(std::max(a, b));
    if (hi <= lo) return lo;
    return std::uniform_real_distribution<double>(lo, hi)(rng);
}

// Build the eight ambient loops. Pure offline synthesis, runs once in the
// ctor; allocation is fine here (the realtime thread doesn't exist yet).
// Order mirrors the game's AmbientTag enum.
std::vector<std::vector<float>> buildAmbientLoops(unsigned int seed) {
    std::minstd_rand rng(seed ^ 0xA5B17E3Du);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    auto frames = [](float seconds) {
        return static_cast<size_t>(seconds * kAmbientRate);
    };
    const float dt = 1.0f / kAmbientRate;

    // 8-bit quantize after normalizing to a modest peak: the staircase grain
    // is the PS1 character, the master gain happens at mix time.
    auto finalize = [](std::vector<float>& b) {
        float peak = 1e-6f;
        for (float v : b) peak = std::max(peak, std::fabs(v));
        const float norm = 0.8f / peak;
        for (float& v : b) {
            v = std::round(std::clamp(v * norm, -1.0f, 1.0f) * 127.0f) / 127.0f;
        }
    };

    std::vector<std::vector<float>> loops(kNumAmbientLoops);

    // 0 running_water: lowpassed noise, cutoff and amplitude burbling.
    {
        auto& b = loops[0];
        b.resize(frames(3.0f));
        float lp = 0.0f, wob = 0.0f;
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            wob = 0.5f + 0.5f * std::sin(t * kTwoPi * 1.7f) * std::sin(t * kTwoPi * 0.43f);
            const float cut = 600.0f + 900.0f * wob;
            const float c = 1.0f - std::exp(-dt * kTwoPi * cut);
            lp += (u(rng) - lp) * c;
            b[i] = lp * (0.6f + 0.4f * wob);
        }
        finalize(b);
    }
    // 1 waves: deep lowpassed noise swelling at ~0.18 Hz, hiss on the crest.
    {
        auto& b = loops[1];
        b.resize(frames(5.6f));
        float lp = 0.0f, hiss = 0.0f;
        const float cLow = 1.0f - std::exp(-dt * kTwoPi * 320.0f);
        const float cHiss = 1.0f - std::exp(-dt * kTwoPi * 2400.0f);
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            float swell = std::sin(t * kTwoPi / 5.6f); // one full wave per loop
            swell = std::max(0.0f, swell);
            swell *= swell;
            const float w = u(rng);
            lp += (w - lp) * cLow;
            hiss += (w - hiss) * cHiss;
            b[i] = lp * (0.25f + 0.75f * swell) + (hiss - lp) * 0.18f * swell;
        }
        finalize(b);
    }
    // 2 wind: noise through a slowly wandering lowpass, breathing level.
    {
        auto& b = loops[2];
        b.resize(frames(4.8f));
        float lp = 0.0f;
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            const float wander = 0.5f + 0.5f * std::sin(t * kTwoPi / 4.8f + 1.3f);
            const float cut = 280.0f + 620.0f * wander;
            const float c = 1.0f - std::exp(-dt * kTwoPi * cut);
            lp += (u(rng) - lp) * c;
            b[i] = lp * (0.45f + 0.55f * wander);
        }
        finalize(b);
        // Broadband noise reads much louder than the tonal loops at equal
        // peak; pull wind back so it sits under the footsteps.
        for (float& v : b) v *= 0.65f;
    }
    // 3 drone: three detuned low sines beating, slow AM. A room with intent.
    {
        auto& b = loops[3];
        b.resize(frames(4.0f));
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            const float s = std::sin(t * kTwoPi * 57.0f) +
                            0.6f * std::sin(t * kTwoPi * 59.5f) +
                            0.35f * std::sin(t * kTwoPi * 86.0f);
            b[i] = s * (0.8f + 0.2f * std::sin(t * kTwoPi / 4.0f));
        }
        finalize(b);
    }
    // 4 rain: sparse impulse field through a mid lowpass — patter, not hiss.
    {
        auto& b = loops[4];
        b.resize(frames(3.2f));
        float lp = 0.0f;
        const float c = 1.0f - std::exp(-dt * kTwoPi * 1500.0f);
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        for (size_t i = 0; i < b.size(); ++i) {
            const float drop = (u01(rng) < 0.055f) ? u(rng) * 1.8f : 0.0f;
            lp += (drop + u(rng) * 0.12f - lp) * c;
            b[i] = lp;
        }
        finalize(b);
    }
    // 5 machinery: mains-adjacent hum + motor whine + a clank every 1.3 s.
    {
        auto& b = loops[5];
        b.resize(frames(2.6f));
        float lp = 0.0f;
        const float c = 1.0f - std::exp(-dt * kTwoPi * 900.0f);
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            float s = 0.5f * std::sin(t * kTwoPi * 49.0f);
            s += 0.18f * ((std::fmod(t * 178.0f, 1.0f) < 0.5f) ? 1.0f : -1.0f); // whine
            const float sinceClank = std::fmod(t, 1.3f);
            if (sinceClank < 0.18f) {
                lp += (u(rng) - lp) * c;
                s += lp * 1.6f * (1.0f - sinceClank / 0.18f);
            }
            b[i] = s;
        }
        finalize(b);
    }
    // 6 silence: not digital zero — a tiny hiss floor, the sound of a tape
    // of nothing. Quiet enough to read as silence with presence.
    {
        auto& b = loops[6];
        b.resize(frames(2.0f));
        for (size_t i = 0; i < b.size(); ++i) b[i] = u(rng) * 0.05f;
        finalize(b);
        for (float& v : b) v *= 0.10f; // pull the normalized floor way down
    }
    // 7 birds_wrong: occasional FM chirps that bend the wrong way (downward
    // swoop, reversed envelope — they swell INTO the cutoff), over faint air.
    {
        auto& b = loops[7];
        b.resize(frames(4.4f));
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);
        // Pre-rolled chirp times/pitches so the loop is identical every pass.
        float chirpT[4], chirpF[4];
        for (int k = 0; k < 4; ++k) {
            chirpT[k] = 0.3f + u01(rng) * 3.6f;
            chirpF[k] = 900.0f + u01(rng) * 1400.0f;
        }
        for (size_t i = 0; i < b.size(); ++i) {
            const float t = i * dt;
            float s = u(rng) * 0.025f; // air
            for (int k = 0; k < 4; ++k) {
                const float dt2 = t - chirpT[k];
                if (dt2 >= 0.0f && dt2 < 0.22f) {
                    const float env = dt2 / 0.22f;        // reversed: swells then cuts
                    const float f = chirpF[k] * (1.0f - 0.5f * env); // wrong-way bend
                    s += std::sin(dt2 * kTwoPi * f) * env * env * 0.8f;
                }
            }
            b[i] = s;
        }
        finalize(b);
    }

    return loops;
}

} // namespace

// ----------------------------------------------------------------------------

struct Audio::Impl {
    ma_device device{};
    bool deviceInited = false; // ma_device_init succeeded (dtor must uninit)
    bool deviceOk = false;     // device actually started (ok())

    // Written by the audio callback, read by the game thread for the overlay.
    std::atomic<bool> droppedOutFlag{false};

    // Back-pointer to the owning Audio's params block. Safe because Audio owns
    // Impl and params outlives the device (uninit happens in ~Audio before
    // params is destroyed — member destruction order is declaration order
    // reversed, and m_impl is declared after params).
    AudioParams* params = nullptr;

    // ------ everything below is owned exclusively by the callback thread ----
    // (seeded/primed in the ctor BEFORE ma_device_start, so no race exists)

    float oscPhase[kNumOsc] = {0.0f, 0.0f, 0.0f, 0.0f}; // cycles, [0,1)
    float lfoPhase = 0.0f;                              // cycles, [0,1)
    float env = 0.0f; // smoothed master gain; starts silent and fades in

    // Dread throb state. Its envelope is separate from `env` on purpose: env
    // is gated by the dropout machine, and the throb must keep beating through
    // the silence windows.
    float throbCarrierPhase = 0.0f; // cycles, [0,1)
    float throbPulsePhase = 0.0f;   // cycles, [0,1)
    float throbEnv = 0.0f;          // smoothed dread*gain target

    // Footstep one-shot voice. Like the throb, it rides outside the dropout
    // envelope — your own body keeps sounding when the room goes silent.
    uint32_t stepSeen = 0;          // last stepCounter value handled
    float stepKnockPhase = 0.0f;    // cycles, [0,1)
    float stepKnockHz = 0.0f;       // current knock pitch (droops per sample)
    float stepKnockEnv = 0.0f;
    float stepNoiseEnv = 0.0f;
    float stepNoiseLP = 0.0f;       // one-pole lowpass state for the scuff
    float stepAmp = 0.0f;
    float stepPan = 0.0f;           // -kStepPan / +kStepPan, alternating feet
    bool stepLeftFoot = false;
    uint32_t noiseState = 0x9E3779B9u; // xorshift32 — per-sample scuff noise

    // Jump one-shot voice: the step's knock+scuff pair with the pitch bending
    // up, mono/center (it's your own body leaving the ground, not a foot
    // landing to one side).
    uint32_t jumpSeen = 0;       // last jumpCounter value handled
    float jumpPhase = 0.0f;      // cycles, [0,1)
    float jumpHz = 0.0f;         // current knock pitch (sweeps up per sample)
    float jumpKnockEnv = 0.0f;
    float jumpNoiseEnv = 0.0f;
    float jumpNoiseLP = 0.0f;    // one-pole lowpass state for the scuff

    // NPC mumble one-shot voice (see kMumble* constants).
    uint32_t mumbleSeen = 0;
    float mumbleEnv = 0.0f;
    float mumbleSyllPhase = 0.0f; // cycles, [0,1)
    float mumbleLP = 0.0f;        // voice-band lowpass state

    // Lamp hum continuous voice (see kHum* constants).
    float humEnv = 0.0f;
    float humPhase = 0.0f; // cycles, [0,1)

    // Door creak one-shot voice (see kCreak* constants).
    uint32_t creakSeen = 0;
    float creakEnv = 0.0f;
    float creakHz = kCreakBaseHz;
    float creakPhase = 0.0f;      // cycles, [0,1)
    float creakTremPhase = 0.0f;  // cycles, [0,1)

    // Echo footstep: a pending re-fire of the step voice. -1 = none armed.
    // The countdown is in samples; the echo reuses the step voice outright
    // (its envelopes are dead long before 0.3 s), so no second voice exists.
    int echoCountdown = -1;
    float echoHz = 0.0f;   // pitch captured at the real step, pre-droop
    float echoVigor = 0.0f;
    float echoPan = 0.0f;  // opposite side of the step that spawned it

    // Breath voice state. Separate envelope from `env` — like the throb, the
    // breathing keeps moving through the dropout silence windows.
    float breathPhase = 0.0f; // cycles, [0,1)
    float breathEnv = 0.0f;   // smoothed breathGain target
    float breathLP = 0.0f;    // one-pole lowpass state

    // Ambient loop bank + voices. The bank is built in the ctor BEFORE
    // ma_device_start, then never mutated — the callback only reads it.
    std::vector<std::vector<float>> ambientLoops; // kNumAmbientLoops mono buffers
    struct AmbVoice {
        int cur = -1;      // loop currently sounding (-1 = none)
        int want = -1;     // loop requested by the game
        double pos = 0.0;  // sample cursor into the loop
        float env = 0.0f;  // smoothed gain (also gates the cur->want swap)
        float lfo = 0.0f;  // detune wobble phase, cycles
    };
    AmbVoice ambVoice[3]; // 0 = zone A, 1 = zone B, 2 = water

    std::minstd_rand rng; // dropout scheduling RNG — cheap, and quality is irrelevant here
    enum class State { Playing, Silent };
    State state = State::Playing;
    double stateSecondsLeft = 0.0; // time remaining in the current state

    static void dataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);
};

// ----------------------------------------------------------------------------
// The realtime callback. Rules in force here: no locks, no allocation, no I/O.
// AudioParams atomics are read exactly once at the top; everything else is
// plain local math on state this thread owns.
// ----------------------------------------------------------------------------
void Audio::Impl::dataCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
    auto* self = static_cast<Impl*>(pDevice->pUserData);
    auto* out = static_cast<float*>(pOutput);

    const AudioParams& p = *self->params;
    const float gain = loadf(p.gain);
    const bool enabled = p.enabled.load(std::memory_order_relaxed);
    const float dropoutMin = loadf(p.dropoutMin);
    const float dropoutMax = loadf(p.dropoutMax);
    const float intervalMin = loadf(p.intervalMin);
    const float intervalMax = loadf(p.intervalMax);

    const float decay = std::clamp(loadf(p.decay), 0.0f, 1.0f);
    const float dread = std::clamp(loadf(p.dread), 0.0f, 1.0f);
    const float breathGain = std::clamp(loadf(p.breathGain), 0.0f, 1.0f);

    // Debug trims: scale each voice's target, sanitized against negatives.
    const float throbLevel = std::max(0.0f, loadf(p.throbLevel));
    const float breathLevel = std::max(0.0f, loadf(p.breathLevel));
    const float stepLevel = std::max(0.0f, loadf(p.stepLevel));
    const float waterLevel = std::max(0.0f, loadf(p.waterLevel));

    // --- ambience voice targets (sanitize tags: out-of-range -> none) ---
    auto tagOk = [](int t) { return (t >= 0 && t < kNumAmbientLoops) ? t : -1; };
    self->ambVoice[0].want = tagOk(p.zoneTagA.load(std::memory_order_relaxed));
    self->ambVoice[1].want = tagOk(p.zoneTagB.load(std::memory_order_relaxed));
    self->ambVoice[2].want = tagOk(p.waterTag.load(std::memory_order_relaxed));
    const float zoneBlend = std::clamp(loadf(p.zoneBlend), 0.0f, 1.0f);
    const float ambGain = std::max(0.0f, loadf(p.ambientGain));
    const float waterGain = std::clamp(loadf(p.waterGain), 0.0f, 1.0f);
    const float voiceGain[3] = {ambGain * (1.0f - zoneBlend),
                                ambGain * zoneBlend,
                                ambGain * waterGain * waterLevel};

    // --- footstep trigger: one per callback buffer is enough (steps are
    // ~290 ms apart at max sprint cadence vs 5-10 ms buffers). stepSeen is
    // updated unconditionally so steps queued while disabled don't all fire
    // at once on re-enable. ---
    const uint32_t stepCount = p.stepCounter.load(std::memory_order_relaxed);
    if (stepCount != self->stepSeen) {
        self->stepSeen = stepCount;
        if (enabled) {
            const float vigor = std::clamp(loadf(p.stepVigor), 0.0f, 1.0f);
            // Pitch jitter widens as coherence rots — the footsteps stop
            // agreeing with themselves before the world does.
            std::uniform_real_distribution<float> j(-1.0f, 1.0f);
            const float jitter = j(self->rng) * (0.06f + 0.25f * decay);
            self->stepKnockHz = (kStepKnockBaseHz + kStepKnockVigorHz * vigor) * (1.0f + jitter);
            self->stepKnockPhase = 0.0f;
            self->stepKnockEnv = 0.9f; // instant attack — a footfall, not a swell
            self->stepNoiseEnv = 0.5f + 0.5f * vigor;
            self->stepAmp = 0.10f + 0.10f * vigor;
            self->stepLeftFoot = !self->stepLeftFoot;
            self->stepPan = self->stepLeftFoot ? -kStepPan : kStepPan;

            // Deep in the decay, sometimes the step comes back. Arm a delayed
            // re-fire of this same voice — wrong side, a touch flat, quieter.
            std::uniform_real_distribution<float> r01(0.0f, 1.0f);
            if (decay > kEchoDecayGate && self->echoCountdown < 0 &&
                r01(self->rng) < kEchoOdds) {
                std::uniform_real_distribution<float> d(kEchoDelayMinS, kEchoDelayMaxS);
                self->echoCountdown =
                    static_cast<int>(d(self->rng) * static_cast<float>(kSampleRate));
                self->echoHz = self->stepKnockHz * kEchoPitch;
                self->echoVigor = vigor;
                self->echoPan = -self->stepPan;
            }
        }
    }

    // --- jump trigger: same publish pattern (and the same one-per-buffer
    // argument) as the steps; jumpSeen also updates unconditionally so jumps
    // queued while disabled don't fire on re-enable. ---
    const uint32_t jumpCount = p.jumpCounter.load(std::memory_order_relaxed);
    if (jumpCount != self->jumpSeen) {
        self->jumpSeen = jumpCount;
        if (enabled) {
            // Same widening pitch jitter as the steps — the body's sounds stop
            // agreeing with themselves as the dream rots.
            std::uniform_real_distribution<float> j(-1.0f, 1.0f);
            const float jitter = j(self->rng) * (0.06f + 0.25f * decay);
            self->jumpHz = kJumpKnockBaseHz * (1.0f + jitter);
            self->jumpPhase = 0.0f;
            self->jumpKnockEnv = 0.9f;  // instant attack, like a footfall
            self->jumpNoiseEnv = 0.8f;  // the push-off scuff
        }
    }

    // --- mumble trigger: same publish pattern as steps/jumps. ---
    const uint32_t mumbleCount = p.mumbleCounter.load(std::memory_order_relaxed);
    if (mumbleCount != self->mumbleSeen) {
        self->mumbleSeen = mumbleCount;
        if (enabled) {
            self->mumbleEnv = 1.0f;
            self->mumbleSyllPhase = 0.0f;
        }
    }

    const float humTarget =
        enabled ? std::clamp(loadf(p.lampHumGain), 0.0f, 1.0f) : 0.0f;

    // --- door creak trigger: same publish pattern. ---
    const uint32_t creakCount = p.doorCreakCounter.load(std::memory_order_relaxed);
    if (creakCount != self->creakSeen) {
        self->creakSeen = creakCount;
        if (enabled) {
            std::uniform_real_distribution<float> j(-1.0f, 1.0f);
            const float jitter = j(self->rng) * (0.08f + 0.25f * decay);
            self->creakHz = kCreakBaseHz * (1.0f + jitter);
            self->creakEnv = 1.0f;
            self->creakPhase = 0.0f;
            self->creakTremPhase = 0.0f;
        }
    }

    constexpr float dt = 1.0f / static_cast<float>(kSampleRate);
    // 1 - e^(-dt/tau): per-sample one-pole coefficient. std::exp isn't
    // constexpr-portable, so compute once on first entry (this thread only).
    static const float envCoef = 1.0f - std::exp(-dt / kEnvTauSeconds);

    constexpr float lfoInc = kLfoFreqHz / static_cast<float>(kSampleRate);

    for (ma_uint32 i = 0; i < frameCount; ++i) {
        // --- dropout state machine (per-sample so windows land precisely) ---
        self->stateSecondsLeft -= dt;
        if (self->stateSecondsLeft <= 0.0) {
            if (self->state == State::Playing) {
                self->state = State::Silent;
                self->stateSecondsLeft = uniformSeconds(self->rng, dropoutMin, dropoutMax);
            } else {
                self->state = State::Playing;
                self->stateSecondsLeft = uniformSeconds(self->rng, intervalMin, intervalMax);
            }
        }

        // --- envelope: one-pole toward target. The dropout never gates the
        // signal directly; it only moves this target, and the smoothing turns
        // the hard edge into a slow exhale. ---
        const float target = (enabled && self->state == State::Playing) ? gain : 0.0f;
        self->env += (target - self->env) * envCoef;

        // --- the drone itself ---
        const float lfo = 1.0f + kLfoDepth * std::sin(self->lfoPhase * kTwoPi);
        self->lfoPhase += lfoInc;
        if (self->lfoPhase >= 1.0f) self->lfoPhase -= 1.0f;

        float s = 0.0f;
        for (int o = 0; o < kNumOsc; ++o) {
            s += kOscGain[o] * std::sin(self->oscPhase[o] * kTwoPi);
            self->oscPhase[o] += kOscFreqHz[o] * (1.0f + kOscDetune[o] * decay) * dt;
            if (self->oscPhase[o] >= 1.0f) self->oscPhase[o] -= 1.0f;
        }
        s *= kDroneNorm * lfo;

        // --- dread throb: rides OUTSIDE the dropout envelope. Target tracks
        // enabled (so waking still silences it) and dread, smoothed by its own
        // slow one-pole so it swells in rather than switching on. ---
        static const float throbCoef = 1.0f - std::exp(-dt / kThrobTauSeconds);
        const float throbTarget = enabled ? kThrobGain * dread * throbLevel : 0.0f;
        self->throbEnv += (throbTarget - self->throbEnv) * throbCoef;
        float throb = 0.0f;
        if (self->throbEnv > 1e-5f) {
            // Positive half-sine squared: a thump, then real space until the
            // next beat. The carrier sits under the drone's lowest oscillator.
            const float pulse = std::max(0.0f, std::sin(self->throbPulsePhase * kTwoPi));
            throb = std::sin(self->throbCarrierPhase * kTwoPi) * pulse * pulse * self->throbEnv;
        }
        self->throbCarrierPhase += kThrobCarrierHz * dt;
        if (self->throbCarrierPhase >= 1.0f) self->throbCarrierPhase -= 1.0f;
        self->throbPulsePhase += kThrobPulseHz * dt;
        if (self->throbPulsePhase >= 1.0f) self->throbPulsePhase -= 1.0f;

        // --- the breath: lowpassed noise under an asymmetric inhale/exhale
        // cycle (quick in, long out, a pause at the bottom). Like the throb it
        // rides outside the dropout windows, and its slow envelope is what
        // turns the game zeroing breathGain mid-stare into a held breath. ---
        static const float breathCoef = 1.0f - std::exp(-dt / kBreathTauSeconds);
        const float breathTarget = enabled ? kBreathGain * breathGain * breathLevel : 0.0f;
        self->breathEnv += (breathTarget - self->breathEnv) * breathCoef;
        float breath = 0.0f;
        if (self->breathEnv > 1e-5f) {
            const float ph = self->breathPhase;
            float shape = 0.0f;
            if (ph < 0.35f) {
                shape = std::sin((ph / 0.35f) * (0.25f * kTwoPi));          // inhale: 0 -> 1
            } else if (ph < 0.85f) {
                shape = std::cos(((ph - 0.35f) / 0.5f) * (0.25f * kTwoPi)); // exhale: 1 -> 0
            }                                                               // then the pause
            uint32_t x = self->noiseState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self->noiseState = x;
            const float white = static_cast<float>(x) * (1.0f / 2147483648.0f) - 1.0f;
            static const float breathLpCoef = 1.0f - std::exp(-dt * kTwoPi * kBreathLpHz);
            self->breathLP += (white - self->breathLP) * breathLpCoef;
            breath = self->breathLP * shape * self->breathEnv;
        }
        self->breathPhase += kBreathCycleHz * dt;
        if (self->breathPhase >= 1.0f) self->breathPhase -= 1.0f;

        // --- echo footstep: the armed re-fire lands here. The real step's
        // envelopes (tau 0.09s / 0.03s) are dead long before the 0.3s minimum
        // delay, so retriggering the same voice never cuts anything off. The
        // foot alternation is deliberately untouched — your own gait stays
        // honest; only the extra step is wrong. ---
        // Advance the countdown whether or not audio is enabled, so an echo
        // armed before a disable doesn't freeze and re-fire on re-enable (the
        // step/jump/mumble/creak triggers drop queued events the same way).
        // Only emit the echo when enabled and the countdown elapses.
        if (self->echoCountdown >= 0 && --self->echoCountdown < 0 && enabled) {
            self->stepKnockHz = self->echoHz;
            self->stepKnockPhase = 0.0f;
            self->stepKnockEnv = 0.9f;
            self->stepNoiseEnv = 0.5f + 0.5f * self->echoVigor;
            self->stepAmp = (0.10f + 0.10f * self->echoVigor) * kEchoAmp;
            self->stepPan = self->echoPan;
        }

        // --- footstep voice: knock (low sine, pitch droop) + noise scuff
        // (xorshift32 -> one-pole lowpass). Rides outside the dropout envelope
        // like the throb. Skipped entirely once both envelopes die out. ---
        static const float knockCoef = 1.0f - std::exp(-dt / kStepKnockTauSeconds);
        static const float noiseCoef = 1.0f - std::exp(-dt / kStepNoiseTauSeconds);
        static const float scuffLpCoef = 1.0f - std::exp(-dt * kTwoPi * kStepNoiseLpHz);
        float stepL = 0.0f, stepR = 0.0f;
        if (self->stepKnockEnv > 1e-4f || self->stepNoiseEnv > 1e-4f) {
            self->stepKnockHz *= 1.0f - 0.8f * dt; // pitch droop — a dull thud
            self->stepKnockPhase += self->stepKnockHz * dt;
            if (self->stepKnockPhase >= 1.0f) self->stepKnockPhase -= 1.0f;
            const float knock = std::sin(self->stepKnockPhase * kTwoPi) * self->stepKnockEnv;
            self->stepKnockEnv -= self->stepKnockEnv * knockCoef;

            uint32_t x = self->noiseState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self->noiseState = x;
            const float white = static_cast<float>(x) * (1.0f / 2147483648.0f) - 1.0f;
            self->stepNoiseLP += (white - self->stepNoiseLP) * scuffLpCoef;
            const float scuff = self->stepNoiseLP * self->stepNoiseEnv;
            self->stepNoiseEnv -= self->stepNoiseEnv * noiseCoef;

            const float step = (knock + 0.6f * scuff) * self->stepAmp * stepLevel;
            stepL = step * (1.0f - std::max(0.0f, self->stepPan));
            stepR = step * (1.0f - std::max(0.0f, -self->stepPan));
        }

        // --- jump voice: the footstep's knock + scuff, except the knock's
        // pitch bends gently up as the envelope dies — a footfall in reverse,
        // the body leaving instead of landing. Mono/center, scaled by the
        // same body trim as the steps, outside the dropout envelope. ---
        static const float jumpKnockCoef = 1.0f - std::exp(-dt / kJumpKnockTauSeconds);
        static const float jumpNoiseCoef = 1.0f - std::exp(-dt / kJumpNoiseTauSeconds);
        float jump = 0.0f;
        if (self->jumpKnockEnv > 1e-4f || self->jumpNoiseEnv > 1e-4f) {
            self->jumpHz *= 1.0f + kJumpSweepRate * dt;
            self->jumpPhase += self->jumpHz * dt;
            if (self->jumpPhase >= 1.0f) self->jumpPhase -= 1.0f;
            const float knock = std::sin(self->jumpPhase * kTwoPi) * self->jumpKnockEnv;
            self->jumpKnockEnv -= self->jumpKnockEnv * jumpKnockCoef;

            uint32_t x = self->noiseState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self->noiseState = x;
            const float white = static_cast<float>(x) * (1.0f / 2147483648.0f) - 1.0f;
            self->jumpNoiseLP += (white - self->jumpNoiseLP) * scuffLpCoef;
            const float scuff = self->jumpNoiseLP * self->jumpNoiseEnv;
            self->jumpNoiseEnv -= self->jumpNoiseEnv * jumpNoiseCoef;

            jump = (knock + 0.6f * scuff) * kJumpAmp * stepLevel;
        }

        // --- mumble voice: syllable-gated lowpassed noise. Mono/center,
        // outside the dropout envelope (a voice keeps talking when the room
        // holds its breath). Decay slows the cadence and darkens the band —
        // deep-dream residents slur. ---
        static const float mumbleCoef = 1.0f - std::exp(-dt / kMumbleTauSeconds);
        float mumble = 0.0f;
        if (self->mumbleEnv > 1e-3f) {
            uint32_t x = self->noiseState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self->noiseState = x;
            const float white = static_cast<float>(x) * (1.0f / 2147483648.0f) - 1.0f;
            // Voice band narrows with decay (0.020 -> 0.012 one-pole coef).
            const float lpCoef = 0.020f - 0.008f * decay;
            self->mumbleLP += (white - self->mumbleLP) * lpCoef;

            self->mumbleSyllPhase += kMumbleSyllHz * (1.0f - 0.45f * decay) * dt;
            if (self->mumbleSyllPhase >= 1.0f) self->mumbleSyllPhase -= 1.0f;
            const float syll = std::fabs(std::sin(self->mumbleSyllPhase * kTwoPi));

            mumble = self->mumbleLP * syll * self->mumbleEnv * kMumbleAmp * 8.0f;
            self->mumbleEnv -= self->mumbleEnv * mumbleCoef;
        }

        // --- lamp hum: proximity-faded mains buzz. Continuous, mono, and
        // outside the dropout envelope — the lamp doesn't care whether the
        // room is holding its breath. ---
        static const float humCoef = 1.0f - std::exp(-dt / kHumTauSeconds);
        float hum = 0.0f;
        self->humEnv += (humTarget - self->humEnv) * humCoef;
        if (self->humEnv > 1e-4f) {
            self->humPhase += kHumHz * (1.0f + 0.04f * decay) * dt;
            if (self->humPhase >= 1.0f) self->humPhase -= 1.0f;
            const float ph = self->humPhase * kTwoPi;
            hum = (std::sin(ph) + 0.22f * std::sin(3.0f * ph)) *
                  self->humEnv * kHumAmp;
        }

        // --- door creak voice: stick-slip squeal. The pitch glides down as
        // the env decays (hz rides the env) and a fast tremolo roughs up the
        // tone so it reads as wood against wood, not a beep. Mono/center,
        // outside the dropout envelope like the other body sounds. ---
        static const float creakCoef = 1.0f - std::exp(-dt / kCreakTauSeconds);
        float creak = 0.0f;
        if (self->creakEnv > 1e-3f) {
            const float hz = self->creakHz * (0.72f + 0.28f * self->creakEnv);
            self->creakPhase += hz * dt;
            if (self->creakPhase >= 1.0f) self->creakPhase -= 1.0f;
            self->creakTremPhase += kCreakTremHz * dt;
            if (self->creakTremPhase >= 1.0f) self->creakTremPhase -= 1.0f;
            const float trem =
                0.65f + 0.35f * std::sin(self->creakTremPhase * kTwoPi);
            creak = std::sin(self->creakPhase * kTwoPi) * trem *
                    self->creakEnv * kCreakAmp;
            self->creakEnv -= self->creakEnv * creakCoef;
        }

        // --- ambient loop voices: zone A/B crossfade + positional water.
        // Nearest-sample playback from the 22050 Hz bank (the aliasing IS the
        // texture). Each voice's envelope doubles as the tag-swap gate: a new
        // tag fades the old loop out, swaps at the bottom, fades back in. The
        // detune wobble scales with decay so the place slowly goes seasick.
        // Rides outside the dropout envelope — the world keeps sounding when
        // the room tone holds its breath. ---
        static const float ambCoef = 1.0f - std::exp(-dt / kAmbTauSeconds);
        float amb = 0.0f;
        for (int v = 0; v < 3; ++v) {
            Impl::AmbVoice& av = self->ambVoice[v];
            const bool match = (av.cur == av.want);
            const float tgt =
                (enabled && match && av.cur >= 0) ? voiceGain[v] : 0.0f;
            av.env += (tgt - av.env) * ambCoef;
            if (!match && av.env < 1e-3f) {
                av.cur = av.want;
                av.pos = 0.0;
            }
            if (av.cur >= 0 && av.env > 1e-4f) {
                const std::vector<float>& loop = self->ambientLoops[av.cur];
                if (!loop.empty()) {
                    amb += loop[static_cast<size_t>(av.pos)] * av.env;
                    av.lfo += (0.11f + 0.05f * v) * dt;
                    if (av.lfo >= 1.0f) av.lfo -= 1.0f;
                    av.pos += kAmbientStep *
                              (1.0f + decay * 0.12f * std::sin(av.lfo * kTwoPi));
                    if (av.pos >= static_cast<double>(loop.size())) {
                        av.pos -= static_cast<double>(loop.size());
                    }
                }
            }
        }

        // Soft safety clip. At nominal levels tanh is ~transparent (input is
        // well under 1.0); it only matters if someone cranks params.gain.
        // The drone is mono by design — the room hums the same everywhere —
        // but footsteps alternate slightly left/right, the mix's only stereo
        // asymmetry.
        out[i * kChannels + 0] = std::tanh(s * self->env + throb + breath + amb + stepL + jump + mumble + creak + hum);
        out[i * kChannels + 1] = std::tanh(s * self->env + throb + breath + amb + stepR + jump + mumble + creak + hum);
    }

    // Publish dropout state for the overlay. Once per callback is plenty —
    // the overlay redraws at frame rate, not sample rate.
    self->droppedOutFlag.store(self->state == State::Silent, std::memory_order_relaxed);
}

// ----------------------------------------------------------------------------

Audio::Audio(unsigned int seed)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->params = &params;

    // Seed and prime the dropout scheduler here, BEFORE the device starts —
    // the callback thread doesn't exist yet, so touching its state is safe.
    // After ma_device_start this state belongs to the callback alone.
    m_impl->rng.seed(seed);
    m_impl->ambientLoops = buildAmbientLoops(seed); // before start: no race
    m_impl->state = Impl::State::Playing;
    m_impl->stateSecondsLeft = uniformSeconds(m_impl->rng,
                                              loadf(params.intervalMin),
                                              loadf(params.intervalMax));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;   // we do float DSP; let miniaudio convert
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kSampleRate;
    cfg.dataCallback = &Impl::dataCallback;
    cfg.pUserData = m_impl.get();          // raw pointer is fine: Impl outlives the device

    // Audio failure is never fatal. A dream you can't hear is still a dream;
    // the game just runs silent and ok() stays false.
    if (ma_device_init(nullptr, &cfg, &m_impl->device) != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] device init failed; running silent\n");
        return;
    }
    m_impl->deviceInited = true;

    if (ma_device_start(&m_impl->device) != MA_SUCCESS) {
        std::fprintf(stderr, "[audio] device start failed; running silent\n");
        ma_device_uninit(&m_impl->device);
        m_impl->deviceInited = false;
        return;
    }
    m_impl->deviceOk = true;
}

Audio::~Audio() {
    // ma_device_uninit stops the device and joins the callback — after this
    // returns, no thread can touch Impl, so unique_ptr teardown is safe.
    if (m_impl && m_impl->deviceInited) {
        ma_device_uninit(&m_impl->device);
    }
}

bool Audio::ok() const {
    return m_impl->deviceOk;
}

bool Audio::droppedOut() const {
    return m_impl->droppedOutFlag.load(std::memory_order_relaxed);
}

} // namespace liminal
