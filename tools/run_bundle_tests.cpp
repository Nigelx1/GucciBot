// Share my windows' macro file (src/tools/share_bundle): the .zip that holds a
// macro and its sidecars. Run from the repo root (run_tests.bat).
//
// 1. Round trip: a macro and three sidecars (one big enough to deflate, one
//    that doesn't), packed and unpacked, come back byte for byte; packing the
//    same share twice gives the same bytes; looksLikeShare knows it and not a
//    plain zip.
// 2. The zip is a real one: every entry's name, the manifest's text, and
//    stored vs deflated, read back by readZip; an empty sidecar.
// 3. Refused, with a reason and nothing to use: a damaged byte (CRC), a file
//    cut short, a zip without share.json, a newer version, a macro name with a
//    folder or "..", a Windows device name, sidecar extensions with folders or
//    a second dot (another file's, a backup's), an entry that says it is
//    bigger than the limit, an encrypted entry, two entries with one name.
// 4. Left out, not refused: entries a share doesn't have (a note, a Mac's
//    __MACOSX folder, ../ names), never in what unpack hands back.
//
// run_bundle_tests --read <file>: unpacks one (a share made in the game, or a
// zip another tool made) and lists what's in it.

#include "tools/share_bundle.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace bundle = gucci::bundle;

namespace {

    int g_failed = 0;
    int g_checks = 0;

    void check(bool ok, std::string const& what) {
        ++g_checks;
        if (!ok) {
            std::printf("FAIL: %s\n", what.c_str());
            ++g_failed;
        }
    }

    std::vector<uint8_t> bytesOf(std::string const& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

    std::vector<uint8_t> noise(size_t n, uint32_t seed) {
        std::vector<uint8_t> v(n);
        for (auto& b : v) {
            seed = seed * 1664525u + 1013904223u;
            b = static_cast<uint8_t>(seed >> 24);
        }
        return v;
    }

    bundle::Share sample() {
        bundle::Share s;
        s.macroFile = "KOCMOC UNLEASHED New.brrr";
        s.macro = noise(596, 1);
        std::string json = "{\"marks\":[";
        for (int i = 0; i < 4000; ++i)
            json += "{\"frame\":" + std::to_string(i * 7) + ",\"window\":3},";
        json += "{}]}";
        s.sidecars.push_back({".fwac", bytesOf(json)});      // deflates well
        s.sidecars.push_back({".path", noise(100000, 2)});   // doesn't: stored
        s.sidecars.push_back({".trainer", {}});              // empty
        s.level = "KOCMOC \"UNLEASHED\"\n\xC3\xA9";
        s.madeWith = "GucciBot 2.0.0-beta.5";
        return s;
    }

    // An entry's local header and central record, found by name, for the
    // damage tests.
    size_t findCentral(std::vector<uint8_t> const& zip, std::string const& name) {
        for (size_t i = 0; i + 46 + name.size() <= zip.size(); ++i)
            if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 1 && zip[i + 3] == 2 &&
                std::memcmp(zip.data() + i + 46, name.data(), name.size()) == 0)
                return i;
        return std::string::npos;
    }

    bool refused(std::vector<uint8_t> const& zip, std::string const& what, std::string* why = nullptr) {
        bundle::Share out;
        std::string error;
        bool const ok = bundle::unpack(zip, out, error);
        if (why)
            *why = error;
        check(!ok && !error.empty(), what + " (refused with a reason; got: " + (ok ? "accepted" : error) + ")");
        return !ok;
    }

    std::vector<uint8_t> zipOf(std::vector<bundle::Entry> const& entries) {
        std::string error;
        auto z = bundle::writeZip(entries, error);
        check(!z.empty(), "writeZip: " + error);
        return z;
    }

    std::string manifest(std::string const& macro, int version = 1) {
        return "{\"format\": \"macro with frame windows\", \"version\": " + std::to_string(version) +
               ", \"macro\": \"" + macro + "\", \"sidecars\": [\".fwac\"], \"level\": \"x\", \"made_with\": \"y\"}";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--read") {
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<uint8_t> const bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bundle::Share sh;
        std::string error;
        bool const ok = bundle::unpack(bytes, sh, error);
        std::printf("looksLikeShare=%d unpack=%d error=%s\n", bundle::looksLikeShare(bytes) ? 1 : 0, ok ? 1 : 0,
                    error.c_str());
        if (ok) {
            std::printf("macro %s: %zu bytes; level %s; made with %s\n", sh.macroFile.c_str(), sh.macro.size(),
                        sh.level.c_str(), sh.madeWith.c_str());
            for (auto const& sc : sh.sidecars)
                std::printf("sidecar %s: %zu bytes\n", sc.ext.c_str(), sc.bytes.size());
        }
        return ok ? 0 : 1;
    }
    // ---- 1. round trip
    auto const s = sample();
    std::string error;
    auto const zip = bundle::pack(s, error);
    check(!zip.empty() && error.empty(), "pack: " + error);
    check(bundle::looksLikeShare(zip), "looksLikeShare on a packed share");
    auto const again = bundle::pack(s, error);
    check(again == zip, "packing the same share twice gives the same bytes");
    bundle::Share back;
    check(bundle::unpack(zip, back, error), "unpack: " + error);
    check(back.macroFile == s.macroFile, "macro file name back");
    check(back.macro == s.macro, "macro bytes back");
    check(back.level == s.level, "level back (quotes, newline, UTF-8)");
    check(back.madeWith == s.madeWith, "made_with back");
    check(back.sidecars.size() == 3, "three sidecars back");
    for (auto const& want : s.sidecars) {
        bool found = false;
        for (auto const& got : back.sidecars)
            if (got.ext == want.ext) {
                found = true;
                check(got.bytes == want.bytes, "sidecar " + want.ext + " byte for byte");
            }
        check(found, "sidecar " + want.ext + " is there");
    }

