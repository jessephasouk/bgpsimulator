#include <gtest/gtest.h>
#include "as_graph.h"
#include "announcement.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <algorithm>

/**
 * Test fixture for Section 3.7: Output and Tests
 * 
 * Tests CSV output functionality and validates routing table correctness
 * across various scenarios from simple to complex.
 */
class OutputTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up any leftover test files
        std::remove("test_output.csv");
    }

    void TearDown() override {
        // Clean up test files
        std::remove("test_output.csv");
    }

    /**
     * Helper: Parse CSV file and return map of (asn, prefix) -> as_path
     */
    std::unordered_map<std::string, std::string> parseCSV(const std::string& filename) {
        std::unordered_map<std::string, std::string> routes;
        std::ifstream file(filename);
        EXPECT_TRUE(file.is_open()) << "Could not open CSV file: " << filename;
        
        std::string line;
        std::getline(file, line);  // Skip header
        
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string asn_str, prefix, as_path;
            
            // Parse: asn,prefix,as_path
            std::getline(iss, asn_str, ',');
            std::getline(iss, prefix, ',');
            std::getline(iss, as_path);
            
            std::string key = asn_str + "," + prefix;
            routes[key] = as_path;
        }
        
        return routes;
    }

    /**
     * Helper: Count lines in CSV (excluding header)
     */
    size_t countCSVLines(const std::string& filename) {
        std::ifstream file(filename);
        size_t count = 0;
        std::string line;
        std::getline(file, line);  // Skip header
        while (std::getline(file, line)) {
            count++;
        }
        return count;
    }
};

/**
 * Test 1: Single Announcement - Tiny Graph
 * 
 * Scenario: AS1 originates, propagates to AS2 (provider), then to AS3
 * 
 * Topology:
 *   AS1 (origin) -> AS2 (provider) -> AS3 (provider)
 * 
 * Expected routes:
 *   AS1: [1]
 *   AS2: [2, 1]
 *   AS3: [3, 2, 1]
 */
TEST_F(OutputTest, SingleAnnouncementTinyGraph) {
    ASGraph graph;
    
    // Create simple linear topology
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    // AS1 -> AS2 -> AS3 (provider chain)
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    graph.flattenGraph();
    
    // AS1 originates 10.0.0.0/8
    IPPrefix prefix("10.0.0.0/8");
    as1->seedAnnouncement(prefix);
    
    // Propagate
    graph.propagateAll();
    
    // Dump to CSV
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    // Parse and validate
    auto routes = parseCSV("test_output.csv");
    
    // Check AS1 (origin)
    EXPECT_EQ(routes["1,10.0.0.0/8"], "1");
    
    // Check AS2 (learned from customer AS1)
    EXPECT_EQ(routes["2,10.0.0.0/8"], "2 1");
    
    // Check AS3 (learned from customer AS2)
    EXPECT_EQ(routes["3,10.0.0.0/8"], "3 2 1");
    
    // Should have exactly 3 routes
    EXPECT_EQ(countCSVLines("test_output.csv"), 3);
}

/**
 * Test 2: Larger Graph with Multiple Paths
 * 
 * Scenario: Diamond topology with 6 ASes
 * 
 * Topology:
 *        AS5 --- AS6
 *       /   \   /   \
 *      AS2   AS3   AS4
 *       \     |     /
 *          AS1 (origin)
 * 
 * AS1 originates, all ASes receive via shortest path
 */
