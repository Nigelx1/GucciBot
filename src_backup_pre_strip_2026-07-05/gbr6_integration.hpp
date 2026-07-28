#pragma once
// gbr6_integration.hpp — GucciBot 10.0

#include <cstdint>
#include <filesystem>

// Autoclicker run tracking
void GBR6_recordAutoclickerInput(uint32_t frame, bool pressed, bool player2);
void GBR6_flushAutoclickerRun(bool player2);
void GBR6_resetACTrackers();

// Extension routing
bool isGBR6Extension(const std::filesystem::path& path);
std::filesystem::path getGBR6Path(const std::string& macroName);
