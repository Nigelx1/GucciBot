#include "client.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

#include "absense/compat/devlog.hpp"
#include "absense/world/card.hpp"
#include "absense/world/world.hpp"

using namespace geode::prelude;

namespace absense::gpu {

namespace {

constexpr const char* kExe = "absense-gpu.exe";

// The rise per unit of x a dash orb sends the classic player along (not
// sideways, not going left, not platformer): physics/player.cpp,
// startDashing, clamped to 70 degrees either way like the game does it.
float dashRise(GameObject* ring) {
    float angle = ring->getObjectRotation();
    if (ring->isFlipX()) angle += 180.0f;
    if (std::fabs(ring->getRotationX() - ring->getRotationY()) > 179.0f) angle += 180.0f;
    angle = std::fmod(-angle, 360.0f);
    if (angle < -180.0f) {
        angle += 360.0f;
    } else if (angle > 180.0f) {
        angle -= 360.0f;
    }
    float helper = angle;
    if (std::fabs(helper) > 90.0f) {
        angle = helper <= 0.0f ? -180.0f : 180.0f;
        helper = angle - helper;
        if (helper < -180.0f) {
            helper += 360.0f;
        } else if (helper > 180.0f) {
            helper -= 360.0f;
        }
    }
    angle = std::clamp(helper, -70.0f, 70.0f);
    const float rad = angle * 0.01745329f;
    const float across = std::fabs(std::cos(rad));
    return across > 1.0e-4f ? std::sin(rad) / across : 0.0f;
}

// What the shader needs to know about a game object, or nothing when it is
// something the simplified model has no idea about (a trigger, a teleport, a
// dual portal): those are left out, and the exact simulation catches whatever
// the card therefore got wrong. Object::flags carries the kFlag... bits.
bool classify(GameObject* o, Object& out) {
    if (!o || o->m_isGroupDisabled || o->m_isDisabled) return false;
    uint32_t kind;
    float power = 1.0f;
    uint32_t flags = 0;
    switch (o->m_objectType) {
        case GameObjectType::Solid:
        case GameObjectType::Breakable:
            kind = kSolid;
            break;
        case GameObjectType::Slope:
            kind = kSlope;
            break;
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
            kind = kHazard;
            break;
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing: {
            auto* ring = static_cast<RingObject*>(o);
            if (ring->m_isMultiActivate) flags |= kFlagMultiActivate;
            switch (o->m_objectType) {
                case GameObjectType::YellowJumpRing: kind = kJumpOrb; power = 1.0f; break;
                case GameObjectType::PinkJumpRing: kind = kJumpOrb; power = 0.72f; break;
                case GameObjectType::RedJumpRing: kind = kJumpOrb; power = 1.38f; break;
                case GameObjectType::GravityRing: kind = kGravityOrb; power = 0.8f; break;
                case GameObjectType::GreenRing: kind = kGravityOrb; power = 1.0f; break;
                case GameObjectType::DropRing: kind = kDropOrb; break;
                case GameObjectType::SpiderOrb:
                    kind = kSpiderOrb;
                    if (o->isFacingDown()) flags |= kFlagFacingDown;
                    break;
                case GameObjectType::DashRing: kind = kDashOrb; power = dashRise(o); break;
                default: kind = kGravityDashOrb; power = dashRise(o); break;
            }
            break;
        }
        case GameObjectType::YellowJumpPad:
            kind = kJumpPad;
            power = 1.0f;
            break;
        case GameObjectType::PinkJumpPad:
            kind = kJumpPad;
            power = 0.65f;
            break;
        case GameObjectType::RedJumpPad:
            kind = kJumpPad;
            power = 1.25f;
            break;
        case GameObjectType::GravityPad:
            kind = kGravityPad;
            if (o->isFacingDown()) flags |= kFlagFacingDown;
            break;
        case GameObjectType::SpiderPad:
            kind = kSpiderPad;
            if (o->isFacingDown()) flags |= kFlagFacingDown;
            break;
        case GameObjectType::InverseGravityPortal:
            kind = kGravityPortalUp;
            break;
        case GameObjectType::NormalGravityPortal:
            kind = kGravityPortalDown;
            break;
        case GameObjectType::GravityTogglePortal:
            kind = kGravityToggle;
            break;
        // The modes as PlayerState::mode numbers them (Trajectory::FakeMode).
        case GameObjectType::CubePortal: kind = kModePortal; power = 0.0f; break;
        case GameObjectType::ShipPortal: kind = kModePortal; power = 1.0f; break;
        case GameObjectType::BallPortal: kind = kModePortal; power = 2.0f; break;
        case GameObjectType::UfoPortal: kind = kModePortal; power = 3.0f; break;
        case GameObjectType::WavePortal: kind = kModePortal; power = 4.0f; break;
        case GameObjectType::RobotPortal: kind = kModePortal; power = 5.0f; break;
        case GameObjectType::SpiderPortal: kind = kModePortal; power = 6.0f; break;
        case GameObjectType::SwingPortal: kind = kModePortal; power = 7.0f; break;
        case GameObjectType::MiniSizePortal:
            kind = kSizePortal;
            power = 0.6f;
            break;
        case GameObjectType::RegularSizePortal:
            kind = kSizePortal;
            power = 1.0f;
            break;
        default:
            // A speed portal is a trigger that goes by id: the speeds physics/collisions.cpp
            // triggerObject hands the next tick.
            switch (o->m_objectID) {
                case 200: power = 0.7f; break;
                case 201: power = 0.9f; break;
                case 202: power = 1.1f; break;
                case 203: power = 1.3f; break;
                case 1334: power = 1.6f; break;
                default: return false;
            }
            kind = kSpeedPortal;
            break;
    }
    const cocos2d::CCRect r = o->getObjectRect();
    out.minX = r.getMinX();
    out.minY = r.getMinY();
    out.maxX = r.getMaxX();
    out.maxY = r.getMaxY();
    out.radius = o->m_objectRadius > 0.0f ? o->m_objectRadius * std::max(o->m_scaleX, o->m_scaleY) : 0.0f;
    out.power = power;
    out.kind = kind;
    out.flags = flags;
    out.vx = 0.0f;
    out.vy = 0.0f;
    return true;
}

// How far an object moved on the game's last step, when that step really was
// one tick of a move: moveObjects (0x22dd50) stamps what it moves with the
// layer's command index (m_unk4C4), and the index goes up by two a tick, so
// an older stamp means m_lastPosition is left over from a move that has
// ended. The bounds are the ones the exact simulation carries a mover by
// (trajectory.cpp, MovingObjects::begin).
bool liveStep(GameObject* o, uint32_t commandIndex, float& vx, float& vy) {
    vx = vy = 0.0f;
    const uint32_t marker = (uint32_t)o->m_unk4C4;
    if (marker == 0 || marker + 2u < commandIndex) return false;
    const cocos2d::CCPoint step = o->getPosition() - o->m_lastPosition;
    if (std::fabs(step.x) < 0.0005f && std::fabs(step.y) < 0.0005f) return false;
    if (std::fabs(step.x) > 40.0f || std::fabs(step.y) > 40.0f) return false;  // teleported, not moving
    vx = step.x;
    vy = step.y;
    return true;
}

// Things that change the physics in ways the simplified model has no idea
// about. Past one of these the card is not slightly wrong, it is playing a
// different game, so the slice records where they are and the mod asks the
// card only about the stretch in front of the nearest one. Modes, sizes,
// speeds, gravity toggles, dash, drop and spider orbs and spider pads are
// modelled now (see classify); what is left moves the player somewhere else
// or adds a second one.
bool changesEverything(GameObject* o) {
    if (!o || o->m_isGroupDisabled || o->m_isDisabled) return false;
    switch (o->m_objectType) {
        // (Mirror portals only flip the screen: the physics goes on unchanged,
        // so the card keeps looking past them.)
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::TeleportOrb:
        case GameObjectType::CustomRing:
            return true;
        default:
            break;
    }
    // The gravity trigger, the gameplay rotation and the teleport trigger
    // (physics/collisions.cpp, triggerObject).
    switch (o->m_objectID) {
        case 2066: case 2900: case 3022:
            return true;
        default:
            return false;
    }
}

}  // namespace

Client& Client::get() {
    static Client instance;
    return instance;
}

void Client::setEnabled(bool on) {
    if (m_enabled == on) return;
    m_enabled = on;
    if (!on) stop();
}

void Client::stop() {
#ifdef _WIN32
    if (m_process) {
        sendFrame(Message::Bye, nullptr, 0);
        // Never 200 ms on the drawing thread. Bye was sent, so a healthy
        // program is already on its way out; this wait only ever ran its full
        // length when the program was wedged - and it is terminated on the
        // next line either way. Nothing reads from it after stop().
        WaitForSingleObject((HANDLE)m_process, 0);
        TerminateProcess((HANDLE)m_process, 0);
        CloseHandle((HANDLE)m_process);
    }
    if (m_toApp) CloseHandle((HANDLE)m_toApp);
    if (m_fromApp) CloseHandle((HANDLE)m_fromApp);
    m_process = nullptr;
    m_toApp = nullptr;
    m_fromApp = nullptr;
    m_levelObjects = -1;
#else
    m_process = nullptr;
#endif
}

bool Client::start() {
#ifdef _WIN32
    if (m_process) return true;
    if (m_broken) return false;
    const auto exe = Mod::get()->getResourcesDir() / kExe;
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) {
        m_message = "absense-gpu.exe is not in the mod's resources";
        m_broken = true;
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &sa, 1 << 20) || !CreatePipe(&outRead, &outWrite, &sa, 1 << 20)) {
        m_message = "could not make the pipes";
        m_broken = true;
        return false;
    }
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   exe.parent_path().wstring().c_str(), &si, &pi);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        m_message = "absense-gpu.exe would not start";
        m_broken = true;
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_toApp = inWrite;
    m_fromApp = outRead;

    // Who answered, and is it from this build? The exe ships inside the mod,
    // but a stale one left in the resources folder would read the level and
    // the batches by another layout.
    const uint32_t version = kVersion;
    if (!sendFrame(Message::Hello, &version, sizeof(version))) {
        stop();
        m_broken = true;
        return false;
    }
    Message type{};
    std::vector<uint8_t> payload;
    if (!readFrame(type, payload, 5000) || type != Message::HelloReply) {
        m_message = payload.empty() ? "the card did not answer" : std::string(payload.begin(), payload.end());
        stop();
        m_broken = true;
        return false;
    }
    m_device.assign(payload.begin(), payload.end());
    m_message = "using " + m_device;
    devlog::logf(devlog::Cat::AbsensePathfinder, "graphics card search: %s", m_device.c_str());
    return true;