TEST_F(OutputTest, LargerGraphMultiplePaths) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);  // Origin
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as5 = graph.getOrCreateNode(5);
    auto as6 = graph.getOrCreateNode(6);
    
    // Bottom tier: AS1 has three providers
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as1->addProvider(as3);
    as3->addCustomer(as1);
    as1->addProvider(as4);
    as4->addCustomer(as1);
    
    // Middle tier: AS2, AS3, AS4 all connect to AS5 and AS6
    as2->addProvider(as5);
    as5->addCustomer(as2);
    as3->addProvider(as5);
    as5->addCustomer(as3);
    as3->addProvider(as6);
    as6->addCustomer(as3);
    as4->addProvider(as6);
    as6->addCustomer(as4);
    
    // Top tier: AS5 and AS6 are peers
    as5->addPeer(as6);
    as6->addPeer(as5);
    
    graph.flattenGraph();
    
    IPPrefix prefix("192.168.0.0/16");
    as1->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // Verify AS1 (origin)
    EXPECT_EQ(routes["1,192.168.0.0/16"], "1");
    
    // Verify direct providers of AS1
    EXPECT_EQ(routes["2,192.168.0.0/16"], "2 1");
    EXPECT_EQ(routes["3,192.168.0.0/16"], "3 1");
    EXPECT_EQ(routes["4,192.168.0.0/16"], "4 1");
    
    // AS5 should have path via AS2 or AS3 (both length 3)
    std::string as5_path = routes["5,192.168.0.0/16"];
    EXPECT_TRUE(as5_path == "5 2 1" || as5_path == "5 3 1");
    
    // AS6 should have path via AS3 or AS4 (both length 3)
    std::string as6_path = routes["6,192.168.0.0/16"];
    EXPECT_TRUE(as6_path == "6 3 1" || as6_path == "6 4 1");
    
    // Should have exactly 6 routes (one per AS)
    EXPECT_EQ(countCSVLines("test_output.csv"), 6);
}

/**
 * Test 3: Two Announcements Same Prefix
 * 
 * Scenario: AS1 and AS100 both announce 10.0.0.0/8
 *           AS4 is connected to both and must choose
 * 
 * Topology:
 *     AS1 (origin)     AS100 (origin)
 *        |                  |
 *       AS2                AS3
 *        \                  /
 *              AS4
 * 
 * AS4 receives two routes, chooses based on relationship/path length
 */
TEST_F(OutputTest, TwoAnnouncementsSamePrefix) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);    // Origin 1
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as100 = graph.getOrCreateNode(100); // Origin 2
    
    // Left branch: AS1 -> AS2 -> AS4
    as1->addProvider(as2);
    as2->addCustomer(as1);
    as2->addProvider(as4);
    as4->addCustomer(as2);
    
    // Right branch: AS100 -> AS3 -> AS4
    as100->addProvider(as3);
    as3->addCustomer(as100);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    graph.flattenGraph();
    
    // Both origin same prefix
    IPPrefix prefix("10.0.0.0/8");
    as1->seedAnnouncement(prefix);
    as100->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // Origins should have their own routes
    EXPECT_EQ(routes["1,10.0.0.0/8"], "1");
    EXPECT_EQ(routes["100,10.0.0.0/8"], "100");
    
    // AS2 only knows about AS1
    EXPECT_EQ(routes["2,10.0.0.0/8"], "2 1");
    
    // AS3 only knows about AS100
    EXPECT_EQ(routes["3,10.0.0.0/8"], "3 100");
    
    // AS4 receives from both customers (AS2 and AS3)
    // Both are customers, same relationship
    // AS2's path: [4, 2, 1] (length 3)
    // AS3's path: [4, 3, 100] (length 3)
    // Should choose based on next_hop: 2 < 3, so chooses AS2
    EXPECT_EQ(routes["4,10.0.0.0/8"], "4 2 1");
    
    // Should have 5 routes total
    EXPECT_EQ(countCSVLines("test_output.csv"), 5);
}

/**
 * Test 4: Conflict Resolution - Path Length Matters
 * 
 * Scenario: AS4 receives same prefix via short and long paths
 * 
 * Topology:
 *          AS100 (origin)
 *         /            \
 *       AS2             AS3
 *         \            /
 *              AS4
 * 
 * AS4 receives via AS2 (direct: [4,2,100]) and AS3 (direct: [4,3,100])
 * Both same relationship, same length -> next_hop tie-breaker
 */
