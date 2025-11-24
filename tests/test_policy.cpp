#include "bgp.h"
#include <gtest/gtest.h>

// ============================================================================
// BGP Policy Tests
// ============================================================================

class BGPTest : public ::testing::Test {
protected:
    BGP bgp;
    IPPrefix google_dns{"8.8.8.0/24"};
    IPPrefix cloudflare_dns{"1.1.1.0/24"};
    
    uint32_t google = 15169;
    uint32_t level3 = 3356;
    uint32_t verizon = 701;
    uint32_t comcast = 7922;
};

TEST_F(BGPTest, EmptyRIBInitially) {
    EXPECT_FALSE(bgp.hasRoute(google_dns));
    EXPECT_EQ(bgp.getBestAnnouncement(google_dns), std::nullopt);
    EXPECT_EQ(bgp.getLocalRIBSize(), 0);
    EXPECT_EQ(bgp.getTotalReceivedCount(), 0);
}

TEST_F(BGPTest, ReceiveSingleAnnouncement) {
    Announcement ann(google_dns, google);
    bgp.receiveAnnouncement(ann);
    
    EXPECT_TRUE(bgp.hasRoute(google_dns));
    EXPECT_EQ(bgp.getLocalRIBSize(), 1);
    EXPECT_EQ(bgp.getTotalReceivedCount(), 1);
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getPrefix(), google_dns);
    EXPECT_EQ(best->getOriginASN(), google);
}

TEST_F(BGPTest, ReceiveMultiplePrefixes) {
    Announcement ann1(google_dns, google);
    Announcement ann2(cloudflare_dns, 13335);
    
    bgp.receiveAnnouncement(ann1);
    bgp.receiveAnnouncement(ann2);
    
    EXPECT_TRUE(bgp.hasRoute(google_dns));
    EXPECT_TRUE(bgp.hasRoute(cloudflare_dns));
    EXPECT_EQ(bgp.getLocalRIBSize(), 2);
    EXPECT_EQ(bgp.getTotalReceivedCount(), 2);
}