    // ---- 2. the zip underneath
    std::vector<bundle::Entry> entries;
    check(bundle::readZip(zip, entries, error), "readZip: " + error);
    check(entries.size() == 5, "five entries (manifest, macro, three sidecars)");
    if (entries.size() == 5) {
        check(entries[0].name == "share.json", "the manifest comes first");
        check(entries[1].name == "KOCMOC UNLEASHED New.brrr", "then the macro");
        check(entries[2].name == "sidecars/KOCMOC UNLEASHED New.brrr.fwac", "sidecars under sidecars/");
        std::string const text(entries[0].bytes.begin(), entries[0].bytes.end());
        check(text.find("\"version\": 1") != std::string::npos, "manifest has the version");
        check(text.find("\\\"UNLEASHED\\\"\\n") != std::string::npos, "manifest escapes quotes and newlines");
    }
    // method: the .fwac deflated (8), the noise stored (0)
    size_t const fwac = findCentral(zip, "sidecars/KOCMOC UNLEASHED New.brrr.fwac");
    size_t const path = findCentral(zip, "sidecars/KOCMOC UNLEASHED New.brrr.path");
    check(fwac != std::string::npos && zip[fwac + 10] == 8, "the .fwac is deflated");
    check(path != std::string::npos && zip[path + 10] == 0, "the noise is stored");
    check(zip.size() < 100000 + 596 + 40000, "deflating made it smaller than the files");
    check(!bundle::looksLikeShare(zipOf({{"a.txt", bytesOf("hi")}})), "a plain zip doesn't look like a share");
    check(!bundle::looksLikeShare(bytesOf("GBR6 not a zip at all")), "a macro doesn't look like a share");