TEST_F(OutputTest, ConflictResolutionPathLength) {
    ASGraph graph;
    
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as100 = graph.getOrCreateNode(100);  // Origin
    
    // AS100 has two providers
    as100->addProvider(as2);
    as2->addCustomer(as100);
    as100->addProvider(as3);
    as3->addCustomer(as100);
    
    // Both AS2 and AS3 connect to AS4
    as2->addProvider(as4);
    as4->addCustomer(as2);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    graph.flattenGraph();
    
    IPPrefix prefix("172.16.0.0/12");
    as100->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // AS100 origin
    EXPECT_EQ(routes["100,172.16.0.0/12"], "100");
    
    // AS2 and AS3 learn from customer
    EXPECT_EQ(routes["2,172.16.0.0/12"], "2 100");
    EXPECT_EQ(routes["3,172.16.0.0/12"], "3 100");
    
    // AS4 receives from both customers (AS2 and AS3)
    // Both paths same length (3), both customers
    // Chooses based on next_hop: 2 < 3
    EXPECT_EQ(routes["4,172.16.0.0/12"], "4 2 100");
    
    EXPECT_EQ(countCSVLines("test_output.csv"), 4);
}

/**
 * Test 5: Specification Example from Section 3.6
 * 
 * Scenario: AS4 receives from AS3 and AS666 for same prefix
 *           Both are customers, AS666 has shorter path
 * 
 * Topology:
 *     AS100 --- AS3 --- AS4
 *                        |
 *                      AS666
 * 
 * AS4 should choose AS666 (shorter path: [4,666] vs [4,3,100])
 */
TEST_F(OutputTest, SpecificationExampleOutput) {
    ASGraph graph;
    
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    auto as100 = graph.getOrCreateNode(100);
    auto as666 = graph.getOrCreateNode(666);
    
    // AS100 -> AS3 -> AS4
    as100->addProvider(as3);
    as3->addCustomer(as100);
    as3->addProvider(as4);
    as4->addCustomer(as3);
    
    // AS666 -> AS4 (direct)
    as666->addProvider(as4);
    as4->addCustomer(as666);
    
    graph.flattenGraph();
    
    // Both announce same prefix
    IPPrefix prefix("1.2.0.0/16");
    as100->seedAnnouncement(prefix);
    as666->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // Origins
    EXPECT_EQ(routes["100,1.2.0.0/16"], "100");
    EXPECT_EQ(routes["666,1.2.0.0/16"], "666");
    
    // AS3 only sees AS100 (its customer)
    EXPECT_EQ(routes["3,1.2.0.0/16"], "3 100");
    
    // AS4 sees both:
    //   Via AS3: [4, 3, 100] (length 3)
    //   Via AS666: [4, 666] (length 2)
    // Chooses AS666 (shorter path)
    EXPECT_EQ(routes["4,1.2.0.0/16"], "4 666");
    
    EXPECT_EQ(countCSVLines("test_output.csv"), 4);
}

/**
 * Test 6: Multiple Prefixes - Different Origins
 * 
 * Scenario: Multiple prefixes from different ASes
 * 
 * AS1 announces 10.0.0.0/8
 * AS2 announces 20.0.0.0/8
 * Both propagate through AS3, which sends to both customers
 */
