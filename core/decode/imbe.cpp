// P25 Phase 1 IMBE vocoder, decode side. See core/decode/imbe.h for the
// specification reference, the clean-room record and the five places where
// TIA-102.BABA contradicts itself.
//
// The six annex tables in the first block below were extracted from the
// standard's own PDF text layer by script, not retyped, and each was
// cross-checked before it landed here. The checks are reproduced in
// tests/decode/test_imbe.cpp.

#include "core/decode/imbe.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <format>
#include <limits>
#include <numbers>
#include <utility>

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

// -------------------------------------------------------------------------
// Annex E, gain quantizer levels, indexed by b2.
constexpr std::array<double, 64> kGainLevels = {
    -2.842205, -2.694235, -2.558260, -2.382850,
    -2.221042, -2.095574, -1.980845, -1.836058,
    -1.645556, -1.417658, -1.261301, -1.125631,
    -0.958207, -0.781591, -0.555837, -0.346976,
    -0.147249, 0.027755, 0.211495, 0.388380,
    0.552873, 0.737223, 0.932197, 1.139032,
    1.320955, 1.483433, 1.648297, 1.801447,
    1.942731, 2.118613, 2.321486, 2.504443,
    2.653909, 2.780654, 2.925355, 3.076390,
    3.220825, 3.402869, 3.585096, 3.784606,
    3.955521, 4.155636, 4.314009, 4.444150,
    4.577542, 4.735552, 4.909493, 5.085264,
    5.254767, 5.411894, 5.568094, 5.738523,
    5.919215, 6.087701, 6.280685, 6.464201,
    6.647736, 6.834672, 7.022583, 7.211777,
    7.471016, 7.738948, 8.124863, 8.695827,
};

// Annex F, bit allocation for the transformed gain vector, rows L = 9..56,
// columns b3..b7.
constexpr std::array<std::array<std::uint8_t, 5>, 48> kGainBits = {{
    {{10, 9, 9, 9, 9}},  // L = 9
    {{9, 9, 8, 8, 8}},  // L = 10
    {{8, 8, 8, 7, 7}},  // L = 11
    {{8, 7, 7, 7, 7}},  // L = 12
    {{7, 7, 7, 6, 6}},  // L = 13
    {{7, 6, 6, 6, 6}},  // L = 14
    {{7, 6, 6, 6, 5}},  // L = 15
    {{6, 6, 6, 5, 5}},  // L = 16
    {{6, 6, 5, 5, 5}},  // L = 17
    {{6, 5, 5, 5, 5}},  // L = 18
    {{6, 5, 5, 4, 4}},  // L = 19
    {{6, 5, 5, 4, 4}},  // L = 20
    {{5, 5, 5, 4, 4}},  // L = 21
    {{5, 5, 4, 4, 4}},  // L = 22
    {{5, 4, 4, 4, 4}},  // L = 23
    {{5, 4, 4, 4, 4}},  // L = 24
    {{5, 4, 4, 4, 3}},  // L = 25
    {{5, 4, 4, 3, 3}},  // L = 26
    {{5, 4, 4, 3, 3}},  // L = 27
    {{4, 4, 4, 3, 3}},  // L = 28
    {{4, 4, 4, 3, 3}},  // L = 29
    {{4, 4, 4, 3, 3}},  // L = 30
    {{4, 4, 3, 3, 3}},  // L = 31
    {{4, 4, 3, 3, 3}},  // L = 32
    {{4, 3, 3, 3, 3}},  // L = 33
    {{4, 3, 3, 3, 3}},  // L = 34
    {{4, 3, 3, 3, 3}},  // L = 35
    {{4, 3, 3, 3, 3}},  // L = 36
    {{4, 3, 3, 3, 2}},  // L = 37
    {{4, 3, 3, 3, 2}},  // L = 38
    {{4, 3, 3, 3, 2}},  // L = 39
    {{4, 3, 3, 3, 2}},  // L = 40
    {{4, 3, 3, 2, 2}},  // L = 41
    {{4, 3, 3, 2, 2}},  // L = 42
    {{4, 3, 3, 2, 2}},  // L = 43
    {{4, 3, 3, 2, 2}},  // L = 44
    {{4, 3, 3, 2, 2}},  // L = 45
    {{3, 3, 3, 2, 2}},  // L = 46
    {{3, 3, 3, 2, 2}},  // L = 47
    {{3, 3, 3, 2, 2}},  // L = 48
    {{3, 3, 3, 2, 2}},  // L = 49
    {{3, 3, 3, 2, 2}},  // L = 50
    {{3, 3, 3, 2, 2}},  // L = 51
    {{3, 3, 2, 2, 2}},  // L = 52
    {{3, 3, 2, 2, 2}},  // L = 53
    {{3, 3, 2, 2, 2}},  // L = 54
    {{3, 3, 2, 2, 2}},  // L = 55
    {{3, 3, 2, 2, 2}},  // L = 56
}};

