/**
 * TestRandomMapper.cpp — Unit tests for RandomMapper (standalone, no BDS deps).
 *
 * Compile and run:
 *   xmake f -p windows -a x64 -m debug
 *   xmake run PersonalizedTest
 *
 * Or standalone:
 *   cl /EHsc /std:c++20 /I../src tests/TestRandomMapper.cpp ../src/Utils/RandomMapper.cpp
 */

#include "Utils/RandomMapper.h"
#include <cstdio>
#include <cassert>
#include <vector>
#include <algorithm>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) printf("  TEST: %-50s", name);
#define PASS() do { printf("PASS\n"); ++g_pass; } while(0)
#define FAIL(msg) do { printf("FAIL — %s\n", msg); ++g_fail; } while(0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while(0)

void test_fisher_yates_bijective() {
    TEST("Fisher-Yates is bijective (N=100)");
    constexpr size_t N = 100;
    auto perm = personalized::RandomMapper::fisherYatesShuffle(42, N, 1);

    if (perm.size() != N) { FAIL("wrong size"); return; }

    // Check every value in [0, N) appears exactly once
    std::vector<int> seen(N, 0);
    for (size_t i = 0; i < N; ++i) {
        if (perm[i] >= N) { FAIL("out of range"); return; }
        seen[perm[i]]++;
    }
    for (size_t i = 0; i < N; ++i) {
        if (seen[i] != 1) { FAIL("not bijective"); return; }
    }
    PASS();
}

void test_fisher_yates_deterministic() {
    TEST("Fisher-Yates is deterministic (same seed → same result)");
    auto a = personalized::RandomMapper::fisherYatesShuffle(12345, 50, 1);
    auto b = personalized::RandomMapper::fisherYatesShuffle(12345, 50, 1);

    if (a != b) { FAIL("different results for same seed"); return; }
    PASS();
}

void test_fisher_yates_different_seeds() {
    TEST("Fisher-Yates differs across seeds");
    auto a = personalized::RandomMapper::fisherYatesShuffle(111, 50, 1);
    auto b = personalized::RandomMapper::fisherYatesShuffle(222, 50, 1);

    // Extremely unlikely to be identical for different seeds
    if (a == b) { FAIL("identical for different seeds (astronomically unlikely)"); return; }
    PASS();
}

void test_partial_scramble_identity() {
    TEST("PartialScramble(0.0) = identity");
    auto perm = personalized::RandomMapper::partialScramble(42, 20, 0.0, 1);

    for (size_t i = 0; i < 20; ++i) {
        if (perm[i] != i) { FAIL("not identity at intensity 0"); return; }
    }
    PASS();
}

void test_partial_scramble_full() {
    TEST("PartialScramble(1.0) ≈ full shuffle (no fixed points likely)");
    auto perm = personalized::RandomMapper::partialScramble(42, 100, 1.0, 3);

    int fixedPoints = 0;
    for (size_t i = 0; i < 100; ++i) {
        if (perm[i] == i) ++fixedPoints;
    }

    // For a full shuffle of 100 items, expected fixed points ≈ 1
    if (fixedPoints > 10) { FAIL("too many fixed points for full shuffle"); return; }
    PASS();
}

void test_feistel_bijective() {
    TEST("Feistel FPE is bijective (N=97)");
    constexpr size_t N = 97;  // Prime to test non-power-of-2
    personalized::RandomMapper mapper(9999, N);

    std::vector<int> seen(N, 0);
    for (size_t i = 0; i < N; ++i) {
        size_t mapped = mapper.map(i);
        if (mapped >= N) {
            FAIL("map out of range");
            return;
        }
        seen[mapped]++;
    }

    for (size_t i = 0; i < N; ++i) {
        if (seen[i] != 1) { FAIL("not bijective"); return; }
    }
    PASS();
}

void test_feistel_inverse() {
    TEST("Feistel encrypt/decrypt are inverses");
    constexpr size_t N = 200;
    personalized::RandomMapper mapper(7777, N);

    for (size_t i = 0; i < N; ++i) {
        size_t enc = mapper.map(i);
        size_t dec = mapper.unmap(enc);
        if (dec != i) {
            FAIL("encrypt/decrypt mismatch");
            return;
        }
    }
    PASS();
}

int main() {
    printf("══════════════════════════════════════════\n");
    printf("  RandomMapper Test Suite\n");
    printf("══════════════════════════════════════════\n\n");

    test_fisher_yates_bijective();
    test_fisher_yates_deterministic();
    test_fisher_yates_different_seeds();
    test_partial_scramble_identity();
    test_partial_scramble_full();
    test_feistel_bijective();
    test_feistel_inverse();

    printf("\n────────────────────────────────────────────\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("────────────────────────────────────────────\n");

    return g_fail > 0 ? 1 : 0;
}
