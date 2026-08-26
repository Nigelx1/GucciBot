#include "GucciBot.hpp"

#include <Geode/modify/PlayLayer.hpp>

#include <fstream>
#include <unordered_set>

using namespace geode::prelude;

namespace {
    std::unordered_set<int> loadKnownIds(std::filesystem::path const& path) {
        std::unordered_set<int> known;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            auto comma = line.find(',');
            if (comma == std::string::npos) continue;
            try { known.insert(std::stoi(line.substr(0, comma))); } catch (...) {}
        }
        return known;
    }
}

class $modify(ObjectIDDumpPL, PlayLayer) {
    void createObjectsFromSetupFinished() {
        PlayLayer::createObjectsFromSetupFinished();
        if (!m_objects) return;

        auto path = Mod::get()->getSaveDir() / "guccibot_objectids.log";
        auto known = loadKnownIds(path);
        std::string levelName = m_level ? std::string(m_level->m_levelName) : "unknown";

        std::ofstream out(path, std::ios::app);
        for (auto* go : CCArrayExt<GameObject*>(m_objects)) {
            if (!go) continue;
            int id = go->m_objectID;
            if (known.count(id)) continue;
            known.insert(id);
            out << id << "," << static_cast<int>(go->m_objectType) << "," << levelName << "\n";
        }
    }
};
