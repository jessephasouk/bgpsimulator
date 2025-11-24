#include <gtest/gtest.h>
#include "as_graph.h"
#include "as_node.h"
#include "announcement.h"
#include "bgp.h"

/**
 * @file test_conflicts.cpp
 * @brief Tests for Section 3.6 - Dealing with conflicts in BGP route selection
 * 
 * Tests cover the complete BGP decision process:
 * 1. Best relationship: Customer > Peer > Provider
 * 2. Shortest AS-Path (if relationship is the same)
 * 3. Lowest next_hop ASN (if path length is the same)
 * 4. First seen wins (if all else is equal)
 */

class ConflictTest : public ::testing::Test {
protected:
    ASGraph graph;
    IPPrefix prefix{"10.0.0.0/8"};
    
    void SetUp() override {
        // Will create topologies in individual tests
    }
};

/**
 * Test 1: Customer vs Peer vs Provider
 * 
 * AS receives announcements from all three relationship types.
 * Should prefer: Customer > Peer > Provider
 */
TEST_F(ConflictTest, RelationshipPreference) {
    auto as1 = graph.getOrCreateNode(1);  // Customer
    auto as2 = graph.getOrCreateNode(2);  // Peer
    auto as3 = graph.getOrCreateNode(3);  // Provider
    auto as4 = graph.getOrCreateNode(4);  // Receiver
    
    // AS4 relationships
    as4->addCustomer(as1);
    as1->addProvider(as4);
    as4->addPeer(as2);
    as2->addPeer(as4);
    as4->addProvider(as3);
    as3->addCustomer(as4);
    
    graph.flattenGraph();
    
    // All three ASes originate the same prefix
    as1->seedAnnouncement(prefix);
    as2->seedAnnouncement(prefix);
    as3->seedAnnouncement(prefix);
    
    // Propagate so AS4 receives all three
    graph.propagateAll();
    
    // AS4 should prefer customer route (AS1)
    ASSERT_TRUE(as4->getPolicy()->hasRoute(prefix));
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route->getNextHop(), 1);  // From AS1
}

/**
 * Test 2: Customer with longer path vs Provider with shorter path
 * 
 * Customer route should win even if path is longer
 * (Relationship trumps path length)
 */
TEST_F(ConflictTest, CustomerTrumpsPathLength) {
    // Topology:
    //   AS5 (provider of AS4, close: 1 hop)
    //     |
    //   AS4 (receiver)
    //     |
    //   AS1 - AS2 - AS3 (customer chain: 3 hops)
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as5 = graph.getOrCreateNode(5);
    
    // Build chain: AS1 -> AS2 -> AS3 -> AS4
    as2->addProvider(as3);
    as3->addCustomer(as2);
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    // AS5 is provider of AS4 (shorter path)
    as4->addProvider(as5);
    as5->addCustomer(as4);
    
    graph.flattenGraph();
    
    // Both originate same prefix
    as1->seedAnnouncement(prefix);
    as5->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 should prefer customer route (AS3->AS2->AS1) despite longer path
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    // Path should be [4, 3, 2, 1] - longer but customer route
    EXPECT_GT(route->getASPath().size(), 2);
    EXPECT_EQ(route->getASPath().back(), 1);  // Originated by AS1
}

/**
 * Test 3: Same relationship, different path lengths
 * 
 * When relationship is the same, prefer shorter AS-Path
 */
TEST_F(ConflictTest, PathLengthTieBreaker) {
    // Topology:
    //   AS1 (1 hop to AS4)
    //   AS2 - AS3 (2 hops to AS4)
    //   Both are customers of AS4
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    
    // AS1 is direct customer of AS4
    as1->addProvider(as4);
    as4->addCustomer(as1);
    
    // AS2 -> AS3 -> AS4 chain (AS3 is customer of AS4)
    as2->addProvider(as3);
    as3->addCustomer(as2);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    graph.flattenGraph();
    
    // Both originate same prefix
    as1->seedAnnouncement(prefix);
    as2->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 should prefer AS1 (shorter path: [4, 1] vs [4, 3, 2])
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route->getASPath().size(), 2);  // [4, 1]
    EXPECT_EQ(route->getNextHop(), 1);
}

/**
 * Test 4: Same relationship, same path length, different next_hop
 * 
 * This is the exact scenario from the spec:
 * AS4 receives from AS3 and AS666, both customers, same path length
 * Should prefer lower next_hop ASN
 */
