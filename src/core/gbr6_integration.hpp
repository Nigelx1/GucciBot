#pragma once

#include <cstdint>
#include <filesystem>

namespace gucci {

    void GBR6_recordAutoclickerInput(uint32_t frame, bool pressed, bool player2);
    void GBR6_flushAutoclickerRun(bool player2);
    void GBR6_resetACTrackers();

    bool isGBR6Extension(const std::filesystem::path& path);
    std::filesystem::path getGBR6Path(const std::string& macroName);

} // namespace gucci
