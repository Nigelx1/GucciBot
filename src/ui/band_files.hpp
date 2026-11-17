#pragma once

// GucciBot (2026-11-16): the Bands card's files. Written fresh for 2.0 from
// today's Calculate code (Nigel: "keep them for 2.0").
//
// - Your own band sounds and pictures, brought in with the file picker (one
//   file, or every sound and picture in a folder) and kept in the mod's
//   config folder, in bands/. A band points at one as "bands/<file>", the
//   relative path its Sound and Image already took (config folder first).
// - Band sound packs: the bands' sounds under a name, in one .zip (any zip
//   tool opens it):
//
//       bandsounds.json        what it is, the pack's name, and each band's sound
//       sounds/<file>          each sound file, byte for byte
//
//   Importing one is strict, because it comes from someone else, with Import
//   a macro's rules (the share file's zip reader, tools/share_bundle): plain
//   file names only, sound files only, sizes and CRCs checked, at most 64
//   entries and 64 MB, and all or nothing: every check passes before a file
//   is written, the sounds go into bands/<pack name>/ in one step, and the
//   bands change only once they are all there. Nothing is written anywhere
//   but that folder.

#include <filesystem>
#include <string>
#include <vector>

namespace gucci::ui::bandfiles {

    enum class Kind { Sound, Image };

    struct Result {
        bool ok = false;
        std::string note;   // what happened, in plain words
        std::string path;   // a file brought in: what a band's Sound or Image takes ("bands/x.ogg");
                            // an exported pack: where it was saved
    };

    // <config folder>/bands, where imported files are kept.
    std::filesystem::path folder();
    // <save folder>/band packs, where exported packs go.
    std::filesystem::path packsFolder();

    // One file, copied into bands/ (the same bytes already there are used as
    // they are; another file of that name is kept as "name (2).ext").
    Result importFile(std::filesystem::path const& picked, Kind kind);
    // importFile, then set on the band with that id (saved, markers drawn
    // again): what the picker's result for a band goes through.
    Result chooseFile(std::filesystem::path const& picked, Kind kind, int bandId);
    // Every sound and picture in a folder (not its subfolders), the same way.
    Result importFolder(std::filesystem::path const& dir);
    // The files in bands/ of one kind (packs' folders too), as a band takes
    // them, sorted.
    std::vector<std::string> imported(Kind kind);

    // The bands' sounds under `name`, to <save folder>/band packs/<name>.zip.
    Result exportPack(std::string const& name);
    // A pack's sounds onto the bands, then saved. A band gets the pack's
    // sound for the same window range, or else for its place in the list.
    Result importPack(std::filesystem::path const& zip);

    // ---- the Bands card (pages/calculate.cpp)

    // The name Export a pack uses (an imported pack's name fills it in).
    std::string& packName();
    // The pickers. Each opens Geode's file picker; the result is handed over
    // on the game's thread by poll(), which the card calls every frame. A
    // band's file goes to the band with that id.
    void pickFile(Kind kind, int bandId);
    void pickFolder();
    void pickPack();
    bool picking();
    // True when it changed the bands (a file set on one, a pack's sounds).
    bool poll();
    // The card's note after an import or export ("" for none).
    std::string const& note();
    bool noteOk();
    void setNote(bool ok, std::string text);

} // namespace gucci::ui::bandfiles
