//
// Copyright © 2021 Arm Limited.
// SPDX-License-Identifier: Apache-2.0
//

#include "../src/DebuggingContext.hpp"
#include "../src/GraphNodes.hpp"
#include "../src/cascading/Cascading.hpp"
#include "../src/cascading/CombinerDFS.hpp"
#include "TestUtils.hpp"

#include <catch.hpp>

namespace dfs = ethosn::support_library::depth_first_search;

using namespace ethosn::support_library;
using namespace ethosn::command_stream;
using namespace depth_first_search;

namespace
{

void AddNodeToPart(GraphOfParts& gOfParts,
                   Node* node,
                   const EstimationOptions& estOpt,
                   const CompilationOptions& compOpt,
                   const HardwareCapabilities& hwCaps)
{
    gOfParts.m_Parts.push_back(std::make_unique<Part>(gOfParts.GeneratePartId(), estOpt, compOpt, hwCaps));
    (*(gOfParts.m_Parts.back())).m_SubGraph.push_back(node);
}

}    // namespace

/// Simple Node type for tests.
/// Includes a friendly name and ignores shape, quantisation info etc. so that tests
/// can focus on graph topology.
class NameOnlyNode : public Node
{
public:
    NameOnlyNode(NodeId id, std::string name)
        : Node(id,
               TensorShape(),
               ethosn::support_library::DataType::UINT8_QUANTIZED,
               QuantizationInfo(),
               CompilerDataFormat::NONE,
               std::set<uint32_t>{ 0 })
        , m_Name(name)
    {}

    DotAttributes GetDotAttributes() override
    {
        return DotAttributes(std::to_string(m_Id), m_Name, "");
    }

    bool IsPrepared() override
    {
        return false;
    }

    std::string m_Name;
};

TEST_CASE("IsPartInput", "[CombinerDFS]")
{
    Graph graph;
    /* Create graph:

      A   D
       \ /
        C
       / \
      B   E

    */
    NameOnlyNode* nodeA = graph.CreateAndAddNode<NameOnlyNode>("a");
    NameOnlyNode* nodeB = graph.CreateAndAddNode<NameOnlyNode>("b");
    NameOnlyNode* nodeC = graph.CreateAndAddNode<NameOnlyNode>("c");
    NameOnlyNode* nodeD = graph.CreateAndAddNode<NameOnlyNode>("d");
    NameOnlyNode* nodeE = graph.CreateAndAddNode<NameOnlyNode>("e");

    graph.Connect(nodeA, nodeC, 0);
    graph.Connect(nodeB, nodeC, 0);
    graph.Connect(nodeC, nodeD, 0);
    graph.Connect(nodeC, nodeE, 0);

    const CompilationOptions compOpt;
    const EstimationOptions estOpt;
    const HardwareCapabilities hwCaps = GetEthosN78HwCapabilities();

    GraphOfParts gOfParts;
    AddNodeToPart(gOfParts, nodeA, estOpt, compOpt, hwCaps);
    AddNodeToPart(gOfParts, nodeB, estOpt, compOpt, hwCaps);
    AddNodeToPart(gOfParts, nodeC, estOpt, compOpt, hwCaps);
    AddNodeToPart(gOfParts, nodeD, estOpt, compOpt, hwCaps);
    AddNodeToPart(gOfParts, nodeE, estOpt, compOpt, hwCaps);

    size_t count = 0;
    for (auto&& p : gOfParts.m_Parts)
    {
        REQUIRE(p->m_PartId == count);
        ++count;
    }

    dfs::Combiner combiner(gOfParts, hwCaps, estOpt);

    REQUIRE(combiner.IsPartInput(*gOfParts.m_Parts.at(0).get()) == true);
    REQUIRE(combiner.IsPartInput(*gOfParts.m_Parts.at(1).get()) == true);
    REQUIRE(combiner.IsPartInput(*gOfParts.m_Parts.at(2).get()) == false);
    REQUIRE(combiner.IsPartInput(*gOfParts.m_Parts.at(3).get()) == false);
    REQUIRE(combiner.IsPartInput(*gOfParts.m_Parts.at(4).get()) == false);
}