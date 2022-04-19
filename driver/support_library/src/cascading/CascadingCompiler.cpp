//
// Copyright © 2022 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "../Compiler.hpp"
#include "CascadingCompiler.hpp"
#include "CascadingCompilerUtils.hpp"
#include "PartUtils.hpp"

#include <memory>

namespace ethosn
{
namespace support_library
{
namespace cascading_compiler
{

CascadingCompiler::CascadingCompiler(const OpGraph& mergedOpGraph,
                                     const std::set<uint32_t>& operationIds,
                                     const HardwareCapabilities& capabilities,
                                     const CompilationOptions& compilationOptions)
    : m_MergedOpGraph{ mergedOpGraph }
    , m_OperationIds{ operationIds }
    , m_Capabilities{ capabilities }
    , m_CompilationOptions{ compilationOptions }
{}

CascadingCompiler::~CascadingCompiler()
{}

std::unique_ptr<CompiledNetwork> CascadingCompiler::Compile()
{
    OpGraph::OpList opsInExecutionOrder = m_MergedOpGraph.GetOps();

    assert(opsInExecutionOrder.size() != 0 && m_CommandStreamAgents.size() == 0);

    try
    {
        for (auto currentOp : opsInExecutionOrder)
        {
            if (IsObjectOfType<DmaOp>(currentOp))
            {
                ProcessDmaOp(currentOp);
            }
            else if (IsObjectOfType<MceOp>(currentOp))
            {
                ProcessMceOp(currentOp);
            }
            else if (IsObjectOfType<PleOp>(currentOp))
            {
                ProcessPleOp(currentOp);
            }
            else if (IsObjectOfType<ConcatOp>(currentOp))
            {
                ProcessConcatOp(currentOp);
            }
            else
            {
                throw NotSupportedException("Op is not currently supported by the Cascading Compiler");
            }
        }
    }
    catch (const NotSupportedException& e)
    {
        g_Logger.Error("Error: %s", e.what());
        return std::unique_ptr<CompiledNetworkImpl>(nullptr);
    }

    // Add the lifetime information of the intermediate DRAM buffers so the memory required to store these
    // buffers is reduced
    AddLifetimeInfoForIntermediateDramBuffers();

    // Add the generated command stream to the buffer manager
    for (auto& agent : m_CommandStreamAgents)
    {
        m_CommandStream.EmplaceBack<Agent>(agent);
    }
    m_BufferManager.AddCommandStream(m_CommandStream);

    // Create the compiled network using the updated BufferManager instance
    std::unique_ptr<CompiledNetworkImpl> compiledNetwork = std::make_unique<CompiledNetworkImpl>(
        m_BufferManager.GetConstantDmaData(), m_BufferManager.GetConstantControlUnitData(),
        m_BufferManager.GetBuffers(), m_OperationIds);

    return compiledNetwork;
}

std::vector<Agent> CascadingCompiler::GetCommandStreamOfAgents()
{
    return m_CommandStreamAgents;
}

void CascadingCompiler::ProcessDmaOp(Op* const ptrDmaOp)
{
    // Get the input buffers to the Dma Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrDmaOp);
    Buffer* outputBuffer             = m_MergedOpGraph.GetOutput(ptrDmaOp);

    assert(inputBuffers.size() == 1 && outputBuffer != nullptr);

    if (inputBuffers[0]->m_Location == Location::Dram && outputBuffer->m_Location == Location::Sram)
    {
        if (inputBuffers[0]->m_Format != CascadingBufferFormat::WEIGHT)
        {
            AddIfmStreamerToCommandStream(static_cast<DmaOp*>(ptrDmaOp));
        }
        else
        {
            AddWeightStreamerToCommandStream(static_cast<DmaOp*>(ptrDmaOp));
        }
    }
    else if (inputBuffers[0]->m_Location == Location::Sram && outputBuffer->m_Location == Location::Dram)
    {
        AddOfmStreamerToCommandStream(static_cast<DmaOp*>(ptrDmaOp));
    }
    else
    {
        assert(false);
    }

    ETHOSN_UNUSED(ptrDmaOp);
}

void CascadingCompiler::ProcessMceOp(Op* const ptrMceOp)
{
    // Get the input buffers to the Mce Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrMceOp);
    assert(inputBuffers.size() == 2);
    assert(inputBuffers[0]->m_Offset.has_value());
    assert(inputBuffers[1]->m_Offset.has_value());

    // Get the output buffer from the Mce Op
    Buffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrMceOp);
    assert(outputBuffer != nullptr);