#else
    // The GPU helper is a Windows program.
    return false;
#endif
}

bool Client::sendFrame(Message type, const void* payload, size_t bytes) {
#ifdef _WIN32
    if (!m_toApp) return false;
    FrameHeader h{kMagic, type, (uint32_t)bytes};
    DWORD wrote = 0;
    if (!WriteFile((HANDLE)m_toApp, &h, sizeof(h), &wrote, nullptr) || wrote != sizeof(h)) return false;
    size_t sent = 0;
    while (sent < bytes) {
        const DWORD chunk = (DWORD)std::min<size_t>(bytes - sent, 1u << 20);
        if (!WriteFile((HANDLE)m_toApp, (const uint8_t*)payload + sent, chunk, &wrote, nullptr) || wrote == 0) {
            return false;
        }
        sent += wrote;
    }
    return true;
#else
    (void)type;
    (void)payload;
    (void)bytes;
    return false;
#endif
}

bool Client::readFrame(Message& type, std::vector<uint8_t>& payload, unsigned timeoutMs) {
#ifdef _WIN32
    if (!m_fromApp) return false;
    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + std::chrono::milliseconds(timeoutMs);
    auto readExactly = [&](void* into, size_t bytes) {
        size_t got = 0;
        while (got < bytes) {
            DWORD available = 0;
            if (!PeekNamedPipe((HANDLE)m_fromApp, nullptr, 0, nullptr, &available, nullptr)) return false;
            if (available == 0) {
                const auto at = std::chrono::steady_clock::now();
                if (at > deadline) return false;
                // Sleep(0) yields only to a ready thread of this priority on
                // this core and otherwise returns at once, so the drawing
                // thread spun at 100% for the whole wait - taking the core
                // away from absense-gpu.exe and making the very timeout it
                // was waiting on more likely. Spin for the first millisecond
                // (a fast batch still comes back in the same microsecond it
                // would have), then give the card its core back.
                if (at - began < std::chrono::milliseconds(1)) {
                    Sleep(0);
                } else {
                    Sleep(1);
                }
                continue;
            }
            DWORD read = 0;
            const DWORD want = (DWORD)std::min<size_t>(bytes - got, available);
            if (!ReadFile((HANDLE)m_fromApp, (uint8_t*)into + got, want, &read, nullptr) || read == 0) return false;
            got += read;
        }
        return true;
    };
    FrameHeader h{};
    if (!readExactly(&h, sizeof(h)) || h.magic != kMagic) return false;
    payload.resize(h.bytes);
    if (h.bytes && !readExactly(payload.data(), h.bytes)) return false;
    type = h.type;
    return true;
#else
    (void)type;
    (void)payload;
    (void)timeoutMs;
    return false;
#endif
}

