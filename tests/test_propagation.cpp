#include <gtest/gtest.h>
#include "as_graph.h"
#include "as_node.h"
#include "announcement.h"
#include "bgp.h"

/**
 * @file test_propagation.cpp
 * @brief Tests for Section 3.5 - BGP announcement propagation
 * 
 * Tests cover:
 * 1. Propagation up (to providers)
 * 2. Propagation across (to peers)
 * 3. Propagation down (to customers)
 * 4. Full propagation (all phases)
 * 5. Export policies (valley-free routing)
 * 6. Loop prevention
 * 7. AS-Path accumulation
 */

class PropagationTest : public ::testing::Test {
protected:
    ASGraph graph;
    IPPrefix prefix{"10.0.0.0/8"};
    
    void SetUp() override {
        // Will create topologies in individual tests
    }
};

/**
 * Test 1: Basic propagation up (customer to provider)
 * 
 * Topology:
 *   AS3 (provider, rank 1)
 *     |
 *   AS1 (customer, rank 0)
 * 
 * AS1 originates 10.0.0.0/8, should propagate to AS3
 */
TEST_F(PropagationTest, BasicPropagateUp) {
    // Create simple 2-AS topology
    auto as1 = graph.getOrCreateNode(1);
    auto as3 = graph.getOrCreateNode(3);
    
    // AS3 is provider, AS1 is customer
    as1->addProvider(as3);
    as3->addCustomer(as1);
    
    // Flatten graph to assign ranks
    graph.flattenGraph();
    EXPECT_EQ(as1->getPropagationRank(), 0);  // Edge AS
    EXPECT_EQ(as3->getPropagationRank(), 1);  // Provider
    
    // AS1 seeds announcement
    as1->seedAnnouncement(prefix);
    
    // Verify AS1 has the route
    ASSERT_TRUE(as1->getPolicy()->hasRoute(prefix));
    auto as1_route = as1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as1_route.has_value());
    EXPECT_EQ(as1_route->getReceivedFrom(), RelationshipType::ORIGIN);
    EXPECT_EQ(as1_route->getASPath().size(), 1);
    EXPECT_EQ(as1_route->getASPath()[0], 1);
    
    // Propagate up
    graph.propagateUp();
    
    // Verify AS3 received the announcement
    ASSERT_TRUE(as3->getPolicy()->hasRoute(prefix));
    auto as3_route = as3->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as3_route.has_value());
    
    // AS3 should see it as FROM_CUSTOMER
    EXPECT_EQ(as3_route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    
    // AS-Path should be [3, 1]
    EXPECT_EQ(as3_route->getASPath().size(), 2);
    EXPECT_EQ(as3_route->getASPath()[0], 3);
    EXPECT_EQ(as3_route->getASPath()[1], 1);
    
    // Next hop should be AS1
    EXPECT_EQ(as3_route->getNextHop(), 1);
}

/**
 * Test 2: Multi-hop propagation up
 * 
 * Topology:
 *   AS5 (tier-1, rank 2)
 *     |
 *   AS3 (tier-2, rank 1)
 *     |
 *   AS1 (edge, rank 0)
 * 
 * AS1's announcement should reach AS5 through AS3
 */
TEST_F(PropagationTest, MultiHopPropagateUp) {
    auto as1 = graph.getOrCreateNode(1);
    auto as3 = graph.getOrCreateNode(3);
    auto as5 = graph.getOrCreateNode(5);
    
    // Build provider-customer chain
    as1->addProvider(as3);
    as3->addCustomer(as1);
    as3->addProvider(as5);
    as5->addCustomer(as3);
    
    graph.flattenGraph();
    EXPECT_EQ(as1->getPropagationRank(), 0);
    EXPECT_EQ(as3->getPropagationRank(), 1);
    EXPECT_EQ(as5->getPropagationRank(), 2);
    
    // AS1 originates
    as1->seedAnnouncement(prefix);
    
    // Propagate up
    graph.propagateUp();
    
    // Verify AS5 has route with correct AS-Path [5, 3, 1]
    ASSERT_TRUE(as5->getPolicy()->hasRoute(prefix));
    auto route = as5->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->getASPath().size(), 3);
    EXPECT_EQ(route->getASPath()[0], 5);
    EXPECT_EQ(route->getASPath()[1], 3);
    EXPECT_EQ(route->getASPath()[2], 1);
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

