// Ported from Silicate (anticroom's 2026-09-26 source drop), GPL-3.
// Kept as close to verbatim as possible so the next drop merges; the only
// edits are include paths onto GucciBot's shim, marked GucciBot:.
#include "builder.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/UndoObject.hpp>

using namespace geode::prelude;

namespace tbuf {

static void applyScale(GameObject* object, float scaleX, float scaleY) {
    object->updateCustomScaleX(scaleX);
    object->updateCustomScaleY(scaleY);
}

BlockShape measureObject(LevelEditorLayer* editor, int objectId) {
    BlockShape shape;
    if (!editor) return shape;

    auto* sample = editor->createObject(objectId, {0.f, 0.f}, true);
    if (!sample) {
        log::warn("[trailbuf] could not create object {}, assuming 30x30",
                  objectId);
        return shape;
    }

    cocos2d::CCRect const rect = sample->getObjectRect();
    editor->removeObject(sample, true);

    if (rect.size.width <= 0.f || rect.size.height <= 0.f) {
        log::warn("[trailbuf] object {} has no hitbox, assuming 30x30",
                  objectId);
        return shape;
    }

    shape = BlockShape{rect.size.width, rect.size.height, rect.getMidX(),
                       rect.getMidY()};
    log::info("[trailbuf] object {} hitbox is {:.2f}x{:.2f}", objectId,
              shape.width, shape.height);
    return shape;
}

std::vector<RectF> collectSolids(LevelEditorLayer* editor, RectF const& area) {
    std::vector<RectF> solids;
    if (!editor) return solids;

    auto* objects = editor->m_objects;
    if (!objects) return solids;

    for (unsigned int i = 0; i < objects->count(); i++) {
        auto* object =
            static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!object) continue;
        if (object->m_objectType != GameObjectType::Solid) continue;
        if (object->m_isPassable) continue;

        cocos2d::CCRect const r = object->getObjectRect();
        RectF const rect{r.origin.x, r.origin.y, r.origin.x + r.size.width,
                         r.origin.y + r.size.height};
        if (rect.width() <= 0.f || rect.height() <= 0.f) continue;
        if (!rect.overlaps(area)) continue;

        solids.push_back(rect);
    }

    log::info("[trailbuf] {} solid objects near the trail", solids.size());
    return solids;
}

BuildReport placeBlocks(LevelEditorLayer* editor, int objectId,
                        BlockShape const& shape,
                        std::vector<GenBlock> const& blocks) {
    BuildReport report;
    if (!editor) {
        report.message = "No editor.";
        return report;
    }

    auto* created = CCArray::create();

    auto const onStep = [](float scale) {
        return std::round(scale / SCALE_STEP) * SCALE_STEP;
    };

    for (auto const& block : blocks) {
        float const scaleX = onStep(block.w / shape.width);
        float const scaleY = onStep(block.h / shape.height);
        CCPoint const position{block.x - shape.offsetX * scaleX,
                               block.y - shape.offsetY * scaleY};

        auto* object = editor->createObject(objectId, position, true);
        if (!object) continue;

        applyScale(object, scaleX, scaleY);
        created->addObject(object);
    }

    if (created->count() == 0) {
        report.message = "The editor refused to create any objects.";
        return report;
    }

    report.placedRects.reserve(created->count());
    for (unsigned int i = 0; i < created->count(); i++) {
        auto* object = static_cast<GameObject*>(created->objectAtIndex(i));
        cocos2d::CCRect const r = object->getObjectRect();
        report.placedRects.push_back(RectF{r.origin.x, r.origin.y,
                                           r.origin.x + r.size.width,
                                           r.origin.y + r.size.height});
    }

    if (editor->m_undoObjects) {
        editor->m_undoObjects->addObject(
            UndoObject::createWithArray(created, UndoCommand::Paste));
    }
    if (editor->m_redoObjects) {
        editor->m_redoObjects->removeAllObjects();
    }
    if (editor->m_editorUI) {
        editor->m_editorUI->updateObjectInfoLabel();
    }

    report.ok = true;
    report.placed = static_cast<int>(created->count());
    return report;
}

}  // namespace tbuf
