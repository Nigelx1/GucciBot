#pragma once

// Moving last launch's log aside (guccibot_mcp.log, guccibot_fw.log) without
// losing a log another game is still writing. Two games can share one save
// folder (a test copy, or a restart whose old process hasn't quit yet).
//
// - Windows: an open log can't be renamed (the C runtime opens files without
//   FILE_SHARE_DELETE), so the rename itself fails while another game holds it,
//   and that game's lines are safe.
// - macOS, iOS, Android: rename moves an open file, and the next rotation would
//   then replace it, so a running game's later lines went nowhere. Every game
//   holds a shared lock on guccibot_logs.lock for its whole launch, and a log
//   is only moved under the exclusive lock, which it gets only when no other
//   game holds the shared one. (flock turns exclusive back into shared in two
//   steps; another game asking in exactly that moment could still think it's
//   alone. If the lock file can't be made or locked, the log is moved as before.)

#include <filesystem>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <mutex>
#endif

namespace gucci::logrotate {

    // Empty when `path` was moved to `old` (or there was nothing to move);
    // otherwise why it wasn't, for the banner and Geode's log.
    inline std::string rotate(std::filesystem::path const& path, std::filesystem::path const& old) {
        std::error_code ec;
        bool const exists = std::filesystem::exists(path, ec);
#ifndef _WIN32
        static std::mutex lockMutex;
        static int lockFd = -2;
        std::lock_guard guard(lockMutex);
        if (lockFd == -2) {
            auto const lockPath = (path.parent_path() / "guccibot_logs.lock").string();
            lockFd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        }
        if (lockFd >= 0) {
            if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
                int const err = errno;
                // Keep (or take) this game's shared hold either way.
                ::flock(lockFd, LOCK_SH);
                if (err == EWOULDBLOCK)
                    return exists ? "another game is using this save folder" : std::string();
                // Locking doesn't work here: move it as before.
            } else {
                if (exists)
                    std::filesystem::rename(path, old, ec);  // replaces an existing old.log
                ::flock(lockFd, LOCK_SH);
                return exists && ec ? ec.message() : std::string();
            }
        }
#endif
        if (!exists)
            return {};
        // rename replaces an existing old.log (MSVC: MoveFileExW with
        // MOVEFILE_REPLACE_EXISTING; POSIX rename does too). Removing it first
        // lost it whenever the rename then failed.
        std::filesystem::rename(path, old, ec);
        return ec ? ec.message() : std::string();
    }

    // The line put at the top of this launch's lines when the last log
    // couldn't be moved aside, so it's appended to instead.
    inline std::string banner(std::string const& why) {
        return "\n===== new launch (the last log couldn't be moved aside: " + why +
               "; appending to it) =====\n";
    }
}
