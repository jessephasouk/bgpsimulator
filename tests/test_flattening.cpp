#include <gtest/gtest.h>
#include "as_graph.h"

/**
 * Test suite for graph flattening (Section 3.3)
 * 
 * Tests the propagation rank assignment algorithm that converts
 * the AS graph into a vector of vectors for ordered announcement propagation.
 */

class FlatteningTest : public ::testing::Test {
protected:
    ASGraph graph;
};

/**
 * Test simple linear topology: Edge -> Tier2 -> Tier1
 * 
 * Topology:
 *   AS3 (Tier-1, no providers)
 *    |
 *   AS2 (Tier-2)
 *    |
 *   AS1 (Edge, no customers)
 * 
 * Expected ranks:
 * - AS1: rank 0 (edge)
 * - AS2: rank 1 (provider of AS1)
 * - AS3: rank 2 (provider of AS2)
 */
TEST_F(FlatteningTest, SimpleLinearTopology) {
    auto edge = graph.getOrCreateNode(1);
    auto tier2 = graph.getOrCreateNode(2);
    auto tier1 = graph.getOrCreateNode(3);
    
    // AS2 is provider of AS1
    edge->addProvider(tier2);
    tier2->addCustomer(edge);
    
    // AS3 is provider of AS2
    tier2->addProvider(tier1);
    tier1->addCustomer(tier2);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 3 ranks
    ASSERT_EQ(flattened.size(), 3);
    
    // Rank 0: AS1 (edge)
    EXPECT_EQ(flattened[0].size(), 1);
    EXPECT_EQ(flattened[0][0], 1);
    EXPECT_EQ(edge->getPropagationRank(), 0);
    
    // Rank 1: AS2 (tier-2)
    EXPECT_EQ(flattened[1].size(), 1);
    EXPECT_EQ(flattened[1][0], 2);
    EXPECT_EQ(tier2->getPropagationRank(), 1);
    
    // Rank 2: AS3 (tier-1)
    EXPECT_EQ(flattened[2].size(), 1);
    EXPECT_EQ(flattened[2][0], 3);
    EXPECT_EQ(tier1->getPropagationRank(), 2);
    
    EXPECT_EQ(graph.getMaxRank(), 2);
}

/**
 * Test multiple edge ASes with shared provider
 * 
 * Topology:
 *       AS3 (Tier-2)
 *       /  \
 *    AS1  AS2 (both edge ASes)
 * 
 * Expected ranks:
 * - AS1, AS2: rank 0 (both edge)
 * - AS3: rank 1 (provider of both)
 */
TEST_F(FlatteningTest, MultipleEdgeASesSameProvider) {
    auto edge1 = graph.getOrCreateNode(1);
    auto edge2 = graph.getOrCreateNode(2);
    auto provider = graph.getOrCreateNode(3);
    
    // AS3 is provider of AS1 and AS2
    edge1->addProvider(provider);
    edge2->addProvider(provider);
    provider->addCustomer(edge1);
    provider->addCustomer(edge2);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 2 ranks
    ASSERT_EQ(flattened.size(), 2);
    
    // Rank 0: AS1 and AS2 (both edge)
    EXPECT_EQ(flattened[0].size(), 2);
    EXPECT_EQ(edge1->getPropagationRank(), 0);
    EXPECT_EQ(edge2->getPropagationRank(), 0);
    
    // Rank 1: AS3 (provider)
    EXPECT_EQ(flattened[1].size(), 1);
    EXPECT_EQ(flattened[1][0], 3);
    EXPECT_EQ(provider->getPropagationRank(), 1);
}

/**
 * Test diamond topology (multiple paths to top)
 * 
 * Topology:
 *       AS4 (Tier-1)
 *       /  \
 *    AS2  AS3 (Tier-2)
 *       \  /
 *       AS1 (Edge)
 * 
 * Expected ranks:
 * - AS1: rank 0
 * - AS2, AS3: rank 1 (both providers of AS1)
 * - AS4: rank 2 (provider of AS2 and AS3)
 */
TEST_F(FlatteningTest, DiamondTopology) {
    auto edge = graph.getOrCreateNode(1);
    auto tier2a = graph.getOrCreateNode(2);
    auto tier2b = graph.getOrCreateNode(3);
    auto tier1 = graph.getOrCreateNode(4);
    
    // AS2 and AS3 are providers of AS1
    edge->addProvider(tier2a);
    edge->addProvider(tier2b);
    tier2a->addCustomer(edge);
    tier2b->addCustomer(edge);
    
    // AS4 is provider of AS2 and AS3
    tier2a->addProvider(tier1);
    tier2b->addProvider(tier1);
    tier1->addCustomer(tier2a);
    tier1->addCustomer(tier2b);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 3 ranks
    ASSERT_EQ(flattened.size(), 3);
    
    // Rank 0: AS1
    EXPECT_EQ(flattened[0].size(), 1);
    EXPECT_EQ(edge->getPropagationRank(), 0);
    
    // Rank 1: AS2 and AS3
    EXPECT_EQ(flattened[1].size(), 2);
    EXPECT_EQ(tier2a->getPropagationRank(), 1);
    EXPECT_EQ(tier2b->getPropagationRank(), 1);
    
    // Rank 2: AS4
    EXPECT_EQ(flattened[2].size(), 1);
    EXPECT_EQ(tier1->getPropagationRank(), 2);
}

