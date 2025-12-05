#include <gtest/gtest.h>
#include "as_graph.h"
#include "rov.h"
#include "announcement.h"
#include <fstream>

/**
 * Test fixture for Section 4: ROV (Route Origin Validation)
 * 
 * Tests BGP security features that defend against prefix hijacks.
 */
class ROVTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up any test files
        std::remove("test_rov_deployment.txt");
    }

    void TearDown() override {
        // Clean up test files
        std::remove("test_rov_deployment.txt");
    }

    /**
     * Helper: Create ROV deployment file
     */
    void createROVFile(const std::vector<uint32_t>& asns, const std::string& filename) {
        std::ofstream file(filename);
        for (uint32_t asn : asns) {
            file << asn << "\n";
        }
        file.close();
    }
};

/**
 * Test 1: ROV Policy Filters Invalid Announcements
 * 
 * Scenario: ROV-enabled AS receives legitimate and hijacked announcements
 * Expected: Accepts legitimate, drops hijacked
 */
TEST_F(ROVTest, ROVFiltersInvalidAnnouncements) {
    ROV rov_policy;
    
    IPPrefix prefix("10.0.0.0/8");
    
    // Legitimate announcement (rov_invalid=false)
    Announcement legit(prefix, 1, false);
    rov_policy.receiveAnnouncement(legit);
    
    // Should be stored
    EXPECT_TRUE(rov_policy.hasRoute(prefix));
    auto best = rov_policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getOriginASN(), 1);
    
    // Hijacked announcement (rov_invalid=true)
    Announcement hijack(prefix, 666, true);
    rov_policy.receiveAnnouncement(hijack);
    
    // Should still have legitimate route (hijack dropped)
    best = rov_policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getOriginASN(), 1);  // Still AS1, not AS666
    
    // Check received queue - should only have legit announcement
    auto received = rov_policy.getReceivedAnnouncements(prefix);
    EXPECT_EQ(received.size(), 1);  // Only legitimate announcement stored
    EXPECT_EQ(received[0].getOriginASN(), 1);
}

/**
 * Test 2: BGP Policy Accepts All Announcements
 * 
 * Scenario: Non-ROV AS receives legitimate and hijacked announcements
 * Expected: Accepts both (vulnerable!)
 */
TEST_F(ROVTest, BGPAcceptsAllAnnouncements) {
    BGP bgp_policy;
    
    IPPrefix prefix("10.0.0.0/8");
    
    // Legitimate announcement
    Announcement legit(prefix, 1, false);
    bgp_policy.receiveAnnouncement(legit);
    
    // Hijacked announcement (rov_invalid=true, but BGP doesn't check)
    Announcement hijack(prefix, 666, true);
    bgp_policy.receiveAnnouncement(hijack);
    
    // BGP should have both announcements
    auto received = bgp_policy.getReceivedAnnouncements(prefix);
    EXPECT_EQ(received.size(), 2);  // Both stored (vulnerable!)
    
    // BGP will choose based on relationship/path (not ROV validity)
    // Both are ORIGIN, so BGP behavior is undefined (could be either)
    auto best = bgp_policy.getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
}

/**
 * Test 3: Simple Hijack Scenario
 * 
 * Topology:
 *   AS666 (attacker) announces 1.2.0.0/16 [rov_invalid=true]
 *      |
 *   AS2 (ROV-enabled)
 *      |
 *   AS3 (no ROV)
 * 
 * Expected: AS2 drops hijack, AS3 never sees it
 */
TEST_F(ROVTest, SimpleHijackScenario) {
    ASGraph graph;
    
    auto as666 = graph.getOrCreateNode(666);  // Attacker
    auto as2 = graph.getOrCreateNode(2);      // ROV-enabled
    auto as3 = graph.getOrCreateNode(3);      // Downstream
    
    // Topology: AS666 -> AS2 -> AS3
    as666->addProvider(as2);
    as2->addCustomer(as666);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // Deploy ROV to AS2 only
    as2->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // AS666 announces hijack
    IPPrefix prefix("1.2.0.0/16");
    as666->seedAnnouncement(prefix, true);  // rov_invalid=true
    
    // Propagate
    graph.propagateAll();
    
    // AS666 has the route (it originated it)
    EXPECT_TRUE(as666->getPolicy()->hasRoute(prefix));
    
    // AS2 (ROV) should NOT have the route (filtered)
    EXPECT_FALSE(as2->getPolicy()->hasRoute(prefix));
    
    // AS3 should NOT have the route (AS2 never forwarded it)
    // Note: AS3 may have no policy if it never received any routes
    EXPECT_TRUE(!as3->getPolicy() || !as3->getPolicy()->hasRoute(prefix));
}

