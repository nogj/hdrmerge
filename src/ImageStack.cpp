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

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "BoxBlur.hpp"
#include "ImageStack.hpp"
#include "Log.hpp"
#include "RawParameters.hpp"

#ifdef __SSE2__
    #include <x86intrin.h>
#endif

using namespace std;
using namespace hdrmerge;

namespace {

struct ExposureEdge {
    size_t first;
    size_t second;
    double difference;
    double scatter;
    double weight;
    double robustWeight;
};

bool solveExposureSystem(size_t imageCount, const vector<ExposureEdge> & edges,
                         vector<double> & solution) {
    const size_t unknowns = imageCount - 1; // The darkest image anchors the scale.
    vector<vector<double>> matrix(unknowns, vector<double>(unknowns, 0.0));
    vector<double> rhs(unknowns, 0.0);
    for (const ExposureEdge & edge : edges) {
        const double weight = edge.weight * edge.robustWeight;
        if (edge.first < unknowns) {
            matrix[edge.first][edge.first] += weight;
            rhs[edge.first] += weight * edge.difference;
        }
        if (edge.second < unknowns) {
            matrix[edge.second][edge.second] += weight;
            rhs[edge.second] -= weight * edge.difference;
        }
        if (edge.first < unknowns && edge.second < unknowns) {
            matrix[edge.first][edge.second] -= weight;
            matrix[edge.second][edge.first] -= weight;
        }
    }

    for (size_t column = 0; column < unknowns; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1; row < unknowns; ++row)
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) pivot = row;
        if (std::abs(matrix[pivot][column]) < 1e-12) return false;
        if (pivot != column) {
            std::swap(matrix[pivot], matrix[column]);
            std::swap(rhs[pivot], rhs[column]);
        }
        const double divisor = matrix[column][column];
        for (size_t c = column; c < unknowns; ++c) matrix[column][c] /= divisor;
        rhs[column] /= divisor;
        for (size_t row = 0; row < unknowns; ++row) {
            if (row == column) continue;
            const double factor = matrix[row][column];
            for (size_t c = column; c < unknowns; ++c)
                matrix[row][c] -= factor * matrix[column][c];
            rhs[row] -= factor * rhs[column];
        }
    }
    solution.assign(imageCount, 0.0);
    std::copy(rhs.begin(), rhs.end(), solution.begin());
    return true;
}

double medianAbsoluteResidual(const vector<ExposureEdge> & edges,
                              const vector<double> & solution) {
    vector<double> residuals;
    residuals.reserve(edges.size());
    for (const ExposureEdge & edge : edges)
        residuals.push_back(std::abs(solution[edge.first] - solution[edge.second] - edge.difference));
    if (residuals.empty()) return 0.0;
    const size_t middle = residuals.size() / 2;
    nth_element(residuals.begin(), residuals.begin() + middle, residuals.end());
    return residuals[middle];
}

} // namespace


int ImageStack::addImage(Image && i) {
    if (images.empty()) {
        width = i.getWidth();
        height = i.getHeight();
    }
    images.push_back(std::move(i));
    int n = images.size() - 1;
    while (n > 0 && images[n] < images[n - 1]) {
        std::swap(images[n], images[n - 1]);
        --n;
    }
    return n;
}