/**
 * Test 3: Propagation across peers (single hop)
 * 
 * Topology:
 *   AS1 --- AS2 (peers)
 * 
 * AS1 originates, should send to AS2
 * But AS2 should NOT re-send to other peers (valley-free)
 */
TEST_F(PropagationTest, PropagateAcrossPeers) {
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    
    // Peer relationship (bidirectional)
    as1->addPeer(as2);
    as2->addPeer(as1);
    
    graph.flattenGraph();
    
    // AS1 originates
    as1->seedAnnouncement(prefix);
    
    // Propagate across peers
    graph.propagateAcross();
    
    // AS2 should have the route
    ASSERT_TRUE(as2->getPolicy()->hasRoute(prefix));
    auto route = as2->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_PEER);
    EXPECT_EQ(route->getASPath().size(), 2);
    EXPECT_EQ(route->getASPath()[0], 2);
    EXPECT_EQ(route->getASPath()[1], 1);
}

/**
 * Test 4: Peer export policy (valley-free routing)
 * 
 * Topology:
 *   AS3 (provider)
 *     |
 *   AS1 --- AS2 (peers)
 * 
 * AS3 originates, AS1 receives from provider
 * AS1 should NOT send provider route to peer AS2
 */
// NOTE: This test is disabled because we removed Gao-Rexford export policy filtering
// to match the assignment specification, which says "send all announcements" without
// mentioning selective export based on relationship types.
TEST_F(PropagationTest, DISABLED_PeerExportPolicy) {
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    // AS1 is customer of AS3
    as1->addProvider(as3);
    as3->addCustomer(as1);
    
    // AS1 and AS2 are peers
    as1->addPeer(as2);
    as2->addPeer(as1);
    
    graph.flattenGraph();
    
    // AS3 originates
    as3->seedAnnouncement(prefix);
    
    // Propagate up (AS1 won't receive since AS3 is higher rank)
    // So manually propagate down from AS3 to AS1
    graph.propagateDown();
    
    // AS1 should have route from provider
    ASSERT_TRUE(as1->getPolicy()->hasRoute(prefix));
    auto as1_route = as1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as1_route.has_value());
    EXPECT_EQ(as1_route->getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    
    // Now try peer propagation
    graph.propagateAcross();

    // AS2 should not receive provider-learned routes from peer AS1
    EXPECT_FALSE(as2->getPolicy()->hasRoute(prefix));
}

/**
 * Test 5: Propagation down (provider to customer)
 * 
 * Topology:
 *   AS5 (tier-1, rank 1)
 *     |
 *   AS1 (edge, rank 0)
 * 
 * AS5 originates, should propagate to AS1
 */
TEST_F(PropagationTest, BasicPropagateDown) {
    auto as1 = graph.getOrCreateNode(1);
    auto as5 = graph.getOrCreateNode(5);
    
    as1->addProvider(as5);
    as5->addCustomer(as1);
    
    graph.flattenGraph();
    
    // AS5 originates
    as5->seedAnnouncement(prefix);
    
    // Propagate down
    graph.propagateDown();
    
    // AS1 should have route
    ASSERT_TRUE(as1->getPolicy()->hasRoute(prefix));
    auto route = as1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    EXPECT_EQ(route->getASPath().size(), 2);
    EXPECT_EQ(route->getASPath()[0], 1);
    EXPECT_EQ(route->getASPath()[1], 5);
}