void Client::sendLevel(GJBaseGameLayer* pl, float fromX, float toX, uint64_t tick, uint32_t horizon) {
    if (!pl || !pl->m_objects) return;
    if (!m_enabled || m_broken) return;  // nothing to send it to (and no scan of every object for nothing)
    // The program must be running before the level goes out, or its first
    // batch is scored against no level at all.
    if (!start()) return;
    if (m_levelObjects >= 0 && fromX >= m_levelFrom && toX <= m_levelTo) {
        // Still covered. Moving objects go out with how fast they are moving
        // and the shader carries them on from the slice's own tick
        // (PlayerState::moveOrigin), so a slice is good for a while after the
        // game has ticked on; a re-send is a scan of every object in the
        // level, a pipe transfer and a new upload, so it waits until the
        // carry has grown long.
        const uint64_t behind = tick > m_levelReadAt ? tick - m_levelReadAt : m_levelReadAt - tick;
        // Half a horizon of drift: an eased move is followed exactly to the
        // middle of the horizon the slice was taken for and carried straight
        // on from there, so that is how far it is worth trusting.
        if (!m_levelMoves || behind <= std::max<uint64_t>(60, horizon / 2)) return;
    }

    // A margin of a whole horizon ahead (and a little behind), so the next
    // few decisions do not need a new list.
    const float span = std::max(600.0f, toX - fromX);
    fromX -= 200.0f;
    toX += span;
    // Where the World has the objects it moves in the middle of the horizon
    // the card is about to be asked about: an eased move is then no further
    // out at either end of it than it has to be. Without the World (off, or a
    // level it could not read) every object goes out at its own last step,
    // exactly as the fallback path carries it.
    const world::CardSlice slice = world::cardSlice(pl, (int)(horizon / 2u), world::stepDt());
    const uint32_t commandIndex = pl->m_gameState.m_commandIndex;
    std::vector<Object> objects;
    objects.reserve(4096);
    m_unknownX.clear();
    // Which of them are moving, by how far they are from the player: a level
    // can have thousands, and the card pays for every one of them in every
    // column it can sweep through, so only the nearest few hundred are
    // carried (the cap the exact simulation uses).
    std::vector<std::pair<float, uint32_t>> movers;
    const int n = pl->m_objects->count();
    Object o{};
    for (int i = 0; i < n; i++) {
        auto* obj = static_cast<GameObject*>(pl->m_objects->objectAtIndex(i));
        if (!obj) continue;
        const float x = obj->getPositionX();
        if (x < fromX - 60.0f || x > toX + 60.0f) continue;
        if (changesEverything(obj)) m_unknownX.push_back(x);
        if (!classify(obj, o)) continue;
        if (const world::CardPose* pose = slice.ok ? slice.find(obj->m_uniqueID) : nullptr) {
            // The box classify took is around where the object stands now:
            // move it to where the World has it, and the shader carries it on
            // from there.
            const float dx = (float)pose->x - obj->getPositionX();
            const float dy = (float)pose->y - obj->getPositionY();
            o.minX += dx;
            o.maxX += dx;
            o.minY += dy;
            o.maxY += dy;
            o.vx = pose->dx;
            o.vy = pose->dy;
        } else if (!slice.ok) {
            liveStep(obj, commandIndex, o.vx, o.vy);
        }
        if (o.vx != 0.0f || o.vy != 0.0f) movers.push_back({std::fabs(x - fromX), (uint32_t)objects.size()});
        objects.push_back(o);
        if (objects.size() >= 65536) break;  // 2 MB of frame; the program's buffers grow to whatever is sent
    }
    if (movers.size() > 512) {
        // The furthest ones go out where they stand, without a speed: a wrong
        // guess costs the card's ranking a little, a slice the card cannot
        // score inside its two seconds costs the session its card.
        std::nth_element(movers.begin(), movers.begin() + 512, movers.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 512; i < movers.size(); i++) {
            objects[movers[i].second].vx = 0.0f;
            objects[movers[i].second].vy = 0.0f;
        }
        movers.resize(512);
    }
    const bool anyMoves = !movers.empty();
    if (objects.size() >= 65536) {
        // The list is cut in the order the level keeps its objects, not by
        // distance, so a hole can land anywhere in the slice: say so.
        devlog::logf(devlog::Cat::AbsensePathfinder, "graphics card: slice cut at %zu objects", objects.size());
    }

    std::sort(m_unknownX.begin(), m_unknownX.end());

    // The level's floor is not one of its objects (it is the ground layer):
    // without it every idea falls through the ground after its first tick,
    // can never jump again (no landing, no onGround) and never meets a spike
    // standing on the ground. The player stands at y 105 with a 15 half
    // height, so the floor's top is at 90; the ship rides along it like the
    // top of any block.
    {
        Object floor{};
        floor.minX = fromX;
        floor.maxX = toX;
        floor.maxY = 90.0f;
        floor.minY = 90.0f - 120.0f;
        floor.radius = 0.0f;
        floor.power = 0.0f;
        floor.kind = kSolid;
        floor.flags = 0;
        floor.vx = 0.0f;
        floor.vy = 0.0f;
        objects.push_back(floor);
    }

    std::vector<uint8_t> payload(sizeof(LevelHeader) + sizeof(Object) * objects.size());
    LevelHeader head{};
    head.objectCount = (uint32_t)objects.size();
    head.minX = fromX;
    head.maxX = toX;
    // How far a mover has to be looked for either side of where the slice put
    // it: a batch runs a horizon, and PlayerState::moveOrigin can stand it
    // half a horizon behind (the objects are placed at the middle of one) or
    // half a horizon on (the re-send rule above).
    head.sweepTicks = std::min<uint32_t>(horizon * 2u + 64u, 4096u);
    std::memcpy(payload.data(), &head, sizeof(head));
    if (!objects.empty()) {
        std::memcpy(payload.data() + sizeof(head), objects.data(), sizeof(Object) * objects.size());
    }
    if (!sendFrame(Message::Level, payload.data(), payload.size())) {
        stop();
        return;
    }
    m_levelFrom = fromX;
    m_levelTo = toX;
    m_levelObjects = (int)objects.size();
    m_levelReadAt = tick;
    // The tick the objects in it stand at: the game's own, or the middle of
    // the horizon when the World placed them there.
    m_levelTick = tick + (uint64_t)(slice.ok ? slice.ahead : 0);
    m_levelMoves = anyMoves || slice.moves;
}