void ImageStack::calculateSaturationLevel(const RawParameters & params, bool useCustomWl) {
    // Calculate max value of brightest image and assume it is saturated
    Image& brightest = images.front();

    std::vector<std::vector<size_t>> histograms(4, std::vector<size_t>(brightest.getMax() + 1));

    #pragma omp parallel
    {
        std::vector<std::vector<size_t>> histogramsThr(4, std::vector<size_t>(brightest.getMax() + 1));
        #pragma omp for schedule(dynamic,16) nowait
        for (size_t y = 0; y < height; ++y) {
            // get the color codes from x = 0 to 5, works for bayer and xtrans
            uint16_t fcrow[6];
            for (size_t i = 0; i < 6; ++i) {
                fcrow[i] = params.FC(i, y);
            }
            size_t x = 0;
            for (; x < width - 5; x+=6) {
                for(size_t j = 0; j < 6; ++j) {
                    uint16_t v = brightest(x + j, y);
                    ++histogramsThr[fcrow[j]][v];
                }
            }
            // remaining pixels
            for (size_t j = 0; x < width; ++x, ++j) {
                uint16_t v = brightest(x, y);
                ++histogramsThr[fcrow[j]][v];
            }
        }
        #pragma omp critical
        {
            for (int c = 0; c < 4; ++c) {
                for (std::vector<size_t>::size_type i = 0; i < histograms[c].size(); ++i) {
                    histograms[c][i] += histogramsThr[c][i];
                }
            }
        }
    }

    const size_t threshold = width * height / 10000;

    uint16_t maxPerColor[4] = {0, 0, 0, 0};

    for (int c = 0; c < 4; ++c) {
        for (int i = histograms[c].size() - 1; i >= 0; --i) {
            const size_t v = histograms[c][i];
            if (v > threshold) {
                maxPerColor[c] = i;
                break;
            }
        }
    }


    uint16_t maxPerColors = 0;
    for (int c = 0; c < params.colors; ++c) {
        maxPerColors = std::max(maxPerColors, maxPerColor[c]);
        Log::debug("Observed white candidate for channel ", c, ": ", maxPerColor[c]);
    }

    // Image data has already had its per-channel black level removed. Trust
    // LibRaw's camera white level in that same domain; a flat scene region is
    // not evidence of clipping. An observed plateau may refine it only when it
    // is already close enough to the metadata value to plausibly be clipping.
    const uint16_t metadataWhite = params.max > params.maxBlack
        ? params.max - params.maxBlack : params.max;
    satThreshold = metadataWhite > 0 ? metadataWhite : maxPerColors;
    if (metadataWhite > 0 && maxPerColors >= 0.90 * metadataWhite)
        satThreshold = std::min(metadataWhite, maxPerColors);
    else if (metadataWhite == 0) {
        satThreshold = maxPerColors;
    }

    if (!useCustomWl) { // only scale when no custom white level was specified
        satThreshold *= 0.99;
    }

    Log::debug( "Using white level ", satThreshold );

    for (auto& i : images) {
        i.setSaturationThreshold(satThreshold);
    }
}


void ImageStack::align() {
    if (images.size() > 1) {
        Timer t("Align");
        size_t errors[images.size()];
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < images.size(); ++i) {
            images[i].preScale();
        }
        const size_t reference = images.size() - 1;
        #pragma omp parallel for schedule(dynamic)
        for (size_t i = 0; i < reference; ++i) {
            errors[i] = images[i].alignWith(images[reference]);
        }
        for (size_t i = 0; i < reference; ++i) {
            Log::debug("Image ", i, " aligned to common reference at (", images[i].getDeltaX(),
                       ", ", images[i].getDeltaY(), ") with error ", errors[i]);
        }
        for (auto & i : images) {
            i.releaseAlignData();
        }
    }
}


void ImageStack::align(const RawParameters & params, AlignmentMode mode) {
    align();
    if (mode == AlignmentMode::Integer || images.size() < 2) return;

    const size_t reference = images.size() - 1;
    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < reference; ++i) {
        std::string reason;
        if (images[i].refineAlignment(images[reference], params, mode, reason)) {
            Log::debug("Image ", i, " refined with ", alignmentModeName(mode),
                       " alignment: placement (", images[i].getAlignmentX(), ", ",
                       images[i].getAlignmentY(), "), rotation ", images[i].getAlignmentRotation(),
                       " deg, confidence ", images[i].getAlignmentConfidence());
        } else {
            Log::progress("Warning: image ", i, ": ", alignmentModeName(mode),
                          " alignment rejected; keeping integer alignment (", reason, ").");
        }
    }
}

void ImageStack::crop() {
    if (images.empty()) return;
    int left = std::numeric_limits<int>::min();
    int top = std::numeric_limits<int>::min();
    int right = std::numeric_limits<int>::max();
    int bottom = std::numeric_limits<int>::max();
    for (const auto & image : images) {
        int il, it, ir, ib;
        image.getValidBounds(il, it, ir, ib);
        left = max(left, il);
        top = max(top, it);
        right = min(right, ir);
        bottom = min(bottom, ib);
    }
    if (right <= left || bottom <= top) {
        width = height = 0;
        return;
    }

    // Largest all-valid rectangle. This removes triangular affine-warp borders,
    // rather than treating their zero fill as real black sensor samples.
    const int candidateWidth = right - left;
    std::vector<int> heights(candidateWidth, 0);
    int bestArea = 0, bestLeft = left, bestTop = top, bestRight = left, bestBottom = top;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            bool valid = true;
            for (const auto & image : images) {
                if (!image.contains(x, y)) { valid = false; break; }
            }
            heights[x - left] = valid ? heights[x - left] + 1 : 0;
        }
        std::vector<int> bars;
        for (int x = 0; x <= candidateWidth; ++x) {
            const int current = x == candidateWidth ? 0 : heights[x];
            while (!bars.empty() && heights[bars.back()] > current) {
                const int index = bars.back();
                bars.pop_back();
                const int rectangleLeft = bars.empty() ? 0 : bars.back() + 1;
                const int area = heights[index] * (x - rectangleLeft);
                if (area > bestArea) {
                    bestArea = area;
                    bestLeft = left + rectangleLeft;
                    bestRight = left + x;
                    bestBottom = y + 1;
                    bestTop = bestBottom - heights[index];
                }
            }
            bars.push_back(x);
        }
    }
    width = bestRight > bestLeft ? bestRight - bestLeft : 0;
    height = bestBottom > bestTop ? bestBottom - bestTop : 0;
    for (auto & image : images) {
        image.displace(-bestLeft, -bestTop);
    }
}


