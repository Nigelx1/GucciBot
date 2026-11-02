// The random teleport's pick (src/core/teleport_pick.hpp), outside the game.
// Run by run_tests.bat. The expected numbers were worked out separately from
// GD's instructions (teleportPlayer, 2.2081 Windows), not with this code.
#include "core/teleport_pick.hpp"

#include <cstdint>
#include <cstdio>

using namespace gucci::teleportpick;

static int fails = 0;
#define CHECK(c)                                             \
    do {                                                     \
        if (!(c)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            fails++;                                         \
        }                                                    \
    } while (0)

// Eight draws from an attempt seed, the way an attempt starts the state
// (hook_playlayer.cpp: the seed's low 15 bits).
static bool drawsAre(uint64_t seed, const int (&want)[8]) {
    uint64_t state = seed & 0x7FFF;
    for (int w : want) {
        state = nextState(state);
        if (state != static_cast<uint64_t>(w))
            return false;
    }
    return true;
}

int main() {
    int cases = 0;

    // the draws, from a few seeds
    {
        cases++;
        CHECK(drawsAre(0, {38, 162, 567, 1890, 6210, 20317, 849, 2811}));
        CHECK(drawsAre(1, {41, 172, 600, 1997, 6559, 21457, 4572, 14968}));
        CHECK(drawsAre(12345, {7584, 24804, 15502, 17893, 25701, 18431, 27458, 24168}));
        CHECK(drawsAre(32767, {8737, 28569, 27796, 25272, 17030, 22883, 9228, 30173}));
    }
    // a seed above 15 bits only ever reaches the state through its low 15
    {
        cases++;
        CHECK(drawsAre(12345 + (uint64_t{1} << 15) + (uint64_t{1} << 40),
                       {7584, 24804, 15502, 17893, 25701, 18431, 27458, 24168}));
    }
    // a drawn value never leaves 0..32767
    {
        cases++;
        bool inRange = true;
        uint64_t state = 0;
        for (int i = 0; i < 100000; i++) {
            state = nextState(state);
            inRange &= state <= 0x7FFF;
        }
        CHECK(inRange);
    }
    // where a two- and a three-member group split
    {
        cases++;
        CHECK(memberIndex(2, 0) == 0);
        CHECK(memberIndex(2, 16383) == 0);
        CHECK(memberIndex(2, 16384) == 1);
        CHECK(memberIndex(3, 1) == 0);
        CHECK(memberIndex(3, 10922) == 0);
        CHECK(memberIndex(3, 10923) == 1);
        CHECK(memberIndex(3, 21845) == 2);
        CHECK(memberIndex(3, 21846) == 2);
    }
    // 32767 makes f exactly 1, which GD turns into 0: the first member
    {
        cases++;
        CHECK(memberIndex(2, 32767) == 0);
        CHECK(memberIndex(5, 32767) == 0);
    }
    // the largest draw below that stays inside the group, big groups too
    {
        cases++;
        CHECK(memberIndex(7, 32766) == 6);
        CHECK(memberIndex(100, 32766) == 99);
        CHECK(memberIndex(9999, 32766) == 9998);
        CHECK(memberIndex(16384, 32766) == 16383);
        CHECK(memberIndex(40000, 32766) == 39998);
        CHECK(memberIndex(100000, 32766) == 99996);
    }
    // every draw picks a member that exists
    {
        cases++;
        bool inGroup = true;
        int const counts[] = {2, 3, 7, 100, 9999, 16777216};
        for (int count : counts)
            for (int r = 0; r <= 32767; r++) {
                int const m = memberIndex(count, r);
                inGroup &= m >= 0 && m < count;
            }
        CHECK(inGroup);
    }
    // a run of picks from one seed, as an attempt would make them
    {
        cases++;
        int const want3[10] = {0, 2, 1, 1, 2, 1, 2, 2, 1, 1};
        int const want10[10] = {2, 7, 4, 5, 7, 5, 8, 7, 4, 3};
        uint64_t s3 = 12345 & 0x7FFF, s10 = 12345 & 0x7FFF;
        bool same = true;
        for (int i = 0; i < 10; i++) {
            s3 = nextState(s3);
            s10 = nextState(s10);
            same &= memberIndex(3, static_cast<int>(s3)) == want3[i];
            same &= memberIndex(10, static_cast<int>(s10)) == want10[i];
        }
        CHECK(same);
    }

    std::printf(fails ? "teleport pick: %d case(s), %d FAILED\n" : "teleport pick: %d cases, all passed\n", cases,
                fails);
    return fails ? 1 : 0;
}