/**
 * Test 6: Loop prevention
 * 
 * Create a path where announcement would loop back to originator
 * Should be prevented by AS-Path loop detection
 */
TEST_F(PropagationTest, LoopPrevention) {
    // Topology: AS1 -> AS2 -> AS3, then try to send back to AS1
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // Create a peer link back to AS1 (potential loop)
    as3->addPeer(as1);
    as1->addPeer(as3);
    
    graph.flattenGraph();
    
    // AS1 originates
    as1->seedAnnouncement(prefix);
    
    // Run full propagation
    graph.propagateAll();
    
    // AS3 should have route with path [3, 2, 1]
    ASSERT_TRUE(as3->getPolicy()->hasRoute(prefix));
    auto as3_route = as3->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as3_route.has_value());
    EXPECT_EQ(as3_route->getASPath().size(), 3);
    
    // AS1 should still only have origin route (not receive it back from AS3)
    auto as1_route = as1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as1_route.has_value());
    EXPECT_EQ(as1_route->getReceivedFrom(), RelationshipType::ORIGIN);
    EXPECT_EQ(as1_route->getASPath().size(), 1);  // Still just [1]
}

/**
 * Test 7: Full propagation (up, across, down)
 * 
 * Complex topology with multiple ASes at different ranks
 */
TEST_F(PropagationTest, FullPropagation) {
    // Create diamond topology:
    //     AS5 (tier-1, rank 2)
    //     / \
    //   AS3 AS4 (tier-2, rank 1)
    //     \ /
    //     AS1 (edge, rank 0)
    
    auto as1 = graph.getOrCreateNode(1);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as5 = graph.getOrCreateNode(5);
    
    // AS1 has two providers
    as1->addProvider(as3);
    as3->addCustomer(as1);
    as1->addProvider(as4);
    as4->addCustomer(as1);
    
    // AS3 and AS4 both connect to AS5
    as3->addProvider(as5);
    as5->addCustomer(as3);
    as4->addProvider(as5);
    as5->addCustomer(as4);
    
    // AS3 and AS4 are peers
    as3->addPeer(as4);
    as4->addPeer(as3);
    
    graph.flattenGraph();
    
    // AS1 originates
    as1->seedAnnouncement(prefix);
    
    // Run full propagation
    graph.propagateAll();
    
    // All ASes should have the route
    EXPECT_TRUE(as1->getPolicy()->hasRoute(prefix));
    EXPECT_TRUE(as3->getPolicy()->hasRoute(prefix));
    EXPECT_TRUE(as4->getPolicy()->hasRoute(prefix));
    EXPECT_TRUE(as5->getPolicy()->hasRoute(prefix));
    
    // AS5 should prefer shorter path (through AS3 or AS4, both length 3)
    auto as5_route = as5->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as5_route.has_value());
    EXPECT_EQ(as5_route->getASPath().size(), 3);
    EXPECT_EQ(as5_route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

/**
 * Test 8: Multiple prefixes propagation
 * 
 * Multiple ASes originate different prefixes, all propagate correctly
 * Provider learns both prefixes from customers and redistributes to all
 */
TEST_F(PropagationTest, MultiplePrefixes) {
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    as1->addProvider(as3);
    as3->addCustomer(as1);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    graph.flattenGraph();
    
    // Different ASes originate different prefixes
    IPPrefix prefix1("10.0.0.0/8");
    IPPrefix prefix2("20.0.0.0/8");
    
    as1->seedAnnouncement(prefix1);
    as2->seedAnnouncement(prefix2);
    
    // Propagate
    graph.propagateAll();
    
    // AS3 should have both routes (learned from customers)
    EXPECT_TRUE(as3->getPolicy()->hasRoute(prefix1));
    EXPECT_TRUE(as3->getPolicy()->hasRoute(prefix2));
    
    // AS1 should have both: prefix1 (origin) and prefix2 (from AS3)
    // AS3 redistributes routes from customers to all customers
    EXPECT_TRUE(as1->getPolicy()->hasRoute(prefix1));
    EXPECT_TRUE(as1->getPolicy()->hasRoute(prefix2));  // Learned from AS3
    
    // AS2 should have both: prefix2 (origin) and prefix1 (from AS3)
    EXPECT_TRUE(as2->getPolicy()->hasRoute(prefix1));  // Learned from AS3
    EXPECT_TRUE(as2->getPolicy()->hasRoute(prefix2));
    
    // Verify AS1's origin route is preferred over learned route
    auto as1_prefix1 = as1->getPolicy()->getBestAnnouncement(prefix1);
    ASSERT_TRUE(as1_prefix1.has_value());
    EXPECT_EQ(as1_prefix1->getReceivedFrom(), RelationshipType::ORIGIN);
    
    // Verify AS2's origin route is preferred over learned route
    auto as2_prefix2 = as2->getPolicy()->getBestAnnouncement(prefix2);
    ASSERT_TRUE(as2_prefix2.has_value());
    EXPECT_EQ(as2_prefix2->getReceivedFrom(), RelationshipType::ORIGIN);
}

/**
 * Test 9: Customer routes preferred over peer routes
 * 
 * AS receives same prefix from customer and peer, should prefer customer
 */
TEST_F(PropagationTest, CustomerPreferredOverPeer) {
    // Topology:
    //   AS4 (origin)
    //     |
    //   AS2 (customer of AS3)
    //     
    //   AS1 --- AS3 (peers)
    //     |
    //   AS1 is also customer of AS3
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    
    // AS2 is customer of AS3
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // AS4 is customer of AS2
    as4->addProvider(as2);
    as2->addCustomer(as4);
    
    // AS1 and AS3 are peers
    as1->addPeer(as3);
    as3->addPeer(as1);
    
    // AS1 is also customer of AS3
    as1->addProvider(as3);
    as3->addCustomer(as1);
    
    graph.flattenGraph();
    
    // AS4 originates
    as4->seedAnnouncement(prefix);
    
    // Run full propagation
    graph.propagateAll();
    
    // AS3 should have route from customer (AS2)
    auto as3_route = as3->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as3_route.has_value());
    EXPECT_EQ(as3_route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

/**
 * Test 10: Real-world scenario with large topology
 * 
 * Load CAIDA data and test propagation at scale
 */
TEST_F(PropagationTest, LargeScalePropagation) {
    // Build from CAIDA file
    ASSERT_TRUE(graph.buildFromCAIDAFile("../caida/20250901.as-rel2.txt"));
    
    // Flatten graph
    auto flattened = graph.flattenGraph();
    EXPECT_GT(flattened.size(), 0);
    
    // Seed a few ASes with announcements
    IPPrefix google_prefix("8.8.8.0/24");
    IPPrefix cloudflare_prefix("1.1.1.0/24");
    
    // Google (AS15169) - might not be in CAIDA, use AS1 instead
    auto as1 = graph.getNode(1);
    if (as1) {
        as1->seedAnnouncement(google_prefix);
    }
    
    // Seed another AS
    auto as2 = graph.getNode(2);
    if (as2) {
        as2->seedAnnouncement(cloudflare_prefix);
    }
    
    // Run full propagation (this might take a few seconds)
    graph.propagateAll();
    
    // Verify some ASes received the announcements
    int count_google = 0;
    int count_cloudflare = 0;
    
    for (const auto& [asn, node] : graph.getNodes()) {
        if (node->getPolicy()) {
            if (node->getPolicy()->hasRoute(google_prefix)) {
                count_google++;
            }
            if (node->getPolicy()->hasRoute(cloudflare_prefix)) {
                count_cloudflare++;
            }
        }
        
        // Just check first 1000 to keep test fast
        if (count_google + count_cloudflare > 1000) break;
    }
    
    // Should have propagated to many ASes
    EXPECT_GT(count_google, 0);
    EXPECT_GT(count_cloudflare, 0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
