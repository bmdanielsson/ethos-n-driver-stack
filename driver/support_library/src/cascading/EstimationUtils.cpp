//
// Copyright © 2020-2021 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "EstimationUtils.hpp"

#include "Plan.hpp"

namespace ethosn
{
namespace support_library
{

constexpr uint32_t GetMinNumSlots(bool needNeighbour, uint32_t numStripes)
{
    return std::min(needNeighbour ? 3U : 1U, numStripes);
}

constexpr uint32_t GetEffectiveSize(uint32_t size, uint32_t stripeSize, uint32_t borderBefore, uint32_t borderAfter)
{
    return size + (borderBefore + borderAfter) * ((size - 1U) / stripeSize);
}

uint32_t GetInputMinNumSlotsForBuffering(const bool isStreamingH,
                                         const bool isStreamingW,
                                         const bool isStreamingC,
                                         const bool needNeighbourStripeH,
                                         const bool needNeighbourStripeW,
                                         const uint32_t numStripesH,
                                         const uint32_t numStripesW)
{
    if (isStreamingC)
    {
        return 2U * GetMinNumSlots(needNeighbourStripeH, numStripesH) *
               GetMinNumSlots(needNeighbourStripeW, numStripesW);
    }
    else if (isStreamingW)
    {
        return GetMinNumSlots(needNeighbourStripeW, numStripesW) + 1U;
    }
    else if (isStreamingH)
    {
        return GetMinNumSlots(needNeighbourStripeH, numStripesH) + 1U;
    }
    return 1U;
}

uint32_t GetInputNumReloads(const bool isStreamingH,
                            const bool isStreamingW,
                            const bool isStreamingC,
                            const TensorInfo& weights,
                            const uint32_t ofmProduced,
                            const uint32_t numOutStripesC)
{
    assert(numOutStripesC > 0);

    if (isStreamingC)
    {
        // Round up the number of output channels (HWIO) or the channel multiplier (HWIM, where M=1).
        return utils::DivRoundUp(weights.m_Dimensions[3], ofmProduced) - 1U;
    }
    else if (isStreamingH || isStreamingW)
    {
        return weights.m_DataFormat == DataFormat::HWIM ? 0 : numOutStripesC - 1U;
    }

    return 0;
}

uint32_t GetInputTotalBytes(const HardwareCapabilities& caps,
                            const TensorShape& shape,
                            const TensorShape& stripeShape,
                            const bool isStreamingH,
                            const bool isStreamingW,
                            const bool isStreamingC,
                            const bool needNeighbourStripeH,
                            const bool needNeighbourStripeW,
                            const uint32_t reloads)
{
    uint32_t borderHeight = 0;
    uint32_t borderWidth  = 0;

    // Calculate the total amount of input data to be transferred included reloading.
    if (needNeighbourStripeW && isStreamingC)
    {
        borderWidth = stripeShape[2];
    }

    if (needNeighbourStripeH && (isStreamingC || (isStreamingH && isStreamingW)))
    {
        borderHeight = caps.GetBoundaryStripeHeight();
    }

    const uint32_t effectiveHeight = GetEffectiveSize(shape[1], stripeShape[1], borderHeight, borderHeight);
    const uint32_t effectiveWidth  = GetEffectiveSize(shape[2], stripeShape[2], borderWidth, borderWidth);

    // Total amount of data
    return (reloads + 1U) * shape[0] * effectiveHeight * effectiveWidth * shape[3];
}

InputStats GetInputStats(const HardwareCapabilities& caps,
                         const TensorShape& shape,
                         const TensorShape& stripeShape,
                         const Location location,
                         const uint32_t tileSize,
                         const TensorInfo& weights,
                         const uint32_t numOutStripesC)
{
    InputStats data;

    if (location != Location::Sram)
    {
        const TensorShape stripeShapeValid = {
            std::min(stripeShape[0], shape[0]),
            std::min(stripeShape[1], shape[1]),
            std::min(stripeShape[2], shape[2]),
            std::min(stripeShape[3], shape[3]),
        };
        const uint32_t stripeSize = stripeShape[0] * stripeShape[1] * stripeShape[2] * stripeShape[3];
        assert(stripeSize != 0U);

        const uint32_t numStripesH = utils::GetNumStripesH(shape, stripeShape);
        const uint32_t numStripesW = utils::GetNumStripesW(shape, stripeShape);
        const uint32_t numStripesC = utils::GetNumStripesC(shape, stripeShape);

        const bool needNeighbourStripeH = weights.m_Dimensions[0] > 1U;
        const bool needNeighbourStripeW = weights.m_Dimensions[1] > 1U;

        // Number of ofm produced per iteration
        const uint32_t ofmProduced = caps.GetOgsPerEngine() * caps.GetNumberOfEngines();

        // This might change, it doesn't always need all the boundary slots.
        const uint32_t numBoundarySlots = caps.GetNumBoundarySlots();

        const bool isStreamingH = numStripesH > 1U;
        const bool isStreamingW = numStripesW > 1U;
        const bool isStreamingC = numStripesC > 1U;

        data.m_StripesStats.m_NumReloads =
            GetInputNumReloads(isStreamingH, isStreamingW, isStreamingC, weights, ofmProduced, numOutStripesC);

        // Calculate the total amount of input data to be transferred included reloading.
        const uint32_t total =
            GetInputTotalBytes(caps, shape, stripeShape, isStreamingH, isStreamingW, isStreamingC, needNeighbourStripeH,
                               needNeighbourStripeW, data.m_StripesStats.m_NumReloads);

        // Calculate the minimum amount of data required to start processing.
        uint32_t borderWidth  = 0;
        uint32_t borderHeight = 0;

        if (needNeighbourStripeH && isStreamingH)
        {
            borderHeight = (isStreamingC || isStreamingW) ? caps.GetBoundaryStripeHeight() : stripeShapeValid[1];
        }

        if (needNeighbourStripeW && isStreamingW)
        {
            borderWidth = stripeShapeValid[2];
        }

        const bool isUsingBoundarySlots = needNeighbourStripeH && isStreamingH && isStreamingW && !isStreamingC;
        const uint32_t boundarySize     = isUsingBoundarySlots ? borderHeight * stripeShape[2] * stripeShape[3] : 0;
        const uint32_t numStripesInTile = utils::DivRoundUp(tileSize - (boundarySize * numBoundarySlots), stripeSize);

        data.m_MemoryStats.m_DramNonParallel =
            (stripeShapeValid[1] + borderHeight) * (stripeShapeValid[2] + borderWidth) * stripeShapeValid[3];

        // Determine how much data can be transferred in parallel.
        const uint32_t minNumSlotsForBuffering =
            GetInputMinNumSlotsForBuffering(isStreamingH, isStreamingW, isStreamingC, needNeighbourStripeH,
                                            needNeighbourStripeW, numStripesH, numStripesW);

        const bool buffering = numStripesInTile >= minNumSlotsForBuffering;

        if (buffering)
        {
            data.m_MemoryStats.m_DramParallel = total - data.m_MemoryStats.m_DramNonParallel;
        }
        else
        {
            data.m_MemoryStats.m_DramNonParallel = total;
        }

        data.m_StripesStats.m_NumCentralStripes  = utils::GetNumStripesTotal(shape, stripeShape);
        data.m_StripesStats.m_NumBoundaryStripes = isUsingBoundarySlots ? (numStripesH - 1) * numStripesW : 0;
    }
    else
    {
        data.m_MemoryStats.m_Sram = shape[0] * shape[1] * shape[2] * shape[3];
    }

    return data;
}

OutputStats GetOutputStats(const TensorShape& shape, const TensorShape& stripeShape, const Location location)
{
    OutputStats data;

    const TensorShape& stripeShapeValid = { std::min(stripeShape[0], shape[0]), std::min(stripeShape[1], shape[1]),
                                            std::min(stripeShape[2], shape[2]), std::min(stripeShape[3], shape[3]) };
    const uint32_t stripeSize = stripeShapeValid[0] * stripeShapeValid[1] * stripeShapeValid[2] * stripeShapeValid[3];

    // Total amount of data.
    const uint32_t total = shape[0] * shape[1] * shape[2] * shape[3];

    // Consider the output data transfer only if it is not already in Sram.
    if (location != Location::Sram)
    {
        // Wait to the final stripe to be copied out if required.
        data.m_MemoryStats.m_DramNonParallel    = stripeSize;
        data.m_MemoryStats.m_DramParallel       = total - data.m_MemoryStats.m_DramNonParallel;
        data.m_StripesStats.m_NumCentralStripes = utils::GetNumStripesTotal(shape, stripeShape);
    }
    else
    {
        data.m_MemoryStats.m_Sram = total;
    }
    return data;
}

PleStats GetPleStats(const HardwareCapabilities& caps,
                     const std::vector<TensorShape>& inputShapes,
                     const command_stream::PleOperation& pleOperation)
{
    PleStats pleststs;

    // Number of patches that need to be post processed by the Ple kernel
    uint32_t patchesH = 0;
    uint32_t patchesW = 0;
    uint32_t patchesC = 0;

    for (auto& inputShape : inputShapes)
    {
        patchesH = std::max(utils::DivRoundUp(inputShape[1], caps.GetPatchShape()[1]), patchesH);
        patchesW = std::max(utils::DivRoundUp(inputShape[2], caps.GetPatchShape()[2]), patchesW);
        patchesC = std::max(utils::DivRoundUp(inputShape[3], caps.GetNumberOfEngines() * caps.GetNumberOfPleLanes()),
                            patchesC);
    }

    pleststs.m_NumOfPatches = patchesW * patchesH * patchesC;
    pleststs.m_Operation    = static_cast<uint32_t>(pleOperation);
    return pleststs;
}

PassStats GetConversionStats(const ConversionData& input, const ConversionData& output, bool isDramToDram)
{
    PassStats perfData;

    const TensorShape& inputShape           = input.tensorShape;
    const TensorShape& roundedUpInputShape  = utils::RoundUpHeightAndWidthToBrickGroup(inputShape);
    const TensorShape& outputShape          = output.tensorShape;
    const TensorShape& roundedUpOutputShape = utils::RoundUpHeightAndWidthToBrickGroup(outputShape);

    const bool isInputNHWC  = input.isNhwc;
    const bool isOutputNHWC = output.isNhwc;

    const uint32_t inputSize  = inputShape[0] * inputShape[1] * inputShape[2] * inputShape[3];
    const uint32_t outputSize = outputShape[0] * outputShape[1] * outputShape[2] * outputShape[3];

    const uint32_t roundedUpInputSize =
        roundedUpInputShape[0] * roundedUpInputShape[1] * roundedUpInputShape[2] * roundedUpInputShape[3];
    const uint32_t roundedUpOutputSize =
        roundedUpOutputShape[0] * roundedUpOutputShape[1] * roundedUpOutputShape[2] * roundedUpOutputShape[3];

    if (isDramToDram)
    {
        perfData.m_Input.m_MemoryStats.m_DramNonParallel    = isInputNHWC ? inputSize : roundedUpInputSize;
        perfData.m_Input.m_StripesStats.m_NumCentralStripes = utils::GetNumStripesTotal(inputShape, input.stripeShape);

        perfData.m_Output.m_MemoryStats.m_DramNonParallel = isOutputNHWC ? outputSize : roundedUpOutputSize;
        perfData.m_Output.m_StripesStats.m_NumCentralStripes =
            utils::GetNumStripesTotal(outputShape, output.stripeShape);
    }
    else
    {
        // This is for Sram To Sram conversions. We only handle Dram To Dram or Sram to Sram.
        perfData.m_Input.m_MemoryStats.m_Sram  = roundedUpInputSize;
        perfData.m_Output.m_MemoryStats.m_Sram = roundedUpOutputSize;
    }
    return perfData;
}

InputStats AccountForActivationCompression(InputStats stats, float spaceSavingRatio)
{
    InputStats ret = stats;
    ret.m_MemoryStats.m_DramNonParallel =
        static_cast<uint32_t>(static_cast<float>(stats.m_MemoryStats.m_DramNonParallel) * (1 - spaceSavingRatio));
    ret.m_MemoryStats.m_DramParallel =
        static_cast<uint32_t>(static_cast<float>(stats.m_MemoryStats.m_DramParallel) * (1 - spaceSavingRatio));
    return ret;
}

uint64_t GetPerformanceTotalDataMetric(const NetworkPerformanceData& netPerfData)
{
    return GetPerformanceParallelDataMetric(netPerfData) + GetPerformanceNonParallelDataMetric(netPerfData);
}

uint64_t GetPerformanceNonParallelDataMetric(const NetworkPerformanceData& netPerfData)
{
    uint64_t nonParallelData = 0UL;
    for (PassPerformanceData passPerfData : netPerfData.m_Stream)
    {
        nonParallelData += passPerfData.m_Stats.m_Input.m_MemoryStats.m_DramNonParallel +
                           passPerfData.m_Stats.m_Output.m_MemoryStats.m_DramNonParallel +
                           passPerfData.m_Stats.m_Weights.m_MemoryStats.m_DramNonParallel;
    }
    return nonParallelData;
}

uint64_t GetPerformanceParallelDataMetric(const NetworkPerformanceData& netPerfData)
{
    uint64_t parallelData = 0UL;
    for (PassPerformanceData passPerfData : netPerfData.m_Stream)
    {
        parallelData += passPerfData.m_Stats.m_Input.m_MemoryStats.m_DramParallel +
                        passPerfData.m_Stats.m_Output.m_MemoryStats.m_DramParallel +
                        passPerfData.m_Stats.m_Weights.m_MemoryStats.m_DramParallel;
    }
    return parallelData;
}

uint64_t GetPerformanceNumberOfPassesMetric(const NetworkPerformanceData& netPerfData)
{
    return netPerfData.m_Stream.size();
}

namespace
{

enum class MetricType
{
    Total,
    Parallel,
    NonParallel,
    Passes
};

uint64_t GetPerformanceMetric(const NetworkPerformanceData& netPerfData, const MetricType metricType)
{
    switch (metricType)
    {
        case MetricType::Total:
            return GetPerformanceTotalDataMetric(netPerfData);
        case MetricType::Parallel:
            return GetPerformanceParallelDataMetric(netPerfData);
        case MetricType::NonParallel:
            return GetPerformanceNonParallelDataMetric(netPerfData);
        case MetricType::Passes:
            return GetPerformanceNumberOfPassesMetric(netPerfData);
        default:
        {
            std::string errorMessage = "Error in " + std::string(__func__) + ": metric type " +
                                       std::to_string(static_cast<uint32_t>(metricType)) + " is not implemented";
            throw std::invalid_argument(errorMessage);
        }
    }
}

// Sequence of metric types to compare against
std::vector<MetricType> g_MetricOrder = { MetricType::Total, MetricType::NonParallel, MetricType::Passes };

}    // namespace

std::vector<uint64_t> GetPerformanceMetrics(const NetworkPerformanceData& netPerfData)
{
    std::vector<uint64_t> result(g_MetricOrder.size());
    std::transform(g_MetricOrder.begin(), g_MetricOrder.end(), result.begin(),
                   [&](MetricType m) { return GetPerformanceMetric(netPerfData, m); });
    return result;
}

PerformanceComparisonResult ComparePerformanceData(const NetworkPerformanceData& left,
                                                   const NetworkPerformanceData& right)
{
    for (const auto& metricType : g_MetricOrder)
    {
        const uint64_t metricLeft  = GetPerformanceMetric(left, metricType);
        const uint64_t metricRight = GetPerformanceMetric(right, metricType);

        if (metricLeft < metricRight)
        {
            return PerformanceComparisonResult::LeftBetter;
        }

        if (metricLeft > metricRight)
        {
            return PerformanceComparisonResult::RightBetter;
        }
    }
    return PerformanceComparisonResult::Equal;
}

}    // namespace support_library
}    // namespace ethosn