void ImageStack::computeResponseFunctions() {
    Timer t("Compute response functions");
    if (images.size() < 2) return;

    vector<ExposureEdge> edges;
    for (size_t i = 0; i + 1 < images.size(); ++i) {
        for (size_t j = i + 1; j < images.size(); ++j) {
            const Image::ExposureRatioEstimate estimate = images[i].estimateExposureRatio(images[j]);
            if (!estimate.valid()) continue;
            edges.push_back({i, j, estimate.logRatio, estimate.logScatter,
                             std::min(1e8, estimate.weight), 1.0});
            Log::debug("Exposure pair ", i, " -> ", j, ": ",
                       std::exp(-estimate.logRatio), "x from ", estimate.tiles,
                       " tiles and ", estimate.samples, " samples");
        }
    }

    vector<double> relativeLogs;
    if (!solveExposureSystem(images.size(), edges, relativeLogs)) {
        Log::debug("Global exposure graph is incomplete; using adjacent robust estimates");
        for (int i = static_cast<int>(images.size()) - 2; i >= 0; --i)
            images[i].computeResponseFunction(images[i + 1]);
        return;
    }

    // A few IRLS passes suppress a pair whose overlap disagrees with the rest
    // of the stack, while retaining all consistent constraints.
    for (int iteration = 0; iteration < 4 && edges.size() > 2; ++iteration) {
        const double scale = std::max(0.001, 1.4826 * medianAbsoluteResidual(edges, relativeLogs));
        const double cutoff = 2.5 * scale;
        for (ExposureEdge & edge : edges) {
            const double residual = std::abs(relativeLogs[edge.first] -
                                             relativeLogs[edge.second] - edge.difference);
            edge.robustWeight = residual > cutoff ? cutoff / residual : 1.0;
        }
        if (!solveExposureSystem(images.size(), edges, relativeLogs)) break;
    }

    const double anchor = images.back().getRelativeExposure();
    for (size_t i = 0; i < images.size(); ++i) {
        double scatterSum = 0.0;
        double scatterWeight = 0.0;
        if (i + 1 < images.size()) {
            for (const ExposureEdge & edge : edges) {
                if (edge.first != i && edge.second != i) continue;
                const double weight = edge.weight * edge.robustWeight;
                const double residual = relativeLogs[edge.first] - relativeLogs[edge.second] -
                                        edge.difference;
                scatterSum += weight * (edge.scatter * edge.scatter + residual * residual);
                scatterWeight += weight;
            }
        }
        const double scatter = scatterWeight > 0.0 ? std::sqrt(scatterSum / scatterWeight) : 0.0;
        images[i].setRelativeExposure(anchor * std::exp(relativeLogs[i]), scatter);
        Log::debug("Global response scale ", i, ": ", images[i].getRelativeExposure(),
                   ", log scatter: ", images[i].getResponseScatter());
    }
}


