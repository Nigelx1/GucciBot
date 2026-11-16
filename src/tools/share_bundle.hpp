#pragma once

// GucciBot (2026-11-16): a shared macro with its frame windows, in one file.
// Nigel's ask (2026-11-15): "for the score send could it be an image plus
// like... a full macro+fwc thing u can load up". Share my windows saves this
// next to its picture; Saved macros' Import a macro reads it back.
//
// The file is an ordinary .zip (any zip tool opens it), laid out the way the
// replays folder is:
//
//     share.json                           what it is (format, version, the macro's file name)
//     <macro file>                         the macro, byte for byte (KOCMOC.brrr)
//     sidecars/<macro file><ext>           each of its sidecars, byte for byte (KOCMOC.brrr.fwac, .path ...)
//
// Pure: no Geode, no game, no file system (tools/run_bundle_tests.cpp tests it
// offline). zlib packs and unpacks the entries.
//
// Reading is strict, because the file comes from someone else: only the names
// above (one macro, a plain file name; sidecars named after it, a short
// extension each), no folders or paths of any other kind, no encryption,
// sizes checked before anything is unpacked (and against what unpacks), every
// entry's CRC checked, at most kMaxEntries entries and kMaxTotal bytes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gucci::bundle {

    // What a share file holds.
    struct Share {
        std::string macroFile;              // its file name, UTF-8 ("KOCMOC UNLEASHED New.brrr")
        std::vector<uint8_t> macro;         // the macro file
        struct Sidecar {
            std::string ext;                // after the macro's file name: ".fwac", ".path", ".trainer"...
            std::vector<uint8_t> bytes;
        };
        std::vector<Sidecar> sidecars;
        std::string level;                  // informational: the level's name
        std::string madeWith;               // informational: "GucciBot 2.0.0-beta.5"
    };

    constexpr int kVersion = 1;
    // Plain names inside, nothing that says GucciBot: a disguised menu
    // (GBot) shares these too.
    constexpr char kManifest[] = "share.json";
    constexpr char kFormat[] = "macro with frame windows";
    constexpr size_t kMaxEntries = 64;
    // The biggest real share so far is a few MB (Bloodbath's .path sidecar is
    // 1.2 MB; KOCMOC's whole .zip 257 KB), and unpacking holds every file in
    // memory at once, so a phone can afford these (the ui2 review, 11-16).
    constexpr uint64_t kMaxEntry = 32ull << 20;   // 32 MB, one file unpacked
    constexpr uint64_t kMaxTotal = 64ull << 20;   // 64 MB, all of them

    // A sidecar extension the file may carry: a dot, then 1 to 16 letters,
    // digits or _ (".fwac"). No more dots, so another file's sidecars
    // (KOCMOC.brrr.bak.fwac, a backup's) never come along as this one's.
    bool sidecarExtOk(std::string const& ext);
    // A file name the macro may have: not empty, at most 200 bytes, no
    // folder (/ \), no : * ? " < > |, no control characters, not . or ..,
    // not starting with a dot or a space, not ending in one.
    bool fileNameOk(std::string const& name);

    // The .zip. Empty and `error` set when it can't be made.
    std::vector<uint8_t> pack(Share const& share, std::string& error);
    // Looks like a share file: a zip whose first entry is the manifest (what
    // pack writes). Cheap; unpack is the real check.
    bool looksLikeShare(std::span<uint8_t const> bytes);
    // Reads one back. False with `error` (plain words) when it isn't one or
    // something in it is wrong; nothing in `out` is to be used then.
    bool unpack(std::span<uint8_t const> bytes, Share& out, std::string& error);

    // ---- the zip underneath (also for the tests)

    struct Entry {
        std::string name;  // UTF-8, '/' between folders
        std::vector<uint8_t> bytes;
    };
    // Each entry deflated (stored when that's no smaller). UTF-8 names.
    std::vector<uint8_t> writeZip(std::vector<Entry> const& entries, std::string& error);
    // Every entry, unpacked and CRC-checked. Stored and deflated entries
    // only; no zip64, no encryption, no spanned archives.
    bool readZip(std::span<uint8_t const> bytes, std::vector<Entry>& out, std::string& error);

} // namespace gucci::bundle