    // Construct and add the required agents to the command stream
    // - Ple Loader Agent
    auto mceOpConsumer = m_MergedOpGraph.GetConsumer(outputBuffer, 0);
    assert(mceOpConsumer.first != nullptr && IsObjectOfType<PleOp>(mceOpConsumer.first));

    AgentIdType pleLoaderAgentId = 0;
    PleOp* ptrPleOp              = static_cast<PleOp*>(mceOpConsumer.first);

    if (ptrPleOp->m_LoadKernel == true)
    {
        pleLoaderAgentId = AddPleLoaderToCommandStream(ptrPleOp);
    }

    // - MCE Scheduler Agent
    AgentIdType mceSchedulerAgentId =
        AddMceSchedulerToCommandStream(static_cast<MceOp*>(ptrMceOp), ptrPleOp->m_PleKernelId);

    // Add 'Read After Write' dependency to the MceScheduler agent
    // Read After Write Dependency for [MceScheduler][IfmStreamer]
    AddReadAfterWriteDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::IFM_STREAMER,
                                m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[0])]);
    // Read After Write Dependency for [MceScheduler][WeightStreamer]
    AddReadAfterWriteDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                                m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[1])]);

    // Add 'Write After Read' dependency information to the IfmStreamer and WeightStreamer agents
    // Write After Read Dependency for [IfmStreamer][MceScheduler]
    AddWriteAfterReadDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::IFM_STREAMER,
                                m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[0])]);
    // Write After Read Dependency for [WeightStreamer][MceScheduler]
    AddWriteAfterReadDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                                m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[1])]);

    // Add 'Schedule Time' dependency information to the PLE Loader agent
    // Schedule Time Dependency for [IfmStreamer][MceScheduler]
    AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::IFM_STREAMER,
                              m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[0])]);
    // Schedule Time Dependency for [WeightStreamer][MceScheduler]
    AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::WGT_STREAMER,
                              m_OpToAgentIdMapping[m_MergedOpGraph.GetProducer(inputBuffers[1])]);
    // Schedule Time Dependency for [PLE Loader][MceScheduler]
    if (ptrPleOp->m_LoadKernel == true)
    {
        AddScheduleTimeDependency(AgentType::MCE_SCHEDULER, mceSchedulerAgentId, AgentType::PLE_LOADER,
                                  pleLoaderAgentId);
    }

    // Add 'SRAM Overlap' dependency information
    // No identified SRAM Overlap dependencies
}

void CascadingCompiler::ProcessPleOp(Op* const ptrPleOp)
{
    AddPleSchedulerToCommandStream(static_cast<PleOp*>(ptrPleOp));
}

void CascadingCompiler::ProcessConcatOp(Op* const ptrConcatOp)
{
    ETHOSN_UNUSED(ptrConcatOp);
}

void CascadingCompiler::ProcessSplitOp(Op* const ptrSplitOp)
{
    ETHOSN_UNUSED(ptrSplitOp);
}

void CascadingCompiler::ProcessSpaceToDepthOp(Op* const ptrSpaceToDepthOp)
{
    ETHOSN_UNUSED(ptrSpaceToDepthOp);
}

void CascadingCompiler::ProcessTransposeOp(Op* const ptrTransposeOp)
{
    ETHOSN_UNUSED(ptrTransposeOp);
}

// Private function to add IFM_STREAMER to the command stream
AgentIdType CascadingCompiler::AddIfmStreamerToCommandStream(DmaOp* const ptrDmaOp)
{
    IfmS ifmStreamerData                           = {};
    ifmStreamerData.fmData.dfltStripeSize.height   = 1;
    ifmStreamerData.fmData.dfltStripeSize.width    = 1;
    ifmStreamerData.fmData.dfltStripeSize.channels = 1;
    ifmStreamerData.fmData.numStripes.channels     = 1;

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal                         = 0;
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent ifmStreamerAgent{ ifmStreamerData, dependencyInfo };

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrDmaOp] = agentId;
    m_CommandStreamAgents.push_back(ifmStreamerAgent);

    return agentId;
}