void ImageStack::generateMask(int deghostThreshold) {
    Timer t("Generate mask");
    mask.resize(width, height);
    motionMask.resize(width, height);
    std::fill_n(&motionMask[0], width*height, 0);
    if(images.size() == 1) {
        // single image, fill in zero values
        std::fill_n(&mask[0], width*height, 0);
    } else {
        // multiple images, no need to prefill mask with zeroes. It will be filled correctly on the fly
        #pragma omp parallel for schedule(dynamic)
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                size_t i = 0;
                while (i < images.size() - 1 &&
                    (!images[i].contains(x, y) ||
                    images[i].isSaturatedAround(x, y))) ++i;
                mask(x, y) = i;
                if (deghostThreshold > 0 && i < images.size() - 1) {
                    const size_t reference = images.size() - 1;
                    if (images[reference].contains(x, y)) {
                        const double a = images[i].exposureAt(x, y);
                        const double b = images[reference].exposureAt(x, y);
                        const double scale = std::max(1.0, std::max(std::abs(a), std::abs(b)));
                        const double difference = 100.0 * std::abs(a - b) / scale;
                        // In deep shadows, ordinary shot/read noise can be a large
                        // percentage of the signal. Requiring a small signal-aware
                        // floor avoids turning that noise into a salt-and-pepper mask.
                        const double noiseFloor = 100.0 * 4.0 * std::sqrt(scale) / scale;
                        if (difference >= std::max<double>(deghostThreshold, noiseFloor))
                            motionMask(x, y) = 1;
                    }
                }
            }
        }

        if (deghostThreshold > 0) {
            // Enforce spatial support independently on each Bayer plane. A 3x3
            // neighbourhood with a stride of two never mixes CFA colours.
            Array2D<uint8_t> supported(width, height);
            std::fill_n(&supported[0], width*height, 0);
            #pragma omp parallel for schedule(dynamic)
            for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                    int votes = 0;
                    for (int oy = -2; oy <= 2; oy += 2) {
                        const int yy = static_cast<int>(y) + oy;
                        if (yy < 0 || yy >= static_cast<int>(height)) continue;
                        for (int ox = -2; ox <= 2; ox += 2) {
                            const int xx = static_cast<int>(x) + ox;
                            if (xx >= 0 && xx < static_cast<int>(width) && motionMask(xx, yy)) ++votes;
                        }
                    }
                    supported(x, y) = votes >= 3 ? 1 : 0;
                }
            }

            // Grow the supported core by one same-colour CFA sample. This covers
            // antialiased motion boundaries without bleeding between CFA planes.
            Array2D<uint8_t> coherent(width, height);
            std::fill_n(&coherent[0], width*height, 0);
            #pragma omp parallel for schedule(dynamic)
            for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                    bool moving = false;
                    for (int oy = -2; oy <= 2 && !moving; oy += 2) {
                        const int yy = static_cast<int>(y) + oy;
                        if (yy < 0 || yy >= static_cast<int>(height)) continue;
                        for (int ox = -2; ox <= 2; ox += 2) {
                            const int xx = static_cast<int>(x) + ox;
                            if (xx >= 0 && xx < static_cast<int>(width) && supported(xx, yy)) {
                                moving = true;
                                break;
                            }
                        }
                    }
                    coherent(x, y) = moving ? 1 : 0;
                }
            }
            motionMask = std::move(coherent);
            const uint8_t reference = static_cast<uint8_t>(images.size() - 1);
            #pragma omp parallel for schedule(dynamic)
            for (size_t y = 0; y < height; ++y)
                for (size_t x = 0; x < width; ++x)
                    if (motionMask(x, y) && images[reference].contains(x, y)) mask(x, y) = reference;
        }
    }
    // The mask can be used in compose to get the information about saturated pixels
    // but the mask can be modified in gui, so we have to make a copy to represent the original state
    origMask = mask;
}


double ImageStack::value(size_t x, size_t y) const {
    const Image & img = images[mask(x, y)];
    return img.exposureAt(x, y);
}

