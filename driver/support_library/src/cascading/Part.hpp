//
// Copyright © 2018-2021 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "../Graph.hpp"
#include "Plan.hpp"

namespace ethosn
{
namespace support_library
{

template <typename D, typename B>
D* GetObjectAs(B* obj)
{
    return dynamic_cast<D*>(obj);
}

template <typename D, typename B>
const D* GetObjectAs(const B* obj)
{
    return dynamic_cast<const D*>(obj);
}

template <typename D, typename B>
bool IsObjectOfType(const B* obj)
{
    return (GetObjectAs<D>(obj) != nullptr);
}

using PartId         = size_t;
using StripeSizeType = TensorShape::value_type;

class WeightEncoderCache;

using Plans = std::vector<std::shared_ptr<Plan>>;

class Part : public DebuggableObject
{
public:
    using Nodes = std::vector<Node*>;

    using NumStripesType = uint32_t;
    struct NumStripes
    {
        NumStripesType m_Min;
        NumStripesType m_Max;
        bool operator<(const NumStripes& rhs) const;
    };

    struct MceStripesInfo
    {
        TensorShape m_Input;
        TensorShape m_Output;
        TensorShape m_Weight;
        command_stream::BlockConfig m_BlockConfig = { 8U, 8U };

        bool operator<(const MceStripesInfo& rhs) const;
    };

    struct PleStripesInfo
    {
        TensorShape m_Input;
        TensorShape m_Output;
        command_stream::BlockConfig m_BlockConfig = { 8U, 8U };
        bool operator<(const PleStripesInfo& rhs) const;
    };

    struct MemoryStripeInfo
    {
        NumStripes m_Range;
        TensorShape m_Shape;
        bool operator<(const MemoryStripeInfo& rhs) const;
    };

    struct MemoryStripesInfo
    {
        MemoryStripeInfo m_Input;
        MemoryStripeInfo m_Output;
        MemoryStripeInfo m_Weight;
        MemoryStripeInfo m_PleInput;
        bool operator<(const MemoryStripesInfo& rhs) const;
    };

    struct NumMemoryStripes
    {
        NumStripesType m_Input;
        NumStripesType m_Output;
        NumStripesType m_Weight;
        NumStripesType m_PleInput;
        bool operator<(const NumMemoryStripes& rhs) const;
    };

    // The following structs are intermediate representations of plans
    // describing the size of compute stripes and the size and number of memory stripes

    // A representation of plans with both mce and ple operations
    // this is to enable plans which need identity mce or identity ple operations
    struct MceAndPleInfo
    {
        MceStripesInfo m_MceCompute;
        PleStripesInfo m_PleCompute;
        MemoryStripesInfo m_Memory;
        Lifetime m_Lifetime = Lifetime::Cascade;

        bool operator<(const MceAndPleInfo& rhs) const;
    };

    // A representation of plans without an identity PLE operation
    // this is to enable fusing with subsequent ple operations
    struct MceOnlyInfo
    {
        MceStripesInfo m_MceCompute;
        MemoryStripesInfo m_Memory;
        Lifetime m_Lifetime = Lifetime::Cascade;

        bool operator<(const MceOnlyInfo& rhs) const;
    };

    // A representation of plans without an identity MCE operation
    // this is to enable fusing with preceding mce operations
    struct PleOnlyInfo
    {
        PleStripesInfo m_PleCompute;
        MemoryStripesInfo m_Memory;
        Lifetime m_Lifetime = Lifetime::Cascade;

        bool operator<(const PleOnlyInfo& rhs) const;
    };

    // A representation of plans that only use DMA and thus only
    // have information about memory
    struct DmaOnlyInfo
    {
        MemoryStripeInfo m_Input;
        MemoryStripeInfo m_Output;
        Lifetime m_Lifetime = Lifetime::Cascade;

        bool operator<(const DmaOnlyInfo& rhs) const;
    };

    struct StripeInfos
    {
        std::set<MceAndPleInfo> m_MceAndPleInfos;
        std::set<MceOnlyInfo> m_MceOnlyInfos;
        std::set<PleOnlyInfo> m_PleOnlyInfos;
        std::set<DmaOnlyInfo> m_DmaOnlyInfos;
    };

    Part(PartId id,
         const EstimationOptions& estOpt,
         const CompilationOptions& compOpt,
         const HardwareCapabilities& capabilities)
        : DebuggableObject("Part")
        , m_PartId(id)
        , m_EstimationOptions(estOpt)
        , m_CompilationOptions(compOpt)
        , m_Capabilities(capabilities)
    {}

    virtual Plans GetPlans() const;

    std::vector<const Edge*> GetInputs() const;
    std::vector<const Edge*> GetOutputs() const;

    // SubGraph of Nodes for this Part
    Nodes m_SubGraph;

