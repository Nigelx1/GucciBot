// GucciBot (2026-11-16): gucci_bands, the Bands card's files for a test
// launch (ui/band_files.hpp). A hidden test game can't use the file picker,
// so this hands a path to the same code the picker's result goes through:
// choose (a file for one band) and folder (Import folder). It answers what
// came of it, the files in bands/, and the card's note.

#include "mcp_server.hpp"

#include "analysis/ac/shim.hpp"
#include "hacks/safemode.hpp"
#include "ui/band_files.hpp"

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
        auto schema = matjson::Value::object();
        schema["type"] = "object";
        schema["properties"] = props;

        server.addTool({
            "gucci_bands",
            "The Calculate Bands card's files, test launches only. choose+kind+band: a file brought in and set on "
            "that band (the picker's own code). folder: Import folder. Each answers ok, note and path (the band's new "
            "Sound/Image, or the folder). Always answers sounds and images (what's in bands/), folder, and "
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
                }
                auto sounds = matjson::Value::array();
                for (auto const& s : bf::imported(bf::Kind::Sound))
                    sounds.push(s);
                auto images = matjson::Value::array();
                for (auto const& s : bf::imported(bf::Kind::Image))
                    images.push(s);
                out["sounds"] = sounds;
                out["images"] = images;
                out["folder"] = geode::utils::string::pathToString(bf::folder());
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
                return out;
            },
        });
    }

} // namespace gucci::mcp
