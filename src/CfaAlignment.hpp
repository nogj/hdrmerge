/*
 * CFA-safe geometric alignment for HDRMerge.
 * The transform maps output/reference coordinates to source coordinates.
 */
#ifndef _CFAALIGNMENT_HPP_
#define _CFAALIGNMENT_HPP_

#include <cstdint>
#include <string>

#include "Array2D.hpp"
#include "CFAPattern.hpp"
#include "LoadSaveOptions.hpp"

namespace hdrmerge {

struct AlignmentTransform {
    double m[2][3];
    double confidence;
    bool valid;
    std::string message;

    AlignmentTransform();
    double placementX() const { return -m[0][2]; }
    double placementY() const { return -m[1][2]; }
};

AlignmentTransform estimateAlignmentTransform(const Array2D<uint16_t> & source,
                                                const Array2D<uint16_t> & reference,
                                                int integerDx, int integerDy,
                                                AlignmentMode mode);

bool isSupportedCfaForWarp(const CFAPattern & pattern);

void resampleCfa(const Array2D<uint16_t> & source,
                 Array2D<uint16_t> & destination,
                 Array2D<uint8_t> & validity,
                 const CFAPattern & pattern,
                 const AlignmentTransform & transform);

} // namespace hdrmerge

#endif // _CFAALIGNMENT_HPP_