/**
 * Test 4: Legitimate vs Hijack - ROV Chooses Legitimate
 * 
 * Topology:
 *   AS777 (legit) announces 1.2.0.0/16 [rov_invalid=false]
 *      |
 *   AS2 (ROV) <--- also receives from AS666 (hijack)
 *      |
 *   AS666 (attacker) announces 1.2.0.0/16 [rov_invalid=true]
 * 
 * Expected: AS2 accepts AS777, drops AS666
 */
TEST_F(ROVTest, ROVPrefersLegitimateRoute) {
    ASGraph graph;
    
    auto as777 = graph.getOrCreateNode(777);  // Legitimate origin
    auto as666 = graph.getOrCreateNode(666);  // Attacker
    auto as2 = graph.getOrCreateNode(2);      // ROV-enabled receiver
    
    // Both connect to AS2
    as777->addProvider(as2);
    as2->addCustomer(as777);
    as666->addProvider(as2);
    as2->addCustomer(as666);
    
    // Deploy ROV
    as2->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // Both announce same prefix
    IPPrefix prefix("1.2.0.0/16");
    as777->seedAnnouncement(prefix, false);  // Legitimate
    as666->seedAnnouncement(prefix, true);   // Hijack
    
    // Propagate
    graph.propagateAll();
    
    // AS2 should only have AS777's route
    ASSERT_TRUE(as2->getPolicy()->hasRoute(prefix));
    auto best = as2->getPolicy()->getBestAnnouncement(prefix);
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->getOriginASN(), 777);  // AS777, not AS666
    EXPECT_FALSE(best->isROVInvalid());
    
    // Should only have one announcement (hijack was dropped)
    auto received = as2->getPolicy()->getReceivedAnnouncements(prefix);
    EXPECT_EQ(received.size(), 1);
}

/**
 * Test 5: Mixed Deployment - Some ASes Protected, Some Vulnerable
 * 
 * Topology:
 *         AS777 (legit)    AS666 (hijack)
 *             |                |
 *          AS2 (ROV)       AS3 (no ROV)
 *              \            /
 *                  AS4
 * 
 * Expected:
 * - AS2 only knows about AS777 (protected)
 * - AS3 knows about AS666 (vulnerable)
 * - AS4 receives both routes, chooses based on BGP rules
 */
TEST_F(ROVTest, MixedDeployment) {
    ASGraph graph;
    
    auto as777 = graph.getOrCreateNode(777);  // Legitimate
    auto as666 = graph.getOrCreateNode(666);  // Attacker
    auto as2 = graph.getOrCreateNode(2);      // ROV
    auto as3 = graph.getOrCreateNode(3);      // No ROV
    auto as4 = graph.getOrCreateNode(4);      // Downstream
    
    // Left branch: AS777 -> AS2 -> AS4
    as777->addProvider(as2);
    as2->addCustomer(as777);
    as2->addProvider(as4);
    as4->addCustomer(as2);
    
    // Right branch: AS666 -> AS3 -> AS4
    as666->addProvider(as3);
    as3->addCustomer(as666);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    // Deploy ROV to AS2 only
    as2->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // Both announce same prefix
    IPPrefix prefix("1.2.0.0/16");
    as777->seedAnnouncement(prefix, false);  // Legitimate
    as666->seedAnnouncement(prefix, true);   // Hijack
    
    // Propagate
    graph.propagateAll();
    
    // AS2 (ROV) should only have AS777
    ASSERT_TRUE(as2->getPolicy()->hasRoute(prefix));
    auto as2_best = as2->getPolicy()->getBestAnnouncement(prefix);
    EXPECT_EQ(as2_best->getOriginASN(), 777);
    
    // AS3 (no ROV) should have AS666 (vulnerable!)
    ASSERT_TRUE(as3->getPolicy()->hasRoute(prefix));
    auto as3_best = as3->getPolicy()->getBestAnnouncement(prefix);
    EXPECT_EQ(as3_best->getOriginASN(), 666);
    
    // AS4 receives both routes
    // - Via AS2: [4, 2, 777] (valid)
    // - Via AS3: [4, 3, 666] (invalid but propagated)
    ASSERT_TRUE(as4->getPolicy()->hasRoute(prefix));
    auto as4_received = as4->getPolicy()->getReceivedAnnouncements(prefix);
    EXPECT_EQ(as4_received.size(), 2);  // Both routes present
}

