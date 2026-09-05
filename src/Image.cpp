/*
 *  HDRMerge - HDR exposure merging software.
 *  Copyright 2012 Javier Celaya
 *  jcelaya@gmail.com
 *
 *  This file is part of HDRMerge.
 *
 *  HDRMerge is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  HDRMerge is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with HDRMerge. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "Image.hpp"
#include "Bitmap.hpp"
#include "Histogram.hpp"
#include "Log.hpp"
#include "RawParameters.hpp"
#include "CfaAlignment.hpp"
#include <algorithm>
#include <cmath>
#include <vector>
using namespace std;
using namespace hdrmerge;

namespace {

double median(vector<double> values) {
    if (values.empty()) return 0.0;
    const size_t middle = values.size() / 2;
    nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() & 1) return upper;
    nth_element(values.begin(), values.begin() + middle - 1, values.begin() + middle);
    return 0.5 * (values[middle - 1] + upper);
}

} // namespace


void Image::ResponseFunction::setLinear(double slope) {
    threshold = 65535;
    linear = slope;
    alglib::real_1d_array x = "[0.0, 0.0]";
    alglib::real_1d_array f = "[0.0, 65535.0]";
    x[1] = 65535.0 / linear;
    alglib::spline1dbuildlinear(x, f, 2, nonLinear);
}


void Image::buildImage(uint16_t * rawImage, const RawParameters & params) {
    resize(params.width, params.height);
    size_t size = width*height;
    brightness = 0.0;
    max = 0;
    for (size_t y = 0, ry = params.topMargin; y < height; ++y, ++ry) {
        for (size_t x = 0, rx = params.leftMargin; x < width; ++x, ++rx) {
            uint16_t v = rawImage[ry*params.rawWidth + rx];
            (*this)(x, y) = v;
            brightness += v;
            if (v > max) max = v;
        }
    }
    brightness /= size;
    cfaPattern = params.FC;
    response.setLinear(params.max == 0 ? 1.0 : 65535.0 / params.max);
    subtractBlack(params);
}


Image & Image::operator=(Image && move) {
    *static_cast<Array2D<uint16_t> *>(this) = (Array2D<uint16_t> &&)std::move(move);
    filename = move.filename;
    scaled.swap(move.scaled);
    satThreshold = move.satThreshold;
    max = move.max;
    brightness = move.brightness;
    response = move.response;
    cfaPattern = move.cfaPattern;
    halfLightPercent = move.halfLightPercent;
    validity = std::move(move.validity);
    interpolationVariance = std::move(move.interpolationVariance);
    warped = move.warped;
    alignmentX = move.alignmentX;
    alignmentY = move.alignmentY;
    alignmentConfidence = move.alignmentConfidence;
    alignmentRotation = move.alignmentRotation;
    responseScatter = move.responseScatter;
    return *this;
}


void Image::setSaturationThreshold(uint16_t sat) {
    satThreshold = sat;
    response.threshold = 0.9*sat;
}


void Image::subtractBlack(const RawParameters & params) {
    if (params.hasBlack()) {
        for (size_t y = 0, pos = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x, ++pos) {
                if ((*this)[pos] > params.blackAt(x, y)) {
                    (*this)[pos] -= params.blackAt(x, y);
                } else {
                    (*this)[pos] = 0;
                }
            }
        }
    }
}


double Image::getRelativeExposure() const {
    return response.linear;
}


void Image::setRelativeExposure(double scale, double scatter) {
    response.setLinear(scale);
    responseScatter = std::max(0.0, scatter);
}


double Image::radianceGradientSquared(size_t x, size_t y) const {
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    double gx = 0.0, gy = 0.0;
    if (contains(ix - 2, iy) && contains(ix + 2, iy))
        gx = (exposureAt(x + 2, y) - exposureAt(x - 2, y)) * 0.25;
    else if (contains(ix + 2, iy))
        gx = (exposureAt(x + 2, y) - exposureAt(x, y)) * 0.5;
    else if (contains(ix - 2, iy))
        gx = (exposureAt(x, y) - exposureAt(x - 2, y)) * 0.5;

    if (contains(ix, iy - 2) && contains(ix, iy + 2))
        gy = (exposureAt(x, y + 2) - exposureAt(x, y - 2)) * 0.25;
    else if (contains(ix, iy + 2))
        gy = (exposureAt(x, y + 2) - exposureAt(x, y)) * 0.5;
    else if (contains(ix, iy - 2))
        gy = (exposureAt(x, y) - exposureAt(x, y - 2)) * 0.5;
    return gx * gx + gy * gy;
}


double Image::interpolationVarianceAt(size_t x, size_t y) const {
    return warped && interpolationVariance.contains(x, y) ? interpolationVariance(x, y) : 0.0;
}


Image::ExposureRatioEstimate Image::estimateExposureRatio(const Image & r) const {
    ExposureRatioEstimate estimate;
    const int left = std::max(dx, r.dx);
    const int top = std::max(dy, r.dy);
    const int right = std::min(dx + static_cast<int>(width), r.dx + static_cast<int>(r.width));
    const int bottom = std::min(dy + static_cast<int>(height), r.dy + static_cast<int>(r.height));
    if (left >= right || top >= bottom) return estimate;

    // Equal-area tiles prevent a moving or highly textured region from
    // dominating the exposure estimate. Sampling is capped for large raws.
    const int grid = 16;
    const int tileWidth = std::max(1, (right - left + grid - 1) / grid);
    const int tileHeight = std::max(1, (bottom - top + grid - 1) / grid);
    const double area = static_cast<double>(right - left) * (bottom - top);
    const int stride = std::max(1, static_cast<int>(std::sqrt(area / 300000.0)));
    const double noiseFloor = std::max(32.0, 0.002 * std::min(satThreshold, r.satThreshold));
    const double thisSafeMax = 0.90 * satThreshold;
    const double referenceSafeMax = 0.90 * r.satThreshold;
    vector<double> tileRatios;
    vector<size_t> tileSamples;

    for (int tileY = top; tileY < bottom; tileY += tileHeight) {
        const int tileBottom = std::min(bottom, tileY + tileHeight);
        for (int tileX = left; tileX < right; tileX += tileWidth) {
            const int tileRight = std::min(right, tileX + tileWidth);
            vector<double> ratios;
            ratios.reserve((tileRight - tileX) * (tileBottom - tileY) /
                           std::max(1, stride * stride));
            for (int y = tileY; y < tileBottom; y += stride) {
                for (int x = tileX; x < tileRight; x += stride) {
                    if (!contains(x, y) || !r.contains(x, y)) continue;
                    const int color = cfaPattern(x - dx, y - dy);
                    const int referenceColor = r.cfaPattern(x - r.dx, y - r.dy);
                    if (color != referenceColor) continue;
                    const double value = (*this)(x, y);
                    const double reference = r(x, y);
                    if (value <= noiseFloor || reference <= noiseFloor ||
                        value >= thisSafeMax || reference >= referenceSafeMax) continue;
                    ratios.push_back(std::log(reference / value));
                }
            }
            if (ratios.size() >= 12) {
                tileSamples.push_back(ratios.size());
                tileRatios.push_back(median(std::move(ratios)));
            }
        }
    }
    if (tileRatios.size() < 3) return estimate;

    double centre = median(tileRatios);
    vector<double> deviations;
    deviations.reserve(tileRatios.size());
    for (double ratio : tileRatios) deviations.push_back(std::abs(ratio - centre));
    const double sigma = std::max(0.001, 1.4826 * median(std::move(deviations)));

    // One Huber refinement improves sub-percent precision while retaining the
    // tile median's resistance to motion and local illumination changes.
    double weighted = 0.0;
    double weights = 0.0;
    size_t usedSamples = 0;
    size_t usedTiles = 0;
    const double cutoff = 2.5 * sigma;
    for (size_t i = 0; i < tileRatios.size(); ++i) {
        const double residual = std::abs(tileRatios[i] - centre);
        if (residual > 6.0 * sigma) continue;
        const double weight = residual > cutoff ? cutoff / residual : 1.0;
        weighted += weight * tileRatios[i];
        weights += weight;
        usedSamples += tileSamples[i];
        ++usedTiles;
    }
    if (usedTiles < 3 || weights <= 0.0) return estimate;

    estimate.logRatio = weighted / weights;
    estimate.logScatter = sigma;
    estimate.tiles = usedTiles;
    estimate.samples = usedSamples;
    estimate.weight = usedTiles / (sigma * sigma);
    return estimate;
}


void Image::computeResponseFunction(const Image & r) {
    const ExposureRatioEstimate estimate = estimateExposureRatio(r);
    if (estimate.valid())
        setRelativeExposure(r.getRelativeExposure() * std::exp(estimate.logRatio),
                            std::hypot(r.getResponseScatter(), estimate.logScatter));
}


size_t Image::alignWith(const Image & r) {
    dx = dy = 0;
    const double tolerance = 1.0/16;
    Histogram histFull(begin(), end());
    double halfLightPercent = histFull.getFraction(satThreshold) / 2.0;
    size_t totalError = 0;
    for (int s = scaleSteps - 1; s >= 0; --s) {
        size_t curWidth = width >> (s + 1);
        size_t curHeight = height >> (s + 1);
        size_t minError = curWidth*curHeight;
        Histogram hist1(r.scaled[s].begin(), r.scaled[s].end());
        Histogram hist2(scaled[s].begin(), scaled[s].end());
        uint16_t mth1 = hist1.getPercentile(halfLightPercent);
        uint16_t mth2 = hist2.getPercentile(halfLightPercent);
        uint16_t tolPixels1 = (uint16_t)std::floor(mth1*tolerance);
        uint16_t tolPixels2 = (uint16_t)std::floor(mth2*tolerance);
        Bitmap mtb1(curWidth, curHeight), mtb2(curWidth, curHeight),
        excl1(curWidth, curHeight), excl2(curWidth, curHeight);
        mtb1.mtb(r.scaled[s].begin(), mth1);
        mtb2.mtb(scaled[s].begin(), mth2);
        excl1.exclusion(r.scaled[s].begin(), mth1, tolPixels1);
        excl2.exclusion(scaled[s].begin(), mth2, tolPixels2);
        Bitmap shiftMtb(curWidth, curHeight), shiftExcl(curWidth, curHeight);
        int curDx = dx, curDy = dy;
        for (int i = -1; i <= 1; ++i) {
            for (int j = -1; j <= 1; ++j) {
                shiftMtb.shift(mtb2, curDx + i, curDy + j);
                shiftExcl.shift(excl2, curDx + i, curDy + j);
                shiftMtb.bitwiseXor(mtb1);
                shiftMtb.bitwiseAnd(excl1);
                shiftMtb.bitwiseAnd(shiftExcl);
                size_t err = shiftMtb.count();
                if (err < minError) {
                    dx = curDx + i;
                    dy = curDy + j;
                    minError = err;
                }
            }
        }
        dx <<= 1;
        dy <<= 1;
        totalError += minError;
    }
    alignmentX = dx;
    alignmentY = dy;
    alignmentConfidence = 1.0;
    alignmentRotation = 0.0;
    return totalError;
}


bool Image::refineAlignment(const Image & reference, const RawParameters & params, AlignmentMode mode,
                            std::string & reason) {
    if (mode == AlignmentMode::Integer) return true;
    if (!isSupportedCfaForWarp(params.FC)) {
        reason = "subpixel CFA resampling currently supports 2x2 Bayer sensors only";
        return false;
    }
    AlignmentTransform transform = estimateAlignmentTransform(*this, reference, dx, dy, mode);
    if (!transform.valid) {
        reason = transform.message;
        return false;
    }

    Array2D<uint16_t> resampled;
    Array2D<uint8_t> validMap;
    Array2D<float> interpolationMap;
    resampleCfa(*this, resampled, validMap, params.FC, transform, &interpolationMap);
    *static_cast<Array2D<uint16_t> *>(this) = std::move(resampled);
    validity = std::move(validMap);
    interpolationVariance = std::move(interpolationMap);
    warped = true;
    alignmentX = transform.placementX();
    alignmentY = transform.placementY();
    alignmentConfidence = transform.confidence;
    alignmentRotation = std::atan2(transform.m[1][0], transform.m[0][0]) * 180.0 /
        3.14159265358979323846;
    reason = transform.message;
    return true;
}


bool Image::contains(int x, int y) const {
    if (!Array2D<uint16_t>::contains(x, y)) return false;
    return !warped || validity(x, y) != 0;
}


void Image::displace(int newDx, int newDy) {
    Array2D<uint16_t>::displace(newDx, newDy);
    if (warped) validity.displace(newDx, newDy);
    if (warped) interpolationVariance.displace(newDx, newDy);
}


void Image::getValidBounds(int & left, int & top, int & right, int & bottom) const {
    if (!warped) {
        left = dx;
        top = dy;
        right = dx + static_cast<int>(width);
        bottom = dy + static_cast<int>(height);
        return;
    }
    int rawLeft = static_cast<int>(width), rawTop = static_cast<int>(height);
    int rawRight = 0, rawBottom = 0;
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            if (validity[static_cast<size_t>(y) * width + x]) {
                rawLeft = std::min(rawLeft, x);
                rawTop = std::min(rawTop, y);
                rawRight = std::max(rawRight, x + 1);
                rawBottom = std::max(rawBottom, y + 1);
            }
        }
    }
    left = rawLeft + dx;
    top = rawTop + dy;
    right = rawRight + dx;
    bottom = rawBottom + dy;
}


void Image::preScale() {
    size_t curWidth = width;
    size_t curHeight = height;
    Array2D<uint16_t> * r2 = this;

    scaled.reset(new Array2D<uint16_t>[scaleSteps]);
    for (int s = 0; s < scaleSteps; ++s) {
        scaled[s].resize(curWidth >>= 1, curHeight >>= 1);
        for (size_t y = 0, prevY = 0; y < curHeight; ++y, prevY += 2) {
            for (size_t x = 0, prevX = 0; x < curWidth; ++x, prevX += 2) {
                uint32_t value1 = (*r2)(prevX, prevY),
                    value2 = (*r2)(prevX + 1, prevY),
                    value3 = (*r2)(prevX, prevY + 1),
                    value4 = (*r2)(prevX + 1, prevY + 1);
                scaled[s](x, y) = (value1 + value2 + value3 + value4) >> 2;
            }
        }
        r2 = &scaled[s];
    }
}


uint16_t Image::getMaxAround(size_t x, size_t y) const {
    uint16_t result = 0;
    for (int yy = static_cast<int>(y) - 1; yy <= static_cast<int>(y) + 1; ++yy) {
        for (int xx = static_cast<int>(x) - 1; xx <= static_cast<int>(x) + 1; ++xx) {
            if (contains(xx, yy)) result = std::max(result, (*this)(xx, yy));
        }
    }
    return result;
}


double Image::saturationWeightAround(size_t x, size_t y) const {
    if (satThreshold == 0) return 0.0;
    const double value = getMaxAround(x, y);
    const double fadeStart = 0.85 * satThreshold;
    if (value <= fadeStart) return 1.0;
    if (value >= satThreshold) return 0.0;
    const double remaining = (satThreshold - value) / (satThreshold - fadeStart);
    return remaining * remaining * (3.0 - 2.0 * remaining);
}
