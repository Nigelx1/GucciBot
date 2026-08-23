#include "GucciBot.hpp"
#include "render/renderer.hpp"
#include "calibration.hpp"
#include <Geode/Geode.hpp>
#include <filesystem>
using namespace geode::prelude;
namespace fs = std::filesystem;

// Nigel/Juice, 2026-08-23: a fresh install had neither the FFmpeg DLLs
// SLRenderer needs (getPersistentDir()/libraries/) nor any default
// fw_assets sounds -- both were only ever manually placed by hand on
// Nigel's own machine, so every other install silently couldn't render
// and had no frame-window audio. Both are now bundled as mod resources
// (mod.json, under resources/ffmpeg_libs/ and resources/fw_assets/ in the
// source tree) and copied out to their real working location on first
// load. Geode's packaging FLATTENS all of a mod's resources into a single
// directory (verified directly against the built .geode -- source
// subfolders like ffmpeg_libs/ and fw_assets/ don't survive, everything
// lands together in getResourcesDir()), so the two sets are told apart by
// extension instead: .dll is uniquely the FFmpeg set, .wav is uniquely the
// fw_assets set (every other bundled resource is .mp3/.mov/.ttf/.png/.gdr).
// FFmpeg is all-or-nothing (7 DLLs, SLRenderer can't do anything with a
// partial set) so it's gated on the whole libraries/ folder being
// missing/empty. fw_assets is copied per-file and only when the
// destination doesn't already exist, so a user's own imports/
// customizations (via the "Import Sounds/Images" button) are never
// overwritten by the bundled defaults.
static void ensureBundledAssets() {
    auto* mod = Mod::get();
    std::error_code ec;
    auto resDir = mod->getResourcesDir();

    auto libDir = mod->getPersistentDir() / "libraries";
    bool libDirPopulated = fs::exists(libDir, ec) && !fs::is_empty(libDir, ec);
    if (!libDirPopulated) {
        fs::create_directories(libDir, ec);
        // Separate error_code per file -- one DLL failing to copy (e.g. a
        // transient antivirus lock) must not abort the rest of the batch,
        // or FFmpeg silently ends up with a partial, still-nonfunctional set.
        std::error_code iterEc;
        for (auto& entry : fs::directory_iterator(resDir, iterEc)) {
            if (iterEc) break;
            if (entry.path().extension() != ".dll") continue;
            std::error_code copyEc;
            fs::copy_file(entry.path(), libDir / entry.path().filename(),
                          fs::copy_options::overwrite_existing, copyEc);
        }
        log::info("[GucciBot] First run: populated FFmpeg libraries from bundled resources");
    }

    auto fwDir = mod->getSaveDir() / "fw_assets";
    fs::create_directories(fwDir, ec);
    std::error_code iterEc;
    for (auto& entry : fs::directory_iterator(resDir, iterEc)) {
        if (iterEc) break;
        if (entry.path().extension() != ".wav") continue;
        auto dest = fwDir / entry.path().filename();
        std::error_code existsEc, copyEc;
        if (!fs::exists(dest, existsEc)) {
            fs::copy_file(entry.path(), dest, copyEc);
        }
    }
}

$on_mod(Loaded) {
    ensureBundledAssets();
    GucciEngine::get()->initialize();
    SLRenderer::get()->loadFFmpeg();
    CalibrationService::get().load();
}