TEST_F(OutputTest, MultiplePrefixesDifferentOrigins) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    
    // AS1 -> AS3
    as1->addProvider(as3);
    as3->addCustomer(as1);
    
    // AS2 -> AS3
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    graph.flattenGraph();
    
    // Different prefixes
    IPPrefix prefix1("10.0.0.0/8");
    IPPrefix prefix2("20.0.0.0/8");
    
    as1->seedAnnouncement(prefix1);
    as2->seedAnnouncement(prefix2);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // AS1 has its own prefix, and learns AS2's prefix from AS3
    EXPECT_EQ(routes["1,10.0.0.0/8"], "1");
    EXPECT_EQ(routes["1,20.0.0.0/8"], "1 3 2");  // AS3 sends customer route down
    
    // AS2 has its own prefix, and learns AS1's prefix from AS3
    EXPECT_EQ(routes["2,20.0.0.0/8"], "2");
    EXPECT_EQ(routes["2,10.0.0.0/8"], "2 3 1");  // AS3 sends customer route down
    
    // AS3 has both prefixes from its customers
    EXPECT_EQ(routes["3,10.0.0.0/8"], "3 1");
    EXPECT_EQ(routes["3,20.0.0.0/8"], "3 2");
    
    // Total: 6 routes (2 for AS1, 2 for AS2, 2 for AS3)
    EXPECT_EQ(countCSVLines("test_output.csv"), 6);
}

/**
 * Test 7: Export Policy Filtering
 * 
 * Scenario: Verify provider routes are NOT sent to peers/providers
 * 
 * Topology:
 *       AS1 (origin)
 *          |
 *        AS2 (learns from customer)
 *       /   \
 *     AS3   AS4 (peer)
 *   (provider)
 * 
 * AS2 should:
 * - Send to AS3 (provider) - YES (customer route)
 * - Send to AS4 (peer) - YES (customer route)
 * 
 * AS3 should NOT send to AS4 (provider route -> peer)
 */
TEST_F(OutputTest, ExportPolicyFiltering) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    auto as3 = graph.getOrCreateNode(3);
    auto as4 = graph.getOrCreateNode(4);
    
    // AS1 -> AS2
    as1->addProvider(as2);
    as2->addCustomer(as1);
    
    // AS2 -> AS3 (AS3 is provider)
    as2->addProvider(as3);
    as3->addCustomer(as2);
    
    // AS2 <-> AS4 (peers)
    as2->addPeer(as4);
    as4->addPeer(as2);
    
    graph.flattenGraph();
    
    IPPrefix prefix("8.8.8.0/24");
    as1->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // AS1, AS2, AS3, AS4 should all have routes
    EXPECT_EQ(routes["1,8.8.8.0/24"], "1");
    EXPECT_EQ(routes["2,8.8.8.0/24"], "2 1");
    EXPECT_EQ(routes["3,8.8.8.0/24"], "3 2 1");
    EXPECT_EQ(routes["4,8.8.8.0/24"], "4 2 1");
    
    // All 4 ASes should have the route
    EXPECT_EQ(countCSVLines("test_output.csv"), 4);
}

/**
 * Test 8: Empty Graph - No Announcements
 * 
 * Scenario: Graph with ASes but no announcements seeded
 * CSV should only have header
 */
TEST_F(OutputTest, EmptyGraphNoAnnouncements) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    
    as1->addProvider(as2);
    as2->addCustomer(as1);
    
    graph.flattenGraph();
    
    // No announcements seeded
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    // Should have 0 routes (only header)
    EXPECT_EQ(countCSVLines("test_output.csv"), 0);
}

/**
 * Test 9: Large Scale Simulation
 * 
 * Scenario: Multiple origins, complex topology
 * Validates CSV output format and consistency
 * 
 * Creates a 10-AS network with 3 origins
 */
