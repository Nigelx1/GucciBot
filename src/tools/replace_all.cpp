// Ported from Absense (editor/tools.cpp, by Absent). Logic kept as it was; the
// only changes are GucciBot's logging in place of Absense's devlog, and fmt in
// place of its printf-style format().

#include "tools/replace_all.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/UndoObject.hpp>

#include <optional>
#include <vector>

using namespace geode::prelude;

namespace gucci::editortools {

    namespace {

        // Where probe objects are created. Nothing renders or collides between
        // their creation and removal, so any in-bounds spot does.
        const CCPoint kProbeAt(300.0f, 300.0f);

        void note(std::string const& line) {
            log::info("[GucciBot] editor tools: {}", line);
        }

        unsigned undoCount(LevelEditorLayer* lel) {
            return lel->m_undoObjects ? lel->m_undoObjects->count() : 0u;
        }

        // createObject and removeObject are asked for no undo entries. Should
        // the game make some anyway, they are taken off the end so the batch
        // stays one step. Only entries above `before` are touched, so the
        // user's own history is never shortened.
        void trimUndoList(LevelEditorLayer* lel, unsigned before, char const* what) {
            CCArray* list = lel->m_undoObjects;
            if (!list || list->count() <= before)
                return;
            unsigned const extra = list->count() - before;
            note(fmt::format("{} left {} undo entries despite noUndo; dropping them so "
                             "the batch stays one step",
                             what, extra));
            for (unsigned i = 0; i < extra; i++)
                list->removeLastObject();
        }

        // Registers `objects` as one undo entry of `command` and checks it is
        // the newest entry. When the game did not keep it, a created batch is
        // registered the way the editor's own paste does it, from the
        // selection; a removed batch cannot be selected, so that one is only
        // logged.
        bool registerUndo(LevelEditorLayer* lel, EditorUI* ui, CCArray* objects,
                          UndoCommand command, char const* what) {
            UndoObject* undo = UndoObject::createWithArray(objects, command);
            lel->addToUndoList(undo, /*keepRedo*/ false);
            if (lel->m_undoObjects && lel->m_undoObjects->lastObject() == undo)
                return true;

            note(fmt::format("{}: the undo entry is not the newest one after "
                             "addToUndoList ({} entries)",
                             what, undoCount(lel)));
            if (command != UndoCommand::Paste)
                return false;

            ui->deselectAll();
            ui->selectObjects(objects, /*ignoreFilter*/ true);
            ui->createUndoObject(command, /*addToList*/ true);
            auto* last = lel->m_undoObjects
                             ? geode::cast::typeinfo_cast<UndoObject*>(
                                   lel->m_undoObjects->lastObject())
                             : nullptr;
            return last && last->m_command == command && last->m_objects &&
                   last->m_objects->count() == objects->count();
        }

        // Leaves exactly `objects` selected, with the editor's buttons and its
        // object info label brought up to date.
        void selectBatch(EditorUI* ui, CCArray* objects) {
            ui->deselectAll();
            ui->selectObjects(objects, /*ignoreFilter*/ true);
            ui->updateButtons();
            ui->updateObjectInfoLabel();
        }

        // The editor with no playtest running. A bare LevelEditorLayer::get()
        // is not enough.
        LevelEditorLayer* editorAtRest(std::string& err) {
            auto* lel = LevelEditorLayer::get();
            if (!lel) {
                err = "Open a level in the editor first.";
                return nullptr;
            }
            if (lel->m_playbackMode != PlaybackMode::Not) {
                err = "Stop the playtest first.";
                return nullptr;
            }
            if (!lel->m_editorUI) {
                err = "Editor UI not ready.";
                return nullptr;
            }
            return lel;
        }