#ifndef __SSE2__
// From The GIMP: app/paint-funcs/paint-funcs.c:fatten_region
static Array2D<uint8_t> fattenMask(const Array2D<uint8_t> & mask, int radius) {
    Timer t("Fatten mask");
    size_t width = mask.getWidth(), height = mask.getHeight();
    Array2D<uint8_t> result(width, height);

    int circArray[2 * radius + 1]; // holds the y coords of the filter's mask
    // compute_border(circArray, radius)
    for (int i = 0; i < radius * 2 + 1; i++) {
        double tmp;
        if (i > radius)
            tmp = (i - radius) - 0.5;
        else if (i < radius)
            tmp = (radius - i) - 0.5;
        else
            tmp = 0.0;
        circArray[i] = int(std::sqrt(radius*radius - tmp*tmp));
    }
    // offset the circ pointer by radius so the range of the array
    //     is [-radius] to [radius]
    int * circ = circArray + radius;

    const uint8_t * bufArray[height + 2*radius];
    for (int i = 0; i < radius; i++) {
        bufArray[i] = &mask[0];
    }
    for (size_t i = 0; i < height; i++) {
        bufArray[i + radius] = &mask[i * width];
    }
    for (int i = 0; i < radius; i++) {
        bufArray[i + height + radius] = &mask[(height - 1) * width];
    }
    // offset the buf pointer
    const uint8_t ** buf = bufArray + radius;

    #pragma omp parallel
    {
        unique_ptr<uint8_t[]> buffer(new uint8_t[width * (radius + 1)]);
        unique_ptr<uint8_t *[]> maxArray;  // caches the largest values for each column
        maxArray.reset(new uint8_t *[width + 2 * radius]);
        for (int i = 0; i < radius; i++) {
            maxArray[i] = buffer.get();
        }
        for (size_t i = 0; i < width; i++) {
            maxArray[i + radius] = &buffer[(radius + 1) * i];
        }
        for (int i = 0; i < radius; i++) {
            maxArray[i + width + radius] = &buffer[(radius + 1) * (width - 1)];
        }
        // offset the max pointer
        uint8_t ** max = maxArray.get() + radius;

        #pragma omp for schedule(dynamic)
        for (size_t y = 0; y < height; y++) {
            uint8_t rowMax = 0;
            for (size_t x = 0; x < width; x++) { // compute max array
                max[x][0] = buf[y][x];
                for (int i = 1; i <= radius; i++) {
                    max[x][i] = std::max(std::max(max[x][i - 1], buf[y + i][x]), buf[y - i][x]);
                    rowMax = std::max(max[x][i], rowMax);
                }
            }

            uint8_t last_max = max[0][circ[-1]];
            int last_index = 1;
            for (size_t x = 0; x < width; x++) { // render scan line
                last_index--;
                if (last_index >= 0) {
                    if (last_max == rowMax) {
                        result(x, y) = rowMax;
                    } else {
                        last_max = 0;
                        for (int i = radius; i >= 0; i--)
                            if (last_max < max[x + i][circ[i]]) {
                                last_max = max[x + i][circ[i]];
                                last_index = i;
                            }
                        result(x, y) = last_max;
                    }
                } else {
                    last_index = radius;
                    last_max = max[x + radius][circ[radius]];

                    for (int i = radius - 1; i >= -radius; i--)
                        if (last_max < max[x + i][circ[i]]) {
                            last_max = max[x + i][circ[i]];
                            last_index = i;
                        }
                    result(x, y) = last_max;
                }
            }
        }
    }

    return result;
}
#else // use faster SSE version, crunch 16 bytes at once
// From The GIMP: app/paint-funcs/paint-funcs.c:fatten_region
// SSE version by Ingo Weyrich
static Array2D<uint8_t> fattenMask(const Array2D<uint8_t> & mask, int radius) {
    Timer t("Fatten mask (SSE version)");
    size_t width = mask.getWidth(), height = mask.getHeight();
    Array2D<uint8_t> result(width, height);

    int circArray[2 * radius + 1]; // holds the y coords of the filter's mask
    // compute_border(circArray, radius)
    for (int i = 0; i < radius * 2 + 1; i++) {
        double tmp;
        if (i > radius)
            tmp = (i - radius) - 0.5;
        else if (i < radius)
            tmp = (radius - i) - 0.5;
        else
            tmp = 0.0;
        circArray[i] = int(std::sqrt(radius*radius - tmp*tmp));
    }
    // offset the circ pointer by radius so the range of the array
    //     is [-radius] to [radius]
    int * circ = circArray + radius;

    const uint8_t * bufArray[height + 2*radius];
    for (int i = 0; i < radius; i++) {
        bufArray[i] = &mask[0];
    }
    for (size_t i = 0; i < height; i++) {
        bufArray[i + radius] = &mask[i * width];
    }
    for (int i = 0; i < radius; i++) {
        bufArray[i + height + radius] = &mask[(height - 1) * width];
    }
    // offset the buf pointer
    const uint8_t ** buf = bufArray + radius;

    #pragma omp parallel
    {
        uint8_t buffer[width * (radius + 1)];
        uint8_t *maxArray[radius+1];
        for (int i = 0; i <= radius; i++) {
            maxArray[i] = &buffer[i*width];
        }

        #pragma omp for schedule(dynamic,16)
        for (size_t y = 0; y < height; y++) {
            size_t x = 0;
            for (; x < width-15; x+=16) { // compute max array, use SSE to process 16 bytes at once
                __m128i lmax = _mm_loadu_si128((__m128i*)&buf[y][x]);
                if(radius<2) // max[0] is only used when radius < 2
                    _mm_storeu_si128((__m128i*)&maxArray[0][x],lmax);
                for (int i = 1; i <= radius; i++) {
                    lmax = _mm_max_epu8(_mm_loadu_si128((__m128i*)&buf[y + i][x]),lmax);
                    lmax = _mm_max_epu8(_mm_loadu_si128((__m128i*)&buf[y - i][x]),lmax);
                    _mm_storeu_si128((__m128i*)&maxArray[i][x],lmax);
                }
            }
            for (; x < width; x++) { // compute max array, remaining columns
                uint8_t lmax = buf[y][x];
                if(radius<2) // max[0] is only used when radius < 2
                    maxArray[0][x] = lmax;
                for (int i = 1; i <= radius; i++) {
                    lmax = std::max(std::max(lmax, buf[y + i][x]), buf[y - i][x]);
                    maxArray[i][x] = lmax;
                }
            }

            for (x = 0; (int)x < radius; x++) { // render scan line, first columns without SSE
                uint8_t last_max = maxArray[circ[radius]][x+radius];
                for (int i = radius - 1; i >= -(int)x; i--)
                    last_max = std::max(last_max,maxArray[circ[i]][x + i]);
                result(x, y) = last_max;
            }
            for (; x < width-15-radius+1; x += 16) { // render scan line, use SSE to process 16 bytes at once
                __m128i last_maxv = _mm_loadu_si128((__m128i*)&maxArray[circ[radius]][x+radius]);
                for (int i = radius - 1; i >= -radius; i--)
                    last_maxv = _mm_max_epu8(last_maxv,_mm_loadu_si128((__m128i*)&maxArray[circ[i]][x+i]));
                _mm_storeu_si128((__m128i*)&result(x,y),last_maxv);
            }

            for (; x < width; x++) { // render scan line, last columns without SSE
                int maxRadius = std::min(radius,(int)((int)width-1-(int)x));
                uint8_t last_max = maxArray[circ[maxRadius]][x+maxRadius];
                for (int i = maxRadius-1; i >= -radius; i--)
                    last_max = std::max(last_max,maxArray[circ[i]][x + i]);
                result(x, y) = last_max;
            }
        }
    }

    return result;
}
#endif

