// GucciBot (2026-11-16): gucci_bands, the Bands card's files for a test
// launch (ui/band_files.hpp). A hidden test game can't use the file picker,
// so this hands a path to the same code the picker's result goes through:
// choose (a file for one band), folder (Import folder), export and import (a
// band sound pack). It answers what came of it, the files in bands/, and the
// card's note.

#include "mcp_server.hpp"

#include "analysis/ac/shim.hpp"
#include "hacks/safemode.hpp"
#include "ui/band_files.hpp"
#include "core/geode_compat.hpp" // pathToString on both Geodes

#include <Geode/Geode.hpp>

#include <filesystem>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {
        std::filesystem::path u8path(std::string const& s) {
            return std::filesystem::path(std::u8string(s.begin(), s.end()));
        }
    } // namespace

    void registerBandsTools(Server& server) {
        auto props = matjson::Value::object();
        auto str = [](char const* d) {
            auto v = matjson::Value::object();
            v["type"] = "string";
            v["description"] = d;
            return v;
        };
        props["choose"] = str("a sound or picture file, as a band's Choose a sound / Choose a picture picks it");
        props["kind"] = str("sound (the default) or image, for choose");
        auto band = matjson::Value::object();
        band["type"] = "integer";
        band["description"] = "for choose: the band's place in the list, 1 for the first";
        props["band"] = band;
        props["folder"] = str("a folder, as Import folder picks it");
        props["export"] = str("a pack name: Export pack with that name");
        props["import"] = str("a pack .zip, as Import a pack picks it");
        auto saved = matjson::Value::object();
        saved["type"] = "boolean";
        saved["description"] = "true: also every fwac_ key as it would be written to saved.json now";
        props["saved"] = saved;
        auto schema = matjson::Value::object();
        schema["type"] = "object";
        schema["properties"] = props;

        server.addTool({
            "gucci_bands",
            "The Calculate Bands card's files, test launches only. choose+kind+band: a file brought in and set on "
            "that band (the picker's own code). folder: Import folder. export: Export pack under that name. import: "
            "Import a pack. Each answers ok, note and path (the band's new Sound/Image, the pack's file, or the "
            "folder). Always answers sounds and images (what's in bands/), pack_name, folder, packs_folder, and "
            "bands (each band's sound and image).",
            schema,
            [](matjson::Value const& a) -> matjson::Value {
                if (!gucci::safemode::testLaunch())
                    throw ToolError("test launches only (tools/assist.py launch)");
                namespace bf = gucci::ui::bandfiles;
                auto out = matjson::Value::object();
                auto put = [&](bf::Result const& r) {
                    out["ok"] = r.ok;
                    out["note"] = r.note;
                    out["path"] = r.path;
                    bf::setNote(r.ok, r.note);
                };
                if (a.contains("choose")) {
                    auto const kind = a["kind"].asString().unwrapOr("sound") == "image" ? bf::Kind::Image
                                                                                        : bf::Kind::Sound;
                    auto const& tiers = SLSettings::get()->frameWindow.tiers;
                    int const at = static_cast<int>(a["band"].asInt().unwrapOr(1));
                    if (at < 1 || at > static_cast<int>(tiers.size()))
                        throw ToolError(fmt::format("band is 1 to {}", tiers.size()));
                    put(bf::chooseFile(u8path(a["choose"].asString().unwrapOr("")), kind,
                                       tiers[static_cast<size_t>(at - 1)].id));
                } else if (a.contains("folder")) {
                    put(bf::importFolder(u8path(a["folder"].asString().unwrapOr(""))));
                } else if (a.contains("export")) {
                    auto const name = a["export"].asString().unwrapOr("");
                    bf::packName() = name;
                    put(bf::exportPack(name));
                } else if (a.contains("import")) {
                    put(bf::importPack(u8path(a["import"].asString().unwrapOr(""))));
                }
                auto sounds = matjson::Value::array();
                for (auto const& s : bf::imported(bf::Kind::Sound))
                    sounds.push(s);
                auto images = matjson::Value::array();
                for (auto const& s : bf::imported(bf::Kind::Image))
                    images.push(s);
                out["sounds"] = sounds;
                out["images"] = images;
                out["pack_name"] = bf::packName();
                out["folder"] = gucci::pathToString(bf::folder());
                out["packs_folder"] = gucci::pathToString(bf::packsFolder());
                out["card_note"] = bf::note();
                auto bands = matjson::Value::array();
                for (auto const& t : SLSettings::get()->frameWindow.tiers) {
                    auto b = matjson::Value::object();
                    b["id"] = t.id;
                    b["min"] = static_cast<double>(t.minWindow);
                    b["max"] = static_cast<double>(t.maxWindow);
                    b["sound"] = t.audioPath;
                    b["image"] = t.imagePath;
                    bands.push(b);
                }
                out["bands"] = bands;
                // What a save would write (test launches never write it), to
                // check that defaults leave a settings file as it was.
                if (a["saved"].asBool().unwrapOr(false)) {
                    auto keys = matjson::Value::object();
                    for (auto const& kv : Mod::get()->getSaveContainerConst())
                        if (kv.getKey().value_or("").starts_with("fwac_"))
                            keys[*kv.getKey()] = kv;
                    out["saved"] = keys;
                }
                return out;
            },
        });
    }

} // namespace gucci::mcp
