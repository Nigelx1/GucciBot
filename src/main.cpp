#include "core/GucciBot.hpp"
#include "render/renderer.hpp"
#include "trainers/calibration.hpp"
#include <Geode/Geode.hpp>
#include <filesystem>
using namespace geode::prelude;

using namespace gucci;
namespace fs = std::filesystem;

static void ensureBundledAssets() {
    auto* mod = Mod::get();
    std::error_code ec;
    auto resDir = mod->getResourcesDir();

#ifdef _WIN32
    // The renderer's FFmpeg DLLs, which render/ffmpeg.hpp loads from libraries/
    // (Windows only). Every launch, any bundled DLL that's missing there or a
    // different size is copied in, so an install that once got an incomplete set
    // fills itself in: beta.3's first upload had 5 of the 7, and copying only into
    // an empty folder left those installs short for good. Nothing is loaded yet.
    auto libDir = mod->getPersistentDir() / "libraries";
    fs::create_directories(libDir, ec);
    int copied = 0;
    std::error_code libIterEc;
    for (auto& entry : fs::directory_iterator(resDir, libIterEc)) {
        if (libIterEc)
            break;
        if (entry.path().extension() != ".dll")
            continue;
        auto const dest = libDir / entry.path().filename();
        std::error_code wantEc, haveEc;
        auto const want = fs::file_size(entry.path(), wantEc);
        auto const have = fs::file_size(dest, haveEc);
        if (!wantEc && !haveEc && want == have)
            continue;
        std::error_code copyEc;
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, copyEc);
        if (copyEc)
            log::error("[GucciBot] Couldn't copy {} into libraries: {}", entry.path().filename().string(),
                       copyEc.message());
        else
            ++copied;
    }
    if (copied)
        log::info("[GucciBot] Copied {} FFmpeg file(s) into libraries from bundled resources", copied);
#endif

    auto fwDir = mod->getSaveDir() / "fw_assets";
    fs::create_directories(fwDir, ec);
    std::error_code iterEc;
    for (auto& entry : fs::directory_iterator(resDir, iterEc)) {
        if (iterEc)
            break;
        if (entry.path().extension() != ".wav")
            continue;
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