// Annex F, quantizer step size for the transformed gain vector.
constexpr std::array<std::array<double, 5>, 48> kGainSteps = {{
    {{0.003100, 0.004020, 0.003360, 0.002900, 0.002640}},  // L = 9
    {{0.006200, 0.004020, 0.006720, 0.005800, 0.005280}},  // L = 10
    {{0.012400, 0.008040, 0.006720, 0.011600, 0.010560}},  // L = 11
    {{0.012400, 0.016080, 0.013440, 0.011600, 0.010560}},  // L = 12
    {{0.024800, 0.016080, 0.013440, 0.021750, 0.019800}},  // L = 13
    {{0.024800, 0.030150, 0.025200, 0.021750, 0.019800}},  // L = 14
    {{0.024800, 0.030150, 0.025200, 0.021750, 0.036960}},  // L = 15
    {{0.046500, 0.030150, 0.025200, 0.040600, 0.036960}},  // L = 16
    {{0.046500, 0.030150, 0.047040, 0.040600, 0.036960}},  // L = 17
    {{0.046500, 0.056280, 0.047040, 0.040600, 0.036960}},  // L = 18
    {{0.046500, 0.056280, 0.047040, 0.058000, 0.052800}},  // L = 19
    {{0.046500, 0.056280, 0.047040, 0.058000, 0.052800}},  // L = 20
    {{0.086800, 0.056280, 0.047040, 0.058000, 0.052800}},  // L = 21
    {{0.086800, 0.056280, 0.067200, 0.058000, 0.052800}},  // L = 22
    {{0.086800, 0.080400, 0.067200, 0.058000, 0.052800}},  // L = 23
    {{0.086800, 0.080400, 0.067200, 0.058000, 0.052800}},  // L = 24
    {{0.086800, 0.080400, 0.067200, 0.058000, 0.085800}},  // L = 25
    {{0.086800, 0.080400, 0.067200, 0.094250, 0.085800}},  // L = 26
    {{0.086800, 0.080400, 0.067200, 0.094250, 0.085800}},  // L = 27
    {{0.124000, 0.080400, 0.067200, 0.094250, 0.085800}},  // L = 28
    {{0.124000, 0.080400, 0.067200, 0.094250, 0.085800}},  // L = 29
    {{0.124000, 0.080400, 0.067200, 0.094250, 0.085800}},  // L = 30
    {{0.124000, 0.080400, 0.109200, 0.094250, 0.085800}},  // L = 31
    {{0.124000, 0.080400, 0.109200, 0.094250, 0.085800}},  // L = 32
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.085800}},  // L = 33
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.085800}},  // L = 34
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.085800}},  // L = 35
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.085800}},  // L = 36
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.112200}},  // L = 37
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.112200}},  // L = 38
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.112200}},  // L = 39
    {{0.124000, 0.130650, 0.109200, 0.094250, 0.112200}},  // L = 40
    {{0.124000, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 41
    {{0.124000, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 42
    {{0.124000, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 43
    {{0.124000, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 44
    {{0.124000, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 45
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 46
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 47
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 48
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 49
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 50
    {{0.201500, 0.130650, 0.109200, 0.123250, 0.112200}},  // L = 51
    {{0.201500, 0.130650, 0.142800, 0.123250, 0.112200}},  // L = 52
    {{0.201500, 0.130650, 0.142800, 0.123250, 0.112200}},  // L = 53
    {{0.201500, 0.130650, 0.142800, 0.123250, 0.112200}},  // L = 54
    {{0.201500, 0.130650, 0.142800, 0.123250, 0.112200}},  // L = 55
    {{0.201500, 0.130650, 0.142800, 0.123250, 0.112200}},  // L = 56
}};

// Annex G, bit allocation for the higher order DCT coefficients, rows
// L = 9..56, entries b8..b(L+1), packed end to end.
constexpr std::array<std::uint16_t, 48> kHocOffset = {
    0, 3, 7, 12, 18, 25, 33, 42, 52, 63, 75, 88,
    102, 117, 133, 150, 168, 187, 207, 228, 250, 273, 297, 322,
    348, 375, 403, 432, 462, 493, 525, 558, 592, 627, 663, 700,
    738, 777, 817, 858, 900, 943, 987, 1032, 1078, 1125, 1173, 1222,
};

constexpr std::array<std::uint8_t, 1272> kHocBits = {
    9, 8, 7,  // L = 9
    9, 7, 6, 5,  // L = 10
    9, 7, 6, 5, 4,  // L = 11
    8, 7, 6, 5, 4, 3,  // L = 12
    7, 7, 6, 5, 4, 3, 3,  // L = 13
    7, 7, 5, 4, 4, 3, 4, 3,  // L = 14
    6, 7, 5, 4, 4, 3, 3, 3, 3,  // L = 15
    6, 6, 5, 4, 4, 3, 3, 3, 3, 2,  // L = 16
    5, 5, 5, 4, 4, 4, 3, 3, 2, 3, 2,  // L = 17
    5, 4, 5, 5, 4, 3, 3, 3, 3, 2, 2, 2,  // L = 18
    5, 4, 5, 4, 4, 3, 3, 3, 3, 2, 3, 2, 1,  // L = 19
    5, 4, 5, 4, 4, 3, 3, 2, 3, 2, 1, 3, 2, 1,  // L = 20
    4, 4, 5, 4, 4, 3, 3, 2, 2, 3, 2, 1, 3, 2, 1,  // L = 21
    4, 4, 4, 4, 4, 3, 2, 3, 2, 2, 3, 2, 1, 2, 2, 1,  // L = 22
    4, 3, 4, 4, 3, 4, 3, 2, 3, 2, 2, 2, 2, 1, 2, 2, 1,  // L = 23
    4, 3, 3, 4, 3, 3, 3, 3, 2, 3, 2, 1, 2, 2, 1, 2, 2, 1,  // L = 24
    4, 3, 3, 4, 3, 3, 3, 3, 2, 3, 2, 1, 2, 2, 1, 2, 1, 1, 1,  // L = 25
    4, 3, 3, 4, 3, 3, 3, 2, 2, 3, 2, 1, 2, 2, 1, 1, 2, 2, 1, 1,  // L = 26
    4, 3, 2, 4, 3, 2, 3, 2, 2, 3, 2, 2, 1, 2, 2, 1, 1, 2, 2, 1, 1,  // L = 27
    4, 3, 2, 4, 3, 2, 3, 2, 2, 2, 3, 2, 1, 1, 2, 2, 1, 1, 2, 1, 1, 1,  // L = 28
    3, 3, 2, 4, 3, 2, 2, 3, 2, 2, 2, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1,  // L = 29
    3, 3, 2, 2, 3, 3, 2, 2, 3, 2, 2, 1, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1,  // L = 30
    3, 3, 2, 2, 3, 3, 2, 2, 3, 2, 2, 1, 2, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1,  // L = 31
    3, 3, 2, 2, 3, 3, 2, 2, 3, 2, 2, 1, 2, 2, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 0,  // L = 32
    3, 3, 2, 2, 3, 3, 2, 2, 3, 2, 1, 1, 2, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1,  // L = 33
    3, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 1, 1, 2, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 0,  // L = 34
    3, 2, 2, 2, 3, 2, 2, 2, 2, 3, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 1, 1, 1, 0, 2, 1, 1, 1, 0,  // L = 35
    3, 2, 2, 2, 1, 3, 2, 2, 2, 1, 3, 2, 1, 1, 1, 2, 2, 1, 1, 1, 2, 1, 1, 1, 0, 2, 1, 1, 1, 0,  // L = 36
    3, 2, 2, 2, 1, 3, 2, 2, 2, 2, 3, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0,  // L = 37
    3, 2, 2, 2, 1, 3, 2, 2, 2, 1, 3, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0,  // L = 38
    3, 2, 2, 2, 1, 3, 2, 2, 2, 1, 3, 2, 1, 1, 1, 2, 2, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 0, 0,  // L = 39
    3, 2, 2, 2, 1, 3, 2, 2, 1, 1, 3, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 0, 0,  // L = 40
    3, 2, 2, 1, 1, 3, 2, 2, 2, 1, 1, 3, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 0, 0,  // L = 41
    3, 2, 2, 2, 1, 1, 3, 2, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 0, 0,  // L = 42
    3, 2, 2, 2, 1, 1, 3, 2, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0,  // L = 43
    3, 2, 2, 1, 1, 1, 3, 2, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0, 2, 1, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0,  // L = 44
    3, 2, 2, 1, 1, 1, 3, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0,  // L = 45
    3, 2, 2, 1, 1, 1, 3, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0,  // L = 46
    3, 2, 2, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0, 2, 1, 1, 1, 0, 0, 0,  // L = 47
    3, 2, 2, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0,  // L = 48
    3, 2, 2, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0,  // L = 49
    3, 2, 2, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0,  // L = 50
    3, 2, 2, 1, 1, 1, 1, 3, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0,  // L = 51
    3, 2, 1, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0,  // L = 52
    3, 2, 1, 1, 1, 1, 1, 3, 2, 2, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0,  // L = 53
    3, 2, 2, 1, 1, 1, 1, 0, 3, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0,  // L = 54
    3, 2, 2, 1, 1, 1, 1, 0, 3, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 0,  // L = 55
    3, 2, 2, 1, 1, 1, 1, 0, 3, 2, 2, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 1, 1, 0, 2, 2, 1, 1, 1, 0, 0, 0, 2, 1, 1, 1, 1, 0, 0, 0, 0, 2, 1, 1, 1, 0, 0, 0, 0, 0,  // L = 56
};

// Annex J, log magnitude prediction residual block lengths.
constexpr std::array<std::array<std::uint8_t, 6>, 48> kBlockLengths = {{
    {{1, 1, 1, 2, 2, 2}},  // L = 9
    {{1, 1, 2, 2, 2, 2}},  // L = 10
    {{1, 2, 2, 2, 2, 2}},  // L = 11
    {{2, 2, 2, 2, 2, 2}},  // L = 12
    {{2, 2, 2, 2, 2, 3}},  // L = 13
    {{2, 2, 2, 2, 3, 3}},  // L = 14
    {{2, 2, 2, 3, 3, 3}},  // L = 15
    {{2, 2, 3, 3, 3, 3}},  // L = 16
    {{2, 3, 3, 3, 3, 3}},  // L = 17
    {{3, 3, 3, 3, 3, 3}},  // L = 18
    {{3, 3, 3, 3, 3, 4}},  // L = 19
    {{3, 3, 3, 3, 4, 4}},  // L = 20
    {{3, 3, 3, 4, 4, 4}},  // L = 21
    {{3, 3, 4, 4, 4, 4}},  // L = 22
    {{3, 4, 4, 4, 4, 4}},  // L = 23
    {{4, 4, 4, 4, 4, 4}},  // L = 24
    {{4, 4, 4, 4, 4, 5}},  // L = 25
    {{4, 4, 4, 4, 5, 5}},  // L = 26
    {{4, 4, 4, 5, 5, 5}},  // L = 27
    {{4, 4, 5, 5, 5, 5}},  // L = 28
    {{4, 5, 5, 5, 5, 5}},  // L = 29
    {{5, 5, 5, 5, 5, 5}},  // L = 30
    {{5, 5, 5, 5, 5, 6}},  // L = 31
    {{5, 5, 5, 5, 6, 6}},  // L = 32
    {{5, 5, 5, 6, 6, 6}},  // L = 33
    {{5, 5, 6, 6, 6, 6}},  // L = 34
    {{5, 6, 6, 6, 6, 6}},  // L = 35
    {{6, 6, 6, 6, 6, 6}},  // L = 36
    {{6, 6, 6, 6, 6, 7}},  // L = 37
    {{6, 6, 6, 6, 7, 7}},  // L = 38
    {{6, 6, 6, 7, 7, 7}},  // L = 39
    {{6, 6, 7, 7, 7, 7}},  // L = 40
    {{6, 7, 7, 7, 7, 7}},  // L = 41
    {{7, 7, 7, 7, 7, 7}},  // L = 42
    {{7, 7, 7, 7, 7, 8}},  // L = 43
    {{7, 7, 7, 7, 8, 8}},  // L = 44
    {{7, 7, 7, 8, 8, 8}},  // L = 45
    {{7, 7, 8, 8, 8, 8}},  // L = 46
    {{7, 8, 8, 8, 8, 8}},  // L = 47
    {{8, 8, 8, 8, 8, 8}},  // L = 48
    {{8, 8, 8, 8, 8, 9}},  // L = 49
    {{8, 8, 8, 8, 9, 9}},  // L = 50
    {{8, 8, 8, 9, 9, 9}},  // L = 51
    {{8, 8, 9, 9, 9, 9}},  // L = 52
    {{8, 9, 9, 9, 9, 9}},  // L = 53
    {{9, 9, 9, 9, 9, 9}},  // L = 54
    {{9, 9, 9, 9, 9, 10}},  // L = 55
    {{9, 9, 9, 9, 10, 10}},  // L = 56
}};

// Annex H, bit frame format. Index is the position in the 144 bit frame, two
// bits per symbol, Bit 1 before Bit 0. Value is the code vector index times 32
// plus the bit number within that vector.
constexpr std::array<std::uint16_t, 144> kFrameMap = {
    22, 53, 84, 115, 138, 161, 52, 21, 114, 83, 160, 137,
    20, 51, 82, 113, 136, 206, 50, 19, 112, 81, 205, 135,
    18, 49, 80, 111, 134, 204, 48, 17, 110, 79, 203, 133,
    16, 47, 78, 109, 132, 202, 46, 15, 108, 77, 201, 131,
    14, 45, 76, 107, 130, 200, 44, 13, 106, 75, 199, 129,
    12, 43, 74, 105, 128, 198, 42, 11, 104, 73, 197, 174,
    10, 41, 72, 103, 173, 196, 40, 9, 102, 71, 195, 172,
    8, 39, 70, 101, 171, 194, 38, 7, 100, 69, 193, 170,
    6, 37, 68, 99, 169, 192, 36, 5, 98, 67, 230, 168,
    4, 35, 66, 97, 167, 229, 34, 3, 96, 65, 228, 166,
    2, 33, 64, 142, 165, 227, 32, 1, 141, 118, 226, 164,
    0, 86, 117, 140, 163, 225, 85, 54, 139, 116, 224, 162,
};

// Annex C, pitch refinement window, n = 0..110. The annex prints n = -110..110
// and the two halves match value for value, checked over the whole table, so
// only the right half is stored. It is used for one thing: gamma_w.
constexpr std::array<double, 111> kPitchRefinementWindowHalf = {
    1.000000, 0.999774, 0.999095, 0.997966, 0.996386, 0.994358,
    0.991884, 0.988967, 0.985610, 0.981817, 0.977592, 0.972940,
    0.967866, 0.962377, 0.956477, 0.950174, 0.943474, 0.936386,
    0.928916, 0.921074, 0.912868, 0.904307, 0.895400, 0.886157,
    0.876589, 0.866705, 0.856516, 0.846033, 0.835267, 0.824231,
    0.812935, 0.801391, 0.789612, 0.777610, 0.765397, 0.752986,
    0.740390, 0.727620, 0.714692, 0.701616, 0.688406, 0.675076,
    0.661638, 0.648105, 0.634490, 0.620807, 0.607067, 0.593284,
    0.579470, 0.565639, 0.551802, 0.537971, 0.524160, 0.510379,
    0.496640, 0.482955, 0.469336, 0.455793, 0.442337, 0.428978,
    0.415727, 0.402594, 0.389588, 0.376718, 0.363994, 0.351425,
    0.339018, 0.326782, 0.314724, 0.302851, 0.291171, 0.279689,
    0.268413, 0.257347, 0.246497, 0.235869, 0.225466, 0.215294,
    0.205355, 0.195653, 0.186192, 0.176974, 0.168001, 0.159276,
    0.150799, 0.142572, 0.134596, 0.126872, 0.119398, 0.112176,
    0.105205, 0.098483, 0.092009, 0.085782, 0.079801, 0.074062,
    0.068563, 0.063303, 0.058277, 0.053482, 0.048915, 0.044573,
    0.040451, 0.036546, 0.032852, 0.029365, 0.026081, 0.022995,
    0.020102, 0.017397, 0.014873,
};

// Table 3, uniform quantizer step size for the higher order DCT coefficients,
// as a multiple of the standard deviation. Index is the bit allocation; index
// zero is unused because a zero allocation decodes to zero, equation (71).
constexpr std::array<double, 11> kStepForBits = {
    0.0, 1.2, 0.85, 0.65, 0.40, 0.28, 0.15, 0.08, 0.04, 0.02, 0.01,
};

// Table 4, standard deviation of the higher order DCT coefficients, indexed by
// k. Blocks run to ten elements at L = 56, Annex J, so k stops at ten.
constexpr std::array<double, 11> kSigma = {
    0.0, 0.0, 0.307, 0.241, 0.207, 0.190, 0.179, 0.173, 0.165, 0.170, 0.170,
};

// -------------------------------------------------------------------------
// Section 7.3, error control coding. Both generator matrices are printed in
// systematic form, g = [I | P], with the left most bit the MSB. Only P is
// stored: the identity part is the data itself. Each row below is one row of
// the printed matrix, MSB first.

constexpr std::array<std::uint16_t, 12> kGolayParity = {
    0b11000111010, 0b01100011101, 0b11110110100, 0b01111011010,
    0b00111101101, 0b11011001100, 0b01101100110, 0b00110110011,
    0b11011100011, 0b10101001011, 0b10010011111, 0b10001110101,
};

constexpr std::array<std::uint8_t, 11> kHammingParity = {
    0b1111, 0b1110, 0b1101, 0b1100, 0b1011, 0b1010,
    0b1001, 0b0111, 0b0110, 0b0101, 0b0011,
};

[[nodiscard]] constexpr std::uint32_t golay_parity(std::uint32_t data) {
    std::uint32_t out = 0;
    for (int row = 0; row < 12; ++row) {
        if (((data >> (11 - row)) & 1U) != 0U) {
            out ^= kGolayParity[static_cast<std::size_t>(row)];
        }
    }
    return out;
}

[[nodiscard]] constexpr std::uint32_t hamming_parity(std::uint32_t data) {
    std::uint32_t out = 0;
    for (int row = 0; row < 11; ++row) {
        if (((data >> (10 - row)) & 1U) != 0U) {
            out ^= kHammingParity[static_cast<std::size_t>(row)];
        }
    }
    return out;
}

constexpr std::uint32_t kUnset = 0xFFFFFFFFU;

// Both codes are perfect, so a syndrome table is not an approximation: every
// one of the 2^11 Golay syndromes and 2^4 Hamming syndromes is produced by
// exactly one error pattern within the code's correction radius. That is what
// the static_asserts below check, and they are the strongest available proof
// that the two generator matrices above were transcribed correctly: change one
// bit of either and the mapping stops being a bijection.
consteval std::array<std::uint32_t, 2048> make_golay_patterns() {
    std::array<std::uint32_t, 2048> table{};
    table.fill(kUnset);
    auto note = [&table](std::uint32_t pattern) constexpr {
        const std::uint32_t syndrome =
            golay_parity(pattern >> 11) ^ (pattern & 0x7FFU);
        table[syndrome] = pattern;
    };
    note(0);
    for (int a = 0; a < 23; ++a) {
        note(1U << a);
        for (int b = a + 1; b < 23; ++b) {
            note((1U << a) | (1U << b));
            for (int c = b + 1; c < 23; ++c) {
                note((1U << a) | (1U << b) | (1U << c));
            }
        }
    }
    return table;
}

consteval std::array<std::uint32_t, 16> make_hamming_patterns() {
    std::array<std::uint32_t, 16> table{};
    table.fill(kUnset);
    auto note = [&table](std::uint32_t pattern) constexpr {
        const std::uint32_t syndrome =
            hamming_parity(pattern >> 4) ^ (pattern & 0xFU);
        table[syndrome] = pattern;
    };
    note(0);
    for (int a = 0; a < 15; ++a) {
        note(1U << a);
    }
    return table;
}

constexpr auto kGolayPatterns = make_golay_patterns();
constexpr auto kHammingPatterns = make_hamming_patterns();

static_assert(std::ranges::none_of(kGolayPatterns,
                                   [](std::uint32_t e) { return e == kUnset; }),
              "the [23,12] Golay generator of section 7.3 does not give a "
              "perfect code, so it was transcribed wrongly");
static_assert(std::ranges::none_of(kHammingPatterns,
                                   [](std::uint32_t e) { return e == kUnset; }),
              "the [15,11] Hamming generator of section 7.3 does not give a "
              "perfect code, so it was transcribed wrongly");

struct Decoded {
    std::uint32_t data = 0;
    std::uint32_t corrected = 0;
};

[[nodiscard]] constexpr std::uint32_t popcount(std::uint32_t value) {
    std::uint32_t out = 0;
    while (value != 0) {
        out += value & 1U;
        value >>= 1;
    }
    return out;
}

[[nodiscard]] Decoded golay_decode(std::uint32_t received) {
    const std::uint32_t syndrome =
        golay_parity(received >> 11) ^ (received & 0x7FFU);
    const std::uint32_t pattern = kGolayPatterns[syndrome];
    return {(received ^ pattern) >> 11, popcount(pattern)};
}

[[nodiscard]] Decoded hamming_decode(std::uint32_t received) {
    const std::uint32_t syndrome =
        hamming_parity(received >> 4) ^ (received & 0xFU);
    const std::uint32_t pattern = kHammingPatterns[syndrome];
    return {(received ^ pattern) >> 4, popcount(pattern)};
}

// -------------------------------------------------------------------------
// Section 11.2, the synthesis window of Annex I. The annex prints 211 values
// and every one of them is on this trapezoid to the last printed digit,
// checked entry by entry: unity over |n| <= 55, then a linear ramp in steps of
// 0.02 to zero at |n| = 105.
[[nodiscard]] constexpr double synthesis_window(int n) {
    const int a = n < 0 ? -n : n;
    if (a <= 55) {
        return 1.0;
    }
    if (a <= 105) {
        return static_cast<double>(105 - a) * 0.02;
    }
    return 0.0;
}

[[nodiscard]] double pitch_refinement_window(int n) {
    const std::size_t a = static_cast<std::size_t>(n < 0 ? -n : n);
    return a < kPitchRefinementWindowHalf.size()
               ? kPitchRefinementWindowHalf[a]
               : 0.0;
}

// Equation (121). A function of the two fixed windows and nothing else.
[[nodiscard]] double compute_unvoiced_scaling() {
    double sum_wr = 0.0;
    double sum_wr2 = 0.0;
    for (int n = -110; n <= 110; ++n) {
        const double w = pitch_refinement_window(n);
        sum_wr += w;
        sum_wr2 += w * w;
    }
    double sum_ws2 = 0.0;
    for (int n = -104; n <= 104; ++n) {
        const double w = synthesis_window(n);
        sum_ws2 += w * w;
    }
    return sum_wr * std::sqrt(sum_ws2 / sum_wr2);
}

const double kUnvoicedScaling = compute_unvoiced_scaling();

// e^(-j 2 pi k / 256), the only transcendental the 256 point transforms of
// equations (118) and (125) need. Held as a table so that both transforms are
// a fixed sequence of multiply-adds and two runs of the same frame produce the
// same samples to the last bit.
const std::array<std::complex<double>, 256> kTwiddle = [] {
    std::array<std::complex<double>, 256> table{};
    for (std::size_t k = 0; k < table.size(); ++k) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / 256.0;
        table[k] = {std::cos(angle), std::sin(angle)};
    }
    return table;
}();

[[nodiscard]] const std::complex<double>& twiddle(int product) {
    int index = product % 256;
    if (index < 0) {
        index += 256;
    }
    return kTwiddle[static_cast<std::size_t>(index)];
}

// Section 6.3.1: b2 is always six bits, so the five gain values and the
// L - 6 higher order values must account for the rest of the 88.
[[nodiscard]] std::uint32_t band_count(std::uint32_t harmonics) {
    return harmonics <= 36 ? (harmonics + 2) / 3 : 12;
}

// The prose of sections 6.3.2 and 6.4.2 rather than equations (64) and (72).
// See the header: as printed those equations disagree with Annex G on 1080 of
// its 1272 entries, and this ordering reproduces it exactly for every L.
struct HocEntry {
    std::uint8_t block = 0;  // i, one based
    std::uint8_t index = 0;  // k, one based
};

[[nodiscard]] std::array<HocEntry, 51> hoc_layout(std::uint32_t harmonics) {
    std::array<HocEntry, 51> out{};
    const auto& lengths = kBlockLengths[harmonics - 9];
    std::size_t at = 0;
    for (std::uint8_t block = 1; block <= 6; ++block) {
        for (std::uint8_t k = 2; k <= lengths[block - 1U]; ++k) {
            out[at++] = {block, k};
        }
    }
    return out;
}

// Equations (84) through (93). The modulation vectors are a function of u0
// alone, which is why a wrongly decoded u0 makes the whole rest of the frame
// look like a 50 percent error rate channel, and why the decoder can detect
// that statistically.
[[nodiscard]] std::array<std::uint32_t, 8> modulation_vectors(
    std::uint32_t seed_vector) {
    std::array<std::uint32_t, 115> pr{};
    pr[0] = 16U * seed_vector;
    for (std::size_t n = 1; n < pr.size(); ++n) {
        pr[n] = (173U * pr[n - 1] + 13849U) & 0xFFFFU;
    }
    std::array<std::uint32_t, 8> m{};
    std::size_t at = 1;
    for (std::size_t vector = 1; vector <= 6; ++vector) {
        const int width = vector <= 3 ? 23 : 15;
        std::uint32_t value = 0;
        for (int bit = width - 1; bit >= 0; --bit) {
            value |= (pr[at++] >> 15) << bit;
        }
        m[vector] = value;
    }
    return m;
}

}  // namespace

std::span<const std::uint16_t> imbe_frame_map() noexcept {
    return kFrameMap;
}

Expected<std::size_t> imbe_priority_scan(std::uint32_t harmonics,
                                         std::span<std::uint16_t> cells) {
    if (harmonics < 9 || harmonics > 56) {
        return fail(std::format(
            "IMBE priority scan asked for L = {}. Equation (47) can only "
            "produce 9 through 56 from the 208 valid pitch indices, so there "
            "is no bit allocation for this one.",
            harmonics));
    }
    std::array<std::uint8_t, 58> allocation{};
    const auto& gain_bits = kGainBits[harmonics - 9];
    for (std::uint32_t m = 3; m <= 7; ++m) {
        allocation[m] = gain_bits[m - 3];
    }
    const std::size_t hoc_base = kHocOffset[harmonics - 9];
    for (std::uint32_t m = 8; m <= harmonics + 1; ++m) {
        allocation[m] = kHocBits[hoc_base + (m - 8)];
    }

    // Figure 22. The quantizer values b3 through b(L+1) are drawn as columns
    // with the MSB at the top; the scan walks rows from the top down and reads
    // left to right within a row.
    std::uint8_t widest = 0;
    for (std::uint32_t m = 3; m <= harmonics + 1; ++m) {
        widest = std::max(widest, allocation[m]);
    }
    std::size_t at = 0;
    for (int bit = static_cast<int>(widest) - 1; bit >= 0; --bit) {
        for (std::uint32_t m = 3; m <= harmonics + 1; ++m) {
            if (bit >= static_cast<int>(allocation[m])) {
                continue;
            }
            if (at >= cells.size()) {
                return fail(std::format(
                    "IMBE priority scan for L = {} needs more than the {} "
                    "cells it was given. Pass a span of at least 70.",
                    harmonics, cells.size()));
            }
            cells[at++] = static_cast<std::uint16_t>(m * 16 + bit);
        }
    }

    // Table 1: 88 bits, of which b0 takes 8, b1 takes K, b2 takes 6 and the
    // synchronization bit takes 1. Everything left belongs to the scan. A
    // table that did not add up would corrupt every parameter in the frame and
    // read as a decoder bug, so it is caught here and named.
    const std::uint32_t bands = band_count(harmonics);
    if (at + bands + 15 != 88) {
        return fail(std::format(
            "IMBE bit allocation for L = {} accounts for {} of the {} bits "
            "that TIA-102.BABA Table 1 leaves for b3 through b(L+1) after b0, "
            "b1, b2 and the synchronization bit. Annex F or Annex G has been "
            "transcribed wrongly and no parameter in a frame at this pitch "
            "can be trusted.",
            harmonics, at, 88 - bands - 15));
    }

    // Annex J against Annex G: the higher order coefficients are exactly the
    // elements of the six blocks other than the first of each, so the six
    // block lengths have to sum to L.
    const auto& lengths = kBlockLengths[harmonics - 9];
    std::uint32_t total_length = 0;
    for (const std::uint8_t length : lengths) {
        total_length += length;
    }
    if (total_length != harmonics) {
        return fail(std::format(
            "IMBE Annex J block lengths for L = {} sum to {}. They are the "
            "lengths of the six inverse DCTs that rebuild the prediction "
            "residual, so a sum that is not L leaves part of the spectral "
            "envelope unwritten.",
            harmonics, total_length));
    }
    return at;
}

Status imbe_pack_frame(std::span<const std::uint32_t> vectors,
                       std::span<std::uint8_t> frame) {
    if (vectors.size() != 8) {
        return fail(std::format(
            "IMBE frame packing wants the eight bit vectors u0 through u7 of "
            "TIA-102.BABA section 7.1 and was given {}.",
            vectors.size()));
    }
    if (frame.size() != 144) {
        return fail(std::format(
            "IMBE frame packing writes a 144 bit channel frame and was given "
            "{} bytes of room.",
            frame.size()));
    }
    constexpr std::array<int, 8> kWidths = {12, 12, 12, 12, 11, 11, 11, 7};
    for (std::size_t vector = 0; vector < 8; ++vector) {
        const std::uint32_t limit = 1U << kWidths[vector];
        if (vectors[vector] >= limit) {
            return fail(std::format(
                "IMBE bit vector u{} is {}, which does not fit the {} bits "
                "section 7.1 gives it.",
                vector, vectors[vector], kWidths[vector]));
        }
    }

    // Equations (81) through (83).
    std::array<std::uint32_t, 8> nu{};
    for (std::size_t vector = 0; vector <= 3; ++vector) {
        nu[vector] = (vectors[vector] << 11) | golay_parity(vectors[vector]);
    }
    for (std::size_t vector = 4; vector <= 6; ++vector) {
        nu[vector] = (vectors[vector] << 4) | hamming_parity(vectors[vector]);
    }
    nu[7] = vectors[7];

    // Equation (94).
    const std::array<std::uint32_t, 8> m = modulation_vectors(vectors[0]);
    std::array<std::uint32_t, 8> c{};
    for (std::size_t vector = 0; vector < 8; ++vector) {
        c[vector] = nu[vector] ^ m[vector];
    }

    // Section 7.5 and Annex H.
    for (std::size_t i = 0; i < kFrameMap.size(); ++i) {
        const std::uint16_t slot = kFrameMap[i];
        frame[i] = static_cast<std::uint8_t>((c[slot / 32U] >> (slot % 32U)) &
                                             1U);
    }
    return {};
}

double imbe_synthesis_window(int n) noexcept { return synthesis_window(n); }

double imbe_pitch_refinement_window(int n) noexcept {
    return pitch_refinement_window(n);
}

double ImbeDecoder::unvoiced_scaling_coefficient() noexcept {
    return kUnvoicedScaling;
}

ImbeDecoder::ImbeDecoder() { reset(); }

void ImbeDecoder::reset() {
    // Annex A, variable initialization, decoder side.
    previous_ = Parameters{};
    previous_.harmonics = 30;
    previous_.bands = 10;
    previous_.fundamental = 0.02985 * kPi;
    previous_.unenhanced.fill(1.0);
    previous_.enhanced.fill(0.0);
    previous_.voiced.fill(0);
    previous_.voiced_smoothed.fill(0);
    current_ = previous_;

    local_energy_ = 75000.0;
    error_rate_ = 0.0;

    // Annex A omits tau_M(-1). 20480 is what equation (115) itself yields for
    // a frame with no errors, which is the only choice that makes an
    // error-free first frame behave like an error-free hundredth frame. See
    // the header, disagreement 4.
    amplitude_threshold_ = 20480.0;

    psi_.fill(0.0);
    phi_.fill(0.0);
    phi_previous_.fill(0.0);

    noise_.fill(0.0);
    noise_state_ = 3147.0;  // u(-105), section 11.2
    noise_primed_ = false;

    unvoiced_tail_.fill(0.0);
    u_.fill(0);
    b_.fill(0);
    consecutive_invalid_ = 0;
    report_ = ImbeFrameReport{};
}

// Section 11.2, equation (117). The sequence is shifted 160 samples per
// synthesis frame, so successive frames overlap by 49 of the 209 window
// samples.
void ImbeDecoder::advance_noise() {
    auto step = [this] {
        const double next = 171.0 * noise_state_ + 11213.0;
        noise_state_ = next - 53125.0 * std::floor(next / 53125.0);
        return noise_state_;
    };
    if (!noise_primed_) {
        for (std::size_t i = 0; i < kNoiseWindowSpan; ++i) {
            noise_[i] = step();
        }
        noise_primed_ = true;
        return;
    }
    constexpr std::size_t kHop = 160;
    for (std::size_t i = 0; i + kHop < kNoiseWindowSpan; ++i) {
        noise_[i] = noise_[i + kHop];
    }
    for (std::size_t i = kNoiseWindowSpan - kHop; i < kNoiseWindowSpan; ++i) {
        noise_[i] = step();
    }
}

Status ImbeDecoder::decode(std::span<const std::uint8_t> bits,
                           std::span<float> out) {
    if (bits.size() != kChannelFrameBits && bits.size() != kBitVectorBits) {
        return fail(std::format(
            "IMBE decode was handed {} bits. A P25 Phase 1 voice frame is {} "
            "bits, 88 of model parameters plus 56 of forward error control "
            "(TIA-102.BABA section 7.3); {} is the same frame with the error "
            "control already stripped, the bit vectors u0 through u7 of "
            "section 7.1. Split the stream on the 144 bit boundary, or hand "
            "over the 88 bit vectors, and call again.",
            bits.size(), kChannelFrameBits, kBitVectorBits));
    }
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] > 1) {
            return fail(std::format(
                "IMBE decode was handed the value {} at bit {} of {}. This "
                "interface takes one bit per byte, 0 or 1, in transmission "
                "order, not packed bytes: a frame arriving as 18 packed bytes "
                "has to be unpacked most significant bit first before it gets "
                "here.",
                static_cast<unsigned>(bits[i]), i, bits.size()));
        }
    }
    if (out.size() < kPcmFrames) {
        return fail(std::format(
            "IMBE decode was handed {} samples of output for a frame that is "
            "always {}. One 20 ms frame at 8 kHz is {} samples "
            "(TIA-102.BABA section 11.1); enlarge the buffer.",
            out.size(), kPcmFrames, kPcmFrames));
    }

    if (Status unpacked = unpack(bits); !unpacked) {
        return unpacked;
    }

    // Section 7.6, equations (95) and (96). Computed before anything decides
    // what to do with the frame, because the decision reads both.
    std::uint32_t total = 0;
    for (const std::uint8_t count : report_.corrected) {
        total += count;
    }
    report_.errors_total = total;
    error_rate_ = 0.95 * error_rate_ + 0.000365 * static_cast<double>(total);
    report_.error_rate = error_rate_;

    // Annex K Flow Chart 9, in the order the chart tests them. See the header,
    // disagreement 2: section 7.7 states a different threshold for the first
    // test and section 7.8 a different one for the error rate, and the chart
    // is followed because it is the only one of the two that covers a run of
    // bad frames and the reserved pitch codes.
    const std::uint32_t pitch = report_.pitch_index;
    bool invalid = false;
    ImbeFrameCause cause = ImbeFrameCause::None;
    if (total >= 11 && report_.corrected[0] >= 2) {
        invalid = true;
        cause = ImbeFrameCause::ErrorBurst;
    } else if ((pitch >= 208 && pitch <= 215) || pitch >= 220) {
        invalid = true;
        cause = ImbeFrameCause::ReservedPitchIndex;
    }

    ImbeFrameState state = ImbeFrameState::Decoded;
    if (invalid) {
        ++consecutive_invalid_;
        if (consecutive_invalid_ >= 4) {
            state = ImbeFrameState::Muted;
            cause = ImbeFrameCause::FourthConsecutiveInvalid;
        } else {
            state = ImbeFrameState::Repeated;
        }
    } else {
        consecutive_invalid_ = 0;
        if (pitch >= 216 && pitch <= 219) {
            state = ImbeFrameState::Muted;
            cause = ImbeFrameCause::MuteRequestPitchIndex;
        } else if (error_rate_ >= 0.085) {
            state = ImbeFrameState::Muted;
            cause = ImbeFrameCause::ErrorRate;
        }
    }
    report_.state = state;
    report_.cause = cause;
    report_.consecutive_invalid = consecutive_invalid_;

    if (state == ImbeFrameState::Decoded) {
        decode_parameters();
        enhance_and_smooth();
    } else {
        // Equations (99) through (104), plus v_bar, which those equations omit
        // and synthesis reads. See the header, disagreement 5.
        current_ = previous_;
    }

    report_.harmonics = current_.harmonics;
    report_.voiced_bands = current_.bands;
    report_.fundamental_hz = current_.fundamental *
                             static_cast<double>(kSampleRateHz) / (2.0 * kPi);
    std::uint32_t voiced = 0;
    for (std::uint32_t l = 1; l <= current_.harmonics; ++l) {
        voiced += current_.voiced_smoothed[l];
    }
    report_.voiced_harmonics = voiced;

    advance_noise();

    // Equations (139) and (140). Section 11.3 requires psi to be updated every
    // frame for all 56 harmonics whatever L is, so it runs on a repeat and on
    // a mute as well: the oscillators have to stay on the phase they would
    // have been on, or the first frame after an outage arrives out of step.
    const double omega_prev = previous_.fundamental;
    const double omega_now = current_.fundamental;
    const double half_span = 0.5 * static_cast<double>(kPcmFrames);
    std::uint32_t unvoiced_count = 0;
    for (std::uint32_t l = 1; l <= current_.harmonics; ++l) {
        unvoiced_count += current_.voiced_smoothed[l] == 0 ? 1U : 0U;
    }
    const std::uint32_t quarter = current_.harmonics / 4;
    // Equation (140) derives phi_l only up to max[L(-1), L(0)]. A later frame
    // can have a larger L than either and would then read a phi_l(-1) that was
    // never written, so the same rule runs to 56. See the header, silence 6.
    for (std::uint32_t l = 1; l <= kMaxHarmonics; ++l) {
        psi_[l] += (omega_prev + omega_now) * static_cast<double>(l) * half_span;
        if (l <= quarter || current_.harmonics == 0) {
            phi_[l] = psi_[l];
        } else {
            // Equation (141). u(l) is the shifted noise sequence for this
            // frame, frame local index l, which is offset 104 into the window.
            const double rho =
                2.0 * kPi * noise_[l + 104] / 53125.0 - kPi;
            phi_[l] = psi_[l] + static_cast<double>(unvoiced_count) * rho /
                                    static_cast<double>(current_.harmonics);
        }
    }

    if (state == ImbeFrameState::Muted) {
        synthesize_mute(out);
    } else {
        synthesize(out);
    }

    rotate();
    return {};
}

Status ImbeDecoder::unpack(std::span<const std::uint8_t> bits) {
    report_ = ImbeFrameReport{};
    u_.fill(0);
    b_.fill(0);

    if (bits.size() == kChannelFrameBits) {
        // Section 7.5 and Annex H: undo the intra-frame interleave.
        std::array<std::uint32_t, 8> c{};
        for (std::size_t i = 0; i < kChannelFrameBits; ++i) {
            const std::uint16_t slot = kFrameMap[i];
            c[slot / 32U] |= static_cast<std::uint32_t>(bits[i])
                             << (slot % 32U);
        }

        // Section 7.4: m0 is all zeros, so nu0 is c0 already and Golay
        // decoding it is what produces the seed for everything else.
        const Decoded first = golay_decode(c[0]);
        u_[0] = first.data;
        report_.corrected[0] = static_cast<std::uint8_t>(first.corrected);

        // Equations (84) through (93). Each modulation vector takes the high
        // bit of successive pr values, left most bit first, so pr(1) lands on
        // bit 22 of m1.
        const std::array<std::uint32_t, 8> m = modulation_vectors(u_[0]);

        // Equation (94) run backwards, then the remaining error control.
        for (std::size_t vector = 1; vector <= 3; ++vector) {
            const Decoded d = golay_decode(c[vector] ^ m[vector]);
            u_[vector] = d.data;
            report_.corrected[vector] = static_cast<std::uint8_t>(d.corrected);
        }
        for (std::size_t vector = 4; vector <= 6; ++vector) {
            const Decoded d = hamming_decode(c[vector] ^ m[vector]);
            u_[vector] = d.data;
            report_.corrected[vector] = static_cast<std::uint8_t>(d.corrected);
        }
        // Equation (83): nu7 carries no error control at all, and m7 is zero.
        u_[7] = c[7] & 0x7FU;
    } else {
        // The bit vectors as section 7.1 defines them, MSB first, u0 through
        // u7 end to end. No code ran, so nothing was corrected.
        constexpr std::array<int, 8> kWidths = {12, 12, 12, 12, 11, 11, 11, 7};
        std::size_t at = 0;
        for (std::size_t vector = 0; vector < 8; ++vector) {
            std::uint32_t value = 0;
            for (int bit = kWidths[vector] - 1; bit >= 0; --bit) {
                value |= static_cast<std::uint32_t>(bits[at++]) << bit;
            }
            u_[vector] = value;
        }
    }

    // Section 7.1: b0's six most significant bits sit in u0 bits 11..6 and its
    // two least significant bits in u7 bits 2 and 1.
    const std::uint32_t pitch = ((u_[0] >> 6) << 2) | ((u_[7] >> 1) & 0x3U);
    report_.pitch_index = pitch;
    b_[0] = pitch;
    if (pitch > 207) {
        // Outside the range the encoder uses, section 6.1. L cannot be
        // computed, so the rest of the unpack is skipped and the caller's
        // frame is dealt with by the repeat or mute path.
        return {};
    }

    // Equations (46) through (48).
    const double omega = 4.0 * kPi / (static_cast<double>(pitch) + 39.5);
    const auto harmonics = static_cast<std::uint32_t>(
        std::floor(0.9254 * std::floor(kPi / omega + 0.25)));
    const std::uint32_t bands = band_count(harmonics);

    // Figure 22, priority scanning, together with the two table consistency
    // checks that go with it.
    std::array<std::uint16_t, 70> scan{};
    const Expected<std::size_t> scanned = imbe_priority_scan(harmonics, scan);
    if (!scanned) {
        return std::unexpected(scanned.error());
    }

    auto scan_bit = [&](std::size_t index, std::uint32_t value) {
        const std::uint16_t cell = scan[index];
        b_[cell / 16U] |= value << (cell % 16U);
    };
    auto bit_of = [](std::uint32_t word, int bit) {
        return (word >> bit) & 1U;
    };

    std::size_t at = 0;
    for (int bit = 2; bit >= 0; --bit) {
        scan_bit(at++, bit_of(u_[0], bit));
    }
    for (std::size_t vector = 1; vector <= 3; ++vector) {
        for (int bit = 11; bit >= 0; --bit) {
            scan_bit(at++, bit_of(u_[vector], bit));
        }
    }

    // The second stream: all of b1 MSB first, then bit 2 and bit 1 of b2, then
    // the scan continues. It fills u4 bit 10 down to u7 bit 4.
    std::array<std::pair<std::uint8_t, std::uint8_t>, 36> sink{};
    std::size_t sink_size = 0;
    for (std::uint8_t vector = 4; vector <= 6; ++vector) {
        for (int bit = 10; bit >= 0; --bit) {
            sink[sink_size++] = {vector, static_cast<std::uint8_t>(bit)};
        }
    }
    for (int bit = 6; bit >= 4; --bit) {
        sink[sink_size++] = {7, static_cast<std::uint8_t>(bit)};
    }
    for (std::size_t t = 0; t < sink_size; ++t) {
        const std::uint32_t value = bit_of(u_[sink[t].first], sink[t].second);
        if (t < bands) {
            b_[1] |= value << (bands - 1 - t);
        } else if (t == bands) {
            b_[2] |= value << 2;
        } else if (t == bands + 1) {
            b_[2] |= value << 1;
        } else {
            scan_bit(at++, value);
        }
    }

    // Section 7.1, the tail of u7 and the three bits of u0 that b2 keeps.
    b_[2] |= ((u_[0] >> 3) & 0x7U) << 3;
    b_[2] |= bit_of(u_[7], 3);
    b_[harmonics + 2] = bit_of(u_[7], 0);

    current_.harmonics = harmonics;
    current_.bands = bands;
    current_.fundamental = omega;
    return {};
}

void ImbeDecoder::decode_parameters() {
    const std::uint32_t harmonics = current_.harmonics;
    const std::uint32_t bands = current_.bands;

    // Equations (50) and (51). The subtraction of two floors is bit
    // (K - kappa_l) of b1, which is how it is written here.
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        const std::uint32_t kappa = l <= 36 ? (l + 2) / 3 : 12;
        current_.voiced[l] =
            static_cast<std::uint8_t>((b_[1] >> (bands - kappa)) & 1U);
    }

    // Section 6.4.1, equations (68) through (70).
    std::array<double, 7> transformed{};
    transformed[1] = kGainLevels[b_[2]];
    const auto& gain_bits = kGainBits[harmonics - 9];
    const auto& gain_steps = kGainSteps[harmonics - 9];
    for (std::uint32_t m = 3; m <= 7; ++m) {
        const std::uint32_t width = gain_bits[m - 3];
        transformed[m - 1] =
            width == 0 ? 0.0
                       : gain_steps[m - 3] *
                             (static_cast<double>(b_[m]) -
                              static_cast<double>(1U << (width - 1)) + 0.5);
    }
    std::array<double, 7> gain{};
    for (int i = 1; i <= 6; ++i) {
        double sum = 0.0;
        for (int m = 1; m <= 6; ++m) {
            const double alpha = m == 1 ? 1.0 : 2.0;
            sum += alpha * transformed[static_cast<std::size_t>(m)] *
                   std::cos(kPi * static_cast<double>(m - 1) *
                            (static_cast<double>(i) - 0.5) / 6.0);
        }
        gain[static_cast<std::size_t>(i)] = sum;
    }

    // Section 6.4.2, equation (71), with the index relation taken from the
    // prose rather than from equation (72). See the header, disagreement 1.
    const auto& lengths = kBlockLengths[harmonics - 9];
    std::array<std::array<double, 11>, 7> coefficients{};
    for (int i = 1; i <= 6; ++i) {
        coefficients[static_cast<std::size_t>(i)][1] =
            gain[static_cast<std::size_t>(i)];
    }
    const std::size_t hoc_base = kHocOffset[harmonics - 9];
    const auto layout = hoc_layout(harmonics);
    for (std::uint32_t m = 8; m <= harmonics + 1; ++m) {
        const HocEntry entry = layout[m - 8];
        const std::uint32_t width = kHocBits[hoc_base + (m - 8)];
        if (width == 0) {
            coefficients[entry.block][entry.index] = 0.0;
            continue;
        }
        const double step = kStepForBits[width] * kSigma[entry.index];
        coefficients[entry.block][entry.index] =
            step * (static_cast<double>(b_[m]) -
                    static_cast<double>(1U << (width - 1)) + 0.5);
    }

    // Equations (73) and (74), then the blocks joined back into T_tilde.
    std::array<double, kMaxHarmonics + 2> residual{};
    std::size_t at = 1;
    for (int i = 1; i <= 6; ++i) {
        const int length = lengths[static_cast<std::size_t>(i - 1)];
        for (int jj = 1; jj <= length; ++jj) {
            double sum = 0.0;
            for (int k = 1; k <= length; ++k) {
                const double alpha = k == 1 ? 1.0 : 2.0;
                sum += alpha *
                       coefficients[static_cast<std::size_t>(i)]
                                   [static_cast<std::size_t>(k)] *
                       std::cos(kPi * static_cast<double>(k - 1) *
                                (static_cast<double>(jj) - 0.5) /
                                static_cast<double>(length));
            }
            residual[at++] = sum;
        }
    }

    // Equation (55), then (75) through (79).
    const auto width = static_cast<double>(harmonics);
    double rho = 0.7;
    if (harmonics <= 15) {
        rho = 0.4;
    } else if (harmonics <= 24) {
        rho = 0.03 * width - 0.05;
    }

    const std::uint32_t before = previous_.harmonics;
    auto previous_log = [&](std::size_t index) {
        // Equations (78) and (79).
        if (index == 0) {
            return 0.0;  // log2 of M_0(-1) = 1.0
        }
        const std::size_t clamped = index > before ? before : index;
        return std::log2(previous_.unenhanced[clamped]);
    };

    double mean = 0.0;
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        const double k = static_cast<double>(before) * static_cast<double>(l) /
                         width;
        const double floor_k = std::floor(k);
        const double delta = k - floor_k;
        const auto index = static_cast<std::size_t>(floor_k);
        mean += (1.0 - delta) * previous_log(index) +
                delta * previous_log(index + 1);
    }
    mean *= rho / width;

    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        const double k = static_cast<double>(before) * static_cast<double>(l) /
                         width;
        const double floor_k = std::floor(k);
        const double delta = k - floor_k;
        const auto index = static_cast<std::size_t>(floor_k);
        const double log_amplitude =
            residual[l] + rho * ((1.0 - delta) * previous_log(index) +
                                 delta * previous_log(index + 1)) -
            mean;
        current_.unenhanced[l] = std::exp2(log_amplitude);
    }
    for (std::size_t l = harmonics + 1; l < current_.unenhanced.size(); ++l) {
        current_.unenhanced[l] = 0.0;
    }
}

