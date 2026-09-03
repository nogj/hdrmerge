#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "../src/ImageStack.hpp"
#include "../src/RawParameters.hpp"

using namespace hdrmerge;

namespace {

RawParameters makeParameters(size_t width, size_t height) {
    RawParameters params;
    params.width = params.rawWidth = width;
    params.height = params.rawHeight = height;
    params.max = 65535;
    params.colors = 4;
    params.cdesc = "RGBG";
    params.FC.setPattern(0x4b4b4b4b, (uint8_t (*)(int, int))0);
    for (int c = 0; c < 4; ++c) {
        params.preMul[c] = 1.0f;
        params.camMul[c] = 1.0f;
    }
    return params;
}

Image makeImage(const std::vector<uint16_t> & pixels, const RawParameters & params) {
    Image image(const_cast<uint16_t *>(pixels.data()), params, QString());
    image.setSaturationThreshold(65535);
    return image;
}

bool check(bool condition, const char * message) {
    if (!condition) std::cerr << "FAILED: " << message << std::endl;
    return condition;
}

} // namespace

int main() {
    bool ok = true;
    const size_t width = 32, height = 24;
    RawParameters params = makeParameters(width, height);

    // A global response alone leaves colour-dependent residuals at CFA seams.
    {
        const size_t responseWidth = 64, responseHeight = 48;
        RawParameters responseParams = makeParameters(responseWidth, responseHeight);
        const double multipliers[4] = {2.30, 2.00, 1.75, 2.10};
        std::vector<uint16_t> candidate(responseWidth*responseHeight);
        std::vector<uint16_t> reference(responseWidth*responseHeight);
        for (size_t y = 0; y < responseHeight; ++y) {
            for (size_t x = 0; x < responseWidth; ++x) {
                const size_t pos = y*responseWidth + x;
                reference[pos] = static_cast<uint16_t>(800 + (x*71 + y*43) % 5000);
                const int color = responseParams.FC(x, y);
                candidate[pos] = static_cast<uint16_t>(reference[pos] * multipliers[color]);
            }
        }

        Image brighter = makeImage(candidate, responseParams);
        Image darker = makeImage(reference, responseParams);
        brighter.computeResponseFunction(darker);
        for (size_t y = 0; y < responseHeight; ++y) {
            for (size_t x = 0; x < responseWidth; ++x) {
                const double expected = darker.exposureAt(x, y);
                const double actual = brighter.exposureAt(x, y);
                ok &= check(std::abs(actual - expected) / expected < 0.01,
                            "per-channel response left a CFA exposure discontinuity");
            }
        }
    }

    // A seam directly between layers 0 and 2 must never sample layer 1.
    // Its deliberately extreme boundary value catches the old scalar-index blur.
    {
        std::vector<uint16_t> bright(width*height, 60000);
        std::vector<uint16_t> middle(width*height, 40000);
        std::vector<uint16_t> dark(width*height, 10000);
        for (size_t y = 0; y < height; ++y) middle[y*width + width/2] = 65000;

        ImageStack stack;
        stack.addImage(makeImage(bright, params));
        stack.addImage(makeImage(middle, params));
        stack.addImage(makeImage(dark, params));
        stack.generateMask();
        for (size_t y = 0; y < height; ++y)
            for (size_t x = width/2; x < width; ++x)
                stack.getMask()(x, y) = 2;

        Array2D<float> result = stack.compose(params, 6, false, true);
        for (size_t y = 0; y < height; ++y)
            for (size_t x = 0; x < width; ++x)
                ok &= check(result(x, y) >= 9999.0f && result(x, y) <= 60001.0f,
                            "one-hot feathering introduced an unselected layer");
    }

    // Tukey/inverse-variance averaging must reject a gross temporal outlier.
    {
        std::vector<uint16_t> outlier(width*height, 60000);
        std::vector<uint16_t> sampleA(width*height, 1010);
        std::vector<uint16_t> sampleB(width*height, 1000);
        ImageStack stack;
        stack.addImage(makeImage(outlier, params));
        stack.addImage(makeImage(sampleA, params));
        stack.addImage(makeImage(sampleB, params));
        stack.generateMask();
        Array2D<float> result = stack.compose(params, 0, true, true);
        ok &= check(result(width/2, height/2) >= 990.0f && result(width/2, height/2) <= 1020.0f,
                    "robust temporal average did not reject an outlier");
    }

    // An isolated discrepancy is noise; a spatially supported region is motion.
    {
        std::vector<uint16_t> candidate(width*height, 1100);
        std::vector<uint16_t> reference(width*height, 1000);
        candidate[3*width + 3] = 5000; // isolated hot/noisy sample
        for (size_t y = 10; y <= 16; ++y)
            for (size_t x = 12; x <= 18; ++x)
                candidate[y*width + x] = 5000;

        ImageStack stack;
        stack.addImage(makeImage(candidate, params));
        stack.addImage(makeImage(reference, params));
        stack.generateMask(12);
        ok &= check(stack.getImageAt(3, 3) == 0,
                    "isolated noise was classified as coherent motion");
        ok &= check(stack.getImageAt(15, 13) == 1,
                    "spatially supported motion did not select the reference exposure");
    }

    if (ok) std::cout << "Merge quality tests passed." << std::endl;
    return ok ? 0 : 1;
}
