// Which GD a release's file is for (src/core/update_files.hpp), outside the
// game: the update checker announces a release only when it has a file for
// the running GD. Run by run_tests.bat. The file names are the real ones on
// GitHub (v2.0.0 and everything before it) plus a few the next releases
// might use.
#include "core/update_files.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace gucci::updatefiles;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

int main() {
    int cases = 0;

    // v2.0.0's two files: each GD finds its own, and only its own
    {
        cases++;
        CHECK(fileIsFor("nigelx1.guccibot2208.geode", kGd2081));
        CHECK(!fileIsFor("nigelx1.guccibot2208.geode", kGd2091));
        CHECK(fileIsFor("nigelx1.guccibot2209.geode", kGd2091));
        CHECK(!fileIsFor("nigelx1.guccibot2209.geode", kGd2081));
        std::vector<std::string> const v200{"nigelx1.guccibot2208.geode", "nigelx1.guccibot2209.geode"};
        CHECK(releaseHasFileFor(v200, kGd2081));
        CHECK(releaseHasFileFor(v200, kGd2091));
    }
    // every release before 2.0.0 had one plain file: it names no GD
    {
        cases++;
        std::vector<std::string> const old{"nigelx1.guccibot.geode"};
        CHECK(!releaseHasFileFor(old, kGd2081));
        CHECK(!releaseHasFileFor(old, kGd2091));
    }
    // a 2.209-only release: the 2.2081 build doesn't see it, the 2.2091 build does
    {
        cases++;
        std::vector<std::string> const only2209{"nigelx1.guccibot2209.geode"};
        CHECK(!releaseHasFileFor(only2209, kGd2081));
        CHECK(releaseHasFileFor(only2209, kGd2091));
    }
    // no files at all, and files that aren't .geode
    {
        cases++;
        CHECK(!releaseHasFileFor({}, kGd2081));
        CHECK(!releaseHasFileFor({}, kGd2091));
        CHECK(!fileIsFor("nigelx1.guccibot2209.zip", kGd2091));
        CHECK(!fileIsFor("nigelx1.guccibot2209.geode.sha256", kGd2091));
        CHECK(!fileIsFor("2209.txt", kGd2091));
        CHECK(!fileIsFor(".geode", kGd2091));
        CHECK(!fileIsFor("", kGd2081));
        std::vector<std::string> const extras{"Source code.zip", "checksums.txt", "nigelx1.guccibot2209.geode"};
        CHECK(releaseHasFileFor(extras, kGd2091));
        CHECK(!releaseHasFileFor(extras, kGd2081));
    }
    // other spellings a later release might use: case, a dash, the dotted GD version
    {
        cases++;
        CHECK(fileIsFor("NIGELX1.GUCCIBOT2209.GEODE", kGd2091));
        CHECK(fileIsFor("GucciBot-2.0.1-2208.geode", kGd2081));
        CHECK(!fileIsFor("GucciBot-2.0.1-2208.geode", kGd2091));
        CHECK(fileIsFor("nigelx1.guccibot-gd2.2091.geode", kGd2091));
        CHECK(!fileIsFor("nigelx1.guccibot-gd2.2091.geode", kGd2081));
        CHECK(fileIsFor("nigelx1.guccibot-gd2.2081.geode", kGd2081));
        CHECK(fileIsFor("nigelx1.guccibot-gd22091.geode", kGd2091));
        CHECK(!fileIsFor("nigelx1.guccibot-gd22091.geode", kGd2081));
    }

    std::printf(fails ? "update files: %d case(s), %d FAILED\n" : "update files: %d cases, all passed\n", cases,
                fails);
    return fails ? 1 : 0;
}