namespace {

struct RadianceSample {
    double value;
    double variance;
    double saturationWeight;
    double weight;
};

static double weightedMedian(std::vector<RadianceSample> samples) {
    std::sort(samples.begin(), samples.end(), [](const RadianceSample & a,
                                                 const RadianceSample & b) {
        return a.value < b.value;
    });
    double total = 0.0;
    for (const RadianceSample & sample : samples) total += sample.weight;
    double accumulated = 0.0;
    for (const RadianceSample & sample : samples) {
        accumulated += sample.weight;
        if (2.0 * accumulated >= total) return sample.value;
    }
    return samples.empty() ? 0.0 : samples.back().value;
}


static double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    return values.size() & 1 ? values[middle] : 0.5 * (values[middle - 1] + values[middle]);
}


static double legacyRadianceAverage(const std::vector<Image> & images, size_t first,
                                    size_t x, size_t y, double fallback) {
    std::vector<RadianceSample> samples;
    samples.reserve(images.size() - first);
    std::vector<double> values;
    values.reserve(images.size() - first);

    for (size_t k = first; k < images.size(); ++k) {
        if (!images[k].contains(x, y)) continue;
        const double saturationWeight = images[k].saturationWeightAround(x, y);
        if (saturationWeight <= 0.0) continue;
        const double raw = std::max(1.0, static_cast<double>(images[k](x, y)));
        const double value = images[k].exposureAt(x, y);
        double responseScale = std::abs(value) / raw;
        if (responseScale < 1e-9) responseScale = std::abs(images[k].getRelativeExposure());
        const double variance = std::max(1e-6, responseScale * responseScale * (raw + 16.0));
        samples.push_back({value, variance, saturationWeight, 0.0});
        values.push_back(value);
    }
    if (samples.size() < 2) return fallback;

    const double centre = median(values);
    const RadianceSample * anchor = &samples.front();
    for (const RadianceSample & sample : samples)
        if (std::abs(sample.value - centre) < std::abs(anchor->value - centre)) anchor = &sample;

    double weighted = 0.0;
    double weights = 0.0;
    size_t accepted = 0;
    for (const RadianceSample & sample : samples) {
        const double residual = std::abs(sample.value - anchor->value);
        const double signal = std::max(1.0, std::max(std::abs(sample.value),
                                                     std::abs(anchor->value)));
        const double cutoff = 5.0 * std::sqrt(sample.variance + anchor->variance) + 0.01 * signal;
        const double u = cutoff > 0.0 ? residual / cutoff : 0.0;
        if (u >= 1.0) continue;
        const double robust = (1.0 - u*u) * (1.0 - u*u);
        const double weight = sample.saturationWeight * robust / sample.variance;
        weighted += sample.value * weight;
        weights += weight;
        ++accepted;
    }
    return accepted >= 2 && weights > 0.0 ? weighted / weights : fallback;
}