bool Client::score(const PlayerState& state, uint32_t ticks, const std::vector<Script>& scripts,
                   std::vector<Result>& out) {
    out.clear();
    // The card being gone is worth knowing before the payload and not after:
    // a batch of a few hundred thousand descriptors is tens of megabytes to
    // lay out, and a session that has put the card aside would lay them out
    // again for every decision it makes from here on.
    if (!m_enabled || m_broken || scripts.empty()) return false;
    std::vector<uint8_t> payload(sizeof(BatchHeader) + sizeof(Script) * scripts.size());
    BatchHeader head{};
    head.player = state;
    head.scriptCount = (uint32_t)scripts.size();
    head.ticks = ticks;
    std::memcpy(payload.data(), &head, sizeof(head));
    std::memcpy(payload.data() + sizeof(head), scripts.data(), sizeof(Script) * scripts.size());
    return exchange(Message::Batch, payload, scripts.size(), out);
}

bool Client::scoreRaw(const PlayerState& state, uint32_t ticks, uint32_t scriptCount,
                      const std::vector<uint32_t>& words, std::vector<Result>& out) {
    out.clear();
    const uint32_t wordsPerScript = (ticks + 7) / 8;
    if (!m_enabled || m_broken || scriptCount == 0 || words.size() != (size_t)scriptCount * wordsPerScript) return false;
    std::vector<uint8_t> payload(sizeof(RawBatchHeader) + sizeof(uint32_t) * words.size());
    RawBatchHeader head{};
    head.player = state;
    head.scriptCount = scriptCount;
    head.ticks = ticks;
    head.wordsPerScript = wordsPerScript;
    std::memcpy(payload.data(), &head, sizeof(head));
    if (!words.empty()) std::memcpy(payload.data() + sizeof(head), words.data(), sizeof(uint32_t) * words.size());
    return exchange(Message::RawBatch, payload, scriptCount, out);
}

