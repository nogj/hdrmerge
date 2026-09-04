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
    ok &= check(SaveOptions().averageSamples,
                "noise-aware exposure fusion is not enabled by default");

    // Saturation confidence must fade continuously before clipping.
    {
        std::vector<uint16_t> low(width*height, 800);
        std::vector<uint16_t> transition(width*height, 925);
        std::vector<uint16_t> clipped(width*height, 1000);
        Image lowImage = makeImage(low, params);
        Image transitionImage = makeImage(transition, params);
        Image clippedImage = makeImage(clipped, params);
        lowImage.setSaturationThreshold(1000);
        transitionImage.setSaturationThreshold(1000);
        clippedImage.setSaturationThreshold(1000);
        ok &= check(lowImage.saturationWeightAround(width/2, height/2) == 1.0,
                    "safe samples lost saturation confidence");
        ok &= check(std::abs(transitionImage.saturationWeightAround(width/2, height/2) - 0.5) < 0.01,
                    "saturation confidence is not a smooth transition");
        ok &= check(clippedImage.saturationWeightAround(width/2, height/2) == 0.0,
                    "clipped samples retained saturation confidence");
    }

    // A channel that never clips must not drag the shared saturation threshold
    // down to an ordinary flat scene value.
    {
        const size_t saturationWidth = 128, saturationHeight = 128;
        RawParameters saturationParams = makeParameters(saturationWidth, saturationHeight);
        saturationParams.max = 4095;
        std::vector<uint16_t> pixels(saturationWidth*saturationHeight);
        for (size_t y = 0; y < saturationHeight; ++y) {
            for (size_t x = 0; x < saturationWidth; ++x) {
                const int color = saturationParams.FC(x, y);
                pixels[y*saturationWidth + x] = color == 0 ? 4095 : 30 + 10 * color;
            }
        }
        ImageStack stack;
        stack.addImage(makeImage(pixels, saturationParams));
        stack.calculateSaturationLevel(saturationParams);
        ok &= check(stack.getSaturationThreshold() > 4000,
                    "an unclipped CFA channel collapsed the white level");
    }

    // Recover one common exposure ratio from a smooth raw gradient. A local
    // outlier occupies several tiles but must not bias the global estimate.
    {
        const size_t responseWidth = 192, responseHeight = 128;
        RawParameters responseParams = makeParameters(responseWidth, responseHeight);
        std::vector<uint16_t> bright(responseWidth*responseHeight);
        std::vector<uint16_t> dark(responseWidth*responseHeight);
        std::vector<double> radiance(responseWidth*responseHeight);
        for (size_t y = 0; y < responseHeight; ++y) {
            for (size_t x = 0; x < responseWidth; ++x) {
                const size_t pos = y*responseWidth + x;
                const double dx = static_cast<double>(x) - responseWidth / 2.0;
                const double dy = static_cast<double>(y) - responseHeight / 2.0;
                radiance[pos] = 500.0 + 150.0 * std::sqrt(dx*dx + dy*dy);
                dark[pos] = static_cast<uint16_t>(std::round(radiance[pos]));
                bright[pos] = static_cast<uint16_t>(std::min(65535.0,
                    std::round(4.0 * radiance[pos])));
                if (x >= 20 && x < 44 && y >= 20 && y < 44 && bright[pos] < 50000)
                    bright[pos] = static_cast<uint16_t>(bright[pos] * 1.20);
            }
        }

        Image brighter = makeImage(bright, responseParams);
        Image darker = makeImage(dark, responseParams);
        brighter.setSaturationThreshold(60000);
        darker.setSaturationThreshold(60000);
        ImageStack stack;
        stack.addImage(std::move(brighter));
        stack.addImage(std::move(darker));
        stack.computeResponseFunctions();
        ok &= check(std::abs(stack.getImage(0).getRelativeExposure() - 0.25) < 0.001,
                    "global exposure estimate was biased by a local outlier");

        stack.generateMask();
        Array2D<float> result = stack.compose(responseParams, 3, false, true);
        for (size_t y = 0; y < responseHeight; ++y) {
            for (size_t x = 0; x < responseWidth; ++x) {
                if (x >= 16 && x < 48 && y >= 16 && y < 48) continue;
                const size_t pos = y*responseWidth + x;
                ok &= check(std::abs(result(x, y) - radiance[pos]) / radiance[pos] < 0.003,
                            "exposure switch left a discontinuity in a smooth gradient");
            }
        }
    }

    // All pair constraints must agree on one scale per image rather than
    // accumulating error through an adjacent-image chain.
    {
        const size_t responseWidth = 96, responseHeight = 64;
        RawParameters responseParams = makeParameters(responseWidth, responseHeight);
        std::vector<uint16_t> exposure8(responseWidth*responseHeight);
        std::vector<uint16_t> exposure2(responseWidth*responseHeight);
        std::vector<uint16_t> exposure1(responseWidth*responseHeight);
        for (size_t y = 0; y < responseHeight; ++y) {
            for (size_t x = 0; x < responseWidth; ++x) {
                const size_t pos = y*responseWidth + x;
                const uint16_t value = static_cast<uint16_t>(300 + (x*53 + y*29) % 5000);
                exposure1[pos] = value;
                exposure2[pos] = static_cast<uint16_t>(2 * value);
                exposure8[pos] = static_cast<uint16_t>(8 * value);
            }
        }

        ImageStack stack;
        stack.addImage(makeImage(exposure8, responseParams));
        stack.addImage(makeImage(exposure2, responseParams));
        stack.addImage(makeImage(exposure1, responseParams));
        stack.computeResponseFunctions();
        ok &= check(std::abs(stack.getImage(0).getRelativeExposure() - 0.125) < 0.001,
                    "global system recovered the wrong 8x exposure scale");
        ok &= check(std::abs(stack.getImage(1).getRelativeExposure() - 0.5) < 0.001,
                    "global system recovered the wrong 2x exposure scale");
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

    // Consensus/inverse-variance averaging must reject a gross temporal outlier.
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

    // With only two inconsistent observations there is no consensus: retain
    // the exposure selected by the mask instead of creating a ghosted blend.
    {
        std::vector<uint16_t> selected(width*height, 5000);
        std::vector<uint16_t> conflicting(width*height, 1000);
        ImageStack stack;
        stack.addImage(makeImage(selected, params));
        stack.addImage(makeImage(conflicting, params));
        stack.generateMask();
        Array2D<float> result = stack.compose(params, 0, true, true);
        ok &= check(std::abs(result(width/2, height/2) - 5000.0f) < 1.0f,
                    "inconsistent exposures were blended without consensus");
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
