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

    auto libDir = mod->getPersistentDir() / "libraries";
    bool libDirPopulated = fs::exists(libDir, ec) && !fs::is_empty(libDir, ec);
    if (!libDirPopulated) {
        fs::create_directories(libDir, ec);
        std::error_code iterEc;
        for (auto& entry : fs::directory_iterator(resDir, iterEc)) {
            if (iterEc)
                break;
            if (entry.path().extension() != ".dll")
                continue;
            std::error_code copyEc;
            fs::copy_file(entry.path(),
                          libDir / entry.path().filename(),
                          fs::copy_options::overwrite_existing,
                          copyEc);
        }
        log::info("[GucciBot] First run: populated FFmpeg libraries from bundled resources");
    }

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
