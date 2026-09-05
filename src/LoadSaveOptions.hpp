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

#ifndef _LOADSAVEOPTIONS_H_
#define _LOADSAVEOPTIONS_H_

#include <vector>
#include <QString>

namespace hdrmerge {

enum class AlignmentMode {
    Integer,
    Subpixel,
    Affine
};

enum class FusionMode {
    Off,
    Legacy,
    Robust
};

inline const char * fusionModeName(FusionMode mode) {
    switch (mode) {
        case FusionMode::Off: return "off";
        case FusionMode::Legacy: return "legacy";
        default: return "robust";
    }
}

inline const char * alignmentModeName(AlignmentMode mode) {
    switch (mode) {
        case AlignmentMode::Integer: return "integer";
        case AlignmentMode::Affine: return "affine";
        default: return "subpixel";
    }
}

struct LoadOptions {
    std::vector<QString> fileNames;
    bool align;
    bool crop;
    bool useCustomWl;
    uint16_t customWl;
    bool batch;
    double batchGap;
    bool withSingles;
    int deghostThreshold;
    int bracketSize;
    AlignmentMode alignmentMode;
    LoadOptions() : align(true), crop(true), useCustomWl(false), customWl(16383), batch(false), batchGap(2.0),
        withSingles(false), deghostThreshold(0), bracketSize(0), alignmentMode(AlignmentMode::Subpixel) {}
};


struct SaveOptions {
    int bps;
    int previewSize;
    QString fileName;
    bool saveMask;
    QString maskFileName;
    int featherRadius;
    FusionMode fusionMode;
    bool preserveExposure;
    SaveOptions() : bps(16), previewSize(0), saveMask(false), featherRadius(3),
        fusionMode(FusionMode::Robust), preserveExposure(false) {}
};

} // namespace hdrmerge

#endif // _LOADSAVEOPTIONS_H_
