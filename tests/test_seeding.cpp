#include <gtest/gtest.h>
#include "as_node.h"
#include "as_graph.h"
#include "bgp.h"

/**
 * Test suite for Section 3.4: Seeding the AS graph with announcements
 * 
 * Tests the ability to create origin announcements and store them in an AS's local RIB.
 */

class SeedingTest : public ::testing::Test {
protected:
    void SetUp() override {
        graph.getOrCreateNode(1);
        as1 = graph.getNode(1);
        as1->setPolicy(std::make_unique<BGP>());
    }
    
    ASGraph graph;
    ASNode* as1;
};

/**
 * Test basic seeding of a single announcement
 * 
 * Verifies that:
 * - Announcement is created correctly
 * - Stored in local RIB
 * - Has ORIGIN relationship type
 * - AS-Path contains only the originating AS
 */
TEST_F(SeedingTest, BasicSeeding) {
    IPPrefix prefix("1.2.0.0/16");
    
    // Seed the announcement
    as1->seedAnnouncement(prefix);
    
    // Verify it's in the local RIB
    auto policy = as1->getPolicy();
    ASSERT_NE(policy, nullptr);
    EXPECT_TRUE(policy->hasRoute(prefix));
    
    // Get the announcement and verify details
    auto announcement = policy->getBestAnnouncement(prefix);
    ASSERT_TRUE(announcement.has_value());
    
    // Check prefix matches
    EXPECT_EQ(announcement->getPrefix().toString(), "1.2.0.0/16");
    
    // Check AS-Path is just [1]
    auto as_path = announcement->getASPath();
    ASSERT_EQ(as_path.size(), 1);
    EXPECT_EQ(as_path[0], 1);
    
    // Check next hop is AS1
    EXPECT_EQ(announcement->getNextHop(), 1);
    
    // Check relationship is ORIGIN
    EXPECT_EQ(announcement->getReceivedFrom(), RelationshipType::ORIGIN);
}

/**
 * Test seeding creates policy if not set
 * 
 * Verifies that seedAnnouncement creates a default BGP policy
 * if the AS doesn't have one yet.
 */
TEST_F(SeedingTest, SeedingCreatesPolicy) {
    auto as2 = std::make_shared<ASNode>(2);
    
    // Don't set a policy explicitly
    EXPECT_EQ(as2->getPolicy(), nullptr);
    
    // Seed an announcement
    as2->seedAnnouncement(IPPrefix("8.8.8.0/24"));
    
    // Should have created a policy
    EXPECT_NE(as2->getPolicy(), nullptr);
    
    // Should have the announcement
    EXPECT_TRUE(as2->getPolicy()->hasRoute(IPPrefix("8.8.8.0/24")));
}

/**
 * Test seeding multiple prefixes
 * 
 * Verifies an AS can originate multiple different prefixes.
 * Common scenario: large organizations own multiple IP blocks.
 */
TEST_F(SeedingTest, MultipleSeeds) {
    IPPrefix prefix1("1.2.0.0/16");
    IPPrefix prefix2("8.8.8.0/24");
    IPPrefix prefix3("2001:4860::/32");  // IPv6
    
    as1->seedAnnouncement(prefix1);
    as1->seedAnnouncement(prefix2);
    as1->seedAnnouncement(prefix3);
    
    auto policy = as1->getPolicy();
    EXPECT_TRUE(policy->hasRoute(prefix1));
    EXPECT_TRUE(policy->hasRoute(prefix2));
    EXPECT_TRUE(policy->hasRoute(prefix3));
    
    // Verify each has correct AS path
    auto ann1 = policy->getBestAnnouncement(prefix1);
    auto ann2 = policy->getBestAnnouncement(prefix2);
    auto ann3 = policy->getBestAnnouncement(prefix3);
    
    ASSERT_TRUE(ann1.has_value());
    ASSERT_TRUE(ann2.has_value());
    ASSERT_TRUE(ann3.has_value());
    
    EXPECT_EQ(ann1->getASPath().size(), 1);
    EXPECT_EQ(ann2->getASPath().size(), 1);
    EXPECT_EQ(ann3->getASPath().size(), 1);
}

/**
 * Test seeding with different ASes
 * 
 * Verifies multiple ASes can each originate their own prefixes.
 * This is the normal Internet scenario.
 */
TEST_F(SeedingTest, MultipleASesSeeding) {
    auto google = std::make_shared<ASNode>(15169);
    auto cloudflare = std::make_shared<ASNode>(13335);
    auto mit = std::make_shared<ASNode>(3);
    
    // Each AS announces its own prefix
    google->seedAnnouncement(IPPrefix("8.8.8.0/24"));      // Google DNS
    cloudflare->seedAnnouncement(IPPrefix("1.1.1.0/24"));  // Cloudflare DNS
    mit->seedAnnouncement(IPPrefix("18.0.0.0/8"));         // MIT network
    
    // Verify each AS has its own announcement
    auto google_ann = google->getPolicy()->getBestAnnouncement(IPPrefix("8.8.8.0/24"));
    auto cf_ann = cloudflare->getPolicy()->getBestAnnouncement(IPPrefix("1.1.1.0/24"));
    auto mit_ann = mit->getPolicy()->getBestAnnouncement(IPPrefix("18.0.0.0/8"));
    
    ASSERT_TRUE(google_ann.has_value());
    ASSERT_TRUE(cf_ann.has_value());
    ASSERT_TRUE(mit_ann.has_value());
    
    // Verify AS-Paths are correct
    EXPECT_EQ(google_ann->getASPath()[0], 15169);
    EXPECT_EQ(cf_ann->getASPath()[0], 13335);
    EXPECT_EQ(mit_ann->getASPath()[0], 3);
    
    // Verify next hops
    EXPECT_EQ(google_ann->getNextHop(), 15169);
    EXPECT_EQ(cf_ann->getNextHop(), 13335);
    EXPECT_EQ(mit_ann->getNextHop(), 3);
}

