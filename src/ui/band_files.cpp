// See band_files.hpp. Written fresh for GucciBot 2.0 (2026-11-16) from
// today's Calculate code; the picker is Import a macro's (ui/shell.cpp), the
// size limit is Share my windows' (tools/share_bundle).

#include "ui/band_files.hpp"

#include "analysis/ac/framewindow.hpp"
#include "analysis/ac/fw_settings_store.hpp"
#include "analysis/ac/sound.hpp"
#include "core/GucciBot.hpp"
#include "tools/share_bundle.hpp"
#include "ui/ui.hpp"

#include <Geode/Geode.hpp>

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

        // ---- the card's state

        std::string g_note;
        bool g_noteOk = true;

        enum class Want { None, File, Folder };
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
            auto const dir = folder();
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

    void pickFile(Kind kind, int bandId) { startPick(Want::File, kind, bandId); }
    void pickFolder() { startPick(Want::Folder, Kind::Sound, 0); }
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
                default:
                    break;
            }
        } catch (std::exception const& e) {
            setNote(false, fmt::format("Couldn't bring it in: {}", e.what()));
        }
        return false;
    }

} // namespace gucci::ui::bandfiles