    // ---- 3. refused
    {
        auto bad = zip;
        // A byte in the middle of the stored noise: its CRC fails.
        bad[zip.size() / 2] ^= 0x40;
        std::string why;
        refused(bad, "a damaged byte", &why);
        check(why.find("damaged") != std::string::npos, "says it's damaged: " + why);
    }
    {
        auto cut = zip;
        cut.resize(zip.size() - 30);
        refused(cut, "a file cut short");
        cut.resize(zip.size() / 3);
        refused(cut, "a file cut to a third");
        refused({}, "an empty file");
    }
    refused(zipOf({{"KOCMOC.brrr", bytesOf("m")}}), "a zip without share.json");
    refused(zipOf({{"share.json", bytesOf("{not json")}, {"KOCMOC.brrr", bytesOf("m")}}), "a broken manifest");
    refused(zipOf({{"share.json", bytesOf("{\"format\": \"something else\", \"version\": 1, \"macro\": \"m.brrr\"}")},
                   {"m.brrr", bytesOf("m")}}),
            "another format's manifest");
    refused(zipOf({{"share.json", bytesOf(manifest("m.brrr", 2))}, {"m.brrr", bytesOf("m")}}), "a newer version");
    refused(zipOf({{"share.json", bytesOf(manifest("m.brrr"))}}), "no macro in it");
    refused(zipOf({{"share.json", bytesOf(manifest("m.brrr"))}, {"m.brrr", {}}}), "an empty macro");
    for (std::string const name : {"../evil.brrr", "..\\\\evil.brrr", "sub/evil.brrr", "C:evil.brrr", "/abs.brrr",
                                   "..", ".hidden.brrr", "trailing.brrr ", "CON.brrr", "nul.brrr", "com1.brrr",
                                   "LPT9", "a\\u0001b.brrr", "a*b.brrr"}) {
        // As the manifest says it (JSON-escaped where needed), and an entry by that name.
        refused(zipOf({{"share.json", bytesOf(manifest(name))}, {name, bytesOf("m")}}), "macro name " + name);
    }
    check(!bundle::fileNameOk(std::string("bad\xC3", 4)), "half a UTF-8 character isn't a file name");
    check(!bundle::fileNameOk(std::string(201, 'a')), "a 201-byte name is too long");
    check(bundle::fileNameOk("KOCMOC UNLEASHED New.brrr"), "a plain name is fine");
    check(bundle::fileNameOk("\xC3\xA9t\xC3\xA9 (2).juice"), "UTF-8 and brackets are fine");
    check(bundle::fileNameOk("CONSOLE.brrr"), "CONSOLE isn't a device name");
    for (std::string const ext : {".fwac", ".path", ".trainer", ".fw", ".x_1"})
        check(bundle::sidecarExtOk(ext), "sidecar ext " + ext + " is fine");
    for (std::string const ext : {"", ".", "fwac", ".bak.fwac", "./x", ".a/b", ".a\\b", "..", ".x y",
                                  ".abcdefghijklmnopq"})
        check(!bundle::sidecarExtOk(ext), "sidecar ext '" + ext + "' is refused");
    {
        bundle::Share bad = sample();
        bad.sidecars.push_back({"/../../evil", bytesOf("x")});
        check(bundle::pack(bad, error).empty(), "pack refuses a sidecar ext with a path");
        bad = sample();
        bad.macroFile = "../x.brrr";
        check(bundle::pack(bad, error).empty(), "pack refuses a macro name with a path");
        bad = sample();
        bad.sidecars.push_back({".fwac", bytesOf("twice")});
        check(bundle::pack(bad, error).empty(), "pack refuses the same sidecar twice");
    }
    {
        // An entry that says it unpacks to more than the limit: refused
        // before anything is unpacked.
        auto big = zip;
        size_t const c = findCentral(big, "sidecars/KOCMOC UNLEASHED New.brrr.fwac");
        uint32_t const huge = 0x7FFFFFFF;
        std::memcpy(big.data() + c + 24, &huge, 4);
        std::string why;
        refused(big, "an entry that says it's 2 GB", &why);
        check(why.find("too big") != std::string::npos, "says it's too big: " + why);
        // Just over the limit for one file (the ui2 review: a share is a few
        // MB at most, so the limits are small).
        auto over = zip;
        uint32_t const justOver = static_cast<uint32_t>(bundle::kMaxEntry + 1);
        std::memcpy(over.data() + c + 24, &justOver, 4);
        refused(over, "an entry just over the one-file limit", &why);
        check(why.find("too big") != std::string::npos, "says it's too big: " + why);
        // Two at the one-file limit: over the limit for all of them (both
        // deflated, so neither is caught first as a stored entry whose sizes
        // differ).
        auto twoShare = sample();
        twoShare.sidecars[1].bytes.assign(200000, 'a');
        std::string packErr;
        auto two = bundle::pack(twoShare, packErr);
        check(!two.empty(), "pack two deflated sidecars: " + packErr);
        uint32_t const atLimit = static_cast<uint32_t>(bundle::kMaxEntry);
        std::memcpy(two.data() + findCentral(two, "sidecars/KOCMOC UNLEASHED New.brrr.fwac") + 24, &atLimit, 4);
        size_t const c2 = findCentral(two, "sidecars/KOCMOC UNLEASHED New.brrr.path");
        std::memcpy(two.data() + c2 + 24, &atLimit, 4);
        refused(two, "two entries over the limit for all of them", &why);
        check(why.find("too big") != std::string::npos, "says it's too big: " + why);
        // Says it's smaller than it unpacks to (a zip bomb's lie): refused.
        auto lie = zip;
        uint32_t const small = 10;
        std::memcpy(lie.data() + c + 24, &small, 4);
        refused(lie, "an entry that unpacks to more than it says");
    }
    {
        auto enc = zip;
        size_t const c = findCentral(enc, "KOCMOC UNLEASHED New.brrr");
        enc[c + 8] |= 1;
        std::string why;
        refused(enc, "an encrypted entry", &why);
        check(why.find("password") != std::string::npos, "says it's password protected: " + why);
    }
    {
        auto twice = zipOf({{"share.json", bytesOf(manifest("m.brrr"))}, {"m.brrr", bytesOf("m")},
                            {"m.brrr", bytesOf("other")}});
        refused(twice, "two entries with one name");
    }

    // ---- 4. left out
    {
        auto extra = zipOf({{"share.json", bytesOf(manifest("m.brrr"))},
                            {"m.brrr", bytesOf("macro")},
                            {"sidecars/m.brrr.fwac", bytesOf("windows")},
                            {"sidecars/m.brrr.bak.fwac", bytesOf("a backup's")},
                            {"sidecars/other.brrr.fwac", bytesOf("another macro's")},
                            {"../../evil.dll", bytesOf("no")},
                            {"__MACOSX/._m.brrr", bytesOf("mac")},
                            {"readme.txt", bytesOf("hi")}});
        bundle::Share out;
        check(bundle::unpack(extra, out, error), "extra entries are left out, not refused: " + error);
        check(out.macro == bytesOf("macro"), "the macro from among them");
        check(out.sidecars.size() == 1 && out.sidecars[0].ext == ".fwac" && out.sidecars[0].bytes == bytesOf("windows"),
              "only its own sidecar (not a backup's, not another macro's)");
    }

    if (g_failed) {
        std::printf("bundle: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    std::printf("bundle: all %d checks passed\n", g_checks);
    return 0;
}