/**
 * Test 6: ROV Deployment from File
 * 
 * Scenario: Load ROV ASNs from file and verify deployment
 */
TEST_F(ROVTest, DeployFromFile) {
    ASGraph graph;
    
    // Create topology
    for (uint32_t i = 1; i <= 5; ++i) {
        auto node = graph.getOrCreateNode(i);
        node->setPolicy(std::make_unique<BGP>());  // Initialize with BGP
    }
    
    // Create ROV deployment file
    std::vector<uint32_t> rov_ases = {2, 3, 4};
    createROVFile(rov_ases, "test_rov_deployment.txt");
    
    // Deploy ROV (replaces BGP with ROV for specified ASes)
    ASSERT_TRUE(graph.deployROV("test_rov_deployment.txt"));
    
    // Verify deployment: AS2, AS3, AS4 should have ROV
    auto as1 = graph.getNode(1);
    auto as2 = graph.getNode(2);
    auto as3 = graph.getNode(3);
    auto as4 = graph.getNode(4);
    auto as5 = graph.getNode(5);
    
    // Test by seeing if they filter invalid announcements
    IPPrefix prefix("10.0.0.0/8");
    Announcement hijack(prefix, 666, true);
    
    // AS2, AS3, AS4 should filter (ROV deployed)
    as2->getPolicy()->receiveAnnouncement(hijack);
    as3->getPolicy()->receiveAnnouncement(hijack);
    as4->getPolicy()->receiveAnnouncement(hijack);
    
    EXPECT_FALSE(as2->getPolicy()->hasRoute(prefix));
    EXPECT_FALSE(as3->getPolicy()->hasRoute(prefix));
    EXPECT_FALSE(as4->getPolicy()->hasRoute(prefix));
    
    // AS1, AS5 should accept (no ROV)
    as1->getPolicy()->receiveAnnouncement(hijack);
    as5->getPolicy()->receiveAnnouncement(hijack);
    
    EXPECT_TRUE(as1->getPolicy()->hasRoute(prefix));
    EXPECT_TRUE(as5->getPolicy()->hasRoute(prefix));
}

/**
 * Test 7: Specification Example - AS666 Hijacks AS777's Prefix
 * 
 * From specification: "AS 666 has done what is called a prefix hijack.
 * Even though AS 777 is the correct owner of the prefix 1.2.0.0/16,
 * AS 666 has announced the same prefix, and captured much of the traffic!"
 * 
 * Topology:
 *     AS777 (origin)      AS666 (hijacker)
 *         |                    |
 *       AS3                  AS4 (ROV)
 *         \                  /
 *              AS5
 * 
 * Expected:
 * - AS4 (ROV) filters AS666's hijack
 * - AS3 accepts AS777's legitimate route
 * - AS5 sees route from AS3 (legitimate), not from AS4 (ROV blocked it)
 */
TEST_F(ROVTest, SpecificationExample) {
    ASGraph graph;
    
    auto as777 = graph.getOrCreateNode(777);  // Legitimate owner
    auto as666 = graph.getOrCreateNode(666);  // Hijacker
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);      // Will deploy ROV
    auto as5 = graph.getOrCreateNode(5);
    
    // Topology
    as777->addProvider(as3);
    as3->addCustomer(as777);
    as666->addProvider(as4);
    as4->addCustomer(as666);
    
    as3->addProvider(as5);
    as5->addCustomer(as3);
    as4->addProvider(as5);
    as5->addCustomer(as4);
    
    // Deploy ROV to AS4
    as4->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // Both announce same prefix
    IPPrefix prefix("1.2.0.0/16");
    as777->seedAnnouncement(prefix, false);  // Legitimate
    as666->seedAnnouncement(prefix, true);   // Hijack!
    
    // Propagate
    graph.propagateAll();
    
    // AS777 has its own route
    EXPECT_TRUE(as777->getPolicy()->hasRoute(prefix));
    
    // AS666 has its own route (hijacker)
    EXPECT_TRUE(as666->getPolicy()->hasRoute(prefix));
    
    // AS3 gets legitimate route from AS777
    ASSERT_TRUE(as3->getPolicy()->hasRoute(prefix));
    EXPECT_EQ(as3->getPolicy()->getBestAnnouncement(prefix)->getOriginASN(), 777);
    
    // AS4 (ROV) filters AS666's direct hijack announcement
    // But AS4 receives legitimate route from AS5 (which got it from AS3)
    ASSERT_TRUE(as4->getPolicy()->hasRoute(prefix));
    auto as4_best = as4->getPolicy()->getBestAnnouncement(prefix);
    EXPECT_EQ(as4_best->getOriginASN(), 777);  // Should be legitimate route
    EXPECT_FALSE(as4_best->isROVInvalid());     // Should NOT be invalid
    
    // AS5 receives from AS3 only (AS4 filtered hijack, only sent legit route)
    ASSERT_TRUE(as5->getPolicy()->hasRoute(prefix));
    EXPECT_EQ(as5->getPolicy()->getBestAnnouncement(prefix)->getOriginASN(), 777);
}