void ImbeDecoder::enhance_and_smooth() {
    const std::uint32_t harmonics = current_.harmonics;
    const double omega = current_.fundamental;

    // Section 8, equations (105) and (106).
    double energy = 0.0;
    double correlation = 0.0;
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        const double square =
            current_.unenhanced[l] * current_.unenhanced[l];
        energy += square;
        correlation += square * std::cos(omega * static_cast<double>(l));
    }

    // Equations (107) and (108). The denominator vanishes only if every
    // harmonic sits at a multiple of 2 pi, which no fundamental in the coded
    // range produces for every l at once; the guard is here so that a frame
    // that somehow reaches it leaves the amplitudes alone rather than
    // producing infinities that propagate into the next frame's prediction.
    const double denominator = omega * energy * (energy * energy -
                                                 correlation * correlation);
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        double weight = 1.0;
        if (denominator > 0.0) {
            const double numerator =
                0.96 * kPi *
                (energy * energy + correlation * correlation -
                 2.0 * energy * correlation *
                     std::cos(omega * static_cast<double>(l)));
            weight = std::sqrt(current_.unenhanced[l]) *
                     std::pow(numerator / denominator, 0.25);
        }
        double enhanced = weight * current_.unenhanced[l];
        if (8 * l <= harmonics) {
            enhanced = current_.unenhanced[l];
        } else if (weight > 1.2) {
            enhanced = 1.2 * current_.unenhanced[l];
        } else if (weight < 0.5) {
            enhanced = 0.5 * current_.unenhanced[l];
        }
        current_.enhanced[l] = enhanced;
    }

    // Equations (109) and (110).
    double enhanced_energy = 0.0;
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        enhanced_energy += current_.enhanced[l] * current_.enhanced[l];
    }
    if (enhanced_energy > 0.0) {
        const double scale = std::sqrt(energy / enhanced_energy);
        for (std::uint32_t l = 1; l <= harmonics; ++l) {
            current_.enhanced[l] *= scale;
        }
    }
    for (std::size_t l = harmonics + 1; l < current_.enhanced.size(); ++l) {
        current_.enhanced[l] = 0.0;
    }

    // Equation (111).
    const double candidate = 0.95 * local_energy_ + 0.05 * energy;
    local_energy_ = candidate >= 10000.0 ? candidate : 10000.0;

    // Section 9, equation (112). The first branch is infinity, so no amplitude
    // is ever forced voiced on a clean frame.
    const double shaped = std::pow(local_energy_, 0.375);
    double threshold = 1.414 * shaped;
    if (error_rate_ <= 0.005 && report_.errors_total <= 4) {
        threshold = std::numeric_limits<double>::infinity();
    } else if (error_rate_ <= 0.0125 && report_.corrected[4] == 0) {
        threshold = 45.255 * shaped / std::exp(277.26 * error_rate_);
    }

    // Equation (113).
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        current_.voiced_smoothed[l] =
            current_.enhanced[l] > threshold ? std::uint8_t{1}
                                             : current_.voiced[l];
    }
    for (std::size_t l = harmonics + 1; l < current_.voiced_smoothed.size();
         ++l) {
        current_.voiced_smoothed[l] = 0;
        current_.voiced[l] = 0;
    }

    // Equations (114) through (116).
    double amplitude_sum = 0.0;
    for (std::uint32_t l = 1; l <= harmonics; ++l) {
        amplitude_sum += current_.enhanced[l];
    }
    if (error_rate_ <= 0.005 && report_.errors_total <= 6) {
        amplitude_threshold_ = 20480.0;
    } else {
        amplitude_threshold_ = 6000.0 -
                               300.0 * static_cast<double>(report_.errors_total) +
                               amplitude_threshold_;
    }
    if (amplitude_threshold_ <= amplitude_sum && amplitude_sum > 0.0) {
        const double scale = amplitude_threshold_ / amplitude_sum;
        for (std::uint32_t l = 1; l <= harmonics; ++l) {
            current_.enhanced[l] *= scale;
        }
    }
}

