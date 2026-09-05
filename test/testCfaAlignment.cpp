#include <cmath>
#include <cstdint>
#include <iostream>

#include "CfaAlignment.hpp"

using namespace hdrmerge;

namespace {

CFAPattern rggbPattern() {
    CFAPattern pattern;
    pattern.setPattern(0x94949494u, [] (int, int) { return 0; });
    return pattern;
}

int fail(const char * message) {
    std::cerr << message << std::endl;
    return 1;
}

} // namespace

int main() {
    const int width = 96, height = 80;
    const CFAPattern pattern = rggbPattern();
    Array2D<uint16_t> source(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Disjoint ranges make any cross-phase colour mixing immediately visible.
            const int phase = (y & 1) * 2 + (x & 1);
            source[static_cast<size_t>(y) * width + x] =
                static_cast<uint16_t>(phase * 12000 + 1000 + 3 * x + 5 * y);
        }
    }

    AlignmentTransform identity;
    identity.valid = true;
    Array2D<uint16_t> copied;
    Array2D<uint8_t> copiedValidity;
    Array2D<float> copiedInterpolationVariance;
    resampleCfa(source, copied, copiedValidity, pattern, identity,
                &copiedInterpolationVariance);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pos = static_cast<size_t>(y) * width + x;
            if (!copiedValidity[pos] || copied[pos] != source[pos])
                return fail("identity CFA resampling changed a sample");
            if (copiedInterpolationVariance[pos] != 0.0f)
                return fail("identity CFA resampling introduced interpolation uncertainty");
        }
    }

    AlignmentTransform affine;
    affine.valid = true;
    const double angle = 0.7 * 3.14159265358979323846 / 180.0;
    affine.m[0][0] = std::cos(angle);
    affine.m[0][1] = -std::sin(angle);
    affine.m[1][0] = std::sin(angle);
    affine.m[1][1] = std::cos(angle);
    affine.m[0][2] = 0.45;
    affine.m[1][2] = -0.30;
    Array2D<uint16_t> warped;
    Array2D<uint8_t> validity;
    Array2D<float> interpolationVariance;
    resampleCfa(source, warped, validity, pattern, affine, &interpolationVariance);
    size_t validCount = 0;
    size_t interpolatedCount = 0;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pos = static_cast<size_t>(y) * width + x;
            if (!validity[pos]) continue;
            ++validCount;
            if (interpolationVariance[pos] > 0.0f) ++interpolatedCount;
            const int phase = (y & 1) * 2 + (x & 1);
            const uint16_t lower = static_cast<uint16_t>(phase * 12000 + 900);
            const uint16_t upper = static_cast<uint16_t>(phase * 12000 + 2000);
            if (warped[pos] < lower || warped[pos] > upper)
                return fail("affine CFA resampling mixed different Bayer phases");
        }
    }
    if (validCount < static_cast<size_t>(width * height * 0.85))
        return fail("affine CFA resampling produced an unexpectedly small valid area");
    if (interpolatedCount < validCount / 2)
        return fail("fractional CFA resampling did not report its interpolation footprint");

    std::cout << "CFA alignment tests passed (" << validCount << " valid affine samples)." << std::endl;
    return 0;
}