/**
 * Test 8: ROV Preserves rov_invalid Flag During Propagation
 * 
 * Scenario: Hijacked route propagates through non-ROV ASes
 * Expected: rov_invalid flag is preserved in forwarded announcements
 */
TEST_F(ROVTest, ROVFlagPreservedDuringPropagation) {
    ASGraph graph;
    
    auto as666 = graph.getOrCreateNode(666);  // Hijacker
    auto as2 = graph.getOrCreateNode(2);      // No ROV
    auto as3 = graph.getOrCreateNode(3);      // ROV
    
    // AS666 -> AS2 -> AS3
    as666->addProvider(as2);
    as2->addCustomer(as666);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // Deploy ROV to AS3 only
    as3->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // AS666 announces hijack
    IPPrefix prefix("1.2.0.0/16");
    as666->seedAnnouncement(prefix, true);  // rov_invalid=true
    
    // Propagate
    graph.propagateAll();
    
    // AS2 (no ROV) accepts the hijack
    ASSERT_TRUE(as2->getPolicy()->hasRoute(prefix));
    auto as2_route = as2->getPolicy()->getBestAnnouncement(prefix);
    EXPECT_TRUE(as2_route->isROVInvalid());  // Flag preserved!
    
    // AS3 (ROV) should filter it
    EXPECT_FALSE(as3->getPolicy()->hasRoute(prefix));
}

/**
 * Test 9: Multiple Hijackers
 * 
 * Scenario: Two hijackers announce same prefix
 * Expected: ROV filters both
 */
TEST_F(ROVTest, MultipleHijackers) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);      // Hijacker 1
    auto as2 = graph.getOrCreateNode(2);      // Hijacker 2
    auto as3 = graph.getOrCreateNode(3);      // ROV-enabled receiver
    
    // Both hijackers connect to AS3
    as1->addProvider(as3);
    as3->addCustomer(as1);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // Deploy ROV to AS3
    as3->setPolicy(std::make_unique<ROV>());
    
    graph.flattenGraph();
    
    // Both hijackers announce
    IPPrefix prefix("10.0.0.0/8");
    as1->seedAnnouncement(prefix, true);
    as2->seedAnnouncement(prefix, true);
    
    // Propagate
    graph.propagateAll();
    
    // AS3 should have NO routes (both filtered)
    EXPECT_FALSE(as3->getPolicy()->hasRoute(prefix));
}

/**
 * Test 10: ROV File with Comments and Invalid Entries
 * 
 * Scenario: ROV deployment file has comments, blank lines, invalid ASNs
 * Expected: Handles gracefully, only deploys valid ASNs
 */
TEST_F(ROVTest, ROVFileWithCommentsAndErrors) {
    ASGraph graph;
    
    // Create topology
    for (uint32_t i = 1; i <= 5; ++i) {
        graph.getOrCreateNode(i);
    }
    
    // Create ROV file with various formats
    std::ofstream file("test_rov_deployment.txt");
    file << "# This is a comment\n";
    file << "\n";  // Blank line
    file << "2\n";
    file << "# Another comment\n";
    file << "3\n";
    file << "\n";
    file << "999\n";  // ASN not in graph (should skip)
    file << "4\n";
    file.close();
    
    // Deploy ROV
    ASSERT_TRUE(graph.deployROV("test_rov_deployment.txt"));
    
    // Verify AS2, AS3, AS4 have ROV (AS999 skipped)
    IPPrefix prefix("10.0.0.0/8");
    Announcement hijack(prefix, 666, true);
    
    graph.getNode(2)->getPolicy()->receiveAnnouncement(hijack);
    graph.getNode(3)->getPolicy()->receiveAnnouncement(hijack);
    graph.getNode(4)->getPolicy()->receiveAnnouncement(hijack);
    
    EXPECT_FALSE(graph.getNode(2)->getPolicy()->hasRoute(prefix));
    EXPECT_FALSE(graph.getNode(3)->getPolicy()->hasRoute(prefix));
    EXPECT_FALSE(graph.getNode(4)->getPolicy()->hasRoute(prefix));
}