/**
 * Test disconnected components
 * 
 * Two separate hierarchies:
 * - AS1 (edge) -> AS2 (provider)
 * - AS3 (edge) -> AS4 (provider)
 * 
 * Both should get ranked independently
 */
TEST_F(FlatteningTest, DisconnectedComponents) {
    // First component
    auto edge1 = graph.getOrCreateNode(1);
    auto provider1 = graph.getOrCreateNode(2);
    edge1->addProvider(provider1);
    provider1->addCustomer(edge1);
    
    // Second component (disconnected)
    auto edge2 = graph.getOrCreateNode(3);
    auto provider2 = graph.getOrCreateNode(4);
    edge2->addProvider(provider2);
    provider2->addCustomer(edge2);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 2 ranks
    ASSERT_EQ(flattened.size(), 2);
    
    // Rank 0: AS1 and AS3 (both edge)
    EXPECT_EQ(flattened[0].size(), 2);
    EXPECT_EQ(edge1->getPropagationRank(), 0);
    EXPECT_EQ(edge2->getPropagationRank(), 0);
    
    // Rank 1: AS2 and AS4 (both providers)
    EXPECT_EQ(flattened[1].size(), 2);
    EXPECT_EQ(provider1->getPropagationRank(), 1);
    EXPECT_EQ(provider2->getPropagationRank(), 1);
}

/**
 * Test that peer relationships don't affect ranking
 * 
 * Topology:
 *    AS3 (provider)
 *     |
 *    AS1 (edge) <--peer--> AS2 (edge)
 * 
 * AS1 and AS2 are peers, both should be rank 0
 */
TEST_F(FlatteningTest, PeerRelationshipsIgnored) {
    auto edge1 = graph.getOrCreateNode(1);
    auto edge2 = graph.getOrCreateNode(2);
    auto provider = graph.getOrCreateNode(3);
    
    // AS3 is provider of AS1
    edge1->addProvider(provider);
    provider->addCustomer(edge1);
    
    // AS1 and AS2 are peers (both have no customers)
    edge1->addPeer(edge2);
    edge2->addPeer(edge1);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 2 ranks
    ASSERT_EQ(flattened.size(), 2);
    
    // Rank 0: AS1 and AS2 (both edge, peers don't affect ranking)
    EXPECT_EQ(flattened[0].size(), 2);
    EXPECT_EQ(edge1->getPropagationRank(), 0);
    EXPECT_EQ(edge2->getPropagationRank(), 0);
    
    // Rank 1: AS3
    EXPECT_EQ(flattened[1].size(), 1);
    EXPECT_EQ(provider->getPropagationRank(), 1);
}

/**
 * Test single isolated AS (no relationships)
 * 
 * Should be rank 0 (edge AS with no customers)
 */
TEST_F(FlatteningTest, SingleIsolatedAS) {
    auto isolated = graph.getOrCreateNode(1);
    
    auto flattened = graph.flattenGraph();
    
    // Should have 1 rank
    ASSERT_EQ(flattened.size(), 1);
    
    // Rank 0: AS1 (no customers = edge AS)
    EXPECT_EQ(flattened[0].size(), 1);
    EXPECT_EQ(flattened[0][0], 1);
    EXPECT_EQ(isolated->getPropagationRank(), 0);
}

/**
 * Integration test with CAIDA data
 * 
 * Loads real Internet topology and verifies:
 * - All nodes get ranked
 * - Ranks form valid hierarchy
 * - Edge ASes (no customers) are rank 0
 */
TEST_F(FlatteningTest, CAIDADataFlattening) {
    // Load CAIDA data
    ASSERT_TRUE(graph.buildFromCAIDAFile("../caida/20250901.as-rel2.txt"));
    
    auto flattened = graph.flattenGraph();
    
    // Should have multiple ranks
    EXPECT_GT(flattened.size(), 0);
    
    // All nodes should be ranked
    size_t totalRanked = 0;
    for (const auto& rank : flattened) {
        totalRanked += rank.size();
    }
    EXPECT_EQ(totalRanked, graph.getNodeCount());
    
    // Verify rank 0 ASes have no customers
    for (uint32_t asn : flattened[0]) {
        auto node = graph.getNode(asn);
        ASSERT_NE(node, nullptr);
        EXPECT_TRUE(node->getCustomers().empty()) 
            << "AS" << asn << " at rank 0 has customers!";
        EXPECT_EQ(node->getPropagationRank(), 0);
    }
    
    // Verify each rank's ASes have lower-ranked customers
    for (size_t rank = 1; rank < flattened.size(); rank++) {
        for (uint32_t asn : flattened[rank]) {
            auto node = graph.getNode(asn);
            ASSERT_NE(node, nullptr);
            EXPECT_EQ(node->getPropagationRank(), static_cast<int>(rank));
            
            // Should have at least one customer (otherwise would be rank 0)
            EXPECT_FALSE(node->getCustomers().empty())
                << "AS" << asn << " at rank " << rank << " has no customers!";
        }
    }
    
    std::cout << "\n=== CAIDA Flattening Results ===\n";
    std::cout << "Total ASes: " << graph.getNodeCount() << "\n";
    std::cout << "Number of ranks: " << flattened.size() << "\n";
    std::cout << "Max rank: " << graph.getMaxRank() << "\n";
    for (size_t i = 0; i < flattened.size(); i++) {
        std::cout << "Rank " << i << ": " << flattened[i].size() << " ASes\n";
    }
    std::cout << "================================\n";
}
