#pragma once
// Minimal audio layer: a single procedural room-tone drone (cluster of slow,
// detuned low sines) that randomly drops out entirely for 5-15 seconds.
// Silence arriving is the instrument. No music, ever.
//
// All DSP happens on miniaudio's callback thread; the game pokes parameters
// through atomics only. miniaudio itself is hidden behind a pimpl so this
// header stays cheap to include.

#include <atomic>
#include <cstdint>
#include <memory>

namespace liminal {

struct AudioParams {
    std::atomic<float> gain{0.14f};
    std::atomic<bool> enabled{true};
    // Dropout scheduling (seconds). The callback thread owns the RNG.
    std::atomic<float> dropoutMin{5.0f};
    std::atomic<float> dropoutMax{15.0f};
    std::atomic<float> intervalMin{18.0f};
    std::atomic<float> intervalMax{50.0f};
    // 0..1 coherence decay — plumbed now, will detune/thicken the drone later.
    std::atomic<float> decay{0.0f};
    // 0..1 figure proximity. Drives a heartbeat-like sub-throb that is NOT
    // gated by the drone's dropout windows — when the room tone falls silent
    // and the throb is still there, silence stops being safe.
    std::atomic<float> dread{0.0f};
    // 0..1 breath presence. Like the throb it rides outside the dropout
    // windows: slow inhale/exhale noise that swells as the figure nears.
    // The game zeroes it while the player stares the figure down — the
    // thing holds its breath when it is being seen.
    std::atomic<float> breathGain{0.0f};
    // Footsteps. The game bumps stepCounter once per stride; the callback
    // fires a one-shot voice on each change. stepVigor (0..1, how hard the
    // foot lands) must be stored before the counter — the counter is the
    // publish signal.
    std::atomic<uint32_t> stepCounter{0};
    std::atomic<float> stepVigor{0.5f};
    // Jump takeoff. Same publish pattern as the steps: the game bumps the
    // counter once per jump; the callback fires a short rising chirp.
    std::atomic<uint32_t> jumpCounter{0};
    // NPC speech. Bumped when a resident's line lands; the callback fires a
    // short syllable-shaped mumble (words removed, cadence kept).
    std::atomic<uint32_t> mumbleCounter{0};
    // Door swings. Bumped on each open-start/close-start; the callback fires
    // a short stick-slip creak (descending pitch glide).
    std::atomic<uint32_t> doorCreakCounter{0};
    // 0..1 proximity to the nearest interior lamp; drives a quiet electric
    // hum (continuous voice, not a one-shot).
    std::atomic<float> lampHumGain{0.0f};

    // v3 zone ambience. Tags index the procedural loop bank (same order as
    // the game's AmbientTag enum: running_water, waves, wind, drone, rain,
    // machinery, silence, birds_wrong); -1 = no loop. Two zone voices
    // crossfaded by zoneBlend (0 = all A), plus a positional water voice
    // whose gain the game derives from distance-to-water. decay (above)
    // detunes all three — the ambience unravels with the world.
    std::atomic<int> zoneTagA{-1};
    std::atomic<int> zoneTagB{-1};
    std::atomic<float> zoneBlend{0.0f};
    std::atomic<float> ambientGain{0.03f};
    std::atomic<int> waterTag{-1};
    std::atomic<float> waterGain{0.0f};

    // Debug-panel level trims (1 = authored level). These scale each voice's
    // target gain; the game-driven values above (dread, breathGain, waterGain)
    // stay untouched so tuning doesn't fight the simulation.
    std::atomic<float> throbLevel{1.0f};
    std::atomic<float> breathLevel{1.0f};
    std::atomic<float> stepLevel{1.0f};
    std::atomic<float> waterLevel{1.0f};
};

class Audio {
public:
    explicit Audio(unsigned int seed);
    ~Audio();

    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const;          // device started successfully
    bool droppedOut() const;  // currently in a silence window (for the overlay)

    AudioParams params;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace liminal