/**
 * Test ORIGIN preference
 * 
 * Verifies that ORIGIN announcements are preferred over other relationship types.
 * This is critical for BGP correctness - the origin always wins.
 */
TEST_F(SeedingTest, OriginPreference) {
    IPPrefix prefix("1.2.0.0/16");
    
    // Seed origin announcement
    as1->seedAnnouncement(prefix);
    
    // Try to add a customer announcement (should not replace origin)
    Announcement customer_ann(
        prefix,
        {2, 1},  // Longer path through AS2
        2,
        RelationshipType::FROM_CUSTOMER
    );
    as1->getPolicy()->receiveAnnouncement(customer_ann);
    
    // Origin should still be best
    auto best = as1->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getReceivedFrom(), RelationshipType::ORIGIN);
    EXPECT_EQ(best->getASPath().size(), 1);
}

/**
 * Integration test: Seed in realistic topology
 * 
 * Creates a small topology and seeds an announcement at an edge AS.
 * Verifies the announcement is stored correctly.
 */
TEST_F(SeedingTest, SeedInTopology) {
    ASGraph graph;
    
    // Create simple topology: Google -> ISP -> Tier1
    auto google = graph.getOrCreateNode(15169);
    auto isp = graph.getOrCreateNode(7922);
    auto tier1 = graph.getOrCreateNode(3356);
    
    // Set up relationships
    google->addProvider(isp);
    isp->addCustomer(google);
    isp->addProvider(tier1);
    tier1->addCustomer(isp);
    
    // Google announces its DNS service
    google->seedAnnouncement(IPPrefix("8.8.8.0/24"));
    
    // Verify Google has the announcement
    ASSERT_NE(google->getPolicy(), nullptr);
    EXPECT_TRUE(google->getPolicy()->hasRoute(IPPrefix("8.8.8.0/24")));
    
    auto ann = google->getPolicy()->getBestAnnouncement(IPPrefix("8.8.8.0/24"));
    ASSERT_TRUE(ann.has_value());
    
    // Verify announcement details
    EXPECT_EQ(ann->getPrefix().toString(), "8.8.8.0/24");
    EXPECT_EQ(ann->getASPath().size(), 1);
    EXPECT_EQ(ann->getASPath()[0], 15169);
    EXPECT_EQ(ann->getNextHop(), 15169);
    EXPECT_EQ(ann->getReceivedFrom(), RelationshipType::ORIGIN);
    
    // ISP and Tier1 should NOT have the announcement yet (no propagation yet)
    // That's for a future section
    if (isp->getPolicy()) {
        EXPECT_FALSE(isp->getPolicy()->hasRoute(IPPrefix("8.8.8.0/24")));
    }
    if (tier1->getPolicy()) {
        EXPECT_FALSE(tier1->getPolicy()->hasRoute(IPPrefix("8.8.8.0/24")));
    }
}

/**
 * Test seeding with specification example
 * 
 * Uses the exact example from the specification:
 * - Prefix: 1.2.0.0/16
 * - Next hop: 1
 * - AS-Path: [1]
 * - Relationship: ORIGIN
 */
TEST_F(SeedingTest, SpecificationExample) {
    auto as_one = std::make_shared<ASNode>(1);
    
    // Seed with the example prefix
    as_one->seedAnnouncement(IPPrefix("1.2.0.0/16"));
    
    // Verify all requirements from specification
    auto policy = as_one->getPolicy();
    ASSERT_NE(policy, nullptr) << "Policy should be created";
    
    EXPECT_TRUE(policy->hasRoute(IPPrefix("1.2.0.0/16"))) 
        << "Announcement should be in local RIB";
    
    auto ann = policy->getBestAnnouncement(IPPrefix("1.2.0.0/16"));
    ASSERT_TRUE(ann.has_value()) << "Should retrieve announcement";
    
    EXPECT_EQ(ann->getPrefix().toString(), "1.2.0.0/16") 
        << "Prefix should be 1.2.0.0/16";
    
    EXPECT_EQ(ann->getNextHop(), 1) 
        << "Next hop should be 1";
    
    auto path = ann->getASPath();
    ASSERT_EQ(path.size(), 1) << "AS-Path should have length 1";
    EXPECT_EQ(path[0], 1) << "AS-Path should be [1]";
    
    EXPECT_EQ(ann->getReceivedFrom(), RelationshipType::ORIGIN) 
        << "Relationship should be ORIGIN";
    
    std::cout << "\n=== Seeding Successful ===\n";
    std::cout << "AS1 now has announcement in local RIB:\n";
    std::cout << "  " << ann->toString() << "\n";
    std::cout << "  Relationship: ORIGIN (highest preference)\n";
    std::cout << "===========================\n";
}