        // The editor's save string of an object is "key,value,key,value,..."
        // and key 1 is the object id. Returns the string with that value
        // replaced, or nothing when the string has no id field.
        std::optional<std::string> withObjectId(std::string save, int id) {
            while (!save.empty() && (save.back() == ';' || save.back() == ','))
                save.pop_back();
            std::vector<std::string> fields;
            size_t pos = 0;
            while (true) {
                size_t const comma = save.find(',', pos);
                if (comma == std::string::npos) {
                    fields.push_back(save.substr(pos));
                    break;
                }
                fields.push_back(save.substr(pos, comma - pos));
                pos = comma + 1;
            }
            bool replaced = false;
            for (size_t i = 0; i + 1 < fields.size(); i += 2) {
                if (fields[i] == "1") {
                    fields[i + 1] = std::to_string(id);
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                return std::nullopt;
            std::string out;
            out.reserve(save.size() + 8);
            for (size_t i = 0; i < fields.size(); i++) {
                if (i)
                    out += ',';
                out += fields[i];
            }
            return out;
        }

    } // namespace

    ReplaceResult replaceAll(int fromId, int toId) {
        ReplaceResult out;
        auto fail = [&](std::string message) {
            note("replace all: " + message);
            out.message = std::move(message);
            return out;
        };

        std::string err;
        LevelEditorLayer* lel = editorAtRest(err);
        if (!lel)
            return fail(err);
        EditorUI* ui = lel->m_editorUI;
        if (fromId <= 0 || toId <= 0)
            return fail("Object ids must be positive.");
        if (fromId == toId)
            return fail("Pick two different object ids.");

        // Collected first: replacing changes m_objects.
        std::vector<GameObject*> originals;
        if (lel->m_objects) {
            unsigned const count = lel->m_objects->count();
            for (unsigned i = 0; i < count; i++) {
                auto* o = static_cast<GameObject*>(lel->m_objects->objectAtIndex(i));
                if (o && o->m_objectID == fromId)
                    originals.push_back(o);
            }
        }
        if (originals.empty())
            return fail(fmt::format("No objects with id {} in this level.", fromId));

        // The target id has to be one the game can create; the probe is
        // removed again before anything else happens.
        unsigned const undoAtStart = undoCount(lel);
        {
            GameObject* probe = lel->createObject(toId, kProbeAt, /*noUndo*/ true);
            if (!probe) {
                trimUndoList(lel, undoAtStart, "the probe");
                return fail(fmt::format(
                    "Object id {} cannot be created (unknown id or object limit).", toId));
            }
            lel->removeObject(probe, /*noUndo*/ true);
            trimUndoList(lel, undoAtStart, "the probe");
        }

        CCArray* removed = CCArray::create();
        removed->retain();
        CCArray* created = CCArray::create();
        created->retain();
        size_t failed = 0, noIdField = 0;
        for (GameObject* o : originals) {
            // The full editor string, so position, rotation, scale, flips,
            // groups, colours, z-order and layer all carry over.
            std::optional<std::string> const str =
                withObjectId(std::string(o->getSaveString(lel)), toId);
            if (!str) {
                noIdField++;
                continue;
            }
            CCArray* result = lel->createObjectsFromString(
                gd::string(*str), /*noUndo*/ true, /*noLimit*/ true);
            if (!result || result->count() == 0) {
                failed++;
                continue;
            }
            for (unsigned i = 0; i < result->count(); i++)
                created->addObject(result->objectAtIndex(i));
            // A selected original removed underneath the selection would leave
            // the selection pointing at it, so the selection is dropped before
            // the first removal -- and only then, so a run that ends up
            // changing nothing leaves the user's selection as it was.
            if (removed->count() == 0)
                ui->deselectAll();
            removed->addObject(o);
            lel->removeObject(o, /*noUndo*/ true);
        }
        trimUndoList(lel, undoAtStart, "replace all");
        out.replaced = (int)removed->count();

        if (out.replaced == 0) {
            removed->release();
            created->release();
            return fail(fmt::format("None of the {} objects with id {} could be replaced "
                                    "(object {} could not be created from their data).",
                                    originals.size(), fromId, toId));
        }

        // One undo entry can only do one thing to its objects, so a
        // replacement is two entries: the originals as a delete, the
        // replacements as a paste. Ctrl+Z removes the replacements, a second
        // Ctrl+Z brings the originals back; redo runs them forward in order.
        registerUndo(lel, ui, removed, UndoCommand::DeleteMulti, "replace all (originals)");
        registerUndo(lel, ui, created, UndoCommand::Paste, "replace all (replacements)");
        selectBatch(ui, created);

        std::string message = fmt::format(
            "Replaced {} objects (id {} -> id {}). Ctrl+Z twice undoes it: the first press "
            "removes the new objects, the second brings the originals back.",
            out.replaced, fromId, toId);
        if (failed)
            message += fmt::format(" {} could not be created as id {} and were kept.", failed,
                                   toId);
        if (noIdField)
            message += fmt::format(" {} had no id field in their data and were kept.",
                                   noIdField);

        removed->release();
        created->release();
        note(message);
        out.ok = true;
        out.message = std::move(message);
        return out;
    }

} // namespace gucci::editortools