static double robustRadianceAverage(const std::vector<Image> & images, size_t first,
                                    size_t x, size_t y, double fallback,
                                    double * outputConfidence = nullptr) {
    if (outputConfidence) *outputConfidence = 0.0;
    std::vector<RadianceSample> samples;
    samples.reserve(images.size() - first);
    for (size_t k = first; k < images.size(); ++k) {
        if (!images[k].contains(x, y)) continue;
        const double saturationWeight = images[k].saturationWeightAround(x, y);
        if (saturationWeight <= 0.0) continue;
        const double raw = std::max(1.0, static_cast<double>(images[k](x, y)));
        const double value = images[k].exposureAt(x, y);
        // Generic Poisson-Gaussian sensor model. The constant represents a
        // conservative four-DN read-noise floor; response scaling propagates
        // that variance into the common radiometric domain.
        double responseScale = std::abs(value) / raw;
        if (responseScale < 1e-9) responseScale = std::abs(images[k].getRelativeExposure());
        const double sensorVariance = responseScale * responseScale * (raw + 16.0);
        const double responseScatter = images[k].getResponseScatter();
        const double responseVariance = value * value * responseScatter * responseScatter;
        // The fractional CFA resampling phase is deterministic, not an
        // alignment uncertainty. Treating it as one creates phase-locked
        // weights in otherwise smooth regions; IRLS already rejects genuine
        // registration disagreements through the radiometric residual.
        const double variance = std::max(1e-6, sensorVariance + responseVariance);
        const double weight = saturationWeight / variance;
        samples.push_back({value, variance, saturationWeight, weight});
    }
    if (samples.size() < 2) return samples.empty() ? fallback : samples.front().value;

    double centre = weightedMedian(samples);
    for (int iteration = 0; iteration < 4; ++iteration) {
        double weighted = 0.0;
        double weights = 0.0;
        for (RadianceSample & sample : samples) {
            const double cutoff = 4.685 * std::sqrt(sample.variance);
            const double u = cutoff > 0.0 ? std::abs(sample.value - centre) / cutoff : 0.0;
            const double robust = u < 1.0 ? (1.0 - u*u) * (1.0 - u*u) : 0.0;
            sample.weight = sample.saturationWeight * robust / sample.variance;
            weighted += sample.value * sample.weight;
            weights += sample.weight;
        }
        if (weights <= 0.0) return fallback;
        const double updated = weighted / weights;
        if (std::abs(updated - centre) <= 1e-6 * std::max(1.0, std::abs(centre))) {
            centre = updated;
            break;
        }
        centre = updated;
    }

    double weightSum = 0.0;
    double squaredWeightSum = 0.0;
    double weighted = 0.0;
    for (RadianceSample & sample : samples) {
        const double cutoff = 4.685 * std::sqrt(sample.variance);
        const double u = cutoff > 0.0 ? std::abs(sample.value - centre) / cutoff : 0.0;
        const double robust = u < 1.0 ? (1.0 - u*u) * (1.0 - u*u) : 0.0;
        sample.weight = sample.saturationWeight * robust / sample.variance;
        weightSum += sample.weight;
        squaredWeightSum += sample.weight * sample.weight;
        weighted += sample.value * sample.weight;
    }
    if (weightSum <= 0.0 || squaredWeightSum <= 0.0) return fallback;
    centre = weighted / weightSum;
    const double effectiveSamples = weightSum * weightSum / squaredWeightSum;
    const double confidence = std::max(0.0, std::min(1.0, effectiveSamples - 1.0));
    if (outputConfidence) *outputConfidence = confidence;
    return fallback + confidence * (centre - fallback);
}

} // namespace