    // All valid plans for this Part
    PartId m_PartId;

private:
    void AddNewPlan(Plan::InputMapping&& inputMappings,
                    Plan::OutputMapping&& outputMappings,
                    OwnedOpGraph&& opGraph,
                    Plans& plans) const;
    void CreateOpGraphAndPlan(Node* node,
                              DmaOnlyInfo& dmaInfo,
                              NumMemoryStripes& numMemoryStripes,
                              TraversalOrder order,
                              Location input,
                              Location output,
                              Plans& plans) const;
    void CreatePlanForInputNode(Node* node, Lifetime lifetime, TraversalOrder order, Plans& plans) const;
    void CreatePlanForOutputNode(Node* node, Lifetime lifetime, TraversalOrder order, Plans& plans) const;
    void GenerateWithTraversalOrders(Node* node, WeightEncoderCache& weightEncoderCache, Plans& plans) const;
    void GenerateWithStripeSizes(Node* node,
                                 const std::vector<command_stream::BlockConfig>& blockConfigs,
                                 TraversalOrder order,
                                 WeightEncoderCache& weightEncoderCache,
                                 Plans& plans) const;
    void GenerateWithNumStripes(Node* node,
                                TraversalOrder order,
                                StripeInfos& stripeInfos,
                                WeightEncoderCache& weightEncoderCache,
                                Plans& plans) const;
    void GenerateMcePlans(Node* node,
                          TraversalOrder order,
                          StripeInfos& stripeInfos,
                          WeightEncoderCache& weightEncoderCache,
                          Plans& plans) const;
    void GenerateFuseOnlyPlePlans(Node* node,
                                  TraversalOrder order,
                                  StripeInfos& stripeInfos,
                                  WeightEncoderCache& weightEncoderCache,
                                  Plans& plans) const;
    void GenerateFormatConversionPlans(Node* node,
                                       TraversalOrder order,
                                       StripeInfos& stripeInfos,
                                       Location inputBufferLocaton,
                                       Location outputBufferLocation,
                                       Plans& plans) const;
    void CreateReinterpretDramPlan(Node* node, Plans& plans) const;
    void CreateMceAndIdentityPlePlans(Node* node,
                                      const MceAndPleInfo& info,
                                      TraversalOrder order,
                                      WeightEncoderCache& weightEncoderCache,
                                      Plans& plans) const;
    void CreateMceOnlyPlans(Node* node,
                            const MceOnlyInfo& info,
                            TraversalOrder order,
                            WeightEncoderCache& weightEncoderCache,
                            Plans& plans) const;
    void CreateIdentityMceAndFusedPlePlans(Node* node,
                                           const MceAndPleInfo& info,
                                           TraversalOrder order,
                                           WeightEncoderCache& weightEncoderCache,
                                           Plans& plans) const;
    void CreateFuseOnlyPlans(Node* node, const PleOnlyInfo& info, TraversalOrder order, Plans& plans) const;

    void CreateFormatConversionPlans(Node* node,
                                     DmaOnlyInfo& dmaInfo,
                                     NumMemoryStripes& numMemoryStripes,
                                     TraversalOrder order,
                                     Location inputBufferLocaton,
                                     Location outputBufferLocation,
                                     Plans& plans) const;

    void CreateVirtualSramPlans(
        Node* node, DmaOnlyInfo& dmaInfo, NumMemoryStripes& numMemoryStripes, TraversalOrder order, Plans& plans) const;

    std::pair<Buffer*, Buffer*> AddIdentityMceOpForSubGraph(OwnedOpGraph& opGraph,
                                                            Lifetime lifetime,
                                                            const Part::MceStripesInfo& mceComputeInfo,
                                                            const Part::NumMemoryStripes& numMemoryStripes,
                                                            const Part::MemoryStripesInfo& memoryStripes,
                                                            const TensorShape& inpShape,
                                                            const QuantizationInfo& inpQuantInfo,
                                                            TraversalOrder order,
                                                            WeightEncoderCache& weightEncoderCache) const;

    void AddOpToOpGraphWithInputOutputBuffers(OwnedOpGraph& opGraph,
                                              Node* node,
                                              TraversalOrder order,
                                              DmaOnlyInfo& stripeInfos,
                                              NumMemoryStripes& numMemoryStripes,
                                              Location inputBufferLocation,
                                              Location outputBufferLocation,
                                              Plan::InputMapping& inputMappings,
                                              Plan::OutputMapping& outputMappings) const;

    const EstimationOptions& m_EstimationOptions;
    const CompilationOptions& m_CompilationOptions;
    const HardwareCapabilities& m_Capabilities;
};

using Parts = std::vector<std::unique_ptr<Part>>;

using InPart  = std::pair<bool, PartId>;
using OutPart = std::pair<bool, PartId>;

class GraphOfParts
{
public:
    GraphOfParts() = default;

    size_t GetNumParts() const;
    const Part& GetPart(const PartId id) const;
    const Parts& GetParts() const;

    InPart GetInputPart(const Edge& e) const;
    OutPart GetOutputPart(const Edge& e) const;

    PartId GeneratePartId()
    {
        PartId currId = m_NextPartId;
        ++m_NextPartId;
        return currId;
    }

    Parts m_Parts;
    PartId m_NextPartId = 0;
};

uint32_t CalculateTileSize(Node* node,
                           const HardwareCapabilities& caps,
                           const TensorShape& inputTensorShape,
                           const TensorShape& inputStripeShape,
                           const TensorShape& outputStripeShape,
                           uint32_t numStripes);

}    // namespace support_library
}    // namespace ethosn