// Private function to add WGT_STREAMER to the command stream
AgentIdType CascadingCompiler::AddWeightStreamerToCommandStream(DmaOp* const ptrDmaOp)
{
    WgtS weightStreamerData = {};

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal                         = 0;
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent weightStreamerAgent{ weightStreamerData, dependencyInfo };

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrDmaOp] = agentId;
    m_CommandStreamAgents.push_back(weightStreamerAgent);

    return agentId;
}

// Private function to add MCE_SCHEDULER to the command stream
AgentIdType CascadingCompiler::AddMceSchedulerToCommandStream(MceOp* const ptrMceOp, const PleKernelId pleKernelId)
{
    // Get the input buffers to the Mce Op
    OpGraph::BufferList inputBuffers = m_MergedOpGraph.GetInputs(ptrMceOp);

    // Get the output buffer from the Mce Op
    Buffer* outputBuffer = m_MergedOpGraph.GetOutput(ptrMceOp);

    MceS mceSchedulerData = {};

    mceSchedulerData.ifmTile = {
        static_cast<uint16_t>(inputBuffers[0]->m_Offset.value()), static_cast<uint16_t>(inputBuffers[0]->m_NumStripes),
        static_cast<uint16_t>(impl::CalculateBufferSize(inputBuffers[0]->m_StripeShape, inputBuffers[0]->m_Format))
    };

    mceSchedulerData.wgtTile = {
        static_cast<uint16_t>(inputBuffers[1]->m_Offset.value()), static_cast<uint16_t>(inputBuffers[1]->m_NumStripes),
        static_cast<uint16_t>(impl::CalculateBufferSize(inputBuffers[1]->m_StripeShape, inputBuffers[1]->m_Format))
    };

    mceSchedulerData.blockSize.width  = static_cast<uint8_t>(ptrMceOp->m_BlockConfig.m_BlockWidth());
    mceSchedulerData.blockSize.height = static_cast<uint8_t>(ptrMceOp->m_BlockConfig.m_BlockHeight());

    MceSUtils::SetMcesOfmHeightStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape, ptrMceOp->m_OutputStripeShape);
    MceSUtils::SetMcesOfmWidthStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape, ptrMceOp->m_OutputStripeShape);
    MceSUtils::SetMcesOfmChannelsStripeInfo(mceSchedulerData, outputBuffer->m_TensorShape,
                                            ptrMceOp->m_OutputStripeShape);
    MceSUtils::SetMcesIfmChannelsStripeInfo(mceSchedulerData, inputBuffers[0]->m_TensorShape,
                                            inputBuffers[0]->m_StripeShape);

    MceSUtils::SetStripeIdStrides(mceSchedulerData, outputBuffer->m_Order);

    mceSchedulerData.convStrideXy.x = static_cast<uint8_t>(ptrMceOp->m_Stride.m_X);
    mceSchedulerData.convStrideXy.y = static_cast<uint8_t>(ptrMceOp->m_Stride.m_Y);
    mceSchedulerData.ifmZeroPoint   = static_cast<int16_t>(inputBuffers[0]->m_QuantizationInfo.GetZeroPoint());

    MceSUtils::setMcesOpMode(mceSchedulerData, ptrMceOp->m_Op);
    MceSUtils::setMcesAlgorithm(mceSchedulerData, ptrMceOp->m_Algo);

    mceSchedulerData.filterShape.height = static_cast<uint8_t>(inputBuffers[1]->m_TensorShape[1]);
    mceSchedulerData.filterShape.width  = static_cast<uint8_t>(inputBuffers[1]->m_TensorShape[2]);
    mceSchedulerData.padding.left       = static_cast<uint8_t>(ptrMceOp->m_PadLeft);
    mceSchedulerData.padding.top        = static_cast<uint8_t>(ptrMceOp->m_PadTop);
    mceSchedulerData.ifmDeltaDefault.height =
        static_cast<int8_t>(inputBuffers[0]->m_TensorShape[1] - outputBuffer->m_TensorShape[1]);
    mceSchedulerData.ifmDeltaDefault.width =
        static_cast<int8_t>(inputBuffers[0]->m_TensorShape[2] - outputBuffer->m_TensorShape[2]);
    mceSchedulerData.ifmDeltaEdge.height =
        static_cast<int8_t>(inputBuffers[0]->m_TensorShape[1] - outputBuffer->m_TensorShape[1]);
    mceSchedulerData.ifmDeltaEdge.width =
        static_cast<int8_t>(inputBuffers[0]->m_TensorShape[2] - outputBuffer->m_TensorShape[2]);
    mceSchedulerData.reluActiv.min = ptrMceOp->m_LowerBound;
    mceSchedulerData.reluActiv.max = ptrMceOp->m_UpperBound;
    mceSchedulerData.pleKernelId   = pleKernelId;

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal =
        static_cast<uint16_t>(utils::GetNumStripesTotal(outputBuffer->m_TensorShape, outputBuffer->m_StripeShape));
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent mceSchedulerAgent{ mceSchedulerData, dependencyInfo };

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrMceOp] = agentId;
    m_CommandStreamAgents.push_back(mceSchedulerAgent);

    return agentId;
}

