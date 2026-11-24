// GucciBot (2026-11-24): gucci_layout_census, what Layout mode (Hacks >
// Visual) did to the open level, read-only. The objects counted by GD's own
// type, with how many of each are hidden and colored; the colors GD holds for
// the background, ground and middleground channels; and whether the shader
// layer draws its effects. A screenshot shows the look; this shows that no
// gameplay object was hidden, which a picture of one screen can't.

#include "mcp_server.hpp"

#include "core/GucciBot.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/ColorAction.hpp>
#include <Geode/binding/GJEffectManager.hpp>
#include <Geode/binding/ShaderLayer.hpp>

#include <cstdint>
#include <map>
#include <string>

using namespace geode::prelude;

namespace gucci::mcp {

    namespace {
        // GD's names for its object types (Geode's GameObjectType, the same
        // values on both GD versions).
        char const* typeName(GameObjectType t) {
            switch (t) {
                case GameObjectType::Solid: return "Solid";
                case GameObjectType::Hazard: return "Hazard";
                case GameObjectType::InverseGravityPortal: return "InverseGravityPortal";
                case GameObjectType::NormalGravityPortal: return "NormalGravityPortal";
                case GameObjectType::ShipPortal: return "ShipPortal";
                case GameObjectType::CubePortal: return "CubePortal";
                case GameObjectType::Decoration: return "Decoration";
                case GameObjectType::YellowJumpPad: return "YellowJumpPad";
                case GameObjectType::PinkJumpPad: return "PinkJumpPad";
                case GameObjectType::GravityPad: return "GravityPad";
                case GameObjectType::YellowJumpRing: return "YellowJumpRing";
                case GameObjectType::PinkJumpRing: return "PinkJumpRing";
                case GameObjectType::GravityRing: return "GravityRing";
                case GameObjectType::InverseMirrorPortal: return "InverseMirrorPortal";
                case GameObjectType::NormalMirrorPortal: return "NormalMirrorPortal";
                case GameObjectType::BallPortal: return "BallPortal";
                case GameObjectType::RegularSizePortal: return "RegularSizePortal";
                case GameObjectType::MiniSizePortal: return "MiniSizePortal";
                case GameObjectType::UfoPortal: return "UfoPortal";
                case GameObjectType::Modifier: return "Modifier";
                case GameObjectType::Breakable: return "Breakable";
                case GameObjectType::SecretCoin: return "SecretCoin";
                case GameObjectType::DualPortal: return "DualPortal";
                case GameObjectType::SoloPortal: return "SoloPortal";
                case GameObjectType::Slope: return "Slope";
                case GameObjectType::WavePortal: return "WavePortal";
                case GameObjectType::RobotPortal: return "RobotPortal";
                case GameObjectType::TeleportPortal: return "TeleportPortal";
                case GameObjectType::GreenRing: return "GreenRing";
                case GameObjectType::Collectible: return "Collectible";
                case GameObjectType::UserCoin: return "UserCoin";
                case GameObjectType::DropRing: return "DropRing";
                case GameObjectType::SpiderPortal: return "SpiderPortal";
                case GameObjectType::RedJumpPad: return "RedJumpPad";
                case GameObjectType::RedJumpRing: return "RedJumpRing";
                case GameObjectType::CustomRing: return "CustomRing";
                case GameObjectType::DashRing: return "DashRing";
                case GameObjectType::GravityDashRing: return "GravityDashRing";
                case GameObjectType::CollisionObject: return "CollisionObject";
                case GameObjectType::Special: return "Special";
                case GameObjectType::SwingPortal: return "SwingPortal";
                case GameObjectType::GravityTogglePortal: return "GravityTogglePortal";
                case GameObjectType::SpiderOrb: return "SpiderOrb";
                case GameObjectType::SpiderPad: return "SpiderPad";
                case GameObjectType::EnterEffectObject: return "EnterEffectObject";
                case GameObjectType::TeleportOrb: return "TeleportOrb";
                case GameObjectType::AnimatedHazard: return "AnimatedHazard";
            }
            return "Other";
        }

        matjson::Value prop(char const* type, char const* description) {
            auto p = matjson::Value::object();
            p["type"] = type;
            p["description"] = description;
            return p;
        }

        matjson::Value rgb(cocos2d::ccColor3B c) {
            auto a = matjson::Value::array();
            a.push((int64_t)c.r);
            a.push((int64_t)c.g);
            a.push((int64_t)c.b);
            return a;
        }

        struct Tally {
            int64_t count = 0, hidden = 0, invisible = 0, noTouch = 0, colored = 0, glow = 0, hsv = 0;
        };
    }  // namespace

