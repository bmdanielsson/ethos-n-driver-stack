//
// Copyright © 2021-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "RegistersCommon.hpp"

#include <ethosn_command_stream/cascading/CommandStream.hpp>

namespace ethosn
{
namespace support_library
{

struct PleIfmInfo
{
    int16_t zeroPoint;
    uint16_t multiplier;
    uint16_t shift;
};

/// PLE Scheduler data
struct PleSDesc
{
    /// Output tile
    Tile ofmTile;
    /// Output zero correction
    int16_t ofmZeroPoint;
    /// Default ofm stripe size
    TensorSize defaultStripeSize;
    /// Edge ofm stripe size
    TensorSize edgeStripeSize;
    /// Number of unique stripes in each ofm tensor dimension
    TensorSize numStripes;
    /// Stride info for stripe ID (scalar) to stripe coord (ND) conversion
    TensorSize stripeIdStrides;
    /// Source of input data to PLE
    command_stream::cascading::PleInputMode inputMode;
    /// ID of the PLE kernel used
    command_stream::cascading::PleKernelId pleKernelId;
    /// PLE kernel location in SRAM
    uint32_t pleKernelSramAddr;

    // Additional fields to be used only if inputMode is SRAM

    /// First input tile
    Tile ifmTile0;
    /// First input zero correction, multiplier and shift
    PleIfmInfo ifmInfo0;
    /// Second input tile
    Tile ifmTile1;
    /// Second input zero correction, multiplier and shift
    PleIfmInfo ifmInfo1;
};

/// Generates the StartPleExtraData needed for the given stripe of the given PLE scheduler agent.
command_stream::cascading::StartPleExtraData GenerateStartPleExtraData(const PleSDesc& pleS, uint32_t stripeId);

}    // namespace support_library
}    // namespace ethosn