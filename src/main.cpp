#include "core/GucciBot.hpp"
#include "core/geode_compat.hpp" // gucci::stdPath
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
    auto resDir = gucci::stdPath(mod->getResourcesDir());

#ifdef _WIN32
    // The renderer's FFmpeg DLLs, which render/ffmpeg.hpp loads from libraries/
    // (Windows; macOS's dylibs are below). Every launch, any bundled DLL that's missing there or a
    // different size is copied in, so an install that once got an incomplete set
    // fills itself in: beta.3's first upload had 5 of the 7, and copying only into
    // an empty folder left those installs short for good. Nothing is loaded yet.
    auto libDir = gucci::stdPath(mod->getPersistentDir()) / "libraries";
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
#elif defined(GEODE_IS_MACOS)
    // The same on macOS, for the seven FFmpeg dylibs (render/ffmpeg.hpp loads
    // them from libraries/ too): each launch, one that's missing there or a
    // different size is copied in. An old copy is deleted first rather than
    // written over: macOS remembers a loaded library's code signature by its
    // file, and a signed library changed in place can get the game killed
    // when it loads. Nothing is loaded yet.
    auto libDir = gucci::stdPath(mod->getPersistentDir()) / "libraries";
    fs::create_directories(libDir, ec);
    int copied = 0;
    std::error_code libIterEc;
    for (auto& entry : fs::directory_iterator(resDir, libIterEc)) {
        if (libIterEc)
            break;
        if (entry.path().extension() != ".dylib")
            continue;
        auto const dest = libDir / entry.path().filename();
        std::error_code wantEc, haveEc;
        auto const want = fs::file_size(entry.path(), wantEc);
        auto const have = fs::file_size(dest, haveEc);
        if (!wantEc && !haveEc && want == have)
            continue;
        std::error_code removeEc, copyEc;
        fs::remove(dest, removeEc);
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

    auto fwDir = gucci::stdPath(mod->getSaveDir()) / "fw_assets";
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
