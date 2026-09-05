#include "CfaAlignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef HDRMERGE_HAVE_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#endif

using namespace hdrmerge;

AlignmentTransform::AlignmentTransform() : confidence(0.0), valid(false) {
    m[0][0] = 1.0; m[0][1] = 0.0; m[0][2] = 0.0;
    m[1][0] = 0.0; m[1][1] = 1.0; m[1][2] = 0.0;
}

bool hdrmerge::isSupportedCfaForWarp(const CFAPattern & pattern) {
    return pattern.getRows() == 2 && pattern.getColumns() == 2;
}

#ifdef HDRMERGE_HAVE_OPENCV
namespace {

cv::Mat registrationPreview(const Array2D<uint16_t> & image, int factor) {
    const int width = static_cast<int>(image.getWidth()) / factor;
    const int height = static_cast<int>(image.getHeight()) / factor;
    cv::Mat preview(height, width, CV_32F);

    double sum = 0.0;
    for (int y = 0; y < height; ++y) {
        float * row = preview.ptr<float>(y);
        for (int x = 0; x < width; ++x) {
            uint32_t value = 0;
            for (int yy = 0; yy < factor; ++yy)
                for (int xx = 0; xx < factor; ++xx)
                    value += image[x * factor + xx + (y * factor + yy) * image.getWidth()];
            row[x] = static_cast<float>(value) / static_cast<float>(factor * factor);
            sum += row[x];
        }
    }

    // Log luminance and mean normalization make registration robust to EV gaps.
    const double mean = sum / std::max(1, width * height);
    const float scale = mean > 0.0 ? static_cast<float>(1.0 / mean) : 1.0f;
    preview *= scale;
    cv::log(preview + 1.0f, preview);
    cv::GaussianBlur(preview, preview, cv::Size(5, 5), 0.0);
    return preview;
}

bool sensibleTransform(const cv::Mat & warp, int width, int height, AlignmentMode mode,
                       std::string & reason) {
    const double a = warp.at<float>(0, 0);
    const double b = warp.at<float>(0, 1);
    const double c = warp.at<float>(1, 0);
    const double d = warp.at<float>(1, 1);
    const double tx = warp.at<float>(0, 2);
    const double ty = warp.at<float>(1, 2);
    const double det = a * d - b * c;
    if (!std::isfinite(det) || !std::isfinite(tx) || !std::isfinite(ty)) {
        reason = "registration produced non-finite values";
        return false;
    }
    if (std::abs(tx) > width * 0.20 || std::abs(ty) > height * 0.20) {
        reason = "estimated displacement exceeds 20% of the image";
        return false;
    }
    if (mode == AlignmentMode::Affine) {
        const double scaleX = std::sqrt(a * a + c * c);
        const double scaleY = std::sqrt(b * b + d * d);
        const double angle = std::atan2(c, a) * 180.0 / 3.14159265358979323846;
        if (det <= 0.0 || scaleX < 0.97 || scaleX > 1.03 ||
            scaleY < 0.97 || scaleY > 1.03 || std::abs(angle) > 5.0) {
            reason = "estimated affine transform exceeds safe scale/rotation limits";
            return false;
        }
    }
    return true;
}

} // namespace
#endif