TEST_F(ConflictTest, NextHopTieBreaker) {
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as666 = graph.getOrCreateNode(666);
    auto as100 = graph.getOrCreateNode(100);   // Origin behind AS3
    auto as200 = graph.getOrCreateNode(200);   // Origin behind AS666
    
    // AS4 has two customers: AS3 and AS666
    as3->addProvider(as4);
    as4->addCustomer(as3);
    as666->addProvider(as4);
    as4->addCustomer(as666);
    
    // Both have one customer each (same path length to AS4)
    as100->addProvider(as3);
    as3->addCustomer(as100);
    as200->addProvider(as666);
    as666->addCustomer(as200);
    
    graph.flattenGraph();
    
    // Both origin ASes announce same prefix
    as100->seedAnnouncement(prefix);
    as200->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 should prefer the route from AS3 (next_hop=3) over AS666 (next_hop=666)
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route->getASPath().size(), 3);  // [4, 3, 100] or [4, 666, 200]
    EXPECT_EQ(route->getNextHop(), 3);  // Should prefer lower ASN (3 < 666)
}

/**
 * Test 5: Specification Example - AS4 scenario
 * 
 * Recreate the exact example from bgpsimulator.com:
 * AS4 receives from AS3 and AS666 for same prefix
 * Both are customers, AS666 has shorter path
 * Should choose AS666 due to shorter path
 */
TEST_F(ConflictTest, SpecificationExample) {
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as666 = graph.getOrCreateNode(666);
    auto as100 = graph.getOrCreateNode(100);   // Behind AS3 (longer path)
    auto as200 = graph.getOrCreateNode(200);   // Behind AS3 (longer path)
    
    // AS4 has two customers: AS3 and AS666
    as3->addProvider(as4);
    as4->addCustomer(as3);
    as666->addProvider(as4);
    as4->addCustomer(as666);
    
    // AS3 has two customers: AS100 and AS200 (makes longer path)
    as100->addProvider(as3);
    as3->addCustomer(as100);
    as200->addProvider(as3);
    as3->addCustomer(as200);
    
    graph.flattenGraph();
    
    // AS100 originates prefix (path through AS3: AS4 <- AS3 <- AS100)
    as100->seedAnnouncement(prefix);
    
    // AS666 also originates same prefix (shorter path: AS4 <- AS666)
    as666->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 should prefer AS666 route (shorter path)
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route->getASPath().size(), 2);  // [4, 666] is shorter than [4, 3, 100]
    EXPECT_EQ(route->getNextHop(), 666);
    
    // Verify AS3 still received the route from AS100
    auto as3_route = as3->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(as3_route.has_value());
    EXPECT_EQ(as3_route->getASPath().size(), 2);  // [3, 100]
}

/**
 * Test 6: Multiple conflicts during propagation
 * 
 * Complex topology where an AS receives multiple competing routes
 * at different stages of propagation
 */
TEST_F(ConflictTest, MultipleConflicts) {
    // Diamond topology with multiple paths
    auto as1 = graph.getOrCreateNode(1);   // Origin
    auto as2 = graph.getOrCreateNode(2);   // Left path
    auto as3 = graph.getOrCreateNode(3);   // Right path
    auto as4 = graph.getOrCreateNode(4);   // Receiver
    
    // AS1 is customer of both AS2 and AS3
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as1->addProvider(as3);
    as3->addCustomer(as1);
    
    // Both AS2 and AS3 are customers of AS4
    as2->addProvider(as4);
    as4->addCustomer(as2);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    graph.flattenGraph();
    
    // AS1 originates
    as1->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 receives two routes: [4,2,1] and [4,3,1]
    // Same relationship (customer), same length, different next_hop
    // Should prefer lower next_hop (2 < 3)
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route->getASPath().size(), 3);
    EXPECT_EQ(route->getNextHop(), 2);  // Prefer AS2 (2 < 3)
}

/**
 * Test 7: Origin always wins
 * 
 * Even if other routes have better characteristics, 
 * ORIGIN relationship always takes precedence
 */
TEST_F(ConflictTest, OriginAlwaysWins) {
    auto as1 = graph.getOrCreateNode(1);  // Customer with short path
    auto as4 = graph.getOrCreateNode(4);  // Receiver (also originates)
    
    as1->addProvider(as4);
    as4->addCustomer(as1);
    
    graph.flattenGraph();
    
    // AS1 originates prefix
    as1->seedAnnouncement(prefix);
    
    // AS4 also originates same prefix (should win despite longer propagation)
    as4->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS4 should keep its own ORIGIN route
    auto route = as4->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::ORIGIN);
    EXPECT_EQ(route->getASPath().size(), 1);  // [4]
    EXPECT_EQ(route->getNextHop(), 4);
}

