#include "tools/macro_convert.hpp"

#include "core/gbr6_format.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace gucci::convert {

    namespace {

        using Bytes = std::vector<uint8_t>;

        // ---------------------------------------------------------------- bytes
        //
        // Every platform GucciBot builds for is little-endian, so a memcpy is a
        // little-endian read.

        struct Reader {
            Bytes const& b;
            size_t p = 0;
            bool bad = false;

            explicit Reader(Bytes const& bytes, size_t start = 0) : b(bytes), p(start) {}

            size_t left() const {
                return p < b.size() ? b.size() - p : 0;
            }
            bool has(size_t n) const {
                return left() >= n;
            }
            bool need(size_t n) {
                if (has(n))
                    return true;
                bad = true;
                return false;
            }
            void skip(size_t n) {
                if (need(n))
                    p += n;
            }
            uint8_t u8() {
                return need(1) ? b[p++] : 0;
            }
            template <class T> T le() {
                T v{};
                if (!need(sizeof(T)))
                    return v;
                std::memcpy(&v, b.data() + p, sizeof(T));
                p += sizeof(T);
                return v;
            }
            template <class T> T be() {
                T v{};
                if (!need(sizeof(T)))
                    return v;
                uint8_t tmp[sizeof(T)];
                for (size_t i = 0; i < sizeof(T); i++)
                    tmp[i] = b[p + sizeof(T) - 1 - i];
                std::memcpy(&v, tmp, sizeof(T));
                p += sizeof(T);
                return v;
            }
            // Little-endian, n = 1, 2, 4 or 8 bytes.
            uint64_t uintN(size_t n) {
                if (!need(n))
                    return 0;
                uint64_t v = 0;
                for (size_t i = 0; i < n; i++)
                    v |= uint64_t(b[p + i]) << (8 * i);
                p += n;
                return v;
            }
            // LEB128.
            uint64_t varint() {
                uint64_t v = 0;
                for (int shift = 0; shift < 64; shift += 7) {
                    if (!need(1))
                        return v;
                    uint8_t const c = b[p++];
                    v |= uint64_t(c & 0x7F) << shift;
                    if (!(c & 0x80))
                        return v;
                }
                bad = true;
                return v;
            }
            std::string str(size_t n) {
                if (!need(n))
                    return {};
                std::string s(reinterpret_cast<char const*>(b.data() + p), n);
                p += n;
                return s;
            }
        };

        struct Writer {
            Bytes out;

            void u8(uint8_t v) {
                out.push_back(v);
            }
            template <class T> void le(T v) {
                uint8_t tmp[sizeof(T)];
                std::memcpy(tmp, &v, sizeof(T));
                out.insert(out.end(), tmp, tmp + sizeof(T));
            }
            template <class T> void be(T v) {
                uint8_t tmp[sizeof(T)];
                std::memcpy(tmp, &v, sizeof(T));
                for (size_t i = sizeof(T); i-- > 0;)
                    out.push_back(tmp[i]);
            }
            void uintN(uint64_t v, size_t n) {
                for (size_t i = 0; i < n; i++)
                    out.push_back(uint8_t(v >> (8 * i)));
            }
            void varint(uint64_t v) {
                do {
                    uint8_t const c = uint8_t(v & 0x7F);
                    v >>= 7;
                    out.push_back(v ? uint8_t(c | 0x80) : c);
                } while (v);
            }
            void raw(std::string_view s) {
                out.insert(out.end(), s.begin(), s.end());
            }
            void zeros(size_t n) {
                out.insert(out.end(), n, uint8_t(0));
            }
        };

        bool startsWith(Bytes const& b, std::string_view magic) {
            return b.size() >= magic.size() && std::memcmp(b.data(), magic.data(), magic.size()) == 0;
        }
        bool startsWith(Bytes const& b, std::initializer_list<uint8_t> magic) {
            if (b.size() < magic.size())
                return false;
            size_t i = 0;
            for (uint8_t m : magic)
                if (b[i++] != m)
                    return false;
            return true;
        }

        std::string lower(std::string_view s) {
            std::string out(s);
            for (auto& c : out)
                c = (char)std::tolower((unsigned char)c);
            return out;
        }
        bool endsWith(std::string_view s, std::string_view suffix) {
            if (s.size() < suffix.size())
                return false;
            return lower(s.substr(s.size() - suffix.size())) == lower(suffix);
        }

        std::string_view asText(Bytes const& b) {
            std::string_view t(reinterpret_cast<char const*>(b.data()), b.size());
            if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB &&
                (unsigned char)t[2] == 0xBF)
                t.remove_prefix(3);  // a UTF-8 BOM
            return t;
        }

        std::vector<std::string_view> lines(std::string_view t) {
            std::vector<std::string_view> out;
            size_t start = 0;
            for (size_t i = 0; i <= t.size(); i++) {
                if (i == t.size() || t[i] == '\n') {
                    auto line = t.substr(start, i - start);
                    if (!line.empty() && line.back() == '\r')
                        line.remove_suffix(1);
                    out.push_back(line);
                    start = i + 1;
                }
            }
            return out;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && std::isspace((unsigned char)s.front()))
                s.remove_prefix(1);
            while (!s.empty() && std::isspace((unsigned char)s.back()))
                s.remove_suffix(1);
            return s;
        }

        std::vector<std::string_view> split(std::string_view s, char by) {
            std::vector<std::string_view> out;
            size_t start = 0;
            for (size_t i = 0; i <= s.size(); i++) {
                if (i == s.size() || s[i] == by) {
                    out.push_back(s.substr(start, i - start));
                    start = i + 1;
                }
            }
            return out;
        }

        std::vector<std::string_view> words(std::string_view s) {
            std::vector<std::string_view> out;
            size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace((unsigned char)s[i]))
                    i++;
                size_t const start = i;
                while (i < s.size() && !std::isspace((unsigned char)s[i]))
                    i++;
                if (i > start)
                    out.push_back(s.substr(start, i - start));
            }
            return out;
        }

        bool parseNumber(std::string_view s, double& out) {
            std::string tmp(trim(s));
            if (tmp.empty())
                return false;
            char* end = nullptr;
            double const d = std::strtod(tmp.c_str(), &end);
            if (end == tmp.c_str() || !std::isfinite(d))
                return false;
            out = d;
            return true;
        }
        int64_t toInt(std::string_view s, int64_t fallback = 0) {
            double d;
            return parseNumber(s, d) ? (int64_t)d : fallback;
        }

        // A rate stored as 1/dt or as a float comes back as 239.99998: put it
        // back on the whole number it was.
        double snapTps(double tps) {
            if (!std::isfinite(tps) || tps <= 0.0)
                return 240.0;
            double const r = std::round(tps);
            return std::fabs(tps - r) < 1e-3 ? r : tps;
        }

        std::string numText(double v) {
            if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
                return std::to_string((long long)v);
            char buf[40];
            for (int prec = 6; prec <= 17; prec++) {
                std::snprintf(buf, sizeof buf, "%.*g", prec, v);
                if (std::strtod(buf, nullptr) == v)
                    break;
            }
            return buf;
        }

        // ---------------------------------------------------------- JSON & msgpack
        //
        // One small tree for both: GDR v1 is the same struct in either.

        struct Value {
            enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
            bool b = false;
            double n = 0.0;
            int64_t i = 0;
            bool isInt = false;
            std::string s;
            std::vector<Value> arr;
            std::vector<std::pair<std::string, Value>> obj;

            static Value null() {
                return {};
            }
            static Value ofBool(bool v) {
                Value x;
                x.kind = Bool;
                x.b = v;
                return x;
            }
            static Value ofInt(int64_t v) {
                Value x;
                x.kind = Num;
                x.i = v;
                x.n = (double)v;
                x.isInt = true;
                return x;
            }
            static Value ofNum(double v) {
                Value x;
                x.kind = Num;
                x.n = v;
                return x;
            }
            static Value ofStr(std::string v) {
                Value x;
                x.kind = Str;
                x.s = std::move(v);
                return x;
            }
            static Value array() {
                Value x;
                x.kind = Arr;
                return x;
            }
            static Value object() {
                Value x;
                x.kind = Obj;
                return x;
            }
            Value& set(std::string key, Value v) {
                obj.emplace_back(std::move(key), std::move(v));
                return *this;
            }

            Value const* get(std::string_view key) const {
                if (kind != Obj)
                    return nullptr;
                for (auto const& [k, v] : obj)
                    if (k == key)
                        return &v;
                return nullptr;
            }
            bool isArray() const {
                return kind == Arr;
            }
            double num(double fallback = 0.0) const {
                if (kind == Num)
                    return isInt ? (double)i : n;
                if (kind == Bool)
                    return b ? 1.0 : 0.0;
                if (kind == Str) {
                    double d;
                    if (parseNumber(s, d))
                        return d;
                }
                return fallback;
            }
            int64_t integer(int64_t fallback = 0) const {
                if (kind == Num)
                    return isInt ? i : (int64_t)n;
                return (int64_t)num((double)fallback);
            }
            bool truthy() const {
                switch (kind) {
                    case Null: return false;
                    case Bool: return b;
                    case Num: return num() != 0.0;
                    case Str: return !s.empty();
                    default: return true;
                }
            }
        };

        double numOf(Value const* v, double fallback) {
            return v ? v->num(fallback) : fallback;
        }
        int64_t intOf(Value const* v, int64_t fallback) {
            return v ? v->integer(fallback) : fallback;
        }
        bool truthyOf(Value const* v) {
            return v && v->truthy();
        }
        std::string strOf(Value const* v) {
            if (!v)
                return {};
            if (v->kind == Value::Str)
                return v->s;
            if (v->kind == Value::Num)
                return numText(v->num());
            return {};
        }

        void appendUtf8(std::string& out, uint32_t cp) {
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }

        struct JsonParser {
            std::string_view t;
            size_t p = 0;
            bool bad = false;
            int depth = 0;

            void ws() {
                while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r'))
                    p++;
            }
            bool eat(char c) {
                ws();
                if (p < t.size() && t[p] == c) {
                    p++;
                    return true;
                }
                return false;
            }
            uint32_t hex4() {
                if (p + 4 > t.size()) {
                    bad = true;
                    return 0;
                }
                uint32_t v = 0;
                for (int k = 0; k < 4; k++) {
                    char const c = t[p++];
                    v <<= 4;
                    if (c >= '0' && c <= '9')
                        v |= c - '0';
                    else if (c >= 'a' && c <= 'f')
                        v |= c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F')
                        v |= c - 'A' + 10;
                    else
                        bad = true;
                }
                return v;
            }
            std::string string() {
                std::string out;
                p++;  // the opening quote
                while (p < t.size() && !bad) {
                    char const c = t[p++];
                    if (c == '"')
                        return out;
                    if (c != '\\') {
                        out += c;
                        continue;
                    }
                    if (p >= t.size())
                        break;
                    char const e = t[p++];
                    switch (e) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'u': {
                            uint32_t cp = hex4();
                            if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                                p += 2;
                                uint32_t const lo = hex4();
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                            appendUtf8(out, cp);
                            break;
                        }
                        default: out += e; break;
                    }
                }
                bad = true;
                return out;
            }
            Value value() {
                if (++depth > 64) {
                    bad = true;
                    return {};
                }
                ws();
                Value v;
                if (p >= t.size()) {
                    bad = true;
                } else if (t[p] == '{') {
                    p++;
                    v = Value::object();
                    if (!eat('}')) {
                        do {
                            ws();
                            if (p >= t.size() || t[p] != '"') {
                                bad = true;
                                break;
                            }
                            std::string key = string();
                            if (!eat(':')) {
                                bad = true;
                                break;
                            }
                            v.obj.emplace_back(std::move(key), value());
                        } while (!bad && eat(','));
                        if (!bad && !eat('}'))
                            bad = true;
                    }
                } else if (t[p] == '[') {
                    p++;
                    v = Value::array();
                    if (!eat(']')) {
                        do {
                            v.arr.push_back(value());
                        } while (!bad && eat(','));
                        if (!bad && !eat(']'))
                            bad = true;
                    }
                } else if (t[p] == '"') {
                    v = Value::ofStr(string());
                } else if (t.substr(p, 4) == "true") {
                    p += 4;
                    v = Value::ofBool(true);
                } else if (t.substr(p, 5) == "false") {
                    p += 5;
                    v = Value::ofBool(false);
                } else if (t.substr(p, 4) == "null") {
                    p += 4;
                } else {
                    size_t const start = p;
                    while (p < t.size() && (std::isdigit((unsigned char)t[p]) || t[p] == '-' || t[p] == '+' ||
                                            t[p] == '.' || t[p] == 'e' || t[p] == 'E'))
                        p++;
                    std::string const num(t.substr(start, p - start));
                    double d;
                    if (!parseNumber(num, d)) {
                        bad = true;
                    } else if (num.find_first_of(".eE") == std::string::npos && std::fabs(d) < 9.0e18) {
                        v = Value::ofInt(std::strtoll(num.c_str(), nullptr, 10));
                    } else {
                        v = Value::ofNum(d);
                    }
                }
                depth--;
                return v;
            }
        };

        bool parseJson(std::string_view text, Value& out) {
            JsonParser j{text};
            out = j.value();
            j.ws();
            return !j.bad && j.p == text.size();
        }

        void jsonString(std::string& out, std::string_view s) {
            out += '"';
            for (char c : s) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if ((unsigned char)c < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof buf, "\\u%04x", (unsigned)(unsigned char)c);
                            out += buf;
                        } else {
                            out += c;
                        }
                }
            }
            out += '"';
        }

        // Pretty-printed, two spaces, the way the site's converter writes it.
        void dumpJson(Value const& v, std::string& out, int indent) {
            std::string const pad(size_t(indent + 2), ' '), close(size_t(indent), ' ');
            switch (v.kind) {
                case Value::Null: out += "null"; break;
                case Value::Bool: out += v.b ? "true" : "false"; break;
                case Value::Num: out += v.isInt ? std::to_string((long long)v.i) : numText(v.n); break;
                case Value::Str: jsonString(out, v.s); break;
                case Value::Arr:
                    if (v.arr.empty()) {
                        out += "[]";
                        break;
                    }
                    out += "[\n";
                    for (size_t k = 0; k < v.arr.size(); k++) {
                        out += pad;
                        dumpJson(v.arr[k], out, indent + 2);
                        out += k + 1 < v.arr.size() ? ",\n" : "\n";
                    }
                    out += close + "]";
                    break;
                case Value::Obj:
                    if (v.obj.empty()) {
                        out += "{}";
                        break;
                    }
                    out += "{\n";
                    for (size_t k = 0; k < v.obj.size(); k++) {
                        out += pad;
                        jsonString(out, v.obj[k].first);
                        out += ": ";
                        dumpJson(v.obj[k].second, out, indent + 2);
                        out += k + 1 < v.obj.size() ? ",\n" : "\n";
                    }
                    out += close + "}";
                    break;
            }
        }

        Bytes jsonBytes(Value const& v) {
            std::string out;
            dumpJson(v, out, 0);
            out += '\n';
            return Bytes(out.begin(), out.end());
        }

        struct MsgpackReader {
            Reader r;
            int depth = 0;

            Value value() {
                if (++depth > 64 || !r.has(1)) {
                    r.bad = true;
                    return {};
                }
                Value v;
                uint8_t const b = r.u8();
                if (b <= 0x7F) {
                    v = Value::ofInt(b);
                } else if (b >= 0xE0) {
                    v = Value::ofInt((int64_t)b - 256);
                } else if (b >= 0x80 && b <= 0x8F) {
                    v = map(b & 0x0F);
                } else if (b >= 0x90 && b <= 0x9F) {
                    v = array(b & 0x0F);
                } else if (b >= 0xA0 && b <= 0xBF) {
                    v = Value::ofStr(r.str(b & 0x1F));
                } else {
                    switch (b) {
                        case 0xC0: break;
                        case 0xC2: v = Value::ofBool(false); break;
                        case 0xC3: v = Value::ofBool(true); break;
                        case 0xCA: v = Value::ofNum(r.be<float>()); break;
                        case 0xCB: v = Value::ofNum(r.be<double>()); break;
                        case 0xCC: v = Value::ofInt(r.u8()); break;
                        case 0xCD: v = Value::ofInt(r.be<uint16_t>()); break;
                        case 0xCE: v = Value::ofInt(r.be<uint32_t>()); break;
                        case 0xCF: v = Value::ofInt((int64_t)r.be<uint64_t>()); break;
                        case 0xD0: v = Value::ofInt(r.be<int8_t>()); break;
                        case 0xD1: v = Value::ofInt(r.be<int16_t>()); break;
                        case 0xD2: v = Value::ofInt(r.be<int32_t>()); break;
                        case 0xD3: v = Value::ofInt(r.be<int64_t>()); break;
                        case 0xD9: v = Value::ofStr(r.str(r.u8())); break;
                        case 0xDA: v = Value::ofStr(r.str(r.be<uint16_t>())); break;
                        case 0xDB: v = Value::ofStr(r.str(r.be<uint32_t>())); break;
                        case 0xDC: v = array(r.be<uint16_t>()); break;
                        case 0xDD: v = array(r.be<uint32_t>()); break;
                        case 0xDE: v = map(r.be<uint16_t>()); break;
                        case 0xDF: v = map(r.be<uint32_t>()); break;
                        default: r.bad = true; break;
                    }
                }
                depth--;
                return v;
            }
            Value array(size_t n) {
                Value v = Value::array();
                for (size_t k = 0; k < n && !r.bad; k++)
                    v.arr.push_back(value());
                return v;
            }
            Value map(size_t n) {
                Value v = Value::object();
                for (size_t k = 0; k < n && !r.bad; k++) {
                    Value key = value();
                    std::string name = key.kind == Value::Str ? key.s : numText(key.num());
                    v.obj.emplace_back(std::move(name), value());
                }
                return v;
            }
        };

        void msgpackWrite(Writer& w, Value const& v) {
            auto len = [&](size_t n, uint8_t fix, uint8_t fixMax, uint8_t b8, uint8_t b16, uint8_t b32) {
                if (n <= fixMax) {
                    w.u8(uint8_t(fix | n));
                } else if (b8 && n <= 0xFF) {
                    w.u8(b8);
                    w.u8(uint8_t(n));
                } else if (n <= 0xFFFF) {
                    w.u8(b16);
                    w.be<uint16_t>(uint16_t(n));
                } else {
                    w.u8(b32);
                    w.be<uint32_t>(uint32_t(n));
                }
            };
            switch (v.kind) {
                case Value::Null: w.u8(0xC0); break;
                case Value::Bool: w.u8(v.b ? 0xC3 : 0xC2); break;
                case Value::Num:
                    if (v.isInt) {
                        int64_t const x = v.i;
                        if (x >= 0 && x <= 0x7F) {
                            w.u8(uint8_t(x));
                        } else if (x < 0 && x >= -32) {
                            w.u8(uint8_t(x + 256));
                        } else if (x >= 0 && x <= 0xFFFFFFFFll) {
                            w.u8(0xCE);
                            w.be<uint32_t>(uint32_t(x));
                        } else if (x >= INT32_MIN && x < 0) {
                            w.u8(0xD2);
                            w.be<int32_t>(int32_t(x));
                        } else {
                            w.u8(0xD3);
                            w.be<int64_t>(x);
                        }
                    } else {
                        w.u8(0xCB);
                        w.be<double>(v.n);
                    }
                    break;
                case Value::Str:
                    len(v.s.size(), 0xA0, 31, 0xD9, 0xDA, 0xDB);
                    w.raw(v.s);
                    break;
                case Value::Arr:
                    len(v.arr.size(), 0x90, 15, 0, 0xDC, 0xDD);
                    for (auto const& x : v.arr)
                        msgpackWrite(w, x);
                    break;
                case Value::Obj:
                    len(v.obj.size(), 0x80, 15, 0, 0xDE, 0xDF);
                    for (auto const& [k, x] : v.obj) {
                        len(k.size(), 0xA0, 31, 0xD9, 0xDA, 0xDB);
                        w.raw(k);
                        msgpackWrite(w, x);
                    }
                    break;
            }
        }

        // ------------------------------------------------------------- helpers

        Input mk(int64_t frame, bool down, bool p2, uint8_t button = 1) {
            Input in;
            in.frame = frame;
            in.down = down;
            in.player2 = p2;
            in.button = button >= 1 && button <= 3 ? button : 1;
            return in;
        }

        void sortInputs(Macro& m) {
            std::stable_sort(m.inputs.begin(), m.inputs.end(),
                             [](Input const& a, Input const& b) { return a.frame < b.frame; });
            std::stable_sort(m.tpsChanges.begin(), m.tpsChanges.end(),
                             [](TpsChange const& a, TpsChange const& b) { return a.frame < b.frame; });
        }

        // The inputs in frame order, jump only for formats that only know jumps.
        std::vector<Input> inputsFor(Macro const& m, bool jumpOnly) {
            std::vector<Input> out;
            out.reserve(m.inputs.size());
            for (auto const& in : m.inputs)
                if (!jumpOnly || in.button == 1)
                    out.push_back(in);
            std::stable_sort(out.begin(), out.end(), [](Input const& a, Input const& b) { return a.frame < b.frame; });
            return out;
        }

        bool hasMoves(Macro const& m) {
            for (auto const& in : m.inputs)
                if (in.button != 1)
                    return true;
            return false;
        }

        int64_t lastFrame(std::vector<Input> const& list) {
            int64_t last = 0;
            for (auto const& in : list)
                last = std::max(last, in.frame);
            return last;
        }

        uint32_t u32Frame(int64_t f) {
            return (uint32_t)std::clamp<int64_t>(f, 0, 0xFFFFFFFFll);
        }

        // A practice session flattened to one run: a full restart starts it
        // over; a checkpoint restart drops what the earlier attempt did from
        // where play resumed (the frame of the next command, which is as close
        // as the file says).
        struct Flattener {
            Macro& m;
            int restarts = 0;
            bool pendingFull = false;
            bool pending = false;

            void restart(bool full) {
                restarts++;
                pending = true;
                pendingFull = pendingFull || full;
            }
            // The first command after a restart is at this frame.
            void resumeAt(int64_t frame) {
                if (!pending)
                    return;
                if (pendingFull) {
                    m.inputs.clear();
                    m.tpsChanges.clear();
                } else {
                    std::erase_if(m.inputs, [frame](Input const& in) { return in.frame >= frame; });
                    std::erase_if(m.tpsChanges, [frame](TpsChange const& t) { return t.frame >= frame; });
                }
                pending = pendingFull = false;
            }
            void finish(std::string_view what) {
                if (restarts > 0)
                    m.notes.push_back(std::string(what) + " is a practice session (" + std::to_string(restarts) +
                                      " restarts), flattened to one run. Where an attempt was cut short, the "
                                      "next one took over; check the first spots it died.");
            }
        };

        // ============================================================== formats
        //
        // Each reader takes the raw file and fills in the macro in the format's
        // own frame numbering (read() shifts it); each writer gets the macro
        // already shifted back.

        using ReadFn = bool (*)(Bytes const&, Macro&, std::string&);
        using WriteFn = Bytes (*)(Macro const&, std::vector<std::string>&);

        // ---- plain text: "frame button down player2", '#' lines for comments and the TPS.

        bool readPlain(Bytes const& b, Macro& m, std::string&) {
            for (auto line : lines(asText(b))) {
                line = trim(line);
                if (line.empty())
                    continue;
                if (line[0] == '#') {
                    auto const low = lower(line);
                    auto const at = low.find("tps");
                    if (at != std::string::npos) {
                        auto rest = std::string_view(low).substr(at + 3);
                        while (!rest.empty() && (rest[0] == ' ' || rest[0] == ':' || rest[0] == '='))
                            rest.remove_prefix(1);
                        double d;
                        if (parseNumber(rest, d))
                            m.tps = d;
                    }
                    continue;
                }
                auto const w = words(line);
                if (w.size() < 2)
                    continue;
                m.inputs.push_back(mk(toInt(w[0]), w.size() > 2 ? w[2] != "0" : true, w.size() > 3 ? w[3] != "0" : false,
                                      (uint8_t)toInt(w[1], 1)));
            }
            return true;
        }

        Bytes writePlain(Macro const& m, std::vector<std::string>&) {
            std::string out = "# GucciBot plain text\n# tps: " + numText(m.tps) + "\n# frame button down player2\n";
            for (auto const& in : inputsFor(m, false))
                out += std::to_string(in.frame) + " " + std::to_string(in.button) + " " + (in.down ? "1" : "0") + " " +
                       (in.player2 ? "1" : "0") + "\n";
            return Bytes(out.begin(), out.end());
        }

        // ---- GDR v1: one struct, in MessagePack (.gdr) or JSON (.gdr.json).
        // Its field is "2p", not "p2", and the objects are "bot" and "level".

        void gdr1In(Value const& j, Macro& m) {
            if (auto const* list = j.get("inputs"); list && list->isArray()) {
                for (auto const& i : list->arr) {
                    Value const* btn = i.get("btn");
                    if (!btn)
                        btn = i.get("button");
                    Value const* p2 = i.get("2p");
                    if (!p2)
                        p2 = i.get("p2");
                    m.inputs.push_back(
                        mk(intOf(i.get("frame"), 0), truthyOf(i.get("down")), truthyOf(p2), (uint8_t)intOf(btn, 1)));
                }
            }
            Value const* fps = j.get("framerate");
            if (!fps)
                fps = j.get("fps");
            m.tps = numOf(fps, 240.0);
            Value const* level = j.get("level");
            if (!level)
                level = j.get("levelInfo");
            if (level) {
                m.levelId = intOf(level->get("id"), 0);
                m.levelName = strOf(level->get("name"));
            }
            Value const* bot = j.get("bot");
            if (!bot)
                bot = j.get("botInfo");
            if (bot)
                m.bot = strOf(bot->get("name"));
            m.author = strOf(j.get("author"));
            m.platformer = truthyOf(j.get("platformer")) || hasMoves(m);
        }

        Value gdr1Out(Macro const& m) {
            auto const list = inputsFor(m, false);
            Value inputs = Value::array();
            for (auto const& in : list) {
                Value i = Value::object();
                i.set("frame", Value::ofInt(in.frame))
                    .set("btn", Value::ofInt(in.button))
                    .set("2p", Value::ofBool(in.player2))
                    .set("down", Value::ofBool(in.down));
                inputs.arr.push_back(std::move(i));
            }
            Value bot = Value::object();
            bot.set("name", Value::ofStr("GucciBot")).set("version", Value::ofStr("2.0"));
            Value level = Value::object();
            level.set("id", Value::ofInt(m.levelId)).set("name", Value::ofStr(m.levelName));
            Value j = Value::object();
            j.set("gameVersion", Value::ofNum(2.2081))
                .set("description", Value::ofStr("Converted by GucciBot"))
                .set("version", Value::ofNum(1.0))
                .set("duration", Value::ofNum((double)lastFrame(list) / (m.tps > 0 ? m.tps : 240.0)))
                .set("author", Value::ofStr(m.author.empty() ? "GucciBot" : m.author))
                .set("seed", Value::ofInt(0))
                .set("coins", Value::ofInt(0))
                .set("ldm", Value::ofBool(false))
                .set("bot", std::move(bot))
                .set("level", std::move(level))
                .set("inputs", std::move(inputs))
                .set("framerate", Value::ofNum(m.tps));
            return j;
        }

        bool readGdrJson(Bytes const& b, Macro& m, std::string& err) {
            Value j;
            if (!parseJson(asText(b), j) || j.kind != Value::Obj) {
                err = "It isn't valid JSON.";
                return false;
            }
            gdr1In(j, m);
            return true;
        }
        Bytes writeGdrJson(Macro const& m, std::vector<std::string>&) {
            return jsonBytes(gdr1Out(m));
        }

        bool readGdr(Bytes const& b, Macro& m, std::string& err) {
            MsgpackReader mp{Reader(b)};
            Value const j = mp.value();
            if (mp.r.bad || j.kind != Value::Obj || !j.get("inputs")) {
                err = "The GDR data is cut off or damaged.";
                return false;
            }
            gdr1In(j, m);
            return true;
        }
        Bytes writeGdr(Macro const& m, std::vector<std::string>&) {
            Writer w;
            msgpackWrite(w, gdr1Out(m));
            return w.out;
        }

        // ---- GDR2: "GDR", LEB128 integers, big-endian floats, varint-length
        // strings; inputs pack as delta<<1|down ((delta<<3)|(button<<1)|down
        // in platformer), player 1's run first, then player 2's.

        bool readGdr2(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 3);
            r.varint();  // format version
            auto str = [&] { return r.str((size_t)r.varint()); };
            std::string const inputTag = str();
            m.author = str();
            str();       // description
            r.be<float>();  // duration
            r.varint();  // game version
            m.tps = r.be<double>();
            r.varint();  // seed
            r.varint();  // coins
            r.varint();  // ldm
            m.platformer = r.varint() != 0;
            m.bot = str();
            r.varint();  // bot version
            m.levelId = (int64_t)r.varint();
            m.levelName = str();
            r.skip((size_t)r.varint());  // the replay's extension blob
            uint64_t const deaths = r.varint();
            for (uint64_t d = 0; d < deaths && !r.bad; d++)
                r.varint();
            r.varint();  // total inputs; read to the end instead
            uint64_t p1Left = r.varint();
            if (r.bad) {
                err = "The GDR2 header is cut off.";
                return false;
            }
            int64_t prev = 0;
            while (r.left() > 0 && !r.bad) {
                uint64_t const packed = r.varint();
                if (r.bad)
                    break;
                Input in;
                if (m.platformer) {
                    in.frame = prev + (int64_t)(packed >> 3);
                    in.button = uint8_t((packed >> 1) & 3);
                    if (in.button == 0)
                        in.button = 1;
                } else {
                    in.frame = prev + (int64_t)(packed >> 1);
                    in.button = 1;
                }
                in.down = packed & 1;
                in.player2 = p1Left == 0;
                m.inputs.push_back(in);
                if (!inputTag.empty())
                    r.skip((size_t)r.varint());  // this input's extension
                prev = in.frame;
                // Player 2's run starts its deltas over.
                if (p1Left > 0 && --p1Left == 0)
                    prev = 0;
            }
            return true;
        }

        Bytes writeGdr2(Macro const& m, std::vector<std::string>&) {
            auto const list = inputsFor(m, false);
            bool const plat = m.platformer || hasMoves(m);
            std::vector<Input> p1, p2;
            for (auto const& in : list)
                (in.player2 ? p2 : p1).push_back(in);
            Writer w;
            auto str = [&](std::string_view s) {
                w.varint(s.size());
                w.raw(s);
            };
            w.raw("GDR");
            w.varint(2);
            str("");  // no input extension
            str(m.author.empty() ? "GucciBot" : m.author);
            str("Converted by GucciBot");
            w.be<float>((float)((double)lastFrame(list) / (m.tps > 0 ? m.tps : 240.0)));
            w.varint(22081);
            w.be<double>(m.tps);
            w.varint(0);  // seed
            w.varint(0);  // coins
            w.varint(0);  // ldm
            w.varint(plat ? 1 : 0);
            str("GucciBot");
            w.varint(2);
            w.varint((uint64_t)std::max<int64_t>(0, m.levelId));
            str(m.levelName);
            w.varint(0);  // no replay extension
            w.varint(0);  // no deaths
            w.varint(list.size());
            w.varint(p1.size());
            for (auto const* run : {&p1, &p2}) {
                int64_t prev = 0;
                for (auto const& in : *run) {
                    uint64_t const delta = (uint64_t)std::max<int64_t>(0, in.frame - prev);
                    w.varint(plat ? (delta << 3) | (uint64_t(in.button & 3) << 1) | (in.down ? 1 : 0)
                                  : (delta << 1) | (in.down ? 1 : 0));
                    prev = in.frame;
                }
            }
            return w.out;
        }

        // ---- TcBot (.tcm), from tcm-rs (MIT, tcbot.pro): a 16-byte magic, a
        // 0x40 meta block, then v1 (varint count; varint frame + a byte each)
        // or v2 (a byte per command carrying how the next frame delta is
        // stored).

        constexpr uint8_t kTcmMagic[16] = {0x9f, 0x88, 0x89, 0x84, 0x9f, 0x3b, 0x1d, 0xd8,
                                           0xcc, 0xa1, 0x86, 0x8a, 0x88, 0x99, 0x84, 0x00};

        bool isTcm(Bytes const& b) {
            return b.size() >= 80 && std::memcmp(b.data(), kTcmMagic, 16) == 0;
        }

        bool readTcm(Bytes const& b, Macro& m, std::string& err) {
            uint8_t const version = b[16];
            uint8_t const flags = b[18];
            float tpsOrDt;
            std::memcpy(&tpsOrDt, b.data() + 20, 4);
            double const tps = (version == 1 || (flags & 2)) ? tpsOrDt : (tpsOrDt != 0.f ? 1.0 / tpsOrDt : 240.0);
            m.tps = tps;
            m.bot = "TcBot";
            Reader r(b, 80);
            Flattener flat{m};

            if (version == 1) {
                uint64_t const count = r.varint();
                for (uint64_t k = 0; k < count && !r.bad; k++) {
                    int64_t const frame = (int64_t)r.varint();
                    uint8_t const byte = r.u8();
                    if (r.bad)
                        break;
                    uint8_t const kind = byte & 7;
                    if (kind < 3) {
                        flat.resumeAt(frame);
                        m.inputs.push_back(mk(frame, byte & 0x80, byte & 0x40, uint8_t(kind + 1)));
                    } else if (kind <= 5) {
                        if (kind != 5)
                            flat.restart(kind == 4);
                    }
                }
            } else if (version == 2) {
                int64_t frame = (int64_t)r.varint();
                uint64_t lastDelta = 0;
                while (r.left() > 0) {
                    uint8_t const byte = r.u8();
                    uint8_t const dd = (byte >> 5) & 7;
                    unsigned const blob = (dd >> 1) & 3;
                    bool const magic = dd & 1;
                    uint8_t const input = byte & 3;
                    if (input > 0) {
                        flat.resumeAt(frame);
                        bool const push = byte & 4, p2 = byte & 8, swift = byte & 16;
                        m.inputs.push_back(mk(frame, push, p2, input));
                        // "Swift": the press and the release on the same frame.
                        if (swift)
                            m.inputs.push_back(mk(frame, !push, p2, input));
                    } else {
                        unsigned const custom = (byte >> 2) & 3;
                        bool const extra = byte & 16;
                        if (custom == 3) {
                            if (!extra) {  // a TPS change; with extra, a bugpoint
                                float const t = r.le<float>();
                                if (std::isfinite(t) && t > 0.f) {
                                    flat.resumeAt(frame);
                                    m.tpsChanges.push_back({frame, (double)t});
                                }
                            }
                        } else {
                            if (extra)
                                r.le<uint64_t>();  // the new seed
                            // 0 restart, 1 full restart, 2 death
                            if (custom != 2)
                                flat.restart(custom == 1);
                            frame = 0;
                        }
                    }
                    if (r.bad)
                        break;
                    // The delta to the next command.
                    uint64_t v = 0;
                    if (blob == 1)
                        v = r.u8();
                    else if (blob == 2)
                        v = r.le<uint16_t>();
                    else if (blob == 3)
                        v = r.le<uint32_t>();
                    if (r.bad)
                        break;
                    uint64_t const delta = (magic ? lastDelta : 0) + v;
                    if (blob != 0 && delta != 0)
                        lastDelta = delta;
                    frame += (int64_t)delta;
                }
            } else {
                err = "TcBot format version " + std::to_string(version) + " isn't supported.";
                return false;
            }
            flat.finish("This TcBot macro");
            // The header's rate, unless a change on frame 0 says otherwise.
            for (auto it = m.tpsChanges.begin(); it != m.tpsChanges.end();) {
                if (it->frame == 0) {
                    m.tps = it->tps;
                    it = m.tpsChanges.erase(it);
                } else {
                    ++it;
                }
            }
            return true;
        }

        Bytes writeTcm(Macro const& m, std::vector<std::string>&) {
            // Inputs and TPS changes as one command list in frame order.
            struct Cmd {
                int64_t frame;
                Input in;
                double tps;  // > 0: a TPS change
            };
            std::vector<Cmd> cmds;
            for (auto const& in : inputsFor(m, false))
                cmds.push_back({in.frame, in, 0.0});
            for (auto const& t : m.tpsChanges)
                cmds.push_back({t.frame, {}, t.tps});
            std::stable_sort(cmds.begin(), cmds.end(), [](Cmd const& a, Cmd const& b) { return a.frame < b.frame; });

            Writer w;
            w.out.insert(w.out.end(), kTcmMagic, kTcmMagic + 16);
            Bytes meta(0x40, 0);
            meta[0] = 2;  // version
            meta[2] = 2;  // flags: the rate is a TPS, not a dt
            float const tps = (float)m.tps;
            std::memcpy(meta.data() + 4, &tps, 4);
            w.out.insert(w.out.end(), meta.begin(), meta.end());
            w.varint(cmds.empty() ? 0 : (uint64_t)std::max<int64_t>(0, cmds.front().frame));
            for (size_t k = 0; k < cmds.size(); k++) {
                uint64_t const delta =
                    k + 1 < cmds.size() ? (uint64_t)std::max<int64_t>(0, cmds[k + 1].frame - cmds[k].frame) : 0;
                unsigned const blob = delta == 0 ? 0 : delta <= 0xFF ? 1 : delta <= 0xFFFF ? 2 : 3;
                auto const& c = cmds[k];
                if (c.tps > 0.0) {
                    w.u8(uint8_t((3u << 2) | (blob << 6)));
                    w.le<float>((float)c.tps);
                } else {
                    uint8_t const button = c.in.button >= 1 && c.in.button <= 3 ? c.in.button : 1;
                    w.u8(uint8_t(button | (c.in.down ? 4 : 0) | (c.in.player2 ? 8 : 0) | (blob << 6)));
                }
                if (blob == 1)
                    w.u8(uint8_t(delta));
                else if (blob == 2)
                    w.le<uint16_t>(uint16_t(delta));
                else if (blob == 3)
                    w.le<uint32_t>(uint32_t(delta));
            }
            return w.out;
        }

        // ---- Silicate (.slc). v3 from peony's libslc: "SLC3RPLY", a 64-byte
        // meta block, atoms, a 0xCC footer; the action atom's sections carry
        // frame deltas from the action before.

        bool readSlc3(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 8);
            if (r.le<uint16_t>() != 64) {
                err = "This Silicate macro's header is from a version this doesn't know.";
                return false;
            }
            size_t const metaAt = r.p;
            m.tps = r.le<double>();
            m.seed = r.le<uint64_t>();
            m.hasSeed = true;
            m.bot = "Silicate";
            r.p = metaAt + 64;
            if (b.empty() || b.back() != 0xCC) {
                err = "The Silicate macro is cut off (no end marker).";
                return false;
            }
            size_t const end = b.size() - 1;
            bool sawActions = false;
            while (r.p < end && !r.bad) {
                uint32_t const id = r.le<uint32_t>();
                uint64_t size = r.le<uint64_t>();
                size &= ~(0xFFull << 56);  // the top byte holds flags
                if (r.bad || size > end - r.p) {
                    err = "A block in the Silicate macro runs past its end.";
                    return false;
                }
                size_t const atomEnd = r.p + (size_t)size;
                if (id != 1 || sawActions) {  // only the first action atom is the macro
                    r.p = atomEnd;
                    continue;
                }
                sawActions = true;
                uint64_t const count = r.le<uint64_t>();
                uint64_t made = 0;
                int64_t frame = 0;
                auto addPlayer = [&](uint64_t state) {
                    int64_t const delta = (int64_t)(state >> 4);
                    unsigned const button = (state >> 2) & 3;
                    bool const p2 = state & 2, hold = state & 1;
                    frame += delta;
                    if (button == 0) {  // "swift": a jump press and release on one frame
                        m.inputs.push_back(mk(frame, true, p2, 1));
                        m.inputs.push_back(mk(frame, false, p2, 1));
                        made += 2;
                    } else {
                        m.inputs.push_back(mk(frame, hold, p2, uint8_t(button)));
                        made++;
                    }
                };
                while (made < count && r.p < atomEnd && !r.bad) {
                    uint16_t const h = r.le<uint16_t>();
                    unsigned const kind = h >> 14;
                    if (kind == 0 || kind == 1) {
                        size_t const byteSize = size_t(1) << ((h >> 12) & 3);
                        size_t const length = size_t(1) << ((h >> 8) & 15);
                        size_t const repeats = kind == 1 ? size_t(1) << ((h >> 3) & 31) : 1;
                        if (!r.has(byteSize * length) || r.p + byteSize * length > atomEnd) {
                            r.bad = true;
                            break;
                        }
                        std::vector<uint64_t> states(length);
                        for (auto& s : states)
                            s = r.uintN(byteSize);
                        for (size_t rep = 0; rep < repeats && made < count; rep++)
                            for (auto s : states)
                                addPlayer(s);
                    } else if (kind == 2) {
                        unsigned const type = (h >> 10) & 15;
                        size_t const byteSize = size_t(1) << ((h >> 8) & 3);
                        frame += (int64_t)r.uintN(byteSize);
                        if (type == 3) {  // TPS
                            double const t = r.le<double>();
                            if (std::isfinite(t) && t > 0.0)
                                m.tpsChanges.push_back({frame, t});
                        } else if (type <= 2) {  // restart, full restart, death: a seed each
                            r.le<uint64_t>();
                        }
                        made++;
                    } else {
                        r.bad = true;
                    }
                }
                if (r.bad) {
                    err = "The Silicate macro's inputs are cut off or damaged.";
                    return false;
                }
                r.p = atomEnd;
            }
            if (!sawActions) {
                err = "The Silicate macro has no inputs in it.";
                return false;
            }
            for (auto it = m.tpsChanges.begin(); it != m.tpsChanges.end();) {
                if (it->frame == 0) {
                    m.tps = it->tps;
                    it = m.tpsChanges.erase(it);
                } else {
                    ++it;
                }
            }
            return true;
        }

        Bytes writeSlc3(Macro const& m, std::vector<std::string>&) {
            struct Act {
                int64_t frame;
                Input in;
                double tps;  // > 0: a TPS change
            };
            std::vector<Act> acts;
            for (auto const& in : inputsFor(m, false))
                acts.push_back({in.frame, in, 0.0});
            for (auto const& t : m.tpsChanges)
                acts.push_back({t.frame, {}, t.tps});
            std::stable_sort(acts.begin(), acts.end(), [](Act const& a, Act const& b) { return a.frame < b.frame; });

            Writer body;
            body.le<uint64_t>(acts.size());
            int64_t prev = 0;
            auto sizeExp = [](uint64_t v) -> unsigned {
                return v < (1ull << 8) ? 0 : v < (1ull << 16) ? 1 : v < (1ull << 32) ? 2 : 3;
            };
            for (auto const& a : acts) {
                uint64_t const delta = (uint64_t)std::max<int64_t>(0, a.frame - prev);
                prev = std::max(prev, a.frame);
                if (a.tps > 0.0) {
                    unsigned const e = sizeExp(delta);
                    body.le<uint16_t>(uint16_t((2u << 14) | (3u << 10) | (e << 8)));
                    body.uintN(delta, size_t(1) << e);
                    body.le<double>(a.tps);
                } else {
                    // One input per section: valid, if not the smallest file.
                    uint64_t const state = (delta << 4) | (uint64_t(a.in.button & 3) << 2) |
                                           (a.in.player2 ? 2u : 0u) | (a.in.down ? 1u : 0u);
                    unsigned const e = sizeExp(state);
                    body.le<uint16_t>(uint16_t(e << 12));
                    body.uintN(state, size_t(1) << e);
                }
            }

            Writer w;
            w.raw("SLC3RPLY");
            w.le<uint16_t>(64);
            w.le<double>(m.tps);
            w.le<uint64_t>(m.hasSeed ? m.seed : 0);
            w.le<uint32_t>(2);   // meta version
            w.le<uint32_t>(81);  // build, as Silicate stamps it
            w.le<uint32_t>(0);   // randomness algorithm
            w.zeros(36);
            w.le<uint32_t>(1);  // the action atom
            w.le<uint64_t>(body.out.size());
            w.out.insert(w.out.end(), body.out.begin(), body.out.end());
            w.u8(0xCC);
            return w.out;
        }

        // v2, libslc's older container: "SILL", f64 TPS, a meta block (Silicate
        // keeps its seed first), blobs of fixed-size states, "EOM".
        bool readSlc2(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 4);
            m.tps = r.le<double>();
            uint64_t const metaSize = r.le<uint64_t>();
            if (metaSize >= 8 && r.has((size_t)metaSize)) {
                size_t const at = r.p;
                m.seed = r.le<uint64_t>();
                m.hasSeed = true;
                r.p = at + (size_t)metaSize;
            } else {
                r.skip((size_t)metaSize);
            }
            uint64_t const length = r.le<uint64_t>();
            uint64_t const blobCount = r.le<uint64_t>();
            if (r.bad || blobCount > b.size() || length > b.size()) {
                err = "The Silicate 2 macro's header is damaged.";
                return false;
            }
            struct Blob {
                uint64_t byteSize, start, length;
            };
            std::vector<Blob> blobs((size_t)blobCount);
            for (auto& bl : blobs) {
                bl.byteSize = r.le<uint64_t>();
                bl.start = r.le<uint64_t>();
                bl.length = r.le<uint64_t>();
                if (bl.byteSize != 1 && bl.byteSize != 2 && bl.byteSize != 4 && bl.byteSize != 8)
                    r.bad = true;
            }
            int64_t frame = 0;
            for (auto const& bl : blobs) {
                for (uint64_t k = 0; k < bl.length && !r.bad; k++) {
                    uint64_t const st = r.uintN((size_t)bl.byteSize);
                    frame += (int64_t)(st >> 5);
                    unsigned const type = (st >> 2) & 7;
                    if (type >= 1 && type <= 3)
                        m.inputs.push_back(mk(frame, st & 1, st & 2, uint8_t(type)));
                    else if (type == 7) {
                        double const t = r.le<double>();
                        if (std::isfinite(t) && t > 0.0)
                            m.tpsChanges.push_back({frame, t});
                    }
                }
            }
            if (r.bad) {
                err = "The Silicate 2 macro's inputs are cut off.";
                return false;
            }
            m.bot = "Silicate";
            return true;
        }

        // v1, the oldest: f64 TPS, u32 count, u32 states (frame<<4 | p2<<3 |
        // button<<1 | hold), then the seed.
        bool readSlc1(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b);
            m.tps = r.le<double>();
            uint32_t const count = r.le<uint32_t>();
            if (r.bad || (uint64_t)count * 4 > r.left()) {
                err = "The Silicate macro is cut off.";
                return false;
            }
            for (uint32_t k = 0; k < count; k++) {
                uint32_t const st = r.le<uint32_t>();
                unsigned const button = (st >> 1) & 3;
                if (button == 0)
                    continue;
                m.inputs.push_back(mk(st >> 4, st & 1, st & 8, uint8_t(button)));
            }
            if (r.has(8)) {
                m.seed = r.le<uint64_t>();
                m.hasSeed = true;
            }
            m.bot = "Silicate";
            return true;
        }

        // ---- anticroom's Silicate fork (.scbf1): sub-tick clicks. "SCBF",
        // u16 version, u16, u32 build, f64 TPS, u64 seed, u32 actions, u32
        // trail samples; per action u64 frame, u8 type, u8 bits and 8 bytes of
        // payload (none for a bugpoint); the trail; an FNV-1a 64 checksum.

        uint64_t fnv1a64(uint8_t const* data, size_t n) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t k = 0; k < n; k++) {
                h ^= data[k];
                h *= 0x100000001b3ull;
            }
            return h;
        }

        bool readScbf(Bytes const& b, Macro& m, std::string& err) {
            if (b.size() < 44) {
                err = "The .scbf1 file is cut off.";
                return false;
            }
            size_t const body = b.size() - 8;
            uint64_t stored;
            std::memcpy(&stored, b.data() + body, 8);
            if (fnv1a64(b.data(), body) != stored) {
                err = "The .scbf1 file is cut off or damaged (its checksum doesn't match).";
                return false;
            }
            Reader r(b, 4);
            if (r.le<uint16_t>() != 1) {
                err = "This .scbf1 version isn't supported.";
                return false;
            }
            r.skip(2 + 4);
            m.tps = r.le<double>();
            m.seed = r.le<uint64_t>();
            m.hasSeed = true;
            uint32_t const count = r.le<uint32_t>();
            r.le<uint32_t>();  // trail samples, not needed
            for (uint32_t k = 0; k < count; k++) {
                if (r.p + 10 > body) {
                    err = "The .scbf1 file's inputs are cut off.";
                    return false;
                }
                int64_t const frame = (int64_t)r.le<uint64_t>();
                uint8_t const type = r.u8(), bits = r.u8();
                if (type >= 1 && type <= 3) {
                    Input in = mk(frame, bits & 1, bits & 2, type);
                    double const off = r.le<double>();
                    if (off > 0.0 && off < 1.0)
                        in.subtick = off;
                    m.inputs.push_back(in);
                } else if (type >= 4 && type <= 6) {
                    r.le<uint64_t>();  // a reset's seed
                } else if (type == 7) {
                    double const t = r.le<double>();
                    if (std::isfinite(t) && t > 0.0)
                        m.tpsChanges.push_back({frame, t});
                } else if (type != 8) {
                    err = "The .scbf1 file has an input of a kind this doesn't know.";
                    return false;
                }
            }
            m.bot = "Silicate CBF";
            return true;
        }

        Bytes writeScbf(Macro const& m, std::vector<std::string>&) {
            struct Act {
                int64_t frame;
                Input in;
                double tps;
            };
            std::vector<Act> acts;
            for (auto const& in : inputsFor(m, false))
                acts.push_back({in.frame, in, 0.0});
            for (auto const& t : m.tpsChanges)
                acts.push_back({t.frame, {}, t.tps});
            std::stable_sort(acts.begin(), acts.end(), [](Act const& a, Act const& b) { return a.frame < b.frame; });
            Writer w;
            w.raw("SCBF");
            w.le<uint16_t>(1);
            w.le<uint16_t>(0);
            w.le<uint32_t>(81);
            w.le<double>(m.tps);
            w.le<uint64_t>(m.hasSeed ? m.seed : 0);
            w.le<uint32_t>((uint32_t)acts.size());
            w.le<uint32_t>(0);  // no trail
            for (auto const& a : acts) {
                w.le<uint64_t>((uint64_t)std::max<int64_t>(0, a.frame));
                if (a.tps > 0.0) {
                    w.u8(7);
                    w.u8(0);
                    w.le<double>(a.tps);
                } else {
                    w.u8(a.in.button >= 1 && a.in.button <= 3 ? a.in.button : 1);
                    w.u8(uint8_t((a.in.down ? 1 : 0) | (a.in.player2 ? 2 : 0)));
                    w.le<double>(a.in.subtick > 0.0 && a.in.subtick < 1.0 ? a.in.subtick : 0.0);
                }
            }
            w.le<uint64_t>(fnv1a64(w.out.data(), w.out.size()));
            return w.out;
        }

        // ---- yBot. v2: "ybot", u32 version, u32 meta length, u32 blobs, the
        // meta (FPS as f32 at 24), the blobs, then varints of delta<<4 | flags,
        // flags bit 0 being player ONE. v1 shares the magic: f32 fps, i32
        // count, then (u32 frame, u32 state) records.

        bool readYbot2(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 8);
            uint32_t const metaLen = r.le<uint32_t>();
            uint32_t const blobs = r.le<uint32_t>();
            if (r.bad || metaLen > b.size()) {
                err = "The yBot header is damaged.";
                return false;
            }
            if (metaLen >= 28) {
                float fps;
                if (r.has(28)) {
                    std::memcpy(&fps, b.data() + 16 + 24, 4);
                    m.tps = fps;
                }
            }
            r.p = 16 + metaLen;
            for (uint32_t k = 0; k < blobs && !r.bad; k++)
                r.skip(r.le<uint32_t>());
            int64_t frame = 0;
            while (r.left() > 0 && !r.bad) {
                uint64_t const v = r.varint();
                if (r.bad)
                    break;
                unsigned const flags = v & 15;
                frame += (int64_t)(v >> 4);
                unsigned const button = flags >> 2;
                if (button < 1 || button > 3) {  // an FPS change
                    float const fps = r.le<float>();
                    if (!r.bad && std::isfinite(fps) && fps > 0.f)
                        m.tpsChanges.push_back({frame, (double)fps});
                    continue;
                }
                m.inputs.push_back(mk(frame, flags & 2, (flags & 1) == 0, uint8_t(button)));
            }
            m.bot = "yBot";
            for (auto it = m.tpsChanges.begin(); it != m.tpsChanges.end();) {
                if (it->frame == 0) {
                    m.tps = it->tps;
                    it = m.tpsChanges.erase(it);
                } else {
                    ++it;
                }
            }
            return true;
        }

        Bytes writeYbot2(Macro const& m, std::vector<std::string>&) {
            auto const list = inputsFor(m, false);
            Writer w;
            w.raw("ybot");
            w.le<uint32_t>(1);   // version
            w.le<uint32_t>(36);  // meta length, through TOTAL_PRESSES
            w.le<uint32_t>(0);   // no blobs
            size_t const meta = w.out.size();
            w.zeros(36);
            auto put32 = [&](size_t at, uint32_t v) { std::memcpy(w.out.data() + meta + at, &v, 4); };
            put32(8, (uint32_t)list.size());                                      // PRESSES
            put32(16, list.empty() ? 0 : u32Frame(lastFrame(list) + 1));          // FRAMES
            float const fps = (float)m.tps;
            std::memcpy(w.out.data() + meta + 24, &fps, 4);                       // FPS
            put32(28, (uint32_t)list.size());                                     // TOTAL_PRESSES
            int64_t prev = 0;
            size_t t = 0;
            auto const& changes = m.tpsChanges;
            for (auto const& in : list) {
                while (t < changes.size() && changes[t].frame <= in.frame) {
                    uint64_t const d = (uint64_t)std::max<int64_t>(0, changes[t].frame - prev);
                    w.varint(d << 4);  // button 0: an FPS change
                    w.le<float>((float)changes[t].tps);
                    prev = std::max(prev, changes[t].frame);
                    t++;
                }
                uint64_t const d = (uint64_t)std::max<int64_t>(0, in.frame - prev);
                uint8_t const button = in.button >= 1 && in.button <= 3 ? in.button : 1;
                w.varint((d << 4) | (in.player2 ? 0u : 1u) | (in.down ? 2u : 0u) | (unsigned(button) << 2));
                prev = std::max(prev, in.frame);
            }
            return w.out;
        }

        bool readYbot1(Bytes const& b, Macro& m, std::string&) {
            Reader r(b, 4);
            m.tps = r.le<float>();
            int32_t const count = r.le<int32_t>();
            for (int32_t k = 0; k < count && r.has(8); k++) {
                uint32_t const frame = r.le<uint32_t>();
                uint32_t const st = r.le<uint32_t>();
                m.inputs.push_back(mk(frame, st & 2, st & 1));
            }
            m.bot = "yBot";
            return true;
        }

        // ---- the 2.1-era bots, from nat-converter (with peony's permission).
        // Jump only, a player flag, nothing else.

        bool readZbot(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b);
            float const delta = r.le<float>(), speed = r.le<float>();
            if (r.bad) {
                err = "The zBot macro is cut off.";
                return false;
            }
            double const fps = std::round(1.0 / ((double)delta * (double)speed));
            m.tps = std::isfinite(fps) && fps > 0.0 ? fps : 240.0;
            while (r.has(6)) {
                int32_t const frame = r.le<int32_t>();
                bool const hold = r.u8() == '1';  // ASCII '1' and '0'
                bool const p2 = r.u8() == '1';
                m.inputs.push_back(mk(frame, hold, p2));
            }
            m.bot = "zBot";
            return true;
        }
        Bytes writeZbot(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.le<float>((float)(1.0 / m.tps));
            w.le<float>(1.f);
            for (auto const& in : inputsFor(m, true)) {
                w.le<int32_t>((int32_t)u32Frame(in.frame));
                w.u8(in.down ? '1' : '0');
                w.u8(in.player2 ? '1' : '0');
            }
            return w.out;
        }

        bool readReplayBot(Bytes const& b, Macro& m, std::string& err) {
            if (b.size() < 10 || b[4] != 2) {
                err = "Only ReplayBot's version 2 macros are supported.";
                return false;
            }
            if (b[5] != 1) {
                err = "This ReplayBot macro is X-position based, not frame based.";
                return false;
            }
            Reader r(b, 6);
            m.tps = r.le<float>();
            while (r.has(5)) {
                uint32_t const frame = r.le<uint32_t>();
                uint8_t const st = r.u8();
                m.inputs.push_back(mk(frame, st & 1, st & 2));
            }
            m.bot = "ReplayBot";
            return true;
        }
        Bytes writeReplayBot(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.raw("RPLY");
            w.u8(2);  // version
            w.u8(1);  // frame based
            w.le<float>((float)m.tps);
            for (auto const& in : inputsFor(m, true)) {
                w.le<uint32_t>(u32Frame(in.frame));
                w.u8(uint8_t((in.down ? 1 : 0) | (in.player2 ? 2 : 0)));
            }
            return w.out;
        }

        bool readKdBot(Bytes const& b, Macro& m, std::string&) {
            Reader r(b);
            m.tps = r.le<float>();
            while (r.has(6)) {
                uint32_t const frame = r.le<uint32_t>();
                bool const hold = r.u8() == 1;
                bool const p2 = r.u8() == 1;
                m.inputs.push_back(mk(frame, hold, p2));
            }
            m.bot = "KD-Bot";
            return true;
        }
        Bytes writeKdBot(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.le<float>((float)m.tps);
            for (auto const& in : inputsFor(m, true)) {
                w.le<uint32_t>(u32Frame(in.frame));
                w.u8(in.down ? 1 : 0);
                w.u8(in.player2 ? 1 : 0);
            }
            return w.out;
        }

        bool readRush(Bytes const& b, Macro& m, std::string&) {
            Reader r(b);
            m.tps = r.le<int16_t>();  // a 16-bit whole number, not a float
            while (r.has(5)) {
                uint32_t const frame = r.le<uint32_t>();
                uint8_t const st = r.u8();
                m.inputs.push_back(mk(frame, st & 1, st & 2));
            }
            m.bot = "Rush";
            return true;
        }
        Bytes writeRush(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.le<int16_t>((int16_t)std::lround(m.tps));
            for (auto const& in : inputsFor(m, true)) {
                w.le<uint32_t>(u32Frame(in.frame));
                w.u8(uint8_t((in.down ? 1 : 0) | (in.player2 ? 2 : 0)));
            }
            return w.out;
        }

        bool readFembot(Bytes const& b, Macro& m, std::string&) {
            Reader r(b, 4);
            m.tps = r.le<float>();
            while (r.has(65)) {  // a state byte, a frame, 60 bytes of padding
                uint8_t const st = r.u8();
                uint32_t const frame = r.le<uint32_t>();
                r.skip(60);
                m.inputs.push_back(mk(frame, st & 1, st & 2));
            }
            m.bot = "Fembot";
            return true;
        }
        Bytes writeFembot(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.raw("FBRP");
            w.le<float>((float)m.tps);
            for (auto const& in : inputsFor(m, true)) {
                w.u8(uint8_t((in.down ? 1 : 0) | (in.player2 ? 2 : 0)));
                w.le<uint32_t>(u32Frame(in.frame));
                w.zeros(60);
            }
            return w.out;
        }

        bool readXbot(Bytes const& b, Macro& m, std::string&) {
            auto const ls = lines(asText(b));
            if (!ls.empty()) {
                auto const w = words(ls[0]);
                double d;
                if (w.size() > 1 && parseNumber(w[1], d))
                    m.tps = d;
            }
            // Line 0 is "fps: N", line 1 a bare "frames".
            for (size_t k = 2; k < ls.size(); k++) {
                auto const w = words(ls[k]);
                if (w.size() < 2)
                    continue;
                double st;
                if (!parseNumber(w[0], st))
                    continue;
                int const s = (int)st;
                m.inputs.push_back(mk(toInt(w[1]), s % 2 == 1, s > 1));
            }
            m.bot = "xBot";
            return true;
        }
        Bytes writeXbot(Macro const& m, std::vector<std::string>&) {
            std::string out = "fps: " + numText(m.tps) + "\nframes\n";
            for (auto const& in : inputsFor(m, true))
                out += std::to_string((in.down ? 1 : 0) | (in.player2 ? 2 : 0)) + " " + std::to_string(in.frame) + "\n";
            return Bytes(out.begin(), out.end());
        }

        bool readXdBot(Bytes const& b, Macro& m, std::string&) {
            auto const ls = lines(asText(b));
            double d;
            if (!ls.empty() && parseNumber(ls[0], d))
                m.tps = d;
            int skipped = 0;
            for (size_t k = 1; k < ls.size(); k++) {
                if (trim(ls[k]).empty())
                    continue;
                auto const f = split(ls[k], '|');
                if (f.size() < 4)
                    continue;
                if (trim(f[2]) != "1") {  // a jump row; the others aren't read
                    skipped++;
                    continue;
                }
                m.inputs.push_back(mk(toInt(f[0]), trim(f[1]) == "1", trim(f[3]) != "1"));
            }
            if (skipped)
                m.notes.push_back(std::to_string(skipped) + " non-jump rows in the xdBot macro weren't read.");
            m.bot = "xdBot";
            return true;
        }
        Bytes writeXdBot(Macro const& m, std::vector<std::string>&) {
            std::string out = numText(m.tps) + "\n";
            for (auto const& in : inputsFor(m, true))
                out += std::to_string(in.frame) + "|" + (in.down ? "1" : "0") + "|1|" + (in.player2 ? "0" : "1") + "\n";
            return Bytes(out.begin(), out.end());
        }

        constexpr uint8_t kMhrMagic[8] = {'H', 'A', 'C', 'K', 'P', 'R', 'O', 0x07};

        bool readMhrBin(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 8);
            int32_t const metaSize = r.le<int32_t>();
            m.tps = r.le<int32_t>();  // a whole number
            if (r.bad || metaSize < 4) {
                err = "The Mega Hack replay's header is damaged.";
                return false;
            }
            r.p = 16 + (size_t)(metaSize - 4) + 8;  // the rest of the meta, then 8 reserved bytes
            uint32_t const eventSize = r.le<uint32_t>();
            uint32_t const count = r.le<uint32_t>();
            if (r.bad || eventSize < 8) {
                err = "The Mega Hack replay's header is damaged.";
                return false;
            }
            for (uint32_t k = 0; k < count && r.has(eventSize); k++) {
                size_t const at = r.p;
                r.skip(2);
                bool const hold = r.u8() == 1;
                bool const p2 = r.u8() == 1;
                int32_t const frame = r.le<int32_t>();
                r.p = at + eventSize;
                m.inputs.push_back(mk(frame, hold, p2));
            }
            m.bot = "Mega Hack";
            return true;
        }
        Bytes writeMhrBin(Macro const& m, std::vector<std::string>&) {
            auto const list = inputsFor(m, true);
            Writer w;
            w.out.insert(w.out.end(), kMhrMagic, kMhrMagic + 8);
            w.le<int32_t>(4);  // meta size
            w.le<int32_t>((int32_t)std::lround(m.tps));
            w.zeros(8);         // reserved
            w.le<uint32_t>(32);  // event size
            w.le<uint32_t>((uint32_t)list.size());
            for (auto const& in : list) {
                w.le<uint16_t>(1);
                w.u8(in.down ? 1 : 0);
                w.u8(in.player2 ? 1 : 0);
                w.le<int32_t>((int32_t)u32Frame(in.frame));
                w.zeros(24);
            }
            static constexpr uint8_t tail[16] = {0xFA, 0x67, 0x55, 0x5A, 0x8D, 0x95, 0x94, 0x07,
                                                 0xC9, 0x8C, 0xBA, 0x7F, 0x75, 0x9C, 0xEF, 0x3C};
            w.out.insert(w.out.end(), tail, tail + 16);
            return w.out;
        }

        bool readMhrJson(Bytes const& b, Macro& m, std::string& err) {
            Value j;
            if (!parseJson(asText(b), j)) {
                err = "It isn't valid JSON.";
                return false;
            }
            if (auto const* meta = j.get("meta"))
                m.tps = numOf(meta->get("fps"), 240.0);
            if (auto const* ev = j.get("events"); ev && ev->isArray())
                for (auto const& e : ev->arr)
                    if (e.get("down"))
                        m.inputs.push_back(mk(intOf(e.get("frame"), 0), truthyOf(e.get("down")), truthyOf(e.get("p2"))));
            m.bot = "Mega Hack";
            return true;
        }
        Bytes writeMhrJson(Macro const& m, std::vector<std::string>&) {
            Value meta = Value::object();
            meta.set("fps", Value::ofNum(m.tps)).set("bot", Value::ofStr("GucciBot"));
            Value events = Value::array();
            for (auto const& in : inputsFor(m, true)) {
                Value e = Value::object();
                e.set("frame", Value::ofInt(in.frame))
                    .set("down", Value::ofBool(in.down))
                    .set("p2", Value::ofBool(in.player2));
                events.arr.push_back(std::move(e));
            }
            Value j = Value::object();
            j.set("meta", std::move(meta)).set("events", std::move(events));
            return jsonBytes(j);
        }

        // Echo shipped two JSON shapes and a binary one.
        bool readEchoJson(Bytes const& b, Macro& m, std::string& err) {
            Value j;
            if (!parseJson(asText(b), j)) {
                err = "It isn't valid JSON.";
                return false;
            }
            if (auto const* old = j.get("Echo Replay"); old && old->isArray()) {
                int64_t const start = intOf(j.get("Starting Frame"), 0);
                m.tps = std::round(numOf(j.get("FPS"), 240.0));
                for (auto const& e : old->arr)
                    m.inputs.push_back(
                        mk(intOf(e.get("Frame"), 0) + start, truthyOf(e.get("Hold")), truthyOf(e.get("Player 2"))));
            } else {
                m.tps = std::round(numOf(j.get("fps"), 240.0));
                if (auto const* list = j.get("inputs"); list && list->isArray())
                    for (auto const& e : list->arr)
                        m.inputs.push_back(
                            mk(intOf(e.get("frame"), 0), truthyOf(e.get("holding")), truthyOf(e.get("player_2"))));
            }
            m.bot = "Echo";
            return true;
        }
        Bytes writeEchoJson(Macro const& m, std::vector<std::string>&) {
            Value inputs = Value::array();
            for (auto const& in : inputsFor(m, true)) {
                Value e = Value::object();
                e.set("holding", Value::ofBool(in.down)).set("frame", Value::ofInt(in.frame));
                if (in.player2)
                    e.set("player_2", Value::ofBool(true));  // left out when false
                inputs.arr.push_back(std::move(e));
            }
            Value j = Value::object();
            j.set("fps", Value::ofNum(m.tps)).set("inputs", std::move(inputs));
            return jsonBytes(j);
        }

        bool readEchoBin(Bytes const& b, Macro& m, std::string& err) {
            if (b.size() < 48) {
                err = "The Echo macro is cut off.";
                return false;
            }
            // "DBG\0" at 4: the fat 34-byte action; otherwise 6 bytes.
            bool const full = b[4] == 'D' && b[5] == 'B' && b[6] == 'G' && b[7] == 0;
            size_t const size = full ? 34 : 6;
            float fps;
            std::memcpy(&fps, b.data() + 24, 4);
            m.tps = fps;
            Reader r(b, 48);
            while (r.has(size)) {
                size_t const at = r.p;
                uint32_t const frame = r.le<uint32_t>();
                bool const down = r.u8() == 1;
                bool const p2 = r.u8() == 1;
                r.p = at + size;
                m.inputs.push_back(mk(frame, down, p2));
            }
            m.bot = "Echo";
            return true;
        }

        // OmegaBot 2: Rust bincode 1.x (little-endian, fixed-width integers,
        // u64 lengths, u32 enum tags).
        bool readObot2(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b);
            m.tps = r.le<float>();
            r.le<float>();  // current fps
            if (r.le<uint32_t>() != 1) {
                err = "This OmegaBot macro is X-position based, not frame based.";
                return false;
            }
            r.le<uint64_t>();  // current click
            uint64_t const count = r.le<uint64_t>();
            for (uint64_t k = 0; k < count && r.has(12); k++) {
                uint32_t const loc = r.le<uint32_t>();
                uint32_t const frame = r.le<uint32_t>();
                uint32_t const kind = r.le<uint32_t>();
                if (kind == 1) {  // an FPS change, no input
                    r.skip(4);
                    continue;
                }
                if (loc != 1 || kind < 2 || kind > 5)
                    continue;
                // 2 P1 down, 3 P1 up, 4 P2 down, 5 P2 up
                m.inputs.push_back(mk(frame, kind == 2 || kind == 4, kind >= 4));
            }
            m.bot = "OmegaBot 2";
            return true;
        }

        bool readUrl(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b);
            m.tps = r.le<float>();
            uint8_t const kind = r.u8();  // 0 X position, 1 frames, 2 both
            if (kind == 0) {
                err = "This URL macro is X-position based, not frame based.";
                return false;
            }
            size_t const size = kind == 2 ? 9 : 5;
            while (r.has(size)) {
                uint8_t const st = r.u8();
                if (kind == 2)
                    r.skip(4);  // the X position
                uint32_t const frame = r.le<uint32_t>();
                m.inputs.push_back(mk(frame, st & 1, (st >> 1) & 1));
            }
            m.bot = "URL";
            return true;
        }
        Bytes writeUrl(Macro const& m, std::vector<std::string>&) {
            Writer w;
            w.le<float>((float)m.tps);
            w.u8(1);  // frame based
            for (auto const& in : inputsFor(m, true)) {
                w.u8(uint8_t((in.down ? 1 : 0) | (in.player2 ? 2 : 0)));
                w.le<uint32_t>(u32Frame(in.frame));
            }
            return w.out;
        }

        // TASBot: one row per frame, each player's click 1 (press) or 2 (release).
        bool readTasBot(Bytes const& b, Macro& m, std::string& err) {
            Value j;
            if (!parseJson(asText(b), j)) {
                err = "It isn't valid JSON.";
                return false;
            }
            m.tps = numOf(j.get("fps"), 240.0);
            if (auto const* rows = j.get("macro"); rows && rows->isArray()) {
                for (auto const& e : rows->arr) {
                    int64_t const f = intOf(e.get("frame"), 0);
                    for (int pl = 0; pl < 2; pl++) {
                        auto const* p = e.get(pl ? "player_2" : "player_1");
                        if (!p || !p->get("click"))
                            continue;
                        int64_t const c = intOf(p->get("click"), 0);
                        if (c != 0)
                            m.inputs.push_back(mk(f, c == 1, pl == 1));
                    }
                }
            }
            m.bot = "TASBot";
            return true;
        }
        Bytes writeTasBot(Macro const& m, std::vector<std::string>&) {
            Value rows = Value::array();
            for (auto const& in : inputsFor(m, true)) {
                if (rows.arr.empty() || rows.arr.back().get("frame")->integer() != in.frame) {
                    Value row = Value::object();
                    row.set("frame", Value::ofInt(in.frame));
                    rows.arr.push_back(std::move(row));
                }
                Value click = Value::object();
                click.set("click", Value::ofInt(in.down ? 1 : 2)).set("x_position", Value::ofInt(0));
                rows.arr.back().set(in.player2 ? "player_2" : "player_1", std::move(click));
            }
            Value j = Value::object();
            j.set("fps", Value::ofNum(m.tps)).set("macro", std::move(rows));
            return jsonBytes(j);
        }

        // ---- GucciBot 1.x's old .brrr ("BRR\0", format 4), worked out from a
        // real 1.x macro: a u32 version, a u16, a u32 that puts the payload at
        // 4 + it, three u16-length strings (author, name, level), the TPS and
        // the length as f64s, a timestamp, then the payload's inflated size
        // and the zlib payload itself: a u32 count of (LEB128 frame delta, u8
        // button, u8 bits: 1 player 2, 2 pressed) records.

        bool readBrr(Bytes const& b, Macro& m, std::string& err) {
            Reader r(b, 4);
            uint32_t const version = r.le<uint32_t>();
            r.le<uint16_t>();
            uint32_t const toPayload = r.le<uint32_t>();
            m.author = r.str(r.le<uint16_t>());
            m.name = r.str(r.le<uint16_t>());
            m.levelName = r.str(r.le<uint16_t>());
            size_t const payload = 4 + (size_t)toPayload;
            if (r.bad || payload < r.p + 8 + 8 + 4 || payload >= b.size()) {
                err = "This old GucciBot macro (format " + std::to_string(version) +
                      ") isn't laid out like the ones this reads. Send it in and it can be added.";
                return false;
            }
            r.skip(4);
            double const tps = r.le<double>();
            m.tps = std::isfinite(tps) && tps > 0.0 ? tps : 240.0;
            uint32_t rawSize;
            std::memcpy(&rawSize, b.data() + payload - 4, 4);
            if (rawSize == 0 || rawSize > (64u << 20) || b[payload] != 0x78) {
                err = "This old GucciBot macro (format " + std::to_string(version) +
                      ") isn't laid out like the ones this reads. Send it in and it can be added.";
                return false;
            }
            Bytes raw(rawSize);
            uLongf got = rawSize;
            if (uncompress(raw.data(), &got, b.data() + payload, (uLong)(b.size() - payload)) != Z_OK) {
                err = "This old GucciBot macro is damaged (its inputs don't unpack).";
                return false;
            }
            raw.resize(got);
            Reader in(raw);
            uint32_t const count = in.le<uint32_t>();
            if (in.bad || (uint64_t)count * 3 > raw.size()) {
                err = "This old GucciBot macro is damaged (its input count is wrong).";
                return false;
            }
            int64_t frame = 0;
            for (uint32_t k = 0; k < count; k++) {
                frame += (int64_t)in.varint();
                uint8_t const button = in.u8(), bits = in.u8();
                if (in.bad || button < 1 || button > 3) {
                    err = "This old GucciBot macro is damaged (input " + std::to_string(k) + " doesn't read).";
                    return false;
                }
                m.inputs.push_back(mk(frame, bits & 2, bits & 1, button));
            }
            m.bot = "GucciBot 1.x";
            if (version != 4)
                m.notes.push_back("This is a format " + std::to_string(version) +
                                  " .brrr; only format 4 was checked against a real macro.");
            return true;
        }

        // ============================================================= the table

        struct Entry {
            Format f;
            ReadFn read;
            WriteFn write;
        };

        std::vector<Entry> const& table() {
            static std::vector<Entry> const t = {
                {{"gdr", "GDR (Mega Hack, Macrobot, xdBot and more)", ".gdr", true, false, 0, true}, readGdr, writeGdr},
                {{"gdr2", "GDR2", ".gdr2", true, false, 0, false}, readGdr2, writeGdr2},
                {{"gdrjson", "GDR (JSON)", ".gdr.json", true, false, 0, false}, readGdrJson, writeGdrJson},
                {{"slc3", "Silicate (.slc)", ".slc", true, false, 1, true}, readSlc3, writeSlc3},
                {{"slc2", "Silicate 2 (.slc, older)", ".slc", false, false, 1, false}, readSlc2, nullptr},
                {{"slc1", "Silicate 1 (.slc, oldest)", ".slc", false, false, 1, false}, readSlc1, nullptr},
                {{"scbf1", "Silicate CBF (.scbf1)", ".scbf1", true, false, 1, false}, readScbf, writeScbf},
                {{"tcm", "TcBot (.tcm)", ".tcm", true, false, 0, true}, readTcm, writeTcm},
                {{"ybot2", "yBot (.ybot)", ".ybot", true, false, 0, false}, readYbot2, writeYbot2},
                {{"ybot1", "yBot 1 (.ybot, older)", ".ybot", false, true, 0, false}, readYbot1, nullptr},
                {{"xdbot", "xdBot (.xd)", ".xd", true, true, 0, false}, readXdBot, writeXdBot},
                {{"mhrbin", "Mega Hack Replay (.mhr)", ".mhr", true, true, 0, false}, readMhrBin, writeMhrBin},
                {{"mhrjson", "Mega Hack Replay (JSON)", ".mhr.json", true, true, 0, false}, readMhrJson, writeMhrJson},
                {{"echojson", "Echo (.echo)", ".echo", true, true, 0, false}, readEchoJson, writeEchoJson},
                {{"echobin", "Echo (binary .echo)", ".echo", false, true, 0, false}, readEchoBin, nullptr},
                {{"obot2", "OmegaBot 2 (.replay)", ".replay", false, true, 0, false}, readObot2, nullptr},
                {{"replaybot", "ReplayBot (.replay)", ".replay", true, true, 0, false}, readReplayBot, writeReplayBot},
                {{"zbot", "zBot (.zbf)", ".zbf", true, true, 0, false}, readZbot, writeZbot},
                {{"kdbot", "KD-Bot (.kd)", ".kd", true, true, 0, false}, readKdBot, writeKdBot},
                {{"rush", "Rush (.rush)", ".rush", true, true, 0, false}, readRush, writeRush},
                {{"fembot", "Fembot (.freplay)", ".freplay", true, true, 0, false}, readFembot, writeFembot},
                {{"xbot", "xBot (.xbot)", ".xbot", true, true, 0, false}, readXbot, writeXbot},
                {{"url", "URL (.url)", ".url", true, true, 0, false}, readUrl, writeUrl},
                {{"tasbot", "TASBot (.json)", ".json", true, true, 0, false}, readTasBot, writeTasBot},
                {{"plain", "Plain text (.txt)", ".txt", true, false, 0, false}, readPlain, writePlain},
                {{"brr", "GucciBot 1.x (old .brrr)", ".brrr", false, false, 0, true}, readBrr, nullptr},
            };
            return t;
        }

        Entry const* entry(std::string_view id) {
            for (auto const& e : table())
                if (e.f.id == id)
                    return &e;
            return nullptr;
        }

        bool plausibleFps(double fps) {
            return std::isfinite(fps) && fps > 0.0 && fps < 100000.0;
        }

        // The text formats, from what the text looks like.
        std::string_view detectText(Bytes const& b) {
            auto const t = trim(asText(b));
            if (t.empty())
                return {};
            if (t.front() == '{') {
                Value j;
                if (!parseJson(t, j) || j.kind != Value::Obj)
                    return {};
                auto const* inputs = j.get("inputs");
                bool const list = inputs && inputs->isArray();
                if (list && (j.get("framerate") || j.get("bot") || j.get("botInfo") || j.get("gameVersion")))
                    return "gdrjson";
                if (j.get("Echo Replay"))
                    return "echojson";
                if (list && j.get("fps"))
                    return "echojson";
                if (j.get("events") && j.get("meta"))
                    return "mhrjson";
                if (j.get("macro") && j.get("fps"))
                    return "tasbot";
                return {};
            }
            auto const ls = lines(t);
            if (ls.size() >= 2) {
                double d;
                auto const second = split(ls[1], '|');
                if (parseNumber(ls[0], d) && second.size() >= 4)
                    return "xdbot";
            }
            if (lower(t.substr(0, 4)) == "fps:")
                return "xbot";
            auto const first = trim(ls.empty() ? t : ls[0]);
            if (!first.empty() && first[0] == '#')
                return "plain";
            auto const w = words(first);
            double a, c;
            if (w.size() >= 2 && parseNumber(w[0], a) && parseNumber(w[1], c))
                return "plain";
            return {};
        }

        std::string_view detectId(Bytes const& b, std::string_view fileName) {
            if (startsWith(b, std::string_view("BRR\0", 4)))
                return "brr";
            if (startsWith(b, "SLC3RPLY"))
                return "slc3";
            if (startsWith(b, "SILL"))
                return "slc2";
            if (startsWith(b, "SCBF"))
                return "scbf1";
            if (isTcm(b))
                return "tcm";
            if (startsWith(b, "GDR"))
                return "gdr2";
            if (startsWith(b, "RPLY"))
                return "replaybot";
            if (startsWith(b, "FBRP"))
                return "fembot";
            if (b.size() >= 8 && std::memcmp(b.data(), kMhrMagic, 8) == 0)
                return "mhrbin";
            if (startsWith(b, "META"))
                return "echobin";
            if (startsWith(b, "ybot") && b.size() >= 12) {
                uint32_t v;
                std::memcpy(&v, b.data() + 4, 4);
                return v <= 16 ? "ybot2" : "ybot1";
            }
            if (!b.empty() && ((b[0] >= 0x80 && b[0] <= 0x8F) || b[0] == 0xDE || b[0] == 0xDF)) {
                MsgpackReader mp{Reader(b)};
                Value const j = mp.value();
                if (!mp.r.bad && j.kind == Value::Obj && j.get("inputs") && j.get("inputs")->isArray())
                    return "gdr";
            }
            if (auto const id = detectText(b); !id.empty())
                return id;
            // The ones only their name gives away.
            auto f32 = [&](size_t at) {
                float v = 0.f;
                if (b.size() >= at + 4)
                    std::memcpy(&v, b.data() + at, 4);
                return (double)v;
            };
            if (endsWith(fileName, ".zbf") && b.size() >= 8)
                return "zbot";
            if (endsWith(fileName, ".kd") && b.size() >= 4)
                return "kdbot";
            if (endsWith(fileName, ".rush") && b.size() >= 2)
                return "rush";
            if (endsWith(fileName, ".url") && b.size() >= 5 && b[4] <= 2 && plausibleFps(f32(0)))
                return "url";
            if (endsWith(fileName, ".slc") && b.size() >= 12) {
                double fps;
                std::memcpy(&fps, b.data(), 8);
                if (plausibleFps(fps))
                    return "slc1";
            }
            if (endsWith(fileName, ".replay") && b.size() >= 28) {
                uint32_t type;
                std::memcpy(&type, b.data() + 8, 4);
                if (plausibleFps(f32(0)) && type <= 1)
                    return "obot2";
            }
            return {};
        }

    } // namespace

    std::vector<Format> const& formats() {
        static std::vector<Format> const list = [] {
            std::vector<Format> out;
            for (auto const& e : table())
                out.push_back(e.f);
            return out;
        }();
        return list;
    }

    Format const* find(std::string_view id) {
        for (auto const& f : formats())
            if (f.id == id)
                return &f;
        return nullptr;
    }

    bool looksImportable(std::string_view fileName) {
        static constexpr std::string_view exts[] = {".gdr", ".gdr2", ".json", ".slc", ".scbf1", ".tcm", ".ybot",
                                                    ".xd", ".mhr", ".echo", ".replay", ".zbf", ".kd", ".rush",
                                                    ".freplay", ".xbot", ".url", ".txt", ".brrr"};
        for (auto e : exts)
            if (endsWith(fileName, e))
                return true;
        return false;
    }

    bool isGucciBot(std::vector<uint8_t> const& bytes) {
        return startsWith(bytes, "GBR6");
    }

    Format const* detect(std::vector<uint8_t> const& bytes, std::string_view fileName) {
        if (isGucciBot(bytes))
            return nullptr;
        auto const id = detectId(bytes, fileName);
        return id.empty() ? nullptr : find(id);
    }

    ReadResult read(std::vector<uint8_t> const& bytes, std::string_view fileName, int extraShift) {
        ReadResult res;
        if (bytes.empty()) {
            res.error = "The file is empty.";
            return res;
        }
        if (isGucciBot(bytes)) {
            res.error = "This is already a GucciBot macro.";
            return res;
        }
        res.format = detect(bytes, fileName);
        if (!res.format) {
            res.error = "This isn't a macro format GucciBot knows.";
            return res;
        }
        auto const* e = entry(res.format->id);
        Macro& m = res.macro;
        std::string err;
        if (!e->read(bytes, m, err)) {
            res.error = err.empty() ? "The macro couldn't be read." : err;
            return res;
        }
        m.tps = snapTps(m.tps);
        for (auto& t : m.tpsChanges)
            t.tps = snapTps(t.tps);
        // Into GucciBot's numbering.
        int const shift = res.format->frameShift + extraShift;
        if (shift != 0) {
            for (auto& in : m.inputs)
                in.frame += shift;
            for (auto& t : m.tpsChanges)
                t.frame += shift;
        }
        size_t const before = m.inputs.size();
        std::erase_if(m.inputs, [](Input const& in) { return in.frame < 0; });
        if (m.inputs.size() != before)
            m.notes.push_back(std::to_string(before - m.inputs.size()) +
                              " inputs fell before the start of the level and were left out.");
        std::erase_if(m.tpsChanges, [](TpsChange const& t) { return t.frame <= 0; });
        sortInputs(m);
        if (m.inputs.empty())
            m.notes.push_back("The macro has no inputs in it.");
        if (hasMoves(m))
            m.platformer = true;
        return res;
    }

    WriteResult write(Macro const& src, Format const& f) {
        WriteResult res;
        auto const* e = entry(f.id);
        if (!e || !e->write) {
            res.error = "GucciBot can read " + std::string(f.name) + " but not write it.";
            return res;
        }
        Macro m = src;
        if (f.frameShift != 0) {
            for (auto& in : m.inputs)
                in.frame -= f.frameShift;
            for (auto& t : m.tpsChanges)
                t.frame -= f.frameShift;
            size_t const before = m.inputs.size();
            std::erase_if(m.inputs, [](Input const& in) { return in.frame < 0; });
            if (m.inputs.size() != before)
                res.notes.push_back("An input on the level's first frame has no frame before it in this format and "
                                    "was left out.");
        }
        sortInputs(m);
        if (f.jumpOnly && hasMoves(m))
            res.notes.push_back(std::string(f.name) + " only has the jump button: the Left and Right inputs were "
                                                      "left out.");
        bool const keepsTps = f.id == "tcm" || f.id == "slc3" || f.id == "scbf1" || f.id == "ybot2";
        if (!m.tpsChanges.empty() && !keepsTps)
            res.notes.push_back(std::string(f.name) + " has one rate for the whole macro: the " +
                                std::to_string(m.tpsChanges.size()) + " TPS change(s) were left out.");
        bool subticks = false;
        for (auto const& in : m.inputs)
            subticks = subticks || (in.subtick > 0.0 && in.subtick < 1.0);
        if (subticks && f.id != "scbf1")
            res.notes.push_back("Sub-tick timing (CBF) doesn't carry over: every click lands on its tick.");
        res.bytes = e->write(m, res.notes);
        return res;
    }

    // ------------------------------------------------------------ GucciBot's own

    std::vector<uint8_t> toGucciBot(Macro const& m) {
        std::vector<GBR6Input> p1, p2;
        std::vector<GBR6Subtick> subticks;
        for (auto const& in : m.inputs) {
            GBR6Input g;
            g.frame = u32Frame(in.frame);
            g.button = in.button >= 1 && in.button <= 3 ? in.button : 1;
            g.pressed = in.down;
            g.player2 = in.player2;
            (in.player2 ? p2 : p1).push_back(g);
            if (in.subtick > 0.0 && in.subtick < 1.0)
                subticks.push_back({g.frame, g.button, g.pressed, g.player2, in.subtick});
        }
        GBR6Header hdr;
        hdr.tps = (float)m.tps;
        hdr.name = m.name;
        hdr.rngSeed = m.hasSeed ? m.seed : 0;
        hdr.timestamp =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!m.levelName.empty()) {
            hdr.levelName = m.levelName;
            hdr.flags |= GBR6_HAS_LEVELNAME;
        }
        if (!p2.empty())
            hdr.flags |= GBR6_TWO_PLAYER;
        if (m.platformer)
            hdr.flags |= GBR6_PLATFORMER;
        auto f = GBR6File::fromInputs(hdr, std::move(p1), std::move(p2));
        if (!subticks.empty()) {
            f.subticks = std::move(subticks);
            f.header.flags |= GBR6_HAS_SUBTICK;
        }
        for (auto const& t : m.tpsChanges)
            if (t.frame > 0)
                f.tpsChanges.push_back({u32Frame(t.frame), t.tps});
        if (!f.tpsChanges.empty())
            f.header.flags |= GBR6_HAS_TPS;
        return f.serialize();
    }

    bool fromGucciBot(std::vector<uint8_t> const& bytes, Macro& out, std::string& error) {
        auto f = GBR6File::deserialize(bytes.data(), bytes.size());
        if (!f) {
            error = "The GucciBot macro is damaged.";
            return false;
        }
        out = Macro{};
        out.tps = f->header.tps > 0.f ? (double)f->header.tps : 240.0;
        out.seed = f->header.rngSeed;
        out.hasSeed = true;
        out.name = f->header.name;
        out.levelName = f->header.levelName;
        out.platformer = f->header.platformer();
        out.bot = "GucciBot";
        for (auto const* list : {&f->p1Inputs, &f->p2Inputs})
            for (auto const& g : *list)
                out.inputs.push_back(mk(g.frame, g.pressed, g.player2, g.button));
        for (auto& in : out.inputs)
            for (auto const& st : f->subticks)
                if (st.frame == in.frame && st.button == in.button && st.pressed == in.down &&
                    st.player2 == in.player2) {
                    in.subtick = st.offset;
                    break;
                }
        for (auto const& t : f->tpsChanges) {
            if (t.frame == 0)
                out.tps = t.tps;
            else
                out.tpsChanges.push_back({(int64_t)t.frame, t.tps});
        }
        sortInputs(out);
        if (hasMoves(out))
            out.platformer = true;
        return true;
    }

} // namespace gucci::convert
