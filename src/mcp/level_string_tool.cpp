// GucciBot (2026-11-03): gucci_open_level_string, for levels built outside
// the game (Claude's level tooling writes a level string, not a save).
//
// The level is a fresh GJGameLevel of type Editor that no level list holds:
// it opens in the play layer like a local level's Play button, and leaving
// it drops it. Nothing adds it to CCLocalLevels, and the tool only runs in a
// test launch (tools/assist.py), where GD writes no save at all
// (hooks/test_save_guard.cpp).
//
// A custom song for it comes from a file anywhere on disk: the level is
// pointed at a song id the caller picks (one with no file in GD's folder),
// and GD's song lookups answer that id with the given file. Nothing is
// copied into GD's folders. The override lasts until the next call or the
// end of the launch.

#include "mcp_server.hpp"

#include "hacks/safemode.hpp"

#include <Geode/Geode.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/MusicDownloadManager.hpp>

#include <filesystem>
#include <string>

using namespace geode::prelude;

namespace {
    struct SongOverride {
        int id = 0;
        std::string path;
    };
    SongOverride g_song;
    Ref<GJGameLevel> g_level;  // the last opened one, kept while the play layer may use it

    bool overridden(int id) { return g_song.id != 0 && id == g_song.id; }

    // The first few answers, so a test can see GD asked for the song.
    gd::string answer(char const* who) {
        static int s_logged = 0;
        if (s_logged < 8) {
            ++s_logged;
            log::info("[level string] {}: song {} -> {}", who, g_song.id, g_song.path);
        }
        return g_song.path;
    }
}  // namespace

class $modify(GBLevelStringSongs, MusicDownloadManager) {
    gd::string pathForSong(int id) {
        if (overridden(id)) return answer("pathForSong");
        return MusicDownloadManager::pathForSong(id);
    }
    bool isSongDownloaded(int id) {
        if (overridden(id)) return true;
        return MusicDownloadManager::isSongDownloaded(id);
    }
};

class $modify(GBLevelStringLevel, GJGameLevel) {
    gd::string getAudioFileName() {
        if (overridden(m_songID)) return answer("getAudioFileName");
        return GJGameLevel::getAudioFileName();
    }
};

namespace gucci::mcp {

    namespace {
        matjson::Value lsSchema() {
            auto props = matjson::Value::object();
            auto field = [&](char const* name, char const* type, char const* desc) {
                auto p = matjson::Value::object();
                p["type"] = type;
                p["description"] = desc;
                props[name] = p;
            };
            field("level_string", "string",
                  "the level: its raw string (header;object;object;...) or the compressed form GD "
                  "saves (H4sI...)");
            field("name", "string", "the level's name (shown nowhere it matters; default \"Test level\")");
            field("song_path", "string",
                  "an audio file (mp3/ogg/wav) to play as the level's custom song; needs song_id");
            field("song_id", "integer",
                  "the custom song id the level uses; with song_path, pick one GD has no file for");
            field("audio_track", "integer", "one of RobTop's songs instead (0 Stereo Madness, ...)");
            field("level_id", "integer",
                  "an id for the level (default 0), so tools that check the open level's id "
                  "(gucci_ai_validate_real) accept it; pick one no real level uses");
            auto schema = matjson::Value::object();
            schema["type"] = "object";
            schema["properties"] = props;
            schema["required"] = std::vector<std::string>{"level_string"};
            return schema;
        }

        std::string strArg(matjson::Value const& a, char const* key) {
            if (!a.contains(key)) return {};
            if (auto s = a[key].asString()) return *s;
            return {};
        }

        int64_t numArg(matjson::Value const& a, char const* key, int64_t def) {
            if (!a.contains(key)) return def;
            if (auto i = a[key].asInt()) return *i;
            if (auto d = a[key].asDouble()) return (int64_t)*d;
            return def;
        }
    }  // namespace

    void registerLevelStringTools(Server& server) {
        server.addTool({
            "gucci_open_level_string",
            "Open a level given as a level string in the play layer, without saving it "
            "anywhere (test launches only). song_path + song_id give it a custom song from "
            "any file; screenshots and the other tools work on it as on any level. Leave "
            "it with gucci_quit_level.",
            lsSchema(),
            [](matjson::Value const& a) {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                if (PlayLayer::get() || LevelEditorLayer::get())
                    throw ToolError("leave the level (or the editor) first");
                auto const raw = strArg(a, "level_string");
                if (raw.empty())
                    throw ToolError("level_string is empty");
                std::string packed = raw;
                if (raw.rfind("H4sI", 0) != 0) {
                    packed = std::string(cocos2d::ZipUtils::compressString(gd::string(raw), false, 0));
                    if (packed.empty())
                        throw ToolError("could not compress level_string");
                }

                auto const songPath = strArg(a, "song_path");
                int const songId = (int)numArg(a, "song_id", 0);
                if (!songPath.empty()) {
                    if (songId <= 0)
                        throw ToolError("song_path needs a song_id");
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(std::filesystem::path(songPath), ec))
                        throw ToolError("no file at song_path: " + songPath);
                    g_song.id = songId;
                    g_song.path = songPath;
                } else {
                    g_song = {};
                }

                auto* level = GJGameLevel::create();
                if (!level)
                    throw ToolError("could not make a level");
                auto name = strArg(a, "name");
                if (name.empty()) name = "Test level";
                level->m_levelName = name;
                level->m_levelString = packed;
                level->m_levelType = GJLevelType::Editor;
                level->m_isEditable = true;
                level->m_songID = songId;
                level->m_audioTrack = (int)numArg(a, "audio_track", 0);
                if (auto const id = numArg(a, "level_id", 0); id > 0)
                    level->m_levelID = (int)id;
                g_level = level;

                auto* scene = PlayLayer::scene(level, false, false);
                CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
                auto out = matjson::Value::object();
                out["opening"] = name;
                out["song_id"] = (int64_t)songId;
                out["song_override"] = !songPath.empty();
                out["packed_bytes"] = (int64_t)packed.size();
                out["level_id"] = (int64_t)level->m_levelID.value();
                return out;
            },
        });
    }

}  // namespace gucci::mcp