// Private function to add PLE_LOADER to the command stream
AgentIdType CascadingCompiler::AddPleLoaderToCommandStream(PleOp* const ptrPleOp)
{
    // Create a new Ple Loader agent
    PleL pleLoaderData;
    pleLoaderData.pleKernelId = ptrPleOp->m_PleKernelId;
    pleLoaderData.sramAddr    = static_cast<uint16_t>(ptrPleOp->m_Offset.value());

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal                         = 1;
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent pleLoaderAgent{ pleLoaderData, dependencyInfo };

    // Push the Ple Loader agent to the command stream
    AgentIdType agentId                                           = m_CommandStreamAgents.size();
    m_PleKernelToPleLoaderAgentIdMapping[ptrPleOp->m_PleKernelId] = agentId;
    m_CommandStreamAgents.push_back(pleLoaderAgent);

    return agentId;
}

// Private function to add PLE_SCHEDULER to the command stream
AgentIdType CascadingCompiler::AddPleSchedulerToCommandStream(PleOp* const ptrPleOp)
{
    PleS pleSchedulerData = {};

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal                         = 0;
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent pleSchedulerAgent{ pleSchedulerData, dependencyInfo };

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrPleOp] = agentId;
    m_CommandStreamAgents.push_back(pleSchedulerAgent);

    return agentId;
}

// Private function to add OFM_STREAMER to the command stream
AgentIdType CascadingCompiler::AddOfmStreamerToCommandStream(DmaOp* const ptrDmaOp)
{
    OfmS ofmStreamerData = {};

    AgentDependencyInfo dependencyInfo;
    dependencyInfo.numStripesTotal                         = 0;
    dependencyInfo.readDependencies[0].relativeAgentId     = 0;
    dependencyInfo.readDependencies[1].relativeAgentId     = 0;
    dependencyInfo.writeDependencies[0].relativeAgentId    = 0;
    dependencyInfo.scheduleDependencies[0].relativeAgentId = 0;

    Agent ofmStreamerAgent{ ofmStreamerData, dependencyInfo };

    // Push the Mce Scheduler agent to the command stream
    AgentIdType agentId            = m_CommandStreamAgents.size();
    m_OpToAgentIdMapping[ptrDmaOp] = agentId;
    m_CommandStreamAgents.push_back(ofmStreamerAgent);

    return agentId;
}

// Private function to add ReadAfterWrite Dependency
// Consumer agent creates and own the dependency
inline void CascadingCompiler::AddReadAfterWriteDependency(const AgentType consumerAgentType,
                                                           const AgentIdType consumerAgentId,
                                                           const AgentType producerAgentType,
                                                           const AgentIdType producerAgentId)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    if (producerAgentType != AgentType::WGT_STREAMER)
    {
        Dependency& consumerAgentReadDependency0Ref =
            m_CommandStreamAgents[consumerAgentId].info.readDependencies.at(0);
        consumerAgentReadDependency0Ref.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
        FillConsumerAgentDependency(consumerAgentReadDependency0Ref, consumerAgentType, consumerAgentId,
                                    producerAgentType, producerAgentId);
    }
    else
    {
        Dependency& consumerAgentReadDependency1Ref =
            m_CommandStreamAgents[consumerAgentId].info.readDependencies.at(1);
        consumerAgentReadDependency1Ref.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
        FillConsumerAgentDependency(consumerAgentReadDependency1Ref, consumerAgentType, consumerAgentId,
                                    producerAgentType, producerAgentId);
    }
}

