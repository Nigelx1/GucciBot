// The queues' pure parts (src/tools/queue_core.hpp), outside the game: the
// level IDs the Pathfinder queue's box takes, the macro names it saves under,
// and the saved state a restart reads back. Run by run_tests.bat.
#include "tools/queue_core.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace gucci::queue;

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

    // IDs split by spaces, commas, semicolons, tabs and lines, in order.
    cases++;
    {
        std::vector<std::string> bad;
        auto ids = parseLevelIds("1, 2 3\n10565740;\r\n 4\t5", &bad);
        CHECK((ids == std::vector<int>{1, 2, 3, 10565740, 4, 5}));
        CHECK(bad.empty());
    }

    // A repeat is dropped (the first stays), 0 is dropped quietly, "#123"
    // counts, and anything that isn't a number goes to bad.
    cases++;
    {
        std::vector<std::string> bad;
        auto ids = parseLevelIds("3 #44 3 0 abc 12x -5 99999999999", &bad);
        CHECK((ids == std::vector<int>{3, 44}));
        CHECK((bad == std::vector<std::string>{"abc", "12x", "-5", "99999999999"}));
        CHECK(parseLevelIds("").empty());
        CHECK(parseLevelIds(" ,,; \n").empty());
    }

    // RobTop's levels.
    cases++;
    CHECK(isMainLevel(1) && isMainLevel(22) && isMainLevel(5001) && isMainLevel(5004));
    CHECK(!isMainLevel(0) && !isMainLevel(23) && !isMainLevel(5000) && !isMainLevel(5005) && !isMainLevel(128));

    // Macro names: the level's name made safe, with its ID; and back.
    cases++;
    CHECK(macroNameFor("Bloodbath", 10565740) == "Bloodbath (10565740)");
    CHECK(macroNameFor(" a/b:c? ", 7) == "abc (7)");
    CHECK(macroNameFor("dots...", 7) == "dots (7)");
    CHECK(macroNameFor("", 9) == "Level (9)");
    CHECK(idFromMacroName("Bloodbath (10565740)") == 10565740);
    CHECK(idFromMacroName("Bloodbath (10565740) furthest") == 10565740);
    CHECK(idFromMacroName("Bloodbath (10565740) 2") == 10565740);
    CHECK(idFromMacroName("Stereo Madness (1)") == 1);
    CHECK(idFromMacroName("Bloodbath") == 0);
    CHECK(idFromMacroName("my run (v2)") == 0);
    CHECK(idFromMacroName("x (12) copy") == 0);
    CHECK(idFromMacroName("x ()") == 0);

    // The saved state round-trips, fields with tabs and lines cleaned.
    cases++;
    {
        State st;
        st.kind = Kind::Pathfinder;
        st.limitMinutes = 45;
        st.countWindows = false;
        st.running = true;
        Item a;
        a.levelId = 1;
        a.levelName = "Stereo\tMadness";
        a.status = Status::Beaten;
        a.tries = 1;
        a.bestPercent = 100;
        a.seconds = 75.5;
        a.macro = "Stereo Madness (1)";
        a.windows = 31;
        a.lstar = 1.234;
        a.note = "line one\nline two";
        Item b;
        b.levelId = 99;
        b.status = Status::Failed;
        b.note = "Couldn't download it";
        Item c;
        c.levelId = 2;
        st.items = {a, b, c};
        State back;
        CHECK(parse(serialize(st), back));
        CHECK(back.kind == Kind::Pathfinder && back.limitMinutes == 45 && !back.countWindows && back.running);
        CHECK(back.items.size() == 3);
        if (back.items.size() == 3) {
            CHECK(back.items[0].levelName == "Stereo Madness");
            CHECK(back.items[0].status == Status::Beaten && back.items[0].tries == 1);
            CHECK(back.items[0].bestPercent == 100 && back.items[0].seconds == 75.5);
            CHECK(back.items[0].macro == "Stereo Madness (1)" && back.items[0].windows == 31);
            CHECK(back.items[0].lstar > 1.2339 && back.items[0].lstar < 1.2341);
            CHECK(back.items[0].note == "line one line two");
            CHECK(back.items[1].status == Status::Failed && back.items[1].note == "Couldn't download it");
            CHECK(back.items[2].status == Status::Pending && back.items[2].windows == -1 && back.items[2].lstar == -1);
        }
        CHECK(back.left() == 1);
        // A frame-window queue keeps its kind; Windows line endings read too.
        State fw;
        fw.kind = Kind::FrameWindows;
        Item m;
        m.macro = "my macro";
        fw.items = {m};
        std::string text = serialize(fw);
        std::string crlf;
        for (char ch : text) {
            if (ch == '\n')
                crlf += '\r';
            crlf += ch;
        }
        State fwBack;
        CHECK(parse(crlf, fwBack));
        CHECK(fwBack.kind == Kind::FrameWindows && fwBack.items.size() == 1 && fwBack.items[0].macro == "my macro");
        // Not a queue file: left alone.
        State keep;
        keep.limitMinutes = 7;
        CHECK(!parse("hello\nworld", keep) && keep.limitMinutes == 7);
        CHECK(!parse("", keep));
    }

    // After a restart: a level that was running gets one more try, then is
    // passed over.
    cases++;
    {
        State st;
        Item a, b, c;
        a.status = Status::Running;
        a.tries = 1;
        b.status = Status::Running;
        b.tries = 2;
        c.status = Status::Pending;
        st.items = {a, b, c};
        CHECK(afterRestart(st) == 2);
        CHECK(st.items[0].status == Status::Pending);
        CHECK(st.items[1].status == Status::Failed);
        CHECK(st.items[2].status == Status::Pending);
    }

    // Times.
    cases++;
    CHECK(formatSeconds(12.2) == "12 s");
    CHECK(formatSeconds(245) == "4 min 05 s");
    CHECK(formatSeconds(3720) == "1 h 02 min");
    CHECK(formatSeconds(-3) == "0 s");

    std::printf("queue: %d cases, %d failures\n", cases, fails);
    return fails ? 1 : 0;
}
