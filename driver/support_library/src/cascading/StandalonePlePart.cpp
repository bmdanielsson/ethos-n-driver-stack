//
// Copyright © 2022-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "StandalonePlePart.hpp"

#include "../Utils.hpp"
#include "PartUtils.hpp"
#include "Plan.hpp"
#include "StripeHelper.hpp"

#include <ethosn_utils/Macros.hpp>

#include <memory>

using namespace ethosn::command_stream;
using namespace ethosn::support_library::utils;

namespace ethosn
{
namespace support_library
{
using namespace impl;

StandalonePlePart::StandalonePlePart(PartId id,
                                     const std::vector<TensorShape>& inputTensorShapes,
                                     const TensorShape& outputTensorShape,
                                     const std::vector<QuantizationInfo>& inputQuantizationInfos,
                                     const QuantizationInfo& outputQuantizationInfo,
                                     command_stream::PleOperation op,
                                     const EstimationOptions& estOpt,
                                     const CompilationOptions& compOpt,
                                     const HardwareCapabilities& capabilities,
                                     std::set<uint32_t> correspondingOperationIds,
                                     DataType dataType)
    : BasePart(id, "StandalonePlePart", correspondingOperationIds, estOpt, compOpt, capabilities)
    , m_InputTensorShapes(inputTensorShapes)
    , m_OutputTensorShape(outputTensorShape)
    , m_InputQuantizationInfos(inputQuantizationInfos)
    , m_OutputQuantizationInfo(outputQuantizationInfo)
    , m_KernelOperation(op)
    , m_DataType(dataType)
    , m_StripeConfig(GetDefaultStripeConfig(compOpt, m_DebugTag.c_str()))
{
    assert(m_InputQuantizationInfos.size() == m_InputTensorShapes.size());

    const double outputScale = outputQuantizationInfo.GetScale();
    const double inputScale0 = inputQuantizationInfos[0].GetScale();
    utils::CalculateRescaleMultiplierAndShift(inputScale0 / outputScale, m_Input0Multiplier, m_Input0Shift);

    if (inputTensorShapes.size() == 2)
    {
        const double inputScale1 = inputQuantizationInfos[1].GetScale();
        utils::CalculateRescaleMultiplierAndShift(inputScale1 / outputScale, m_Input1Multiplier, m_Input1Shift);
    }
}

Plans StandalonePlePart::GetPlans(CascadeType cascadeType,
                                  ethosn::command_stream::BlockConfig blockConfig,
                                  Buffer* prevBuffer,
                                  uint32_t numWeightStripes) const
{
    ETHOSN_UNUSED(numWeightStripes);
    ETHOSN_UNUSED(blockConfig);

    if (cascadeType == CascadeType::Middle || cascadeType == CascadeType::End)
    {
        assert(prevBuffer != nullptr);
        if (prevBuffer->m_Location != Location::Sram)
        {
            return {};    // Can't continue section from e.g. PleInputSram
        }
    }

    Plans plans;
    StripeConfig stripeConfig = m_StripeConfig;

    switch (m_KernelOperation)
    {
        case (command_stream::PleOperation::ADDITION):
        case (command_stream::PleOperation::ADDITION_RESCALE):
        {
            // ADDITION, ADDITION_RESCALE both have two inputs
            // that makes them not cascadable in the current
            // design where only SISO part is allowed in
            // a section.
            if (cascadeType == CascadeType::Lonely)
            {
                // All splits are valid
            }
            else
            {
                return Plans{};
            }
            break;
        }
        case (command_stream::PleOperation::AVGPOOL_3X3_1_1_UDMA):
        {
            // AVGPOOL_3X3_1_1_UDMA: only split in D is allowed.
            // This makes it cascadalbe only if the whole input, output
            // tensors are fit into SRAM (in other words no split)
            stripeConfig.DisableSplitWidth();
            stripeConfig.DisableSplitHeight();

            if (cascadeType != CascadeType::Lonely)
            {
                stripeConfig.DisableSplitInputDepth();
                stripeConfig.DisableSplitOutputDepth();
            }
            if (cascadeType == CascadeType::Middle || cascadeType == CascadeType::End)
            {
                assert(prevBuffer != nullptr);

                // A cascadable plan is not possible if the stripe shape of the previous buffer
                // is smaller than the input tensor (in other words a full tensor plan is NOT
                // compatible with its predecessors).
                if (prevBuffer->Sram()->m_StripeShape[1] < m_InputTensorShapes[0][1] ||
                    prevBuffer->Sram()->m_StripeShape[2] < m_InputTensorShapes[0][2] ||
                    prevBuffer->Sram()->m_StripeShape[3] < m_InputTensorShapes[0][3])
                {
                    return Plans{};
                }
            }
            break;
        }
        default:
        {
            assert(false);
            break;
        }
    }

    const uint32_t brickGroupHeight = g_BrickGroupShape[1];
    const uint32_t brickGroupWidth  = g_BrickGroupShape[2];
    const uint32_t brickGroupDepth  = g_BrickGroupShape[3];

    auto addPlan = [&](const TensorShape& outputStripeShape) {
        // Uses block configure (16, 16) which will be ignored
        // by a standalone PLE kernel.
        command_stream::BlockConfig blkConfig = { 16u, 16u };
        std::vector<TensorShape> inputStripes;
        for (uint32_t i = 0; i < m_InputTensorShapes.size(); ++i)
        {
            inputStripes.push_back(outputStripeShape);
        }
        auto op =
            std::make_unique<PleOp>(m_KernelOperation, blkConfig, static_cast<uint32_t>(m_InputTensorShapes.size()),
                                    inputStripes, outputStripeShape, m_DataType, true);
        op->m_Input0Multiplier = m_Input0Multiplier;
        op->m_Input0Shift      = m_Input0Shift;
        op->m_Input1Multiplier = m_Input1Multiplier;
        op->m_Input1Shift      = m_Input1Shift;

        OwnedOpGraph opGraph;
        PartInputMapping inputMappings;
        PartOutputMapping outputMappings;

        // only m_Output is used by AddPleToOpGraph
        NumMemoryStripes numMemoryStripes;
        numMemoryStripes.m_Output = 2;

        std::vector<Buffer*> pleInputBuffers;
        pleInputBuffers.resize(m_InputTensorShapes.size());

        // PLE input buffers
        for (size_t i = 0; i < m_InputTensorShapes.size(); ++i)
        {
            TileSizeCalculation tileSize =
                impl::CalculateTileSize(m_Capabilities, m_InputTensorShapes.at(i), outputStripeShape,
                                        command_stream::cascading::PackedBoundaryThickness{ 0, 0, 0, 0 }, 2u, true);
            SramBuffer* buffer        = opGraph.AddBuffer(std::make_unique<SramBuffer>());
            buffer->m_NumStripes      = 2;
            buffer->m_StripeShape     = outputStripeShape;
            buffer->m_SlotSizeInBytes = tileSize.slotSizeInBytes;
            buffer->m_Format          = CascadingBufferFormat::NHWCB;
            buffer->m_TensorShape     = m_InputTensorShapes.at(i);

            buffer->m_DataType       = m_DataType;
            buffer->m_SizeInBytes    = tileSize.sizeInBytes;
            buffer->m_ForbidFcafWide = tileSize.forbidFcafWide;

            buffer->m_QuantizationInfo = m_InputQuantizationInfos.at(i);
            pleInputBuffers[i]         = buffer;
        }

        // Output buffer
        auto outBufferAndPleOp =
            AddPleToOpGraph(opGraph, outputStripeShape, numMemoryStripes, std::move(op), m_OutputTensorShape,
                            m_OutputQuantizationInfo, m_DataType, m_CorrespondingOperationIds);

        for (size_t i = 0; i < m_InputTensorShapes.size(); ++i)
        {
            opGraph.AddConsumer(pleInputBuffers[i], outBufferAndPleOp.second, static_cast<uint32_t>(i));
            inputMappings[pleInputBuffers[i]] = PartInputSlot{ m_PartId, static_cast<uint32_t>(i) };
        }

        outputMappings[outBufferAndPleOp.first] = PartOutputSlot{ m_PartId, 0 };
        AddNewPlan(std::move(inputMappings), std::move(outputMappings), std::move(opGraph), plans);
    };

    if (stripeConfig.splits.none)
    {
        TensorShape outputStripeShappe = CreateStripe(m_OutputTensorShape, { 0, 0, 0, 0 }, brickGroupDepth);
        addPlan(outputStripeShappe);
    }
    if (stripeConfig.splits.widthOnly)
    {
        TensorShape outputStripeShappe =
            CreateStripe(m_OutputTensorShape, { 0, 0, brickGroupWidth, 0 }, brickGroupDepth);
        addPlan(outputStripeShappe);
    }
    if (stripeConfig.splits.mceAndPleOutputHeight)
    {
        TensorShape outputStripeShappe =
            CreateStripe(m_OutputTensorShape, { 0, brickGroupHeight, 0, 0 }, brickGroupDepth);
        addPlan(outputStripeShappe);
    }

    if (cascadeType == CascadeType::Lonely)
    {
        if (stripeConfig.splits.outputDepthInputDepth)
        {
            TensorShape outputStripeShappe =
                CreateStripe(m_OutputTensorShape, { 0, 0, 0, brickGroupDepth }, brickGroupDepth);
            addPlan(outputStripeShappe);
        }

        if (stripeConfig.splits.widthHeightOutputDepthInputDepth)
        {
            // Inclusive loops so that we generate plans that split only one or two of the dimensions,
            // but with larger stripe shapes than the non-lonely plans above.
            for (uint32_t stripeHeight : StripeShapeLoop::Inclusive(
                     utils::GetHeight(m_OutputTensorShape), brickGroupHeight, stripeConfig.blockHeightMultiplier.min,
                     stripeConfig.blockHeightMultiplier.max))
            {
                for (uint32_t stripeWidth : StripeShapeLoop::Inclusive(
                         utils::GetWidth(m_OutputTensorShape), brickGroupWidth, stripeConfig.blockWidthMultiplier.min,
                         stripeConfig.blockWidthMultiplier.max))
                {
                    for (uint32_t stripeDepth : StripeShapeLoop::Inclusive(
                             utils::GetChannels(m_OutputTensorShape), brickGroupDepth,
                             stripeConfig.ofmDepthMultiplier.min, stripeConfig.ofmDepthMultiplier.max))
                    {
                        TensorShape outputStripeShappe = CreateStripe(
                            m_OutputTensorShape, { 0, stripeHeight, stripeWidth, stripeDepth }, brickGroupDepth);
                        addPlan(outputStripeShappe);
                    }
                }
            }
        }
    }

    return plans;
}

ethosn::support_library::DotAttributes StandalonePlePart::GetDotAttributes(DetailLevel detail) const
{
    DotAttributes result = BasePart::GetDotAttributes(detail);
    if (detail >= DetailLevel::High)
    {
        result.m_Label += "InputTensorShape = " + ArrayToString(m_InputTensorShapes) + "\n";
        result.m_Label += "OutputTensorShape = " + ToString(m_OutputTensorShape) + "\n";
        result.m_Label += "InputQuantizationInfo = " + ArrayToString(m_InputQuantizationInfos) + "\n";
        result.m_Label += "OutputQuantizationInfo = " + ToString(m_OutputQuantizationInfo) + "\n";
    }
    return result;
}

}    // namespace support_library
}    // namespace ethosn
