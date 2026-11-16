// A shared macro with its windows, as one .zip (tools/share_bundle.hpp).

#include "tools/share_bundle.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string_view>

namespace gucci::bundle {

    namespace {

        constexpr uint32_t kLocalSig = 0x04034b50;
        constexpr uint32_t kCentralSig = 0x02014b50;
        constexpr uint32_t kEndSig = 0x06054b50;
        constexpr uint16_t kUtf8Names = 0x0800;  // general purpose flag 11: names are UTF-8
        constexpr uint16_t kStored = 0, kDeflated = 8;
        // 1980-01-01 00:00: the same file twice packs to the same bytes.
        constexpr uint16_t kDosTime = 0, kDosDate = (0 << 9) | (1 << 5) | 1;
        constexpr std::string_view kSidecarDir = "sidecars/";

        void put16(std::vector<uint8_t>& out, uint16_t v) {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        void put32(std::vector<uint8_t>& out, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }
        uint16_t get16(uint8_t const* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
        uint32_t get32(uint8_t const* p) {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        uint32_t crcOf(std::vector<uint8_t> const& bytes) {
            uLong crc = crc32(0L, Z_NULL, 0);
            // zlib's crc32 takes a uInt length: in steps, for anything big.
            size_t done = 0;
            while (done < bytes.size()) {
                size_t const step = std::min<size_t>(bytes.size() - done, 1u << 30);
                crc = crc32(crc, bytes.data() + done, static_cast<uInt>(step));
                done += step;
            }
            return static_cast<uint32_t>(crc);
        }

        // Raw deflate (no zlib header), as a zip's method 8 is.
        bool deflateRaw(std::vector<uint8_t> const& in, std::vector<uint8_t>& out) {
            z_stream zs{};
            if (deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                return false;
            out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 16);
            zs.next_in = const_cast<Bytef*>(in.data());
            zs.avail_in = static_cast<uInt>(in.size());
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            int const rc = deflate(&zs, Z_FINISH);
            out.resize(zs.total_out);
            deflateEnd(&zs);
            return rc == Z_STREAM_END;
        }

        // Exactly `size` bytes out of `in`, or false: more or fewer is a
        // damaged (or lying) entry.
        bool inflateRaw(uint8_t const* in, size_t inSize, uint64_t size, std::vector<uint8_t>& out) {
            z_stream zs{};
            if (inflateInit2(&zs, -15) != Z_OK)
                return false;
            // One byte over: an entry that unpacks to more than it says
            // shows as filling it.
            out.assign(static_cast<size_t>(size) + 1, 0);
            zs.next_in = const_cast<Bytef*>(in);
            zs.avail_in = static_cast<uInt>(inSize);
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            int const rc = inflate(&zs, Z_FINISH);
            bool const ok = rc == Z_STREAM_END && zs.total_out == size;
            inflateEnd(&zs);
            out.resize(static_cast<size_t>(size));
            return ok;
        }

        bool utf8Valid(std::string_view s) {
            size_t i = 0;
            while (i < s.size()) {
                unsigned char const c = static_cast<unsigned char>(s[i]);
                size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
                if (n == 0 || i + n > s.size())
                    return false;
                uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
                for (size_t k = 1; k < n; ++k) {
                    unsigned char const d = static_cast<unsigned char>(s[i + k]);
                    if ((d & 0xC0) != 0x80)
                        return false;
                    cp = cp << 6 | (d & 0x3F);
                }
                // Overlong forms, surrogates and past U+10FFFF aren't UTF-8.
                if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                    (cp >= 0xD800 && cp <= 0xDFFF))
                    return false;
                i += n;
            }
            return true;
        }

        void jsonString(std::string& out, std::string_view s) {
            out += '"';
            for (char ch : s) {
                unsigned char const c = static_cast<unsigned char>(ch);
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof buf, "\\u%04x", c);
                            out += buf;
                        } else {
                            out += ch;
                        }
                }
            }
            out += '"';
        }

