#ifndef GPU_CLIENT_HPP
#define GPU_CLIENT_HPP

// Absense - the mod's side of the graphics-card search.
//
// Starts absense-gpu.exe, keeps it fed with the objects around the player,
// and asks it to score batches of candidate scripts. Everything it hands
// back is a suggestion: the caller turns the best few into ordinary ideas
// and the game's own physics judges them. If the program is missing, the
// card is busy, or anything at all goes wrong, this turns itself off and
// the search carries on exactly as it did before.

#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "protocol.hpp"

class PlayerObject;

namespace absense::gpu {

class Client {
   public:
    static Client& get();

    // On / off (the setting). Starting is lazy: the program is only run when
    // the first batch is asked for.
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    bool running() const { return m_process != nullptr; }
    const std::string& device() const { return m_device; }
    const std::string& message() const { return m_message; }

    // Numbers for the page.
    uint64_t batches() const { return m_batches; }
    uint64_t scriptsScored() const { return m_scripts; }
    double lastMs() const { return m_lastMs; }

    // The objects between `fromX` and `toX` go to the program (it keeps
    // them until the next call). Cheap to call again with the same range.
    // `tick` is the game's tick now and `horizon` how many ticks a batch will
    // ask about: what is on the move is only where the slice puts it for a
    // while, and it is sent where the World has it in the middle of that
    // horizon so the carry either way is as short as it can be.
    void sendLevel(GJBaseGameLayer* pl, float fromX, float toX, uint64_t tick, uint32_t horizon);
    // The tick the slice the program holds stands at (PlayerState::moveOrigin
    // counts from it; with the World on it is the middle of the horizon).
    uint64_t levelTick() const { return m_levelTick; }

    // Scores `scripts` from `state` over `ticks`, filling `out`. False when
    // the card could not be used (the caller then just does without).
    bool score(const PlayerState& state, uint32_t ticks, const std::vector<Script>& scripts,
               std::vector<Result>& out);
    // The same for scripts spelled out tick by tick: `words` holds
    // scriptCount * ((ticks + 7) / 8) words packed with packRawTick.
    bool scoreRaw(const PlayerState& state, uint32_t ticks, uint32_t scriptCount, const std::vector<uint32_t>& words,
                  std::vector<Result>& out);
    // How many scripts the last batch had (lastMs is what it cost).
    uint32_t lastCount() const { return m_lastCount; }

    // The x of the first thing in the slice the model has no idea about (a
    // dual, solo or teleport portal, a teleport or custom orb, the gravity,
    // rotation or teleport trigger) at or after `fromX`, or a huge number
    // when there is none.
    float nextUnknown(float fromX) const;

    void stop();
    // Runs absense-gpu.exe now, outside a decision. Process creation plus the
    // D3D11 device and the shader compile is a third of a second, and without
    // this the first sendLevel of the session pays for it inside one slice.
    void prewarm() {
        if (m_enabled && !m_broken && !m_process) (void)start();
    }
    // Forgets which part of the level the program has: the next sendLevel
    // sends it again. The program itself stays (it ends with the game), so a
    // new run does not pay for launching it inside a decision.
    void forgetLevel() { m_levelObjects = -1; }

   private:
    Client() = default;
    bool start();
    bool sendFrame(Message type, const void* payload, size_t bytes);
    bool readFrame(Message& type, std::vector<uint8_t>& payload, unsigned timeoutMs);
    // Sends a Batch or RawBatch frame and reads its `count` results.
    bool exchange(Message type, const std::vector<uint8_t>& payload, size_t count, std::vector<Result>& out);

    bool m_enabled = false;
    bool m_broken = false;  // something went wrong; do not try again this session
    int m_timeouts = 0;     // batches that ran out of time in a row (two, and the card is left alone this session)
    void* m_process = nullptr;
    void* m_toApp = nullptr;
    void* m_fromApp = nullptr;
    std::string m_device;
    std::string m_message;
    uint64_t m_batches = 0;
    uint64_t m_scripts = 0;
    double m_lastMs = 0.0;
    float m_levelFrom = 0.0f, m_levelTo = 0.0f;
    int m_levelObjects = -1;
    uint64_t m_levelTick = 0;    // the tick the slice's objects stand at
    uint64_t m_levelReadAt = 0;  // the game's tick when it was read (the World may place them ahead of it)
    bool m_levelMoves = false;   // something in the slice was on the move when it was taken
    uint32_t m_lastCount = 0;
    std::vector<float> m_unknownX;  // sorted; see nextUnknown
};

}  // namespace absense::gpu

#endif  // GPU_CLIENT_HPP