void ImbeDecoder::synthesize(std::span<float> out) {
    constexpr int kFrame = static_cast<int>(kPcmFrames);
    const double omega_now = current_.fundamental;
    const double omega_prev = previous_.fundamental;

    // Section 11.2, equation (118).
    std::array<std::complex<double>, kDftSize> spectrum{};
    for (int m = -128; m <= 127; ++m) {
        std::complex<double> sum{0.0, 0.0};
        for (int n = -104; n <= 104; ++n) {
            const double sample =
                noise_[static_cast<std::size_t>(n + 104)] *
                synthesis_window(n);
            sum += sample * twiddle(m * n);
        }
        spectrum[static_cast<std::size_t>(m + 128)] = sum;
    }

    // Equations (119), (120), (122) and (123).
    std::array<std::complex<double>, kDftSize> shaped{};
    const double scale = static_cast<double>(kDftSize) / (2.0 * kPi);
    int lowest = 128;
    int highest = 0;
    for (std::uint32_t l = 1; l <= current_.harmonics; ++l) {
        const double edge_low =
            scale * (static_cast<double>(l) - 0.5) * omega_now;
        const double edge_high =
            scale * (static_cast<double>(l) + 0.5) * omega_now;
        int from = static_cast<int>(std::ceil(edge_low));
        int to = static_cast<int>(std::ceil(edge_high));
        from = std::clamp(from, 0, 128);
        to = std::clamp(to, 0, 128);
        if (l == 1) {
            lowest = from;
        }
        highest = to;
        if (to <= from || current_.voiced_smoothed[l] != 0) {
            continue;  // equation (119): a voiced band contributes no noise
        }
        double band_energy = 0.0;
        for (int eta = from; eta < to; ++eta) {
            band_energy += std::norm(
                spectrum[static_cast<std::size_t>(eta + 128)]);
        }
        const double mean = band_energy / static_cast<double>(to - from);
        if (mean <= 0.0) {
            continue;
        }
        const double gain =
            kUnvoicedScaling * current_.enhanced[l] / std::sqrt(mean);
        for (int m = from; m < to; ++m) {
            shaped[static_cast<std::size_t>(m + 128)] =
                gain * spectrum[static_cast<std::size_t>(m + 128)];
            if (m != 0) {
                shaped[static_cast<std::size_t>(-m + 128)] =
                    gain * spectrum[static_cast<std::size_t>(-m + 128)];
            }
        }
    }
    // Equation (124).
    for (int m = -128; m <= 127; ++m) {
        const int magnitude = m < 0 ? -m : m;
        if (magnitude < lowest || magnitude >= highest) {
            shaped[static_cast<std::size_t>(m + 128)] = {0.0, 0.0};
        }
    }

    // Equation (125). The spectrum is conjugate symmetric by construction, so
    // the transform is real and the imaginary residue is rounding.
    std::array<double, kDftSize> noise_frame{};
    for (int n = -128; n <= 127; ++n) {
        std::complex<double> sum{0.0, 0.0};
        for (int m = -128; m <= 127; ++m) {
            const std::complex<double>& bin =
                shaped[static_cast<std::size_t>(m + 128)];
            // Most of the 256 bins are zeroed by equations (119) and (124).
            // Skipping them is not an approximation: adding zero changes
            // nothing, and the surviving terms are summed in the same order
            // every time, which is what the determinism test rests on.
            if (bin.real() == 0.0 && bin.imag() == 0.0) {
                continue;
            }
            sum += bin * std::conj(twiddle(m * n));
        }
        noise_frame[static_cast<std::size_t>(n + 128)] =
            sum.real() / static_cast<double>(kDftSize);
    }

    auto tail_at = [this](int n) {
        return n < -128 || n > 127
                   ? 0.0
                   : unvoiced_tail_[static_cast<std::size_t>(n + 128)];
    };
    auto frame_at = [&noise_frame](int n) {
        return n < -128 || n > 127
                   ? 0.0
                   : noise_frame[static_cast<std::size_t>(n + 128)];
    };

    // Section 11.3. phi_previous_ is phi_l(-1) and phi_ is phi_l(0), both set
    // before this runs.
    const double span = static_cast<double>(kFrame);
    const std::uint32_t reach =
        std::max(previous_.harmonics, current_.harmonics);
    const bool near_in_frequency =
        std::abs(omega_now - omega_prev) < 0.1 * omega_now;

    std::array<double, kPcmFrames> voiced{};
    for (std::uint32_t l = 1; l <= reach; ++l) {
        // Equations (128) and (129).
        const bool voiced_now =
            l <= current_.harmonics && current_.voiced_smoothed[l] != 0;
        const bool voiced_before =
            l <= previous_.harmonics && previous_.voiced_smoothed[l] != 0;
        const double amplitude_now =
            l <= current_.harmonics ? current_.enhanced[l] : 0.0;
        const double amplitude_before =
            l <= previous_.harmonics ? previous_.enhanced[l] : 0.0;
        if (!voiced_now && !voiced_before) {
            continue;  // equation (130)
        }
        const auto index = static_cast<double>(l);
        if (voiced_now && voiced_before && l < 8 && near_in_frequency) {
            // Equations (134) through (138).
            const double delta_phi =
                phi_[l] - phi_previous_[l] -
                (omega_prev + omega_now) * index * span / 2.0;
            const double wrapped =
                delta_phi -
                2.0 * kPi * std::floor((delta_phi + kPi) / (2.0 * kPi));
            const double delta_omega = wrapped / span;
            for (int n = 0; n < kFrame; ++n) {
                const auto position = static_cast<double>(n);
                const double envelope =
                    amplitude_before +
                    position / span * (amplitude_now - amplitude_before);
                const double theta =
                    phi_previous_[l] +
                    (omega_prev * index + delta_omega) * position +
                    (omega_now - omega_prev) * index * position * position /
                        (2.0 * span);
                voiced[static_cast<std::size_t>(n)] +=
                    envelope * std::cos(theta);
            }
            continue;
        }
        for (int n = 0; n < kFrame; ++n) {
            double sample = 0.0;
            if (voiced_before) {
                // Equations (131) and the first term of (133).
                sample += synthesis_window(n) * amplitude_before *
                          std::cos(omega_prev * static_cast<double>(n) * index +
                                   phi_previous_[l]);
            }
            if (voiced_now) {
                // Equations (132) and the second term of (133).
                const int offset = n - kFrame;
                sample += synthesis_window(offset) * amplitude_now *
                          std::cos(omega_now * static_cast<double>(offset) *
                                       index +
                                   phi_[l]);
            }
            voiced[static_cast<std::size_t>(n)] += sample;
        }
    }

    // Equations (126), (127) and (142).
    for (int n = 0; n < kFrame; ++n) {
        const double w_now = synthesis_window(n);
        const double w_prev = synthesis_window(n - kFrame);
        const double weight = w_now * w_now + w_prev * w_prev;
        const double unvoiced =
            weight > 0.0
                ? (w_now * tail_at(n) + w_prev * frame_at(n - kFrame)) / weight
                : 0.0;
        const double sample =
            unvoiced + 2.0 * voiced[static_cast<std::size_t>(n)];
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(sample) / kFullScale;
    }

    unvoiced_tail_ = noise_frame;
}