TEST_F(BGPTest, ReceivedQueueStoresAllAnnouncements) {
    // Create three different announcements for same prefix
    Announcement ann1(google_dns, google);  // Direct from Google
    
    std::vector<uint32_t> path2 = {level3, google};
    Announcement ann2(google_dns, path2, level3, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> path3 = {verizon, google};
    Announcement ann3(google_dns, path3, verizon, RelationshipType::FROM_PEER);
    
    bgp.receiveAnnouncement(ann1);
    bgp.receiveAnnouncement(ann2);
    bgp.receiveAnnouncement(ann3);
    
    // Should only have one entry in RIB (best route)
    EXPECT_EQ(bgp.getLocalRIBSize(), 1);
    
    // But received queue should have all three
    auto received = bgp.getReceivedAnnouncements(google_dns);
    EXPECT_EQ(received.size(), 3);
    EXPECT_EQ(bgp.getTotalReceivedCount(), 3);
}

// ============================================================================
// BGP Route Selection Tests
// ============================================================================

TEST_F(BGPTest, PreferCustomerOverPeer) {
    // Create two announcements: one from customer, one from peer
    std::vector<uint32_t> customer_path = {level3, google};
    Announcement from_customer(google_dns, customer_path, level3, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> peer_path = {verizon, google};
    Announcement from_peer(google_dns, peer_path, verizon, RelationshipType::FROM_PEER);
    
    bgp.receiveAnnouncement(from_peer);      // Receive peer first
    bgp.receiveAnnouncement(from_customer);  // Then customer
    
    // Should prefer customer route
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(best->getNextHop(), level3);
}

TEST_F(BGPTest, PreferCustomerOverProvider) {
    std::vector<uint32_t> customer_path = {level3, google};
    Announcement from_customer(google_dns, customer_path, level3, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> provider_path = {verizon, google};
    Announcement from_provider(google_dns, provider_path, verizon, RelationshipType::FROM_PROVIDER);
    
    bgp.receiveAnnouncement(from_provider);
    bgp.receiveAnnouncement(from_customer);
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
}

TEST_F(BGPTest, PreferPeerOverProvider) {
    std::vector<uint32_t> peer_path = {level3, google};
    Announcement from_peer(google_dns, peer_path, level3, RelationshipType::FROM_PEER);
    
    std::vector<uint32_t> provider_path = {verizon, google};
    Announcement from_provider(google_dns, provider_path, verizon, RelationshipType::FROM_PROVIDER);
    
    bgp.receiveAnnouncement(from_provider);
    bgp.receiveAnnouncement(from_peer);
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_PEER);
}

TEST_F(BGPTest, PreferShorterPathSameRelationship) {
    // Both from customers, different path lengths
    std::vector<uint32_t> short_path = {level3, google};  // Length 2
    Announcement short_ann(google_dns, short_path, level3, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> long_path = {verizon, comcast, level3, google};  // Length 4
    Announcement long_ann(google_dns, long_path, verizon, RelationshipType::FROM_CUSTOMER);
    
    bgp.receiveAnnouncement(long_ann);   // Receive long first
    bgp.receiveAnnouncement(short_ann);  // Then short
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getPathLength(), 2);
    EXPECT_EQ(best->getNextHop(), level3);
}

TEST_F(BGPTest, RelationshipTrumpsPathLength) {
    // Customer route with long path vs provider route with short path
    // Customer should win despite longer path
    std::vector<uint32_t> long_customer_path = {comcast, verizon, level3, google};  // Length 4
    Announcement from_customer(google_dns, long_customer_path, comcast, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> short_provider_path = {level3, google};  // Length 2
    Announcement from_provider(google_dns, short_provider_path, level3, RelationshipType::FROM_PROVIDER);
    
    bgp.receiveAnnouncement(from_provider);
    bgp.receiveAnnouncement(from_customer);
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(best->getPathLength(), 4);  // Longer path wins because customer
}

TEST_F(BGPTest, FirstSeenWinsTieBreaker) {
    // Same relationship, same path length, different next_hop
    // Should prefer lower next_hop ASN (701 < 3356)
    std::vector<uint32_t> path1 = {level3, google};
    Announcement ann1(google_dns, path1, level3, RelationshipType::FROM_CUSTOMER);
    
    std::vector<uint32_t> path2 = {verizon, google};
    Announcement ann2(google_dns, path2, verizon, RelationshipType::FROM_CUSTOMER);
    
    bgp.receiveAnnouncement(ann1);  // Next hop = 3356
    bgp.receiveAnnouncement(ann2);  // Next hop = 701
    
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    // Should prefer lower next_hop ASN: 701 (verizon) < 3356 (level3)
    EXPECT_EQ(best->getNextHop(), verizon);
}

TEST_F(BGPTest, RouteUpdateChangesRIB) {
    // Receive provider route first
    std::vector<uint32_t> provider_path = {verizon, google};
    Announcement from_provider(google_dns, provider_path, verizon, RelationshipType::FROM_PROVIDER);
    bgp.receiveAnnouncement(from_provider);
    
    auto best1 = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best1.has_value());
    EXPECT_EQ(best1->getReceivedFrom(), RelationshipType::FROM_PROVIDER);
    
    // Now receive better customer route
    std::vector<uint32_t> customer_path = {level3, google};
    Announcement from_customer(google_dns, customer_path, level3, RelationshipType::FROM_CUSTOMER);
    bgp.receiveAnnouncement(from_customer);
    
    // RIB should be updated with better route
    auto best2 = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best2.has_value());
    EXPECT_EQ(best2->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(best2->getNextHop(), level3);
}

// ============================================================================
// Utility Function Tests
// ============================================================================

TEST_F(BGPTest, ClearResetsState) {
    Announcement ann(google_dns, google);
    bgp.receiveAnnouncement(ann);
    
    EXPECT_TRUE(bgp.hasRoute(google_dns));
    EXPECT_EQ(bgp.getLocalRIBSize(), 1);
    
    bgp.clear();
    
    EXPECT_FALSE(bgp.hasRoute(google_dns));
    EXPECT_EQ(bgp.getLocalRIBSize(), 0);
    EXPECT_EQ(bgp.getTotalReceivedCount(), 0);
}

TEST_F(BGPTest, GetReceivedAnnouncementsEmptyPrefix) {
    auto received = bgp.getReceivedAnnouncements(google_dns);
    EXPECT_TRUE(received.empty());
}

// ============================================================================
// Integration Test: Realistic Scenario
// ============================================================================

TEST(BGPIntegration, RealisticMultiPathScenario) {
    BGP bgp;
    IPPrefix google_dns("8.8.8.0/24");
    
    uint32_t google = 15169;
    uint32_t level3 = 3356;
    uint32_t verizon = 701;
    uint32_t comcast = 7922;
    uint32_t att = 7018;
    
    // Scenario: Small ISP receives Google DNS announcement from multiple upstreams
    
    // Path 1: Direct customer connection to Level3 → Google
    std::vector<uint32_t> path1 = {level3, google};
    Announcement ann1(google_dns, path1, level3, RelationshipType::FROM_CUSTOMER);
    
    // Path 2: Peer connection to AT&T → Level3 → Google
    std::vector<uint32_t> path2 = {att, level3, google};
    Announcement ann2(google_dns, path2, att, RelationshipType::FROM_PEER);
    
    // Path 3: Provider Verizon → Comcast → Google (long path)
    std::vector<uint32_t> path3 = {verizon, comcast, google};
    Announcement ann3(google_dns, path3, verizon, RelationshipType::FROM_PROVIDER);
    
    // Path 4: Another provider AT&T → Google (short path)
    std::vector<uint32_t> path4 = {att, google};
    Announcement ann4(google_dns, path4, att, RelationshipType::FROM_PROVIDER);
    
    // Receive all announcements
    bgp.receiveAnnouncement(ann3);  // Provider (long)
    bgp.receiveAnnouncement(ann4);  // Provider (short)
    bgp.receiveAnnouncement(ann2);  // Peer
    bgp.receiveAnnouncement(ann1);  // Customer
    
    // Verify received queue
    EXPECT_EQ(bgp.getTotalReceivedCount(), 4);
    auto received = bgp.getReceivedAnnouncements(google_dns);
    EXPECT_EQ(received.size(), 4);
    
    // BGP should select customer route (ann1) despite other options
    auto best = bgp.getBestAnnouncement(google_dns);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::FROM_CUSTOMER);
    EXPECT_EQ(best->getNextHop(), level3);
    EXPECT_EQ(best->getPathLength(), 2);
    
    // Verify only one entry in RIB
    EXPECT_EQ(bgp.getLocalRIBSize(), 1);
    
    std::cout << "\n=== BGP Route Selection ===" << std::endl;
    std::cout << "Received 4 announcements for " << google_dns.toString() << std::endl;
    std::cout << "Selected: " << best->toString() << std::endl;
    std::cout << "Reason: Customer route preferred (makes money!)" << std::endl;
}
