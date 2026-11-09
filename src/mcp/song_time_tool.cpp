// GucciBot (2026-11-08): gucci_song_time, for Claude's level tooling, which
// has to know where Geometry Dash thinks the song is.
//
// Three answers, all read where GD keeps them:
//   - the music: FMOD's own position of the level's song (music id 0, the
//     channel PlayLayer::prepareMusic loads it on), to the sample;
//   - the level time GD syncs the song to: prepareMusic sets the song to
//     (song offset + m_gameState.m_levelTime) when the music starts;
//   - GD's own time for a position: PlayLayer::timeForPos, which walks the
//     level's sorted speed objects (m_speedObjects) through
//     LevelTools::timeForPos. A start position's attempt starts the song at
//     that time: loadStartPosObject asks it for (player x, 0) with the start
//     position's target order and channel and no song triggers, and
//     loadUpToPosition makes it the level time.
// speed_objects lists that array in GD's own order, with what timeForPos reads
// off each object (m_speedStart, m_speedModType, order, channel, a
// teleport's target), so a wrong answer can be traced to the object behind it.
//
// practice_music_sync (test launches only) forces GD's practice music sync on
// for the rest of the launch, the way Mega Hack's Practice Music Hack does
// for a player: PlayLayer::toggleMusicInPractice decides m_practiceMusicSync
// (the shop's music unlock, or game variable 0125 on an editor level), and
// the hook below keeps it on. Never saved; nothing else changes.
//
// timeForPos sets GD's "last gameplay" statics (rotated, reversed, time warp),
// as every call GD makes to it does; nothing else here writes anything.

#include "mcp_server.hpp"

#include "core/GucciBot.hpp"
#include "hacks/safemode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace geode::prelude;

namespace {
    bool g_forceSync = false;  // practice_music_sync, this launch only
}  // namespace

class $modify(GBSongTimeSync, PlayLayer) {
    void toggleMusicInPractice() {
        PlayLayer::toggleMusicInPractice();
        if (g_forceSync)
            m_practiceMusicSync = true;
    }
};

namespace gucci::mcp {

    namespace {
        matjson::Value songSchema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("xs", "array", "x positions to ask GD's timeForPos about (y 0, as a start position asks)");
            field("points", "array", "[x, y] pairs to ask about");
            field("order", "integer", "the target order to ask with (a start position's; default 0)");
            field("channel", "integer", "the target channel to ask with (a start position's; default 0)");
            field("song_triggers", "boolean", "count song triggers (default false, as a start position asks)");
            field("speed_objects", "boolean",
                  "list GD's sorted speed objects (what timeForPos walks), in GD's order");
            field("from_x", "number", "speed_objects: only those whose m_speedStart.x is at least this");
            field("to_x", "number", "speed_objects: only those whose m_speedStart.x is at most this");
            field("practice_music_sync", "boolean",
                  "test launches only: true forces GD's practice music sync on for the rest of the "
                  "launch (as Mega Hack's Practice Music Hack does), false gives it back to GD");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            return schema;
        }

        double num(matjson::Value const& v, double def = 0.0) {
            if (auto d = v.asDouble())
                return *d;
            if (auto i = v.asInt())
                return (double)*i;
            return def;
        }

        int64_t intArg(matjson::Value const& a, char const* key, int64_t def) {
            if (!a.contains(key))
                return def;
            if (auto i = a[key].asInt())
                return *i;
            if (auto d = a[key].asDouble())
                return (int64_t)*d;
            return def;
        }

        bool boolArg(matjson::Value const& a, char const* key, bool def) {
            if (!a.contains(key))
                return def;
            return a[key].asBool().unwrapOr(def);
        }