// One batch, whichever way its scripts are written: out and back over the
// pipe, with the card put aside for the session when it stalls twice.
bool Client::exchange(Message type, const std::vector<uint8_t>& payload, size_t count, std::vector<Result>& out) {
    out.clear();
    if (!start()) return false;
    if (m_levelObjects < 0) {
        // Never a batch against an empty level: every script would "survive"
        // and the ranking handed back would be noise.
        m_message = "the card has no level yet";
        return false;
    }

    const auto began = std::chrono::steady_clock::now();
    if (!sendFrame(type, payload.data(), payload.size())) {
        m_message = "the card stopped answering";
        stop();
        return false;
    }
    Message replyType{};
    std::vector<uint8_t> reply;
    if (!readFrame(replyType, reply, 2000)) {
        stop();
        if (++m_timeouts >= 2) {
            // The second stall in a row: each one froze the game for two
            // seconds and the relaunch it needs costs more. Not again.
            m_broken = true;
            m_message = "the card took too long twice; not used again this session";
        } else {
            m_message = "the card took too long";
        }
        return false;
    }
    if (replyType != Message::Results) {
        m_message = reply.empty() ? "the card refused" : std::string(reply.begin(), reply.end());
        stop();
        m_broken = true;
        return false;
    }
    out.resize(reply.size() / sizeof(Result));
    if (!out.empty()) std::memcpy(out.data(), reply.data(), out.size() * sizeof(Result));
    m_timeouts = 0;  // it answered: one slow batch on its own is forgiven
    m_batches++;
    m_scripts += count;
    m_lastCount = (uint32_t)count;
    m_lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    return out.size() == count;
}

float Client::nextUnknown(float fromX) const {
    const auto it = std::lower_bound(m_unknownX.begin(), m_unknownX.end(), fromX);
    return it == m_unknownX.end() ? 3.0e38f : *it;
}

}  // namespace absense::gpu