// Private function to add SRAM Overlap Dependency
// Consumer agent creates and own the dependency
inline void CascadingCompiler::AddSramOverlapDependency(const command_stream::cascading::AgentType consumerAgentType,
                                                        const AgentIdType consumerAgentId,
                                                        const command_stream::cascading::AgentType producerAgentType,
                                                        const AgentIdType producerAgentId)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    if (producerAgentType != AgentType::WGT_STREAMER)
    {
        Dependency& consumerAgentReadDependency0Ref =
            m_CommandStreamAgents[consumerAgentId].info.readDependencies.at(0);
        consumerAgentReadDependency0Ref.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
        FillConsumerAgentDependency(consumerAgentReadDependency0Ref, consumerAgentType, consumerAgentId,
                                    producerAgentType, producerAgentId);
    }
    else
    {
        Dependency& consumerAgentReadDependency1Ref =
            m_CommandStreamAgents[consumerAgentId].info.readDependencies.at(1);
        consumerAgentReadDependency1Ref.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
        FillConsumerAgentDependency(consumerAgentReadDependency1Ref, consumerAgentType, consumerAgentId,
                                    producerAgentType, producerAgentId);
    }
}

// Private function to add WriteAfterRead Dependency
// Last consumer agent creates the dependency and assign it to the producer agent
inline void CascadingCompiler::AddWriteAfterReadDependency(const AgentType consumerAgentType,
                                                           const AgentIdType consumerAgentId,
                                                           const AgentType producerAgentType,
                                                           const AgentIdType producerAgentId)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency& producerAgentWriteDependencyRef = m_CommandStreamAgents[producerAgentId].info.writeDependencies.at(0);
    producerAgentWriteDependencyRef.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
    FillProducerAgentDependency(producerAgentWriteDependencyRef, consumerAgentType, consumerAgentId, producerAgentType,
                                producerAgentId);
}

// Private function to add ScheduleTime Dependency
// First consumer agent creates the dependency and assign it to the producer agent
inline void CascadingCompiler::AddScheduleTimeDependency(const AgentType consumerAgentType,
                                                         const AgentIdType consumerAgentId,
                                                         const AgentType producerAgentType,
                                                         const AgentIdType producerAgentId)
{
    AgentIdType relativeAgentId = consumerAgentId - producerAgentId;
    assert(relativeAgentId <= g_MaxRelativeAgentPosition);

    Dependency& producerAgentScheduleDependencyRef =
        m_CommandStreamAgents[producerAgentId].info.scheduleDependencies.at(0);

    // Only the first consumer needs to update the relative agent id of the schedule dependency
    if (producerAgentScheduleDependencyRef.relativeAgentId == 0)
    {
        producerAgentScheduleDependencyRef.relativeAgentId = static_cast<RelativeAgentIdType>(relativeAgentId);
        FillProducerAgentDependency(producerAgentScheduleDependencyRef, consumerAgentType, consumerAgentId,
                                    producerAgentType, producerAgentId);
    }
}

