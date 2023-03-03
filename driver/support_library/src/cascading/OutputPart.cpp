//
// Copyright © 2021-2023 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "../Utils.hpp"
#include "GraphNodes.hpp"
#include "OutputPart.hpp"
#include "PartUtils.hpp"
#include "Plan.hpp"

namespace ethosn
{
namespace support_library
{

Plans OutputPart::GetPlans(CascadeType cascadeType,
                           ethosn::command_stream::BlockConfig blockConfig,
                           Buffer* sramBuffer,
                           uint32_t numWeightStripes) const
{
    ETHOSN_UNUSED(blockConfig);
    ETHOSN_UNUSED(sramBuffer);
    ETHOSN_UNUSED(numWeightStripes);

    Plans plans;

    if (cascadeType == CascadeType::Lonely)
    {
        CreatePlanForOutputPart(plans);
    }

    return plans;
}

OutputPart::~OutputPart()
{}

void OutputPart::CreatePlanForOutputPart(Plans& plans) const
{
    PartInputMapping inputMappings;
    PartOutputMapping outputMappings;
    OwnedOpGraph opGraph;

    CascadingBufferFormat format = impl::GetCascadingBufferFormatFromCompilerDataFormat(m_CompilerDataFormat);

    std::unique_ptr<DramBuffer> buffer = DramBuffer::Build()
                                             .AddFormat(format)
                                             .AddDataType(m_InputDataType)
                                             .AddTensorShape(m_InputTensorShape)
                                             .AddQuantization(m_InputQuantizationInfo)
                                             .AddBufferType(BufferType::Output)
                                             .AddOperationId(*m_CorrespondingOperationIds.begin())
                                             .AddProducerOutputIndex(m_ProducerOutputIndx);

    DramBuffer* bufferRaw    = opGraph.AddBuffer(std::move(buffer));
    inputMappings[bufferRaw] = PartInputSlot{ m_PartId, 0 };

    AddNewPlan(std::move(inputMappings), std::move(outputMappings), std::move(opGraph), plans);
}

ethosn::support_library::DotAttributes OutputPart::GetDotAttributes(DetailLevel detail) const
{
    DotAttributes result = BasePart::GetDotAttributes(detail);
    if (detail >= DetailLevel::High)
    {
        result.m_Label += "CompilerDataFormat = " + ToString(m_CompilerDataFormat) + "\n";
        result.m_Label += "InputTensorShape = " + ToString(m_InputTensorShape) + "\n";
        result.m_Label += "InputQuantizationInfo = " + ToString(m_InputQuantizationInfo) + "\n";
        result.m_Label += "InputDataType = " + ToString(m_InputDataType) + "\n";
    }
    return result;
}

}    // namespace support_library
}    // namespace ethosn
