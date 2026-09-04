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

#ifndef _IMAGE_H_
#define _IMAGE_H_

#include <memory>
#include <string>

#include <QString>

#include <interpolation.h>

#include "Array2D.hpp"
#include "CFAPattern.hpp"
#include "LoadSaveOptions.hpp"


namespace hdrmerge {

class RawParameters;

class Image : public Array2D<uint16_t> {
public:
    struct ExposureRatioEstimate {
        double logRatio = 0.0;
        double weight = 0.0;
        size_t samples = 0;
        size_t tiles = 0;

        bool valid() const { return weight > 0.0 && tiles > 0; }
    };

    static const int scaleSteps = 6;

    Image() : Array2D<uint16_t>(), warped(false), alignmentX(0.0), alignmentY(0.0),
        alignmentConfidence(1.0), alignmentRotation(0.0) {}
    Image(uint16_t * rawImage, const RawParameters & params, const QString& _filename) :
        filename(_filename), warped(false), alignmentX(0.0), alignmentY(0.0),
        alignmentConfidence(1.0), alignmentRotation(0.0)
    {
        buildImage(rawImage, params);
    }
    Image(const Image & copy) = delete;
    Image & operator=(const Image & copy) = delete;
    Image(Image && move) {
        (*this) = std::move(move);
    }
    Image & operator=(Image && move);

    const QString& getFilename() const
    {
        return filename;
    }

    bool good() const {
        return width > 0;
    }
    double exposureAt(size_t x, size_t y) const {
        return response((*this)(x, y));
    }
    uint16_t getMaxAround(size_t x, size_t y) const;
    bool isSaturated(uint16_t v) const {
        return v >= satThreshold;
    }
    bool isSaturated(size_t x, size_t y) const {
        return isSaturated((*this)(x, y));
    }
    bool isSaturatedAround(size_t x, size_t y) const {
        return isSaturated(getMaxAround(x, y));
    }
    double saturationWeightAround(size_t x, size_t y) const;
    double getRelativeExposure() const;
    ExposureRatioEstimate estimateExposureRatio(const Image & reference) const;
    void setRelativeExposure(double scale);
    size_t alignWith(const Image & r);
    bool refineAlignment(const Image & reference, const RawParameters & params, AlignmentMode mode,
                         std::string & reason);
    void preScale();
    void releaseAlignData() {
        scaled.reset();
    }
    void computeResponseFunction(const Image & nextImage);
    bool operator<(const Image & r) {
        return brightness > r.brightness;
    }
    void setSaturationThreshold(uint16_t sat);
    uint16_t getMax() const
    {
        return max;
    }
    bool contains(int x, int y) const;
    void displace(int newDx, int newDy);
    void getValidBounds(int & left, int & top, int & right, int & bottom) const;
    double getAlignmentX() const { return alignmentX; }
    double getAlignmentY() const { return alignmentY; }
    double getAlignmentConfidence() const { return alignmentConfidence; }
    double getAlignmentRotation() const { return alignmentRotation; }

private:
    struct ResponseFunction {
        uint16_t threshold;
        double linear;
        alglib::spline1dinterpolant nonLinear;
        double operator()(uint16_t v) const {
            return v <= threshold ? v * linear : alglib::spline1dcalc(nonLinear, v);
        }
        void setLinear(double slope);
    };

    QString filename;

    std::unique_ptr<Array2D<uint16_t>[]> scaled;
    uint16_t satThreshold, max;
    double brightness;
    ResponseFunction response;
    CFAPattern cfaPattern;
    double halfLightPercent;
    Array2D<uint8_t> validity;
    bool warped;
    double alignmentX;
    double alignmentY;
    double alignmentConfidence;
    double alignmentRotation;

    void subtractBlack(const RawParameters & params);
    void buildImage(uint16_t * rawImage, const RawParameters & params);
};

} // namespace hdrmerge

#endif // _IMAGE_H_