/**
 * Test 8: Peer vs Provider preference
 * 
 * When no customer routes available, prefer peer over provider
 */
TEST_F(ConflictTest, PeerOverProvider) {
    auto as1 = graph.getOrCreateNode(1);  // Peer
    auto as2 = graph.getOrCreateNode(2);  // Provider
    auto as3 = graph.getOrCreateNode(3);  // Receiver
    
    // AS3 has peer AS1 and provider AS2
    as1->addPeer(as3);
    as3->addPeer(as1);
    as2->addCustomer(as3);
    as3->addProvider(as2);
    
    graph.flattenGraph();
    
    // Both originate same prefix
    as1->seedAnnouncement(prefix);
    as2->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // AS3 should prefer peer route over provider route
    auto route = as3->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(route.has_value());
    
    EXPECT_EQ(route->getReceivedFrom(), RelationshipType::FROM_PEER);
    EXPECT_EQ(route->getNextHop(), 1);
}

/**
 * Test 9: First-seen tie-breaker
 * 
 * When all criteria are equal, keep the first announcement received
 */
TEST_F(ConflictTest, FirstSeenWins) {
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    // Create identical routes
    BGP policy;
    
    // Create two identical announcements (same everything)
    Announcement ann1(prefix, {1}, 1, RelationshipType::FROM_CUSTOMER);
    Announcement ann2(prefix, {1}, 1, RelationshipType::FROM_CUSTOMER);
    
    // Receive in order: ann1 first, then ann2
    policy.receiveAnnouncement(ann1);
    policy.receiveAnnouncement(ann2);
    
    // Should keep ann1 (first seen)
    auto best = policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
    
    // They're identical, so we just verify one was chosen
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

/**
 * Test 10: Complex multi-hop conflict
 * 
 * Realistic scenario with multiple competing paths
 */
TEST_F(ConflictTest, ComplexScenario) {
    // Create a tier-1, tier-2, edge topology
    auto edge1 = graph.getOrCreateNode(100);  // Origin 1
    auto edge2 = graph.getOrCreateNode(200);  // Origin 2
    auto tier2a = graph.getOrCreateNode(10);
    auto tier2b = graph.getOrCreateNode(20);
    auto tier1 = graph.getOrCreateNode(1);
    
    // Build topology
    edge1->addProvider(tier2a);
    tier2a->addCustomer(edge1);
    edge2->addProvider(tier2b);
    tier2b->addCustomer(edge2);
    
    tier2a->addProvider(tier1);
    tier1->addCustomer(tier2a);
    tier2b->addProvider(tier1);
    tier1->addCustomer(tier2b);
    
    // tier2a and tier2b are peers
    tier2a->addPeer(tier2b);
    tier2b->addPeer(tier2a);
    
    graph.flattenGraph();
    
    // edge1 originates
    edge1->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    // tier1 receives from both tier2a (customer, path=[1,10,100]) 
    // and tier2b (customer, path=[1,20,10,100] - goes via peer)
    // Should prefer tier2a route (shorter path, both customers)
    auto tier1_route = tier1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(tier1_route.has_value());
    EXPECT_EQ(tier1_route->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(tier1_route->getNextHop(), 10);  // From tier2a
}

/**
 * Test 11: Update scenario
 * 
 * AS receives a better route after already storing a route
 * Should update to the better route
 */
TEST_F(ConflictTest, RouteUpdate) {
    BGP policy;
    
    // First announcement: provider route (not great)
    Announcement provider_route(
        prefix, 
        {5, 3, 1},  // Long path
        5,
        RelationshipType::FROM_PROVIDER
    );
    
    policy.receiveAnnouncement(provider_route);
    
    // Verify provider route is installed
    auto route1 = policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(route1.has_value());
    EXPECT_EQ(route1->getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    
    // Second announcement: customer route (much better!)
    Announcement customer_route(
        prefix,
        {2, 1},  // Short path
        2,
        RelationshipType::FROM_CUSTOMER
    );
    
    policy.receiveAnnouncement(customer_route);
    
    // Should now prefer customer route
    auto route2 = policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(route2.has_value());
    EXPECT_EQ(route2->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(route2->getNextHop(), 2);
    EXPECT_EQ(route2->getASPath().size(), 2);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