        // ---- the manifest: a small JSON reader, enough for an object whose
        // values are strings, numbers, true/false/null, arrays and objects.
        // The top level's strings and whole numbers are kept.
        struct Json {
            std::string_view s;
            size_t i = 0;
            int depth = 0;
            std::map<std::string, std::string> strings;
            std::map<std::string, long long> numbers;

            void ws() {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    ++i;
            }
            static void utf8(std::string& out, uint32_t cp) {
                if (cp < 0x80) {
                    out += static_cast<char>(cp);
                } else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | cp >> 6);
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | cp >> 12);
                    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | cp >> 18);
                    out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
                    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            }
            bool hex4(uint32_t& v) {
                if (i + 4 > s.size())
                    return false;
                v = 0;
                for (int k = 0; k < 4; ++k) {
                    char const c = s[i++];
                    v <<= 4;
                    if (c >= '0' && c <= '9')
                        v |= uint32_t(c - '0');
                    else if (c >= 'a' && c <= 'f')
                        v |= uint32_t(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        v |= uint32_t(c - 'A' + 10);
                    else
                        return false;
                }
                return true;
            }
            bool string(std::string& out) {
                if (i >= s.size() || s[i] != '"')
                    return false;
                ++i;
                while (i < s.size()) {
                    char const c = s[i++];
                    if (c == '"')
                        return true;
                    if (static_cast<unsigned char>(c) < 0x20)
                        return false;
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    if (i >= s.size())
                        return false;
                    char const e = s[i++];
                    switch (e) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case '/': out += '/'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'u': {
                            uint32_t cp;
                            if (!hex4(cp))
                                return false;
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                uint32_t lo;
                                if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
                                    return false;
                                i += 2;
                                if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                                    return false;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                return false;
                            }
                            utf8(out, cp);
                            break;
                        }
                        default: return false;
                    }
                }
                return false;
            }
            bool value(std::string const* key) {
                ws();
                if (i >= s.size())
                    return false;
                char const c = s[i];
                if (c == '"') {
                    std::string v;
                    if (!string(v))
                        return false;
                    if (key)
                        strings[*key] = std::move(v);
                    return true;
                }
                if (c == '{' || c == '[') {
                    if (++depth > 16)
                        return false;
                    char const close = c == '{' ? '}' : ']';
                    ++i;
                    ws();
                    if (i < s.size() && s[i] == close) {
                        ++i;
                        --depth;
                        return true;
                    }
                    while (true) {
                        ws();
                        if (c == '{') {
                            std::string k;
                            if (!string(k))
                                return false;
                            ws();
                            if (i >= s.size() || s[i] != ':')
                                return false;
                            ++i;
                            if (!value(depth == 1 ? &k : nullptr))
                                return false;
                        } else if (!value(nullptr)) {
                            return false;
                        }
                        ws();
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == close) {
                            ++i;
                            --depth;
                            return true;
                        }
                        return false;
                    }
                }
                for (std::string_view lit : {"true", "false", "null"})
                    if (s.substr(i, lit.size()) == lit) {
                        i += lit.size();
                        return true;
                    }
                // A number: whole ones are kept.
                size_t const from = i;
                if (i < s.size() && s[i] == '-')
                    ++i;
                bool whole = true, digits = false;
                while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' ||
                                        s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
                    if (!std::isdigit(static_cast<unsigned char>(s[i])))
                        whole = false;
                    else
                        digits = true;
                    ++i;
                }
                if (!digits)
                    return false;
                if (key && whole && i - from <= 18)
                    numbers[*key] = std::stoll(std::string(s.substr(from, i - from)));
                return true;
            }
            bool parse() {
                ws();
                if (i >= s.size() || s[i] != '{')
                    return false;
                if (!value(nullptr))
                    return false;
                ws();
                return i == s.size();
            }
        };

        bool reservedOnWindows(std::string const& name) {
            // CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension:
            // Windows opens a device instead of a file under those names.
            std::string stem = name.substr(0, name.find('.'));
            while (!stem.empty() && stem.back() == ' ')
                stem.pop_back();
            for (auto& c : stem)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
                return true;
            return stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0) && stem[3] >= '1' &&
                   stem[3] <= '9';
        }

    } // namespace

    bool sidecarExtOk(std::string const& ext) {
        if (ext.size() < 2 || ext.size() > 17 || ext[0] != '.')
            return false;
        return std::all_of(ext.begin() + 1, ext.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        });
    }

    bool fileNameOk(std::string const& name) {
        if (name.empty() || name.size() > 200 || name == "." || name == "..")
            return false;
        if (name.front() == '.' || name.front() == ' ' || name.back() == '.' || name.back() == ' ')
            return false;
        for (char ch : name) {
            unsigned char const c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c == 0x7F || std::string_view("\\/:*?\"<>|").find(ch) != std::string_view::npos)
                return false;
        }
        return utf8Valid(name) && !reservedOnWindows(name);
    }

    std::vector<uint8_t> writeZip(std::vector<Entry> const& entries, std::string& error) {
        if (entries.size() > kMaxEntries) {
            error = "too many files";
            return {};
        }
        std::vector<uint8_t> out, central;
        for (auto const& e : entries) {
            if (e.name.empty() || e.name.size() > 0xFFFF || e.bytes.size() > kMaxEntry) {
                error = "a file in it is too big or has no name";
                return {};
            }
            uint32_t const crc = crcOf(e.bytes);
            std::vector<uint8_t> packed;
            uint16_t method = kDeflated;
            if (!deflateRaw(e.bytes, packed)) {
                error = "couldn't pack " + e.name;
                return {};
            }
            if (packed.size() >= e.bytes.size()) {
                method = kStored;
                packed = e.bytes;
            }
            if (out.size() > 0xFFFFFFFFull - packed.size() - 30 - e.name.size()) {
                error = "the file would be too big";
                return {};
            }
            uint32_t const offset = static_cast<uint32_t>(out.size());
            auto header = [&](std::vector<uint8_t>& v, bool isCentral) {
                put32(v, isCentral ? kCentralSig : kLocalSig);
                if (isCentral)
                    put16(v, 20);  // made by: version 2.0
                put16(v, 20);      // needed to read: 2.0 (deflate)
                put16(v, kUtf8Names);
                put16(v, method);
                put16(v, kDosTime);
                put16(v, kDosDate);
                put32(v, crc);
                put32(v, static_cast<uint32_t>(packed.size()));
                put32(v, static_cast<uint32_t>(e.bytes.size()));
                put16(v, static_cast<uint16_t>(e.name.size()));
                put16(v, 0);  // extra
                if (isCentral) {
                    put16(v, 0);  // comment
                    put16(v, 0);  // disk
                    put16(v, 0);  // internal attributes
                    put32(v, 0);  // external attributes
                    put32(v, offset);
                }
                v.insert(v.end(), e.name.begin(), e.name.end());
            };
            header(out, false);
            out.insert(out.end(), packed.begin(), packed.end());
            header(central, true);
        }
        uint32_t const cdOffset = static_cast<uint32_t>(out.size());
        out.insert(out.end(), central.begin(), central.end());
        put32(out, kEndSig);
        put16(out, 0);
        put16(out, 0);
        put16(out, static_cast<uint16_t>(entries.size()));
        put16(out, static_cast<uint16_t>(entries.size()));
        put32(out, static_cast<uint32_t>(central.size()));
        put32(out, cdOffset);
        put16(out, 0);
        return out;
    }

    bool readZip(std::span<uint8_t const> bytes, std::vector<Entry>& out, std::string& error) {
        out.clear();
        size_t const size = bytes.size();
        uint8_t const* p = bytes.data();
        if (size < 22) {
            error = "it isn't a zip file (too short)";
            return false;
        }
        // The end record, from the back (a comment of up to 64 KB may follow it).
        size_t end = SIZE_MAX;
        for (size_t at = size - 22 + 1; at-- > 0 && size - at <= 22 + 0xFFFF;) {
            if (get32(p + at) == kEndSig && at + 22 + get16(p + at + 20) == size) {
                end = at;
                break;
            }
        }
        if (end == SIZE_MAX) {
            error = "it isn't a zip file, or it's cut short";
            return false;
        }
        uint16_t const disk = get16(p + end + 4), cdDisk = get16(p + end + 6);
        uint16_t const count = get16(p + end + 10);
        uint32_t const cdSize = get32(p + end + 12), cdOffset = get32(p + end + 16);
        if (disk != 0 || cdDisk != 0 || get16(p + end + 8) != count) {
            error = "it's one part of a zip split over several files";
            return false;
        }
        if (count == 0xFFFF || cdOffset == 0xFFFFFFFF || cdSize == 0xFFFFFFFF) {
            error = "it's a zip64 file, which can't be read here";
            return false;
        }
        if (count > kMaxEntries) {
            error = "it has too many files in it";
            return false;
        }
        if (uint64_t(cdOffset) + cdSize > end) {
            error = "it's damaged (its list of files is out of place)";
            return false;
        }

        struct Item {
            std::string name;
            uint16_t method;
            uint32_t crc, csize, usize, local;
        };
        std::vector<Item> items;
        std::set<std::string> names;
        uint64_t total = 0;
        size_t at = cdOffset;
        for (uint16_t n = 0; n < count; ++n) {
            if (at + 46 > uint64_t(cdOffset) + cdSize || get32(p + at) != kCentralSig) {
                error = "it's damaged (its list of files)";
                return false;
            }
            uint16_t const flags = get16(p + at + 8);
            Item it;
            it.method = get16(p + at + 10);
            it.crc = get32(p + at + 16);
            it.csize = get32(p + at + 20);
            it.usize = get32(p + at + 24);
            size_t const nameLen = get16(p + at + 28), extraLen = get16(p + at + 30), commentLen = get16(p + at + 32);
            it.local = get32(p + at + 42);
            if (at + 46 + nameLen + extraLen + commentLen > uint64_t(cdOffset) + cdSize) {
                error = "it's damaged (its list of files)";
                return false;
            }
            it.name.assign(reinterpret_cast<char const*>(p + at + 46), nameLen);
            at += 46 + nameLen + extraLen + commentLen;
            if (flags & 0x0001) {
                error = "it's password protected";
                return false;
            }
            if (it.method != kStored && it.method != kDeflated) {
                error = "it's packed in a way that can't be read here (only stored and deflated files)";
                return false;
            }
            if (it.method == kStored && it.csize != it.usize) {
                error = "it's damaged (" + it.name + ")";
                return false;
            }
            if (it.usize > kMaxEntry || (total += it.usize) > kMaxTotal) {
                error = "it's too big to unpack";
                return false;
            }
            if (!names.insert(it.name).second) {
                error = "it has two files called " + it.name;
                return false;
            }
            items.push_back(std::move(it));
        }

        for (auto const& it : items) {
            if (uint64_t(it.local) + 30 > cdOffset || get32(p + it.local) != kLocalSig) {
                error = "it's damaged (" + it.name + ")";
                return false;
            }
            uint64_t const data = uint64_t(it.local) + 30 + get16(p + it.local + 26) + get16(p + it.local + 28);
            if (data + it.csize > cdOffset) {
                error = "it's damaged or cut short (" + it.name + ")";
                return false;
            }
            Entry e;
            e.name = it.name;
            if (it.method == kStored) {
                e.bytes.assign(p + data, p + data + it.csize);
            } else if (!inflateRaw(p + data, it.csize, it.usize, e.bytes)) {
                error = "it's damaged (" + it.name + " doesn't unpack)";
                return false;
            }
            if (crcOf(e.bytes) != it.crc) {
                error = "it's damaged (" + it.name + " fails its check)";
                return false;
            }
            out.push_back(std::move(e));
        }
        return true;
    }

    std::vector<uint8_t> pack(Share const& share, std::string& error) {
        if (!fileNameOk(share.macroFile)) {
            error = "the macro's file name can't go in a share file";
            return {};
        }
        if (share.macro.empty()) {
            error = "the macro file is empty";
            return {};
        }
        std::string manifest = "{\n  \"format\": ";
        jsonString(manifest, kFormat);
        manifest += ",\n  \"version\": " + std::to_string(kVersion) + ",\n  \"macro\": ";
        jsonString(manifest, share.macroFile);
        manifest += ",\n  \"sidecars\": [";
        for (size_t i = 0; i < share.sidecars.size(); ++i) {
            if (i)
                manifest += ", ";
            jsonString(manifest, share.sidecars[i].ext);
        }
        manifest += "],\n  \"level\": ";
        jsonString(manifest, share.level);
        manifest += ",\n  \"made_with\": ";
        jsonString(manifest, share.madeWith);
        manifest += "\n}\n";

        std::vector<Entry> entries;
        entries.push_back({kManifest, std::vector<uint8_t>(manifest.begin(), manifest.end())});
        entries.push_back({share.macroFile, share.macro});
        std::set<std::string> seen;
        for (auto const& sc : share.sidecars) {
            if (!sidecarExtOk(sc.ext) || !seen.insert(sc.ext).second) {
                error = "a sidecar's name (" + sc.ext + ") can't go in a share file";
                return {};
            }
            entries.push_back({std::string(kSidecarDir) + share.macroFile + sc.ext, sc.bytes});
        }
        return writeZip(entries, error);
    }

    bool looksLikeShare(std::span<uint8_t const> bytes) {
        size_t const n = sizeof(kManifest) - 1;
        return bytes.size() >= 30 + n && get32(bytes.data()) == kLocalSig && get16(bytes.data() + 26) == n &&
               std::memcmp(bytes.data() + 30, kManifest, n) == 0;
    }

    bool unpack(std::span<uint8_t const> bytes, Share& out, std::string& error) {
        out = Share{};
        std::vector<Entry> entries;
        if (!readZip(bytes, entries, error))
            return false;
        Entry const* manifest = nullptr;
        for (auto const& e : entries)
            if (e.name == kManifest)
                manifest = &e;
        if (!manifest) {
            error = "it's a zip file, but not a shared macro (no share.json in it)";
            return false;
        }
        Json json;
        std::string const text(manifest->bytes.begin(), manifest->bytes.end());
        json.s = text;
        if (!json.parse()) {
            error = "its share.json isn't readable";
            return false;
        }
        if (json.strings["format"] != kFormat || !json.numbers.count("version")) {
            error = "its share.json isn't a shared macro's";
            return false;
        }
        if (json.numbers["version"] > kVersion) {
            error = "it was made by a newer version: update to load it";
            return false;
        }
        std::string const macroFile = json.strings["macro"];
        if (!fileNameOk(macroFile)) {
            error = "the macro's file name in it can't be used";
            return false;
        }
        out.macroFile = macroFile;
        out.level = json.strings["level"];
        out.madeWith = json.strings["made_with"];
        std::string const prefix = std::string(kSidecarDir) + macroFile;
        bool haveMacro = false;
        // Anything else in it (a note someone added, a Mac's __MACOSX folder)
        // is left out: only the macro and its sidecars are ever written.
        for (auto& e : entries) {
            if (e.name == macroFile) {
                out.macro = std::move(e.bytes);
                haveMacro = true;
            } else if (e.name.size() > prefix.size() && e.name.compare(0, prefix.size(), prefix) == 0) {
                std::string const ext = e.name.substr(prefix.size());
                if (sidecarExtOk(ext))
                    out.sidecars.push_back({ext, std::move(e.bytes)});
            }
        }
        if (!haveMacro || out.macro.empty()) {
            error = "the macro itself isn't in it";
            return false;
        }
        std::sort(out.sidecars.begin(), out.sidecars.end(),
                  [](Share::Sidecar const& a, Share::Sidecar const& b) { return a.ext < b.ext; });
        return true;
    }

} // namespace gucci::bundle