// Private function to fill the dependency data for Read After Write or SRAM Overlap dependencies
void CascadingCompiler::FillConsumerAgentDependency(command_stream::cascading::Dependency& consumerAgentDependency,
                                                    const command_stream::cascading::AgentType consumerAgentType,
                                                    const AgentIdType consumerAgentId,
                                                    const command_stream::cascading::AgentType producerAgentType,
                                                    const AgentIdType producerAgentId)
{
    Agent& consumerAgent = m_CommandStreamAgents[consumerAgentId];
    Agent& producerAgent = m_CommandStreamAgents[producerAgentId];

    // Add a new 'Read After Write' dependency
    switch (consumerAgentType)
    {
        case AgentType::IFM_STREAMER:
        {
            break;
        }

        case AgentType::WGT_STREAMER:
        {
            break;
        }

        case AgentType::MCE_SCHEDULER:
        {
            // Read After Write Dependency for [MceScheduler][IfmStreamer]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                consumerAgentDependency.outerRatio.other = static_cast<uint8_t>(
                    producerAgent.data.ifm.fmData.numStripes.width * producerAgent.data.ifm.fmData.numStripes.height *
                    producerAgent.data.ifm.fmData.numStripes.channels);
                consumerAgentDependency.outerRatio.self = static_cast<uint8_t>(
                    consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth *
                    consumerAgent.data.mce.numStripes.ifmChannels);

                uint8_t widthRatio  = static_cast<uint8_t>(consumerAgent.data.mce.numStripes.ofmWidth /
                                                          producerAgent.data.ifm.fmData.dfltStripeSize.width);
                uint8_t heightRatio = static_cast<uint8_t>(consumerAgent.data.mce.numStripes.ofmHeight /
                                                           producerAgent.data.ifm.fmData.dfltStripeSize.height);

                assert(consumerAgent.data.mce.numStripes.ifmChannels ==
                       producerAgent.data.ifm.fmData.numStripes.channels);

                consumerAgentDependency.innerRatio.other = static_cast<uint8_t>(widthRatio * heightRatio);
                consumerAgentDependency.innerRatio.self  = 1;

                if ((producerAgent.data.ifm.fmData.numStripes.height > 1 &&
                     consumerAgent.data.mce.filterShape.height > 1) ||
                    (producerAgent.data.ifm.fmData.numStripes.width > 1 &&
                     consumerAgent.data.mce.filterShape.width > 1))
                {
                    consumerAgentDependency.boundary = 1;
                }
                else
                {
                    consumerAgentDependency.boundary = 0;
                }
            }
            // Read After Write Dependency for [MceScheduler][WeightStreamer]
            else if (producerAgentType == AgentType::WGT_STREAMER)
            {
                consumerAgentDependency.outerRatio.other = 1;
                consumerAgentDependency.innerRatio.other = 1;

                if (producerAgent.data.ifm.fmData.numStripes.channels > 1)
                {
                    consumerAgentDependency.outerRatio.self = 1;
                    consumerAgentDependency.innerRatio.self = 1;
                }
                else
                {
                    consumerAgentDependency.outerRatio.self = static_cast<uint8_t>(
                        consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                    consumerAgentDependency.innerRatio.self = static_cast<uint8_t>(
                        consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                }

                consumerAgentDependency.boundary = 0;
            }
            else
            {
                assert(false);
            }

            break;
        }

        case AgentType::PLE_LOADER:
        {
            break;
        }

        case AgentType::PLE_SCHEDULER:
        {
            break;
        }

        case AgentType::OFM_STREAMER:
        {
            break;
        }

        default:
        {
            break;
        }
    }
}