void ImbeDecoder::synthesize_mute(std::span<float> out) {
    // Section 7.8: bypass synthesis and emit noise uniform over [-5, 5]. The
    // standard does not say where the noise comes from, so it comes from the
    // generator the standard already defines for the unvoiced path, equation
    // (117), which keeps the output reproducible without a second seed.
    for (std::size_t n = 0; n < kPcmFrames; ++n) {
        const double uniform = noise_[n] / 53125.0;
        out[n] = static_cast<float>(10.0 * uniform - 5.0) / kFullScale;
    }
    // Nothing was synthesized, so there is no overlap-add tail to carry.
    unvoiced_tail_.fill(0.0);
}

void ImbeDecoder::rotate() {
    previous_ = current_;
    phi_previous_ = phi_;
}

std::string ImbeDecoder::describe_last_frame() const {
    std::string_view state;
    switch (report_.state) {
        case ImbeFrameState::Decoded:
            state = "decoded";
            break;
        case ImbeFrameState::Repeated:
            state = "REPEATED";
            break;
        case ImbeFrameState::Muted:
            state = "MUTED";
            break;
    }
    std::string_view cause;
    switch (report_.cause) {
        case ImbeFrameCause::None:
            cause = "no fault";
            break;
        case ImbeFrameCause::ErrorBurst:
            cause = "error burst, the codes corrected near their limit";
            break;
        case ImbeFrameCause::ReservedPitchIndex:
            cause = "pitch index outside 0..207";
            break;
        case ImbeFrameCause::MuteRequestPitchIndex:
            cause = "pitch index 216..219, which requests a mute";
            break;
        case ImbeFrameCause::ErrorRate:
            cause = "error rate at or above 0.085";
            break;
        case ImbeFrameCause::FourthConsecutiveInvalid:
            cause = "fourth invalid frame in a row";
            break;
    }
    return std::format(
        "IMBE {}: {}. corrected {}/{}/{}/{}/{}/{}/{} bits, total {}, rate "
        "{:.4f}, L {}, K {}, {} of {} harmonics voiced, fundamental {:.1f} Hz",
        state, cause, report_.corrected[0], report_.corrected[1],
        report_.corrected[2], report_.corrected[3], report_.corrected[4],
        report_.corrected[5], report_.corrected[6], report_.errors_total,
        report_.error_rate, report_.harmonics, report_.voiced_bands,
        report_.voiced_harmonics, report_.harmonics, report_.fundamental_hz);
}

}  // namespace revenant::decode