        // The level's song as FMOD plays it: music id 0, the one prepareMusic loads.
        matjson::Value musicJson() {
            auto m = matjson::Value::object();
            auto* engine = FMODAudioEngine::sharedEngine();
            if (!engine) {
                m["engine"] = false;
                return m;
            }
            auto const it = engine->m_fmodMusic.find(0);
            if (it == engine->m_fmodMusic.end()) {
                m["loaded"] = false;
                return m;
            }
            m["loaded"] = true;
            m["channel_id"] = (int64_t)it->second.m_channelID;
            m["file"] = std::string(it->second.m_filePath);
            auto* channel = engine->channelForChannelID(it->second.m_channelID);
            m["channel"] = channel != nullptr;
            if (!channel)
                return m;
            bool playing = false, paused = false;
            channel->isPlaying(&playing);
            channel->getPaused(&paused);
            m["playing"] = playing && !paused;
            m["paused"] = paused;
            unsigned int ms = 0, pcm = 0;
            channel->getPosition(&ms, FMOD_TIMEUNIT_MS);
            channel->getPosition(&pcm, FMOD_TIMEUNIT_PCM);
            m["position_ms"] = (int64_t)ms;
            m["position_pcm"] = (int64_t)pcm;
            FMOD::Sound* sound = nullptr;
            if (channel->getCurrentSound(&sound) == FMOD_OK && sound) {
                float freq = 0.f;
                int priority = 0;
                if (sound->getDefaults(&freq, &priority) == FMOD_OK && freq > 0.f) {
                    m["sample_rate"] = (double)freq;
                    m["position_exact_ms"] = (double)pcm * 1000.0 / (double)freq;
                }
                unsigned int len = 0;
                if (sound->getLength(&len, FMOD_TIMEUNIT_MS) == FMOD_OK)
                    m["length_ms"] = (int64_t)len;
            }
            float pitch = 1.f;
            if (channel->getPitch(&pitch) == FMOD_OK)
                m["pitch"] = (double)pitch;
            // GD's own read of the same thing (what a checkpoint saves).
            m["gd_music_time_ms"] = (int64_t)engine->getMusicTimeMS(0);
            return m;
        }