// Private function to fill the dependency data for Write After Read or Schedule Time dependencies
void CascadingCompiler::FillProducerAgentDependency(command_stream::cascading::Dependency& producerAgentDependency,
                                                    const command_stream::cascading::AgentType consumerAgentType,
                                                    const AgentIdType consumerAgentId,
                                                    const command_stream::cascading::AgentType producerAgentType,
                                                    const AgentIdType producerAgentId)
{
    Agent& consumerAgent = m_CommandStreamAgents[consumerAgentId];
    Agent& producerAgent = m_CommandStreamAgents[producerAgentId];

    // Add a new 'Write After Read' dependency or
    // Add a new 'Schedule Time' dependency
    switch (consumerAgentType)
    {
        case AgentType::IFM_STREAMER:
        {
            break;
        }

        case AgentType::WGT_STREAMER:
        {
            break;
        }

        case AgentType::MCE_SCHEDULER:
        {
            // Write After Read Dependency for [IfmStreamer][MceScheduler] or
            // Schedule Time Dependency for [IfmStreamer][MceScheduler]
            if (producerAgentType == AgentType::IFM_STREAMER)
            {
                producerAgentDependency.outerRatio.other = static_cast<uint8_t>(
                    consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth *
                    consumerAgent.data.mce.numStripes.ifmChannels);
                producerAgentDependency.outerRatio.self = static_cast<uint8_t>(
                    producerAgent.data.ifm.fmData.numStripes.width * producerAgent.data.ifm.fmData.numStripes.height *
                    producerAgent.data.ifm.fmData.numStripes.channels);

                uint8_t widthRatio  = static_cast<uint8_t>(consumerAgent.data.mce.numStripes.ofmWidth /
                                                          producerAgent.data.ifm.fmData.dfltStripeSize.width);
                uint8_t heightRatio = static_cast<uint8_t>(consumerAgent.data.mce.numStripes.ofmHeight /
                                                           producerAgent.data.ifm.fmData.dfltStripeSize.height);

                assert(producerAgent.data.ifm.fmData.numStripes.channels ==
                       consumerAgent.data.mce.numStripes.ifmChannels);

                producerAgentDependency.innerRatio.other = 1;
                producerAgentDependency.innerRatio.self  = static_cast<uint8_t>(widthRatio * heightRatio);

                if ((producerAgent.data.ifm.fmData.numStripes.height > 1 &&
                     consumerAgent.data.mce.filterShape.height > 1) ||
                    (producerAgent.data.ifm.fmData.numStripes.width > 1 &&
                     consumerAgent.data.mce.filterShape.width > 1))
                {
                    producerAgentDependency.boundary = 1;
                }
                else
                {
                    producerAgentDependency.boundary = 0;
                }
            }
            // Write After Read Dependency for [WeightStreamer][MceScheduler] or
            // Schedule Time Dependency for [WeightStreamer][MceScheduler]
            else if (producerAgentType == AgentType::WGT_STREAMER)
            {
                if (producerAgent.data.ifm.fmData.numStripes.channels > 1)
                {
                    producerAgentDependency.outerRatio.other = 1;
                    producerAgentDependency.innerRatio.other = 1;
                }
                else
                {
                    producerAgentDependency.outerRatio.other = static_cast<uint8_t>(
                        consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                    producerAgentDependency.innerRatio.other = static_cast<uint8_t>(
                        consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                }

                producerAgentDependency.outerRatio.self = 1;
                producerAgentDependency.innerRatio.self = 1;

                producerAgentDependency.boundary = 0;
            }
            // Schedule Time Dependency for [PleLoader][MceScheduler]
            else if (producerAgentType == AgentType::PLE_LOADER)
            {
                producerAgentDependency.outerRatio.other = static_cast<uint8_t>(
                    consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                producerAgentDependency.outerRatio.self = 1;

                producerAgentDependency.innerRatio.other = static_cast<uint8_t>(
                    consumerAgent.data.mce.numStripes.ofmHeight * consumerAgent.data.mce.numStripes.ofmWidth);
                producerAgentDependency.innerRatio.self = 1;

                producerAgentDependency.boundary = 0;
            }
            else
            {
                assert(false);
            }

            break;
        }

        case AgentType::PLE_LOADER:
        {
            break;
        }

        case AgentType::PLE_SCHEDULER:
        {
            break;
        }

        case AgentType::OFM_STREAMER:
        {
            break;
        }

        default:
        {
            break;
        }
    }
}

// Private function to add the lifetime information of the intermediate DRAM buffers
void CascadingCompiler::AddLifetimeInfoForIntermediateDramBuffers()
{
    // Lifetime start of the buffer holds the producer agent Id
    AgentIdType lifetimeStart;
    // Lifetime end of the buffer holds the last consumer agent Id
    AgentIdType lifetimeEnd;

    // Add the lifetime information for each intermediate DRAM buffer
    for (Buffer* buffer : m_MergedOpGraph.GetBuffers())
    {
        if (buffer->m_Location == Location::Dram)
        {
            assert(buffer->m_BufferType.has_value());

            // Check that the buffer type is intermediate
            if (buffer->m_BufferType.value() == BufferType::Intermediate)
            {
                // Set the Lifetime start and end of the intermediate DRAM buffer
                Op* producer = m_MergedOpGraph.GetProducer(buffer);
                assert(producer != nullptr);

                lifetimeStart = m_OpToAgentIdMapping.at(producer);
                lifetimeEnd   = 0;

                OpGraph::ConsumersList consumers = m_MergedOpGraph.GetConsumers(buffer);
                assert(consumers.size() >= 1);
                for (auto consumer : consumers)
                {
                    AgentIdType consumerAgentId = m_OpToAgentIdMapping.at(consumer.first);
                    if (consumerAgentId > lifetimeEnd)
                    {
                        lifetimeEnd = consumerAgentId;
                    }
                }

                // Add lifetime information of the corresponding buffer to the buffer manager
                m_BufferManager.MarkBufferUsedAtTime(m_IntermdiateDramBufToBufIdMapping.at(buffer),
                                                     static_cast<uint32_t>(lifetimeStart),
                                                     static_cast<uint32_t>(lifetimeEnd + 1));
            }
        }
    }
}
}    // namespace cascading_compiler
}    // namespace support_library
}    // namespace ethosn
