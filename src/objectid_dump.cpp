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

// One-time diagnostic: dump {objectID -> real GameObjectType} for every object
// GD itself creates, straight from GameObject::m_objectType (the actual runtime
// classification -- Solid/Hazard/Decoration/etc, see Geode/Enums.hpp). This is
// authoritative in a way no community spreadsheet can be, since it's the game's
// own object-type assignment, not a guess. Appends only newly-seen IDs across
// runs so playing a couple of levels builds up a running reference table at
// <mod save dir>/guccibot_objectids.log as "id,objectType,levelName".
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