        matjson::Value speedObjectJson(int index, EffectGameObject* o) {
            auto e = matjson::Value::object();
            e["i"] = (int64_t)index;
            e["id"] = (int64_t)o->m_objectID;
            e["uid"] = (int64_t)o->m_uniqueID;
            e["x"] = o->m_positionX;
            e["y"] = o->m_positionY;
            e["start_x"] = (double)o->m_speedStart.x;
            e["start_y"] = (double)o->m_speedStart.y;
            e["type"] = (int64_t)o->m_speedModType;
            e["order"] = (int64_t)o->m_ordValue;
            e["channel"] = (int64_t)o->m_channelValue;
            if (o->m_isHide)
                e["hidden"] = true;
            if (o->m_targetGroupID != 0)
                e["target_group"] = (int64_t)o->m_targetGroupID;
            if (o->m_speedModType == -2)
                e["time_mod"] = (double)o->m_timeWarpTimeMod;
            if (o->m_speedModType == -5) {
                if (auto* tp = typeinfo_cast<TeleportPortalObject*>(o)) {
                    e["teleport_x"] = (double)tp->m_teleportPosition.x;
                    e["teleport_y"] = (double)tp->m_teleportPosition.y;
                    e["ignore_x"] = tp->m_ignoreX;
                    e["ignore_y"] = tp->m_ignoreY;
                    e["y_offset"] = (double)tp->m_teleportYOffset;
                }
            }
            return e;
        }
    }  // namespace

    void registerSongTimeTools(Server& server) {
        server.addTool({
            "gucci_song_time",
            "Where GD has the song in the open level. music: FMOD's position of the level's song "
            "(position_exact_ms to the sample, playing, gd_music_time_ms as GD reads it). level: "
            "level_time (m_gameState.m_levelTime, what GD syncs the song to), song_offset, "
            "expected_music_ms = (song_offset + level_time) * 1000 and music_minus_expected_ms, "
            "practice and practice_music_sync, the player and the start position. times: GD's own "
            "PlayLayer::timeForPos for each of xs (y 0) or points ([x, y]), with order, channel and "
            "song_triggers as given (a start position asks for (player x, 0) with its target order "
            "and channel, no song triggers). speed_objects: GD's sorted speed objects, what "
            "timeForPos walks. practice_music_sync (test launches only) forces GD's practice music "
            "sync on for the launch, as Mega Hack's Practice Music Hack does. Reads nothing else "
            "and changes nothing else.",
            songSchema(),
            [](matjson::Value const& a) {
                auto* pl = PlayLayer::get();
                if (a.contains("practice_music_sync")) {
                    if (!gucci::safemode::testLaunch())
                        throw ToolError("practice_music_sync: test launches only (tools/assist.py launch)");
                    g_forceSync = boolArg(a, "practice_music_sync", false);
                    if (pl) {
                        if (g_forceSync)
                            pl->m_practiceMusicSync = true;
                        else
                            pl->toggleMusicInPractice();  // GD's own choice back
                    }
                }

                auto out = matjson::Value::object();
                out["frame"] = (int64_t)GucciEngine::get()->updater.getFrame();
                out["music"] = musicJson();
                out["forced_practice_music_sync"] = g_forceSync;
                if (!pl) {
                    out["in_level"] = false;
                    return out;
                }
                out["in_level"] = true;

                int const order = (int)intArg(a, "order", 0);
                int const channel = (int)intArg(a, "channel", 0);
                bool const songTriggers = boolArg(a, "song_triggers", false);

                auto level = matjson::Value::object();
                double const levelTime = pl->m_gameState.m_levelTime;
                float const offset = pl->m_levelSettings ? pl->m_levelSettings->m_songOffset : 0.f;
                level["level_time"] = levelTime;
                level["total_time"] = pl->m_gameState.m_totalTime;
                level["song_offset"] = (double)offset;
                double const expected = ((double)offset + levelTime) * 1000.0;
                level["expected_music_ms"] = expected;
                if (out["music"].contains("position_exact_ms"))
                    level["music_minus_expected_ms"] = num(out["music"]["position_exact_ms"]) - expected;
                level["practice"] = pl->m_isPracticeMode;
                level["practice_music_sync"] = pl->m_practiceMusicSync;
                level["started"] = pl->m_started;
                if (pl->m_player1) {
                    auto const p = pl->m_player1->getPosition();
                    level["player_x"] = (double)p.x;
                    level["player_y"] = (double)p.y;
                    // What a start position here would start the song at.
                    level["time_for_player"] =
                        (double)pl->timeForPos(CCPoint(p.x, 0.f), order, channel, songTriggers, 0);
                }
                if (auto* sp = pl->m_startPosObject) {
                    auto s = matjson::Value::object();
                    s["x"] = sp->m_positionX;
                    s["y"] = sp->m_positionY;
                    if (auto* ss = sp->m_startSettings) {
                        s["target_order"] = (int64_t)ss->m_targetOrder;
                        s["target_channel"] = (int64_t)ss->m_targetChannel;
                    }
                    level["start_pos"] = s;
                }
                level["speed_objects"] = (int64_t)(pl->m_speedObjects ? pl->m_speedObjects->count() : 0);
                out["level"] = level;

                // GD's time for each position asked about.
                std::vector<CCPoint> asked;
                if (a.contains("xs") && a["xs"].isArray()) {
                    auto const& xs = a["xs"];
                    for (size_t i = 0; i < xs.size(); i++)
                        asked.emplace_back((float)num(xs[i]), 0.f);
                }
                if (a.contains("points") && a["points"].isArray()) {
                    auto const& pts = a["points"];
                    for (size_t i = 0; i < pts.size(); i++) {
                        auto const& v = pts[i];
                        if (!v.isArray() || v.size() < 2)
                            throw ToolError("points are [x, y] pairs");
                        asked.emplace_back((float)num(v[0]), (float)num(v[1]));
                    }
                }
                if (asked.size() > 20000)
                    throw ToolError("at most 20000 positions per call");
                if (!asked.empty()) {
                    auto times = matjson::Value::array();
                    for (auto const& p : asked) {
                        auto t = matjson::Value::object();
                        t["x"] = (double)p.x;
                        t["y"] = (double)p.y;
                        t["t"] = (double)pl->timeForPos(p, order, channel, songTriggers, 0);
                        times.push(t);
                    }
                    out["times"] = times;
                }

                if (boolArg(a, "speed_objects", false) && pl->m_speedObjects) {
                    double const lo = a.contains("from_x") ? num(a["from_x"], -1e30) : -1e30;
                    double const hi = a.contains("to_x") ? num(a["to_x"], 1e30) : 1e30;
                    auto list = matjson::Value::array();
                    int i = 0;
                    for (auto* obj : CCArrayExt<CCObject*>(pl->m_speedObjects)) {
                        auto* o = typeinfo_cast<EffectGameObject*>(obj);
                        if (o && o->m_speedStart.x >= lo && o->m_speedStart.x <= hi)
                            list.push(speedObjectJson(i, o));
                        i++;
                    }
                    out["speed_objects"] = list;
                }
                return out;
            },
        });
    }

}  // namespace gucci::mcp