Array2D<float> ImageStack::compose(const RawParameters & params, int featherRadius,
                                   FusionMode fusionMode,
                                   bool preserveExposure,
                                   Array2D<float> * fusionConfidence) const {
    Timer t("Compose");
    Array2D<float> dst(params.rawWidth, params.rawHeight);
    dst.displace(-(int)params.leftMargin, -(int)params.topMargin);
    dst.fillBorders(0.f);
    if (fusionConfidence) {
        fusionConfidence->resize(width, height);
        std::fill_n(&(*fusionConfidence)[0], width*height, 0.0f);
    }

    Array2D<float> numerator(width, height), weightSum(width, height);
    std::fill_n(&numerator[0], width*height, 0.0f);
    std::fill_n(&weightSum[0], width*height, 0.0f);

    const size_t minDimension = std::min(width, height);
    const int safeRadius = minDimension > 2
        ? std::min<int>(std::max(0, featherRadius), static_cast<int>((minDimension - 1) / 2)) : 0;

    // Blur a one-hot selection map for every exposure independently. Unlike
    // blurring numeric layer indices, this cannot invent intermediate layers.
    for (size_t layer = 0; layer < images.size(); ++layer) {
        Array2D<uint8_t> selected(width, height);
        #pragma omp parallel for schedule(dynamic)
        for (size_t y = 0; y < height; ++y)
            for (size_t x = 0; x < width; ++x)
                selected(x, y) = mask(x, y) == layer ? 1 : 0;

        BoxBlur layerWeight(selected);
        if (safeRadius > 0) layerWeight.blur(safeRadius);

        #pragma omp parallel for schedule(dynamic,16)
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                double weight = std::max(0.0f, layerWeight(x, y));
                if (weight <= 1e-7 || !images[layer].contains(x, y)) continue;

                const size_t chosen = mask(x, y);
                const bool manuallySelected = chosen != origMask(x, y) && chosen == layer;
                if ((layer < origMask(x, y) || images[layer].isSaturatedAround(x, y)) &&
                    !manuallySelected) continue;

                const bool automaticMotion = motionMask.size() == mask.size() && motionMask(x, y) &&
                                             chosen == origMask(x, y);
                if (automaticMotion && layer != chosen) continue;

                const double value = images[layer].exposureAt(x, y);
                if (layer != chosen && images[chosen].contains(x, y)) {
                    // Radiometric guidance keeps feathering on the same side of
                    // a real object edge while tolerating small response errors.
                    const double guide = images[chosen].exposureAt(x, y);
                    const double scale = std::max(1.0, std::max(std::abs(value), std::abs(guide)));
                    const double sigma = std::max(32.0, 0.03 * scale + 3.0 * std::sqrt(scale));
                    const double ratio = std::abs(value - guide) / sigma;
                    weight /= 1.0 + ratio*ratio*ratio*ratio;
                }
                if (weight <= 1e-7) continue;
                numerator(x, y) += static_cast<float>(weight * value);
                weightSum(x, y) += static_cast<float>(weight);
            }
        }
    }

    float max = 0.0f;
    #pragma omp parallel
    {
        float maxthr = 0.0f;
        #pragma omp for schedule(dynamic,16) nowait
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                const size_t chosen = mask(x, y);
                double value;
                if (weightSum(x, y) > 1e-7f) {
                    value = numerator(x, y) / weightSum(x, y);
                } else if (images[chosen].contains(x, y)) {
                    value = images[chosen].exposureAt(x, y);
                } else {
                    value = 0.0;
                    for (const auto & image : images) {
                        if (image.contains(x, y)) { value = image.exposureAt(x, y); break; }
                    }
                }

                const bool automaticMotion = motionMask.size() == mask.size() && motionMask(x, y) &&
                                             chosen == origMask(x, y);
                const bool manuallySelected = chosen != origMask(x, y);
                if (fusionMode != FusionMode::Off && !automaticMotion && !manuallySelected) {
                    const size_t firstValid = std::max<size_t>(chosen, origMask(x, y));
                    if (fusionMode == FusionMode::Legacy) {
                        value = legacyRadianceAverage(images, firstValid, x, y, value);
                    } else {
                        double confidence = 0.0;
                        value = robustRadianceAverage(images, firstValid, x, y, value,
                                                      &confidence);
                        if (fusionConfidence) (*fusionConfidence)(x, y) = confidence;
                    }
                }

                dst(x, y) = static_cast<float>(value);
                if (value > maxthr) maxthr = static_cast<float>(value);
            }
        }
        #pragma omp critical
        if (maxthr > max) max = maxthr;
    }

    dst.displace(params.leftMargin, params.topMargin);
    // Scale to params.max and recover the black levels
    float mult = preserveExposure ? params.max / 65535.0f : (max > 0.0f ? params.max / max : 1.0f);
    #pragma omp parallel for
    for (size_t y = 0; y < params.rawHeight; ++y) {
        for (size_t x = 0; x < params.rawWidth; ++x) {
            dst(x, y) *= mult;
        }
    }

    return dst;
}