TEST_F(OutputTest, LargeScaleSimulation) {
    ASGraph graph;
    
    std::vector<std::shared_ptr<ASNode>> ases;
    for (uint32_t i = 1; i <= 10; ++i) {
        ases.push_back(graph.getOrCreateNode(i));
    }
    
    // Create hierarchical topology
    // Tier 1: AS9, AS10 (peers)
    ases[8]->addPeer(ases[9]);
    ases[9]->addPeer(ases[8]);
    
    // Tier 2: AS5-8 (customers of tier 1)
    for (int i = 4; i < 8; ++i) {
        ases[i]->addProvider(ases[8]);
        ases[8]->addCustomer(ases[i]);
        ases[i]->addProvider(ases[9]);
        ases[9]->addCustomer(ases[i]);
    }
    
    // Tier 3: AS1-4 (edge ASes)
    for (int i = 0; i < 4; ++i) {
        int provider = (i % 4) + 4;  // Connect to AS5-8
        ases[i]->addProvider(ases[provider]);
        ases[provider]->addCustomer(ases[i]);
    }
    
    graph.flattenGraph();
    
    // Three origins announce different prefixes
    IPPrefix prefix1("10.0.0.0/8");
    IPPrefix prefix2("20.0.0.0/8");
    IPPrefix prefix3("30.0.0.0/8");
    
    ases[0]->seedAnnouncement(prefix1);  // AS1
    ases[1]->seedAnnouncement(prefix2);  // AS2
    ases[2]->seedAnnouncement(prefix3);  // AS3
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    auto routes = parseCSV("test_output.csv");
    
    // Every AS should have routes to all 3 prefixes
    // 10 ASes * 3 prefixes = 30 routes
    EXPECT_EQ(countCSVLines("test_output.csv"), 30);
    
    // Verify tier-1 ASes have routes to all prefixes
    EXPECT_TRUE(routes.count("9,10.0.0.0/8") > 0);
    EXPECT_TRUE(routes.count("9,20.0.0.0/8") > 0);
    EXPECT_TRUE(routes.count("9,30.0.0.0/8") > 0);
    EXPECT_TRUE(routes.count("10,10.0.0.0/8") > 0);
    EXPECT_TRUE(routes.count("10,20.0.0.0/8") > 0);
    EXPECT_TRUE(routes.count("10,30.0.0.0/8") > 0);
    
    // Verify origins have correct paths
    EXPECT_EQ(routes["1,10.0.0.0/8"], "1");
    EXPECT_EQ(routes["2,20.0.0.0/8"], "2");
    EXPECT_EQ(routes["3,30.0.0.0/8"], "3");
}

/**
 * Test 10: CSV Format Validation
 * 
 * Validates CSV structure:
 * - Header row exists
 * - All rows have 3 columns
 * - ASN is numeric
 * - Prefix is valid format
 * - AS-Path is space-separated numbers
 */
TEST_F(OutputTest, CSVFormatValidation) {
    ASGraph graph;
    
    auto as1 = graph.getOrCreateNode(1);
    auto as2 = graph.getOrCreateNode(2);
    
    as1->addProvider(as2);
    as2->addCustomer(as1);
    
    graph.flattenGraph();
    
    IPPrefix prefix("192.0.2.0/24");
    as1->seedAnnouncement(prefix);
    
    graph.propagateAll();
    
    ASSERT_TRUE(graph.dumpToCSV("test_output.csv"));
    
    // Read file and validate format
    std::ifstream file("test_output.csv");
    ASSERT_TRUE(file.is_open());
    
    std::string header;
    std::getline(file, header);
    EXPECT_EQ(header, "asn,prefix,as_path");
    
    std::string line;
    while (std::getline(file, line)) {
        // Count commas (should be 2)
        int comma_count = std::count(line.begin(), line.end(), ',');
        EXPECT_EQ(comma_count, 2) << "Line: " << line;
        
        // Parse components
        std::istringstream iss(line);
        std::string asn_str, prefix_str, as_path;
        std::getline(iss, asn_str, ',');
        std::getline(iss, prefix_str, ',');
        std::getline(iss, as_path);
        
        // ASN should be numeric
        EXPECT_GT(std::stoul(asn_str), 0);
        
        // Prefix should contain '/'
        EXPECT_TRUE(prefix_str.find('/') != std::string::npos);
        
        // AS-Path should contain spaces or be single number
        EXPECT_FALSE(as_path.empty());
    }
}