AlignmentTransform hdrmerge::estimateAlignmentTransform(const Array2D<uint16_t> & source,
                                                          const Array2D<uint16_t> & reference,
                                                          int integerDx, int integerDy,
                                                          AlignmentMode mode) {
    AlignmentTransform result;
    result.m[0][2] = -integerDx;
    result.m[1][2] = -integerDy;

    if (mode == AlignmentMode::Integer) {
        result.valid = true;
        result.confidence = 1.0;
        result.message = "integer alignment requested";
        return result;
    }

#ifndef HDRMERGE_HAVE_OPENCV
    result.message = "OpenCV is unavailable";
    return result;
#else
    if (source.getWidth() != reference.getWidth() || source.getHeight() != reference.getHeight()) {
        result.message = "source and reference dimensions differ";
        return result;
    }

    // A quarter-size preview is detailed enough for brackets and keeps ECC fast.
    const int factor = 4;
    if (source.getWidth() < 128 || source.getHeight() < 128) {
        result.message = "image is too small for subpixel refinement";
        return result;
    }

    try {
        cv::Mat input = registrationPreview(source, factor);
        cv::Mat target = registrationPreview(reference, factor);
        cv::Mat warp = cv::Mat::eye(2, 3, CV_32F);
        warp.at<float>(0, 2) = static_cast<float>(-integerDx) / factor;
        warp.at<float>(1, 2) = static_cast<float>(-integerDy) / factor;

        const int motion = mode == AlignmentMode::Affine ? cv::MOTION_AFFINE : cv::MOTION_TRANSLATION;
        const cv::TermCriteria criteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 80, 1e-6);
        const double confidence = cv::findTransformECC(target, input, warp, motion, criteria,
                                                        cv::noArray(), 5);
        std::string reason;
        if (!std::isfinite(confidence) || confidence < 0.45) {
            result.message = "registration confidence is below 0.45";
            return result;
        }
        if (!sensibleTransform(warp, input.cols, input.rows, mode, reason)) {
            result.message = reason;
            return result;
        }

        // Convert the inverse map from block-centre coordinates to full resolution.
        const double centre = (factor - 1) * 0.5;
        const double a = warp.at<float>(0, 0);
        const double b = warp.at<float>(0, 1);
        const double c = warp.at<float>(1, 0);
        const double d = warp.at<float>(1, 1);
        result.m[0][0] = a; result.m[0][1] = b;
        result.m[1][0] = c; result.m[1][1] = d;
        result.m[0][2] = factor * warp.at<float>(0, 2) + centre - (a + b) * centre;
        result.m[1][2] = factor * warp.at<float>(1, 2) + centre - (c + d) * centre;
        result.confidence = confidence;
        result.valid = true;
        result.message = "ECC refinement succeeded";
    } catch (const cv::Exception & e) {
        result.message = e.what();
    }
    return result;
#endif
}

void hdrmerge::resampleCfa(const Array2D<uint16_t> & source,
                           Array2D<uint16_t> & destination,
                           Array2D<uint8_t> & validity,
                           const CFAPattern & pattern,
                           const AlignmentTransform & transform,
                           Array2D<float> * interpolationVariance) {
    const int width = static_cast<int>(source.getWidth());
    const int height = static_cast<int>(source.getHeight());
    destination.resize(width, height);
    validity.resize(width, height);
    if (interpolationVariance) interpolationVariance->resize(width, height);

    #pragma omp parallel for schedule(dynamic, 16)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const double sx = transform.m[0][0] * x + transform.m[0][1] * y + transform.m[0][2];
            const double sy = transform.m[1][0] * x + transform.m[1][1] * y + transform.m[1][2];

            // Each Bayer phase is a separate image sampled on a 2x2 lattice.
            const int phaseX = x & 1;
            const int phaseY = y & 1;
            const double planeX = (sx - phaseX) * 0.5;
            const double planeY = (sy - phaseY) * 0.5;
            const int x0p = static_cast<int>(std::floor(planeX));
            const int y0p = static_cast<int>(std::floor(planeY));
            const double fx = planeX - x0p;
            const double fy = planeY - y0p;
            const int x1p = x0p + (fx > 1e-9 ? 1 : 0);
            const int y1p = y0p + (fy > 1e-9 ? 1 : 0);
            const int x0 = 2 * x0p + phaseX;
            const int x1 = 2 * x1p + phaseX;
            const int y0 = 2 * y0p + phaseY;
            const int y1 = 2 * y1p + phaseY;

            const bool inside = x0 >= 0 && x1 < width && y0 >= 0 && y1 < height &&
                pattern(x0, y0) == pattern(x, y) && pattern(x1, y0) == pattern(x, y) &&
                pattern(x0, y1) == pattern(x, y) && pattern(x1, y1) == pattern(x, y);
            const size_t pos = static_cast<size_t>(y) * width + x;
            if (!inside) {
                destination[pos] = 0;
                validity[pos] = 0;
                if (interpolationVariance) (*interpolationVariance)[pos] = 0.0f;
                continue;
            }

            const double top = source[static_cast<size_t>(y0) * width + x0] * (1.0 - fx) +
                               source[static_cast<size_t>(y0) * width + x1] * fx;
            const double bottom = source[static_cast<size_t>(y1) * width + x0] * (1.0 - fx) +
                                  source[static_cast<size_t>(y1) * width + x1] * fx;
            const double value = top * (1.0 - fy) + bottom * fy;
            destination[pos] = static_cast<uint16_t>(std::max(0.0, std::min(65535.0, std::round(value))));
            validity[pos] = 1;
            if (interpolationVariance) {
                // Variance of the bilinear sampling footprint in full-resolution
                // pixel coordinates. It is zero for an exact CFA sample.
                (*interpolationVariance)[pos] = static_cast<float>(
                    4.0 * (fx * (1.0 - fx) + fy * (1.0 - fy)));
            }
        }
    }
}
