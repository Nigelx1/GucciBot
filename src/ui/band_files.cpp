// See band_files.hpp. Written fresh for GucciBot 2.0 (2026-11-16) from
// today's Calculate code; the picker is Import a macro's (ui/shell.cpp), the
// zip is Share my windows' (tools/share_bundle).

#include "ui/band_files.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/fw_settings_store.hpp"
#include "analysis/ac/sound.hpp"
#include "core/GucciBot.hpp"
#include "tools/share_bundle.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>
#include <matjson.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace gucci::ui::bandfiles {

    namespace {

        constexpr char kManifest[] = "bandsounds.json";
        constexpr char kFormat[] = "band sounds";
        constexpr int kVersion = 1;
        constexpr char kSoundDir[] = "sounds/";
        // A folder import brings at most this many files, and skips any one
        // bigger than a pack may hold.
        constexpr size_t kMaxFolderFiles = 200;
        // ...and at most this much in all, so one pick can't freeze the game
        // for long or fill the disk.
        constexpr uintmax_t kMaxFolderBytes = 256ull << 20;

        fs::path u8path(std::string const& s) { return fs::path(std::u8string(s.begin(), s.end())); }
        std::string utf8(fs::path const& p) { return geode::utils::string::pathToString(p); }

        std::string lower(std::string s) {
            for (auto& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool soundExt(std::string const& ext) {
            auto const e = lower(ext);
            return e == ".ogg" || e == ".mp3" || e == ".wav";
        }
        bool imageExt(std::string const& ext) {
            auto const e = lower(ext);
            return e == ".png" || e == ".jpg" || e == ".jpeg";
        }
        bool kindExt(Kind kind, std::string const& ext) { return kind == Kind::Sound ? soundExt(ext) : imageExt(ext); }
        std::string extOf(std::string const& name) {
            auto const dot = name.rfind('.');
            return dot == std::string::npos ? std::string() : name.substr(dot);
        }

        std::vector<uint8_t> readFile(fs::path const& p, std::string& error) {
            std::error_code ec;
            auto const size = fs::file_size(p, ec);
            if (ec) {
                error = "couldn't read it";
                return {};
            }
            if (size > gucci::bundle::kMaxEntry) {
                error = "it's bigger than 32 MB";
                return {};
            }
            std::ifstream in(p, std::ios::binary);
            if (!in) {
                error = "couldn't open it";
                return {};
            }
            std::vector<uint8_t> out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (out.size() != size) {
                error = "couldn't read all of it";
                return {};
            }
            return out;
        }

        bool sameBytes(fs::path const& p, std::vector<uint8_t> const& bytes) {
            std::error_code ec;
            if (fs::file_size(p, ec) != bytes.size() || ec)
                return false;
            std::string ignored;
            return readFile(p, ignored) == bytes;
        }

        // Written whole or not at all: to <file>.part, then renamed.
        std::string writeWhole(fs::path const& to, std::vector<uint8_t> const& bytes) {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            fs::path tmp = to;
            tmp += ".part";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return "couldn't write " + utf8(to.filename());
                out.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out) {
                    out.close();
                    fs::remove(tmp, ec);
                    return "couldn't write " + utf8(to.filename()) + " (is the disk full?)";
                }
            }
            fs::rename(tmp, to, ec);
            if (ec) {
                std::error_code rmEc;
                fs::remove(tmp, rmEc);
                return "couldn't save " + utf8(to.filename()) + ": " + ec.message();
            }
            return "";
        }

        // `want` itself if nothing is there, else "name (2).ext", "(3)"...
        fs::path freePath(fs::path const& want) {
            std::error_code ec;
            if (!fs::exists(want, ec))
                return want;
            auto const stem = want.stem().native();
            auto const ext = want.extension().native();
            for (int n = 2; n < 10000; n++) {
                auto candidate = want.parent_path() / stem;
                candidate += " (" + std::to_string(n) + ")";
                candidate += ext;
                if (!fs::exists(candidate, ec))
                    return candidate;
            }
            return want;
        }

        // A file name as it may be kept: the share file's rule
        // (bundle::fileNameOk), any character it refuses made a _.
        std::string keepableName(std::string name) {
            if (gucci::bundle::fileNameOk(name))
                return name;
            for (auto& ch : name) {
                unsigned char const c = static_cast<unsigned char>(ch);
                if (c < 0x20 || c == 0x7F || std::string_view("\\/:*?\"<>|").find(ch) != std::string_view::npos)
                    ch = '_';
            }
            while (!name.empty() && (name.front() == '.' || name.front() == ' '))
                name.erase(name.begin());
            while (!name.empty() && (name.back() == '.' || name.back() == ' '))
                name.pop_back();
            if (name.size() > 200)
                name.resize(200);
            return gucci::bundle::fileNameOk(name) ? name : std::string();
        }

        // A path under `dir`, relative to it with / between folders, or
        // nothing when it isn't under it.
        std::optional<std::string> under(fs::path const& file, fs::path const& dir) {
            std::error_code ec;
            auto const rel = fs::relative(file, dir, ec);
            if (ec || rel.empty() || *rel.begin() == "..")
                return std::nullopt;
            auto const g = rel.generic_u8string();
            return std::string(g.begin(), g.end());
        }

        // What a band's Sound or Image takes for a file: relative to the
        // config folder ("bands/x.ogg"), which is where a relative one is
        // looked for first.
        std::string bandPath(fs::path const& file) {
            if (auto const rel = under(file, Mod::get()->getConfigDir()))
                return *rel;
            return utf8(file);
        }

        // A band's sound as given: as is when absolute, else the config
        // folder, then the resources (FrameWindowSound::play's rule).
        struct Found {
            fs::path file;
            bool bundled = false;  // in the mod's resources
        };
        std::optional<Found> findSound(std::string const& given) {
            fs::path p = u8path(given);
            std::error_code ec;
            if (p.is_absolute())
                return fs::is_regular_file(p, ec) ? std::optional<Found>(Found{p, false}) : std::nullopt;
            if (auto const c = Mod::get()->getConfigDir() / p; fs::is_regular_file(c, ec))
                return Found{c, false};
            if (auto const r = Mod::get()->getResourcesDir() / p; fs::is_regular_file(r, ec))
                return Found{r, true};
            return std::nullopt;
        }

        std::string bandName(FrameWindowTier const& t, size_t index) {
            if (!t.text.empty())
                return fmt::format("Band {} ({})", index + 1, t.text);
            return t.minWindow == t.maxWindow ? fmt::format("Band {} ({:g})", index + 1, t.minWindow)
                                              : fmt::format("Band {} ({:g}-{:g})", index + 1, t.minWindow, t.maxWindow);
        }

        std::string trimmed(std::string s) {
            auto const a = s.find_first_not_of(" \t");
            if (a == std::string::npos)
                return "";
            return s.substr(a, s.find_last_not_of(" \t") - a + 1);
        }

        // ---- the card's state

        std::string g_packName;
        std::string g_note;
        bool g_noteOk = true;

        enum class Want { None, File, Folder, Pack };
        struct Picked {
            bool ready = false;
            Want what = Want::None;
            Kind kind = Kind::Sound;
            int bandId = 0;
            std::optional<fs::path> path;
            std::string error;
        };
        Picked s_picked;
        bool s_pending = false;

        // Where the picker opens: bands/ (made if it isn't there), as Import a
        // macro opens at replays/imported. Windows and macOS only: Geode hands
        // Android's launcher just the folder's last name, and iOS writes an
        // empty file at the path and doesn't open there.
        std::optional<fs::path> pickerFolder(Want what) {
#if defined(GEODE_IS_DESKTOP)
            std::error_code ec;
            auto const dir = what == Want::Pack ? packsFolder() : folder();
            fs::create_directories(dir, ec);
            if (fs::is_directory(dir, ec))
                return dir;
#else
            (void)what;
#endif
            return std::nullopt;
        }

        arc::Future<int> pickTask(Want what, Kind kind, int bandId) {
            geode::utils::file::FilePickOptions options;
            options.defaultPath = pickerFolder(what);
            if (what == Want::File) {
                geode::utils::file::FilePickOptions::Filter f;
                if (kind == Kind::Sound) {
                    f.description = "Sounds";
                    f.files = {"*.ogg", "*.mp3", "*.wav"};
                } else {
                    f.description = "Pictures";
                    f.files = {"*.png", "*.jpg", "*.jpeg"};
                }
                options.filters.push_back(std::move(f));
            } else if (what == Want::Pack) {
                geode::utils::file::FilePickOptions::Filter f;
                f.description = "Band sound packs";
                f.files = {"*.zip"};
                options.filters.push_back(std::move(f));
            }
            auto const mode = what == Want::Folder ? geode::utils::file::PickMode::OpenFolder
                                                   : geode::utils::file::PickMode::OpenFile;
            geode::utils::file::PickResult result = co_await geode::utils::file::pick(mode, options);
            // Windows fails the whole pick, before the dialog shows, when it
            // can't open at the folder (Geode's nfdwin.cpp): then without it.
            if (options.defaultPath && result.isErr() && result.unwrapErr() == "Unable to set default path to dialog") {
                options.defaultPath.reset();
                result = co_await geode::utils::file::pick(mode, std::move(options));
            }
            Picked got;
            got.ready = true;
            got.what = what;
            got.kind = kind;
            got.bandId = bandId;
            if (result.isOk())
                got.path = std::move(result).unwrap();
            else
                got.error = std::move(result).unwrapErr();
            geode::queueInMainThread([got = std::move(got)]() mutable {
                s_picked = std::move(got);
                s_pending = false;
            });
            co_return 0;
        }

        void startPick(Want what, Kind kind, int bandId) {
            if (s_pending)
                return;
            s_pending = true;
            // The handle is a temporary: dropping it detaches the task.
            (void)geode::async::runtime().spawn(pickTask(what, kind, bandId));
        }

        // A file in bands/ onto a band, saved, and the markers drawn again.
        bool assign(int bandId, Kind kind, std::string const& path) {
            for (auto& t : SLSettings::get()->frameWindow.tiers) {
                if (t.id != bandId)
                    continue;
                (kind == Kind::Sound ? t.audioPath : t.imagePath) = path;
                fwstore::save();
                auto& fw = ::Bot::get()->frameWindow();
                if (kind == Kind::Image && !fw.running())
                    fw.clearDisplay();
                return true;
            }
            return false;
        }

        // The files list, read at most every 2 seconds (the band editor asks
        // every frame) and at once after an import.
        std::map<int, std::vector<std::string>> s_listed;
        std::chrono::steady_clock::time_point s_listedAt{};
        void forgetListed() { s_listed.clear(); }

    } // namespace

    fs::path folder() { return Mod::get()->getConfigDir() / "bands"; }
    fs::path packsFolder() { return Mod::get()->getSaveDir() / "band packs"; }

    Result importFile(fs::path const& picked, Kind kind) {
        Result r;
        std::string const fname = utf8(picked.filename());
        std::string const ext = utf8(picked.extension());
        if (!kindExt(kind, ext)) {
            r.note = kind == Kind::Sound ? fmt::format("{} isn't a sound: an ogg, mp3 or wav is.", fname)
                                         : fmt::format("{} isn't a picture: a png or jpg is.", fname);
            return r;
        }
        std::string error;
        auto const bytes = readFile(picked, error);
        if (!error.empty()) {
            r.note = fmt::format("Couldn't bring in {}: {}.", fname, error);
            return r;
        }
        if (bytes.empty()) {
            r.note = fmt::format("{} is empty.", fname);
            return r;
        }
        // Picked from bands/ itself (the picker opens there): used where it is.
        if (under(picked, folder())) {
            r.ok = true;
            r.path = bandPath(picked);
            r.note = fmt::format("{} is in bands/ already.", fname);
            return r;
        }
        std::string const name = keepableName(fname);
        if (name.empty()) {
            r.note = fmt::format("{}: that file name can't be kept. Rename it and try again.", fname);
            return r;
        }
        std::error_code ec;
        auto const dir = folder();
        fs::create_directories(dir, ec);
        auto want = dir / u8path(name);
        // Already here, the same file: used as it is.
        fs::path to;
        if (fs::exists(want, ec) && sameBytes(want, bytes))
            to = want;
        else if (fs::exists(want, ec) && fs::equivalent(want, picked, ec))
            to = want;
        if (to.empty()) {
            to = freePath(want);
            if (auto err = writeWhole(to, bytes); !err.empty()) {
                r.note = "Couldn't bring it in: " + err + ".";
                return r;
            }
        }
        forgetListed();
        FrameWindowSound::clearCache();
        r.ok = true;
        r.path = bandPath(to);
        r.note = to == want && fname == name ? fmt::format("{} is in bands/.", fname)
                                             : fmt::format("{} is in bands/ as {}.", fname, utf8(to.filename()));
        return r;
    }

    Result chooseFile(fs::path const& picked, Kind kind, int bandId) {
        auto res = importFile(picked, kind);
        if (res.ok && !assign(bandId, kind, res.path)) {
            res.ok = false;
            res.note += " The band it was for is gone.";
        }
        return res;
    }

    Result importFolder(fs::path const& dir) {
        Result r;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            r.note = "That isn't a folder.";
            return r;
        }
        int sounds = 0, images = 0, same = 0;
        std::vector<std::string> skipped;
        size_t seen = 0;
        std::vector<fs::path> files;
        for (auto const& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code typeEc;
            if (!e.is_regular_file(typeEc))
                continue;
            auto const ext = utf8(e.path().extension());
            if (!soundExt(ext) && !imageExt(ext))
                continue;
            files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        uintmax_t total = 0;
        for (auto const& p : files) {
            if (++seen > kMaxFolderFiles) {
                skipped.push_back(fmt::format("{} more (at most {} at a time)", files.size() - kMaxFolderFiles,
                                              kMaxFolderFiles));
                break;
            }
            std::error_code sizeEc;
            auto const size = fs::file_size(p, sizeEc);
            if (!sizeEc && total + size > kMaxFolderBytes) {
                skipped.push_back(fmt::format("{} more (at most {} MB at a time)", files.size() - seen + 1,
                                              kMaxFolderBytes >> 20));
                break;
            }
            if (!sizeEc)
                total += size;
            auto const kind = soundExt(utf8(p.extension())) ? Kind::Sound : Kind::Image;
            auto const before = imported(kind);
            auto const one = importFile(p, kind);
            if (!one.ok) {
                skipped.push_back(one.note);
                continue;
            }
            if (std::find(before.begin(), before.end(), one.path) != before.end())
                same++;
            (kind == Kind::Sound ? sounds : images)++;
        }
        forgetListed();
        if (sounds + images == 0 && skipped.empty()) {
            r.note = fmt::format("{} has no sounds (ogg, mp3, wav) or pictures (png, jpg) in it.", utf8(dir.filename()));
            return r;
        }
        r.ok = sounds + images > 0;
        r.note = fmt::format("From {}: {} sound{} and {} picture{} in bands/{}.", utf8(dir.filename()), sounds,
                             sounds == 1 ? "" : "s", images, images == 1 ? "" : "s",
                             same ? fmt::format(" ({} already there)", same) : "");
        if (!skipped.empty()) {
            r.note += fmt::format("\nLeft out {}:", skipped.size());
            for (auto const& s : skipped)
                r.note += "\n" + s;
        }
        r.path = utf8(folder());
        return r;
    }

    std::vector<std::string> imported(Kind kind) {
        int const key = kind == Kind::Sound ? 0 : 1;
        auto const now = std::chrono::steady_clock::now();
        if (now - s_listedAt > std::chrono::seconds(2))
            s_listed.clear();
        if (auto it = s_listed.find(key); it != s_listed.end())
            return it->second;
        std::vector<std::string> out;
        std::error_code ec;
        auto const dir = folder();
        if (fs::is_directory(dir, ec)) {
            // bands/ and one folder down (a pack's).
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                auto const name = utf8(it->path().filename());
                if (it.depth() > 1 || name.empty() || name.front() == '.') {
                    if (it->is_directory(ec))
                        it.disable_recursion_pending();
                    continue;
                }
                std::error_code typeEc;
                if (it->is_regular_file(typeEc) && kindExt(kind, utf8(it->path().extension())))
                    out.push_back(bandPath(it->path()));
            }
        }
        std::sort(out.begin(), out.end());
        s_listed[key] = out;
        s_listedAt = now;
        return out;
    }

    Result exportPack(std::string const& given) {
        Result r;
        std::string const name = trimmed(given);
        if (name.empty()) {
            r.note = "Give the pack a name first.";
            return r;
        }
        if (!gucci::bundle::fileNameOk(name) || name.size() > 80) {
            r.note = "That name can't be a file name. Leave out \\ / : * ? \" < > | and keep it short.";
            return r;
        }
        auto const& tiers = SLSettings::get()->frameWindow.tiers;
        if (tiers.empty()) {
            r.note = "There are no bands to take sounds from.";
            return r;
        }

        auto bands = matjson::Value::array();
        std::vector<gucci::bundle::Entry> entries;
        entries.push_back({kManifest, {}});
        std::map<std::string, std::vector<uint8_t> const*> byName;  // name in sounds/ -> its bytes
        std::vector<std::vector<uint8_t>> kept;
        kept.reserve(tiers.size());
        uint64_t total = 0;
        int files = 0;
        for (size_t i = 0; i < tiers.size(); ++i) {
            auto const& t = tiers[i];
            auto b = matjson::Value::object();
            b["band"] = static_cast<int64_t>(i + 1);
            b["min"] = static_cast<double>(t.minWindow);
            b["max"] = static_cast<double>(t.maxWindow);
            if (!t.text.empty())
                b["label"] = t.text;
            if (t.audioPath.empty()) {
                b["default"] = true;
            } else {
                auto const found = findSound(t.audioPath);
                if (!found) {
                    r.note = fmt::format("{}'s sound ({}) isn't there, so the pack wasn't made.", bandName(t, i),
                                         t.audioPath);
                    return r;
                }
                std::string const fname = utf8(found->file.filename());
                if (!soundExt(utf8(found->file.extension()))) {
                    r.note = fmt::format("{}'s sound ({}) isn't an ogg, mp3 or wav, so the pack wasn't made.",
                                         bandName(t, i), fname);
                    return r;
                }
                if (found->bundled) {
                    // Every copy of the mod has it: only its name goes in.
                    b["bundled"] = fname;
                } else {
                    std::string error;
                    auto bytes = readFile(found->file, error);
                    if (!error.empty() || bytes.empty()) {
                        r.note = fmt::format("Couldn't read {}'s sound ({}): {}.", bandName(t, i), fname,
                                             error.empty() ? "it's empty" : error);
                        return r;
                    }
                    std::string inPack = keepableName(fname);
                    if (inPack.empty())
                        inPack = fmt::format("band {}{}", i + 1, lower(extOf(fname)));
                    // One copy of a file several bands share; another file
                    // with the same name gets "name (2).ext".
                    std::string chosen;
                    for (int n = 1; n < 1000 && chosen.empty(); ++n) {
                        std::string const candidate =
                            n == 1 ? inPack
                                   : fmt::format("{} ({}){}", inPack.substr(0, inPack.size() - extOf(inPack).size()),
                                                 n, extOf(inPack));
                        auto const it = byName.find(candidate);
                        if (it == byName.end() || *it->second == bytes)
                            chosen = candidate;
                    }
                    if (!byName.contains(chosen)) {
                        total += bytes.size();
                        if (total > gucci::bundle::kMaxTotal) {
                            r.note = "The sounds come to more than 64 MB, more than a pack can hold.";
                            return r;
                        }
                        kept.push_back(std::move(bytes));
                        byName[chosen] = &kept.back();
                        entries.push_back({std::string(kSoundDir) + chosen, kept.back()});
                        files++;
                    }
                    b["sound"] = std::string(kSoundDir) + chosen;
                }
            }
            bands.push(b);
        }
        auto manifest = matjson::Value::object();
        manifest["format"] = kFormat;
        manifest["version"] = kVersion;
        manifest["name"] = name;
        manifest["made_with"] = disguised() ? std::string("GBot")
                                            : "GucciBot " + Mod::get()->getVersion().toVString();
        manifest["bands"] = bands;
        auto const text = manifest.dump();
        entries[0].bytes.assign(text.begin(), text.end());

        std::string error;
        auto const zip = gucci::bundle::writeZip(entries, error);
        if (zip.empty()) {
            r.note = "Couldn't pack it: " + error;
            return r;
        }
        auto const to = packsFolder() / u8path(name + ".zip");
        if (auto err = writeWhole(to, zip); !err.empty()) {
            r.note = "Couldn't save the pack: " + err + ".";
            return r;
        }
        r.ok = true;
        r.path = utf8(to);
        r.note = fmt::format("Saved \"{}\" ({} band{}, {} sound file{}) to {}.", name, tiers.size(),
                             tiers.size() == 1 ? "" : "s", files, files == 1 ? "" : "s", r.path);
        return r;
    }

    Result importPack(fs::path const& zipPath) {
        Result r;
        std::string const fname = utf8(zipPath.filename());
        std::error_code ec;
        auto const size = fs::file_size(zipPath, ec);
        if (ec) {
            r.note = fmt::format("Couldn't read {}.", fname);
            return r;
        }
        if (size > gucci::bundle::kMaxTotal + (1u << 20)) {
            r.note = fmt::format("{} is bigger than a pack can be (64 MB).", fname);
            return r;
        }
        std::ifstream in(zipPath, std::ios::binary);
        std::vector<uint8_t> const bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.size() != size) {
            r.note = fmt::format("Couldn't read all of {}.", fname);
            return r;
        }

        std::vector<gucci::bundle::Entry> entries;
        std::string error;
        if (!gucci::bundle::readZip(bytes, entries, error)) {
            r.note = fmt::format("{} isn't a band sound pack: {}.", fname, error);
            return r;
        }
        gucci::bundle::Entry const* manifest = nullptr;
        std::map<std::string, gucci::bundle::Entry const*> files;
        for (auto const& e : entries) {
            if (e.name == kManifest)
                manifest = &e;
            else if (e.name.starts_with(kSoundDir))
                files[e.name] = &e;
        }
        if (!manifest) {
            r.note = fmt::format("{} isn't a band sound pack (there's no {} in it).", fname, kManifest);
            return r;
        }
        auto parsed = matjson::parse(std::string(manifest->bytes.begin(), manifest->bytes.end()));
        if (!parsed.isOk() || !parsed.unwrap().isObject()) {
            r.note = fmt::format("{}: its {} isn't readable.", fname, kManifest);
            return r;
        }
        auto const json = parsed.unwrap();
        if (json["format"].asString().unwrapOr("") != kFormat) {
            r.note = fmt::format("{} isn't a band sound pack.", fname);
            return r;
        }
        if (json["version"].asInt().unwrapOr(0) > kVersion) {
            r.note = fmt::format("{} was made by a newer version: update to use it.", fname);
            return r;
        }
        std::string const name = trimmed(json["name"].asString().unwrapOr(""));
        if (name.empty() || !gucci::bundle::fileNameOk(name) || name.size() > 80) {
            r.note = fmt::format("{}: the pack's name can't be used as a folder name.", fname);
            return r;
        }
        auto const& list = json["bands"];
        if (!list.isArray() || list.size() == 0 || list.size() > 64) {
            r.note = fmt::format("{} has no bands in it.", fname);
            return r;
        }

        // Every check before anything is written.
        struct Item {
            int band = 0;
            std::optional<float> min, max;
            std::string sound;    // in sounds/, or
            std::string bundled;  // a resource's name, or neither: the default clip
        };
        std::vector<Item> items;
        std::set<std::string> used;
        std::set<int> bandsSeen;
        for (size_t i = 0; i < list.size(); ++i) {
            auto const& b = list[i];
            if (!b.isObject()) {
                r.note = fmt::format("{}: band entry {} isn't readable.", fname, i + 1);
                return r;
            }
            Item it;
            it.band = static_cast<int>(b["band"].asInt().unwrapOr(0));
            if (it.band < 1 || it.band > 999 || !bandsSeen.insert(it.band).second) {
                r.note = fmt::format("{}: band entry {} has no band number, or one already used.", fname, i + 1);
                return r;
            }
            if (b["min"].isNumber() && b["max"].isNumber()) {
                it.min = static_cast<float>(b["min"].asDouble().unwrapOr(0.0));
                it.max = static_cast<float>(b["max"].asDouble().unwrapOr(0.0));
            }
            if (b.contains("sound")) {
                auto const s = b["sound"].asString().unwrapOr("");
                std::string const base = s.starts_with(kSoundDir) ? s.substr(sizeof(kSoundDir) - 1) : "";
                if (base.empty() || !gucci::bundle::fileNameOk(base) || !soundExt(extOf(base)) || !files.contains(s)) {
                    r.note = fmt::format("{}: band {}'s sound ({}) isn't in the pack, or isn't an ogg, mp3 or wav.",
                                         fname, it.band, s);
                    return r;
                }
                if (files.at(s)->bytes.empty()) {
                    r.note = fmt::format("{}: band {}'s sound ({}) is empty.", fname, it.band, base);
                    return r;
                }
                it.sound = base;
                used.insert(base);
            } else if (b.contains("bundled")) {
                auto const s = b["bundled"].asString().unwrapOr("");
                std::error_code rc;
                if (!gucci::bundle::fileNameOk(s) || !soundExt(extOf(s)) ||
                    !fs::is_regular_file(Mod::get()->getResourcesDir() / u8path(s), rc)) {
                    r.note = fmt::format("{}: band {}'s sound ({}) isn't one this version has.", fname, it.band, s);
                    return r;
                }
                it.bundled = s;
            }
            items.push_back(std::move(it));
        }

        // The sounds into bands/<name>/ in one step: written into a fresh
        // folder beside it, which then takes its place.
        // Only a folder an earlier pack import made (it holds the marker
        // file) is replaced; anything else already there under that name is
        // the user's own, so the pack goes in beside it as "name (2)".
        auto const base = folder();
        constexpr char kPackMarker[] = ".guccibot_pack";
        auto const madeByPack = [&](fs::path const& p) {
            std::error_code mc;
            return fs::is_directory(p, mc) && fs::is_regular_file(p / kPackMarker, mc);
        };
        fs::path target = base / u8path(name);
        for (int n = 2; n < 1000 && fs::exists(target, ec) && !madeByPack(target); ++n)
            target = base / u8path(fmt::format("{} ({})", name, n));
        if (fs::exists(target, ec) && !madeByPack(target)) {
            r.note = fmt::format("Couldn't bring in \"{}\": too many folders with that name in bands/.", name);
            return r;
        }
        fs::create_directories(base, ec);
        fs::path staging;
        for (int n = 0; n < 1000; ++n) {
            staging = base / fmt::format(".incoming-{}", n);
            if (!fs::exists(staging, ec))
                break;
        }
        auto const giveUp = [&](std::string why) {
            std::error_code rmEc;
            fs::remove_all(staging, rmEc);
            r.note = fmt::format("Couldn't bring in \"{}\": {}. Nothing was changed.", name, why);
            return r;
        };
        if (!used.empty()) {
            fs::create_directories(staging, ec);
            if (ec)
                return giveUp("couldn't make a folder in bands/ (" + ec.message() + ")");
            for (auto const& f : used)
                if (auto err = writeWhole(staging / u8path(f), files.at(std::string(kSoundDir) + f)->bytes);
                    !err.empty())
                    return giveUp(err);
            if (auto err = writeWhole(staging / kPackMarker, {}); !err.empty())
                return giveUp(err);
            if (fs::exists(target, ec)) {
                fs::path old = staging;
                old += "-old";
                fs::rename(target, old, ec);
                if (ec)
                    return giveUp("couldn't replace the pack already in bands/ (" + ec.message() + ")");
                fs::rename(staging, target, ec);
                if (ec) {
                    std::error_code back;
                    fs::rename(old, target, back);
                    return giveUp("couldn't put it in bands/ (" + ec.message() + ")");
                }
                std::error_code rmEc;
                fs::remove_all(old, rmEc);
            } else {
                fs::rename(staging, target, ec);
                if (ec)
                    return giveUp("couldn't put it in bands/ (" + ec.message() + ")");
            }
        }

        // The bands: the same window range first, then the same place.
        auto& tiers = SLSettings::get()->frameWindow.tiers;
        std::vector<bool> taken(tiers.size(), false);
        std::vector<int> unmatched;
        int set = 0;
        std::vector<std::pair<size_t, Item const*>> plan;
        std::vector<Item const*> left;
        for (auto const& it : items) {
            bool found = false;
            if (it.min && it.max)
                for (size_t i = 0; i < tiers.size(); ++i)
                    if (!taken[i] && tiers[i].minWindow == *it.min && tiers[i].maxWindow == *it.max) {
                        taken[i] = true;
                        plan.push_back({i, &it});
                        found = true;
                        break;
                    }
            if (!found)
                left.push_back(&it);
        }
        for (auto const* it : left) {
            size_t const i = static_cast<size_t>(it->band - 1);
            if (i < tiers.size() && !taken[i]) {
                taken[i] = true;
                plan.push_back({i, it});
            } else {
                unmatched.push_back(it->band);
            }
        }
        for (auto const& [i, it] : plan) {
            tiers[i].audioPath = !it->sound.empty()     ? bandPath(target / u8path(it->sound))
                                 : !it->bundled.empty() ? it->bundled
                                                        : std::string();
            set++;
        }
        fwstore::save();
        FrameWindowSound::clearCache();
        forgetListed();
        g_packName = name;
        r.ok = true;
        r.path = utf8(target);
        r.note = fmt::format("\"{}\" is in: sounds for {} band{}.", name, set, set == 1 ? "" : "s");
        if (!unmatched.empty()) {
            std::string list;
            for (size_t i = 0; i < unmatched.size(); ++i)
                list += (i ? ", " : "") + std::to_string(unmatched[i]);
            r.note += fmt::format(" The pack has more bands than you do: its band{} {} had nowhere to go.",
                                  unmatched.size() == 1 ? "" : "s", list);
        }
        return r;
    }

    std::string& packName() { return g_packName; }

    void pickFile(Kind kind, int bandId) { startPick(Want::File, kind, bandId); }
    void pickFolder() { startPick(Want::Folder, Kind::Sound, 0); }
    void pickPack() { startPick(Want::Pack, Kind::Sound, 0); }
    bool picking() { return s_pending; }

    std::string const& note() { return g_note; }
    bool noteOk() { return g_noteOk; }
    void setNote(bool ok, std::string text) {
        g_noteOk = ok;
        g_note = std::move(text);
    }

    bool poll() {
        if (!s_picked.ready)
            return false;
        Picked got = std::move(s_picked);
        s_picked = Picked{};
        if (!got.error.empty()) {
#if defined(GEODE_IS_ANDROID)
            // Geode's launcher sends this same "Permission error" when the
            // picker is cancelled, when the file has no real path, and when
            // all-files access is refused (Import a macro's note).
            setNote(false, fmt::format("No file came back from the picker ({}). If you chose one, Android didn't let "
                                       "the mod open it. Copy your files into {} instead, and pick them from the "
                                       "list under Sound or Image.",
                                       got.error, utf8(folder())));
#else
            setNote(false, fmt::format("The file picker failed: {}\nYou can also copy files into {} and pick them "
                                       "from the list under Sound or Image.",
                                       got.error, utf8(folder())));
#endif
            return false;
        }
        if (!got.path)
            return false;  // cancelled
        try {
            switch (got.what) {
                case Want::File: {
                    auto const res = chooseFile(*got.path, got.kind, got.bandId);
                    setNote(res.ok, res.note);
                    return res.ok;
                }
                case Want::Folder: {
                    auto const res = importFolder(*got.path);
                    setNote(res.ok, res.note);
                    break;
                }
                case Want::Pack: {
                    auto const res = importPack(*got.path);
                    setNote(res.ok, res.note);
                    return res.ok;
                }
                default:
                    break;
            }
        } catch (std::exception const& e) {
            setNote(false, fmt::format("Couldn't bring it in: {}", e.what()));
        }
        return false;
    }

} // namespace gucci::ui::bandfiles
