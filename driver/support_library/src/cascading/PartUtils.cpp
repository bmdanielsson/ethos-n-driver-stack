//
// Copyright © 2021-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "PartUtils.hpp"

#include "../Utils.hpp"

using namespace ethosn::support_library::utils;

namespace ethosn
{
namespace support_library
{

namespace impl
{

CascadingBufferFormat GetFormat(Location location)
{
    switch (location)
    {
        case Location::Dram:
            return CascadingBufferFormat::NHWC;
        case Location::PleInputSram:
        case Location::Sram:
            return CascadingBufferFormat::NHWCB;
        case Location::VirtualSram:
            return CascadingBufferFormat::NHWC;
        default:
            throw NotSupportedException("Unknown location");
    }
}

CascadingBufferFormat GetCascadingBufferFormatFromCompilerDataFormat(const CompilerDataFormat& format)
{
    switch (format)
    {
        case (CompilerDataFormat::NHWC):
            return CascadingBufferFormat::NHWC;
        case (CompilerDataFormat::NCHW):
            return CascadingBufferFormat::NCHW;
        case (CompilerDataFormat::NHWCB):
            return CascadingBufferFormat::NHWCB;
        case (CompilerDataFormat::WEIGHT):
            return CascadingBufferFormat::WEIGHT;
        default:
        {
            std::string error = "In " + std::string(ETHOSN_FUNCTION_SIGNATURE) + ": value " +
                                std::to_string(static_cast<uint32_t>(format)) + " is not valid";
            throw NotSupportedException(error.c_str());
        }
    }
}

std::pair<uint32_t, uint32_t>
    CalculateTileSize(const HardwareCapabilities& caps,
                      const TensorShape& inputTensorShape,
                      const TensorShape& inputStripeShape,
                      command_stream::cascading::PackedBoundaryThickness packedBoundaryThickness,
                      uint32_t numStripes,
                      bool couldSourceBeFcaf)
{
    // Calculate the size needed for each slot. This is based on the space needed for one stripe,
    // but might need additional space for packed boundary data, and rounding because of FCAF.
    // If the tile could be decompressed from FCAF then we need
    // to make sure we have full FCAF cells available as the HW always writes to SRAM in full FCAF
    // cell size if the source is FCAF compressed (only in width and height though, channels is fine)
    const TensorShape stripeShapeInclBoundary = {
        1,
        GetHeight(inputStripeShape) + packedBoundaryThickness.top + packedBoundaryThickness.bottom,
        GetWidth(inputStripeShape) + packedBoundaryThickness.left + packedBoundaryThickness.right,
        GetChannels(inputStripeShape),
    };

    bool couldSourceBeFcafDeep = couldSourceBeFcaf && !AnyPackedBoundaryData(packedBoundaryThickness) &&
                                 IsCompressionFormatCompatibleWithStripeShape(CompilerDataCompressedFormat::FCAF_DEEP,
                                                                              inputStripeShape, inputTensorShape);
    bool couldSourceBeFcafWide = couldSourceBeFcaf && !AnyPackedBoundaryData(packedBoundaryThickness) &&
                                 IsCompressionFormatCompatibleWithStripeShape(CompilerDataCompressedFormat::FCAF_WIDE,
                                                                              inputStripeShape, inputTensorShape);

    TensorShape stripeShapeRoundedUpFcaf = stripeShapeInclBoundary;
    if (couldSourceBeFcafDeep)
    {
        stripeShapeRoundedUpFcaf[1] =
            utils::RoundUpToNearestMultiple(GetHeight(stripeShapeRoundedUpFcaf), GetHeight(g_FcafDeepCellShape));
        stripeShapeRoundedUpFcaf[2] =
            utils::RoundUpToNearestMultiple(GetWidth(stripeShapeRoundedUpFcaf), GetWidth(g_FcafDeepCellShape));
    }
    if (couldSourceBeFcafWide)
    {
        stripeShapeRoundedUpFcaf[1] =
            utils::RoundUpToNearestMultiple(GetHeight(stripeShapeRoundedUpFcaf), GetHeight(g_FcafWideCellShape));
        stripeShapeRoundedUpFcaf[2] =
            utils::RoundUpToNearestMultiple(GetWidth(stripeShapeRoundedUpFcaf), GetWidth(g_FcafWideCellShape));
    }

    const uint32_t slotSize      = TotalSizeBytes(stripeShapeRoundedUpFcaf);
    uint32_t inputFullStripeSize = slotSize * numStripes;

    // If the tensor doesn't have many stripes in it, then it's possible that we would allocate
    // more space in the tile than will actually be used (e.g. tensor is 65 high, stripes are 64 high,
    // numStripesInTile = 2). We therefore clamp the tile size to avoid allocating too much.
    // We also need to account for FCAF here as above.

    // If packed boundary data is used then we can't do this optimisation, because boundary data is
    // always laid out afterwards and assumes the full stripe shape.
    if (AnyPackedBoundaryData(packedBoundaryThickness))
    {
        return { slotSize, inputFullStripeSize };
    }

    uint32_t widthMultiple  = g_BrickGroupShape[2];
    uint32_t heightMultiple = g_BrickGroupShape[1];
    if (couldSourceBeFcafDeep)
    {
        widthMultiple  = std::max(widthMultiple, GetWidth(g_FcafDeepCellShape));
        heightMultiple = std::max(heightMultiple, GetHeight(g_FcafDeepCellShape));
    }
    if (couldSourceBeFcafWide)
    {
        widthMultiple  = std::max(widthMultiple, GetWidth(g_FcafWideCellShape));
        heightMultiple = std::max(heightMultiple, GetHeight(g_FcafWideCellShape));
    }

    const uint32_t inputTileSize = utils::MaxTileSize(inputTensorShape, caps, widthMultiple, heightMultiple);
    return { slotSize, std::min(inputTileSize, inputFullStripeSize) };
}

}    // namespace impl
}    // namespace support_library
}    // namespace ethosn