    void registerLayoutCensusTools(Server& server) {
        auto props = matjson::Value::object();
        props["from_x"] = prop("number", "with to_x: list each object in this stretch (at most 300)");
        props["to_x"] = prop("number", "the stretch's right edge");
        auto schema = matjson::Value::object();
        schema["type"] = "object";
        schema["properties"] = props;

        server.addTool({
            "gucci_layout_census",
            "What Layout mode did to the open level, read-only. layout_mode (the switch); colors: what GD holds "
            "for channels 1000 (background), 1001 and 1009 (ground), 1002 (ground line), 1013 and 1014 "
            "(middleground) as [r, g, b]; background_sprite: the background's drawn color; shaders: the shader "
            "layer exists and draws its effects (uses_shaders); types: the level's objects counted by GD's type, "
            "each with count, hidden (the Hide flag), invisible (GD won't draw it), no_touch, colored (a main or "
            "detail color channel above 0), glow and hsv. With from_x and to_x, objects lists each object in that "
            "stretch: uid, id, type, x, y, hide, invisible, no_touch, sprite_visible, opacity, main_color, "
            "detail_color, glow, dont_enter, dont_fade.",
            schema,
            [](matjson::Value const& a) {
                auto* pl = PlayLayer::get();
                if (!pl)
                    throw ToolError("not in a level");
                auto* gb = GucciEngine::get();
                auto out = matjson::Value::object();
                out["layout_mode"] = gb->layoutMode;

                auto colors = matjson::Value::object();
                if (auto* fx = pl->m_effectManager) {
                    for (int id : {1000, 1001, 1002, 1009, 1013, 1014})
                        if (auto* action = fx->getColorAction(id))
                            colors[std::to_string(id)] = rgb(action->m_color);
                }
                out["colors"] = colors;
                if (pl->m_background)
                    out["background_sprite"] = rgb(pl->m_background->getColor());

                auto shaders = matjson::Value::object();
                shaders["exists"] = pl->m_shaderLayer != nullptr;
                if (pl->m_shaderLayer)
                    shaders["uses_shaders"] = pl->m_shaderLayer->m_state.m_usesShaders;
                out["shaders"] = shaders;

                bool const list = a.contains("from_x") && a.contains("to_x");
                double const from = list ? a["from_x"].asDouble().unwrapOr(0.0) : 0.0;
                double const to = list ? a["to_x"].asDouble().unwrapOr(0.0) : 0.0;
                std::map<std::string, Tally> tally;
                auto objects = matjson::Value::array();
                int64_t listed = 0, total = 0;
                if (pl->m_objects) {
                    for (auto* o : CCArrayExt<GameObject*>(pl->m_objects)) {
                        if (!o)
                            continue;
                        ++total;
                        bool const colored = o->m_activeMainColorID > 0 || o->m_activeDetailColorID > 0;
                        auto& t = tally[typeName(o->m_objectType)];
                        ++t.count;
                        t.hidden += o->m_isHide;
                        t.invisible += o->m_isInvisible;
                        t.noTouch += o->m_isNoTouch;
                        t.colored += colored;
                        t.glow += !o->m_hasNoGlow;
                        t.hsv += o->m_baseUsesHSV || o->m_detailUsesHSV;
                        double const x = o->m_positionX;
                        if (!list || x < from || x > to || listed >= 300)
                            continue;
                        ++listed;
                        auto e = matjson::Value::object();
                        e["uid"] = (int64_t)o->m_uniqueID;
                        e["id"] = (int64_t)o->m_objectID;
                        e["type"] = typeName(o->m_objectType);
                        e["x"] = x;
                        e["y"] = o->m_positionY;
                        e["hide"] = o->m_isHide;
                        e["invisible"] = o->m_isInvisible;
                        e["no_touch"] = o->m_isNoTouch;
                        e["sprite_visible"] = o->isVisible();
                        e["opacity"] = (int64_t)o->getOpacity();
                        e["main_color"] = (int64_t)o->m_activeMainColorID;
                        e["detail_color"] = (int64_t)o->m_activeDetailColorID;
                        e["glow"] = !o->m_hasNoGlow;
                        e["dont_enter"] = o->m_isDontEnter;
                        e["dont_fade"] = o->m_isDontFade;
                        objects.push(e);
                    }
                }
                auto types = matjson::Value::object();
                for (auto const& [name, t] : tally) {
                    auto e = matjson::Value::object();
                    e["count"] = t.count;
                    e["hidden"] = t.hidden;
                    e["invisible"] = t.invisible;
                    e["no_touch"] = t.noTouch;
                    e["colored"] = t.colored;
                    e["glow"] = t.glow;
                    e["hsv"] = t.hsv;
                    types[name] = e;
                }
                out["objects_total"] = total;
                out["types"] = types;
                if (list)
                    out["objects"] = objects;
                return out;
            },
        });
    }

} // namespace gucci::mcp
