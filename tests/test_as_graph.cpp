#include "as_graph.h"
#include <gtest/gtest.h>
#include <fstream>
#include <chrono>

// Test fixture for AS Graph tests
class ASGraphTest : public ::testing::Test {
protected:
    ASGraph graph;
    
    void SetUp() override {
        // Clean up any leftover test files
        std::remove("test_no_cycle.txt");
        std::remove("test_cycle.txt");
        std::remove("test_peer_cycle.txt");
        std::remove("test_complex.txt");
        std::remove("test_invalid.txt");
    }
    
    void TearDown() override {
        // Clean up test files
        std::remove("test_no_cycle.txt");
        std::remove("test_cycle.txt");
        std::remove("test_peer_cycle.txt");
        std::remove("test_complex.txt");
        std::remove("test_invalid.txt");
    }
};

TEST_F(ASGraphTest, BasicNodeCreation) {
    auto node1 = graph.getOrCreateNode(100);
    auto node2 = graph.getOrCreateNode(200);
    
    EXPECT_EQ(node1->getASN(), 100);
    EXPECT_EQ(node2->getASN(), 200);
    EXPECT_EQ(graph.getNodeCount(), 2);
    
    // Getting the same node should return the same pointer
    auto node1_again = graph.getOrCreateNode(100);
    EXPECT_EQ(node1, node1_again);
    EXPECT_EQ(graph.getNodeCount(), 2);
}

TEST_F(ASGraphTest, ProviderCustomerRelationship) {
    auto provider = graph.getOrCreateNode(100);
    auto customer = graph.getOrCreateNode(200);
    
    provider->addCustomer(customer);
    customer->addProvider(provider);
    
    EXPECT_EQ(provider->getCustomers().size(), 1);
    EXPECT_EQ(customer->getProviders().size(), 1);
    EXPECT_EQ(provider->getCustomers().count(customer), 1);
    EXPECT_EQ(customer->getProviders().count(provider), 1);
}

TEST_F(ASGraphTest, PeerRelationship) {
    auto peer1 = graph.getOrCreateNode(100);
    auto peer2 = graph.getOrCreateNode(200);
    
    peer1->addPeer(peer2);
    peer2->addPeer(peer1);
    
    EXPECT_EQ(peer1->getPeers().size(), 1);
    EXPECT_EQ(peer2->getPeers().size(), 1);
    EXPECT_EQ(peer1->getPeers().count(peer2), 1);
    EXPECT_EQ(peer2->getPeers().count(peer1), 1);
}

TEST_F(ASGraphTest, NoCycles) {
    // Create a simple hierarchy: AS1 -> AS2 -> AS3
    std::ofstream file("test_no_cycle.txt");
    file << "1|2|-1|test\n";  // AS1 provides to AS2
    file << "2|3|-1|test\n";  // AS2 provides to AS3
    file.close();
    
    bool success = graph.buildFromCAIDAFile("test_no_cycle.txt");
    
    EXPECT_TRUE(success);
    EXPECT_FALSE(graph.hasProviderCustomerCycles());
    EXPECT_EQ(graph.getNodeCount(), 3);
}

TEST_F(ASGraphTest, CycleDetection) {
    // Create a cycle: AS1 -> AS2 -> AS3 -> AS1
    std::ofstream file("test_cycle.txt");
    file << "1|2|-1|test\n";  // AS1 provides to AS2
    file << "2|3|-1|test\n";  // AS2 provides to AS3
    file << "3|1|-1|test\n";  // AS3 provides to AS1 (creates cycle)
    file.close();
    
    bool success = graph.buildFromCAIDAFile("test_cycle.txt");
    
    EXPECT_FALSE(success);  // Should fail due to cycle
    EXPECT_TRUE(graph.hasProviderCustomerCycles());
}

TEST_F(ASGraphTest, PeerCyclesAllowed) {
    // Create peer relationships that form a "cycle" (which is fine for peers)
    std::ofstream file("test_peer_cycle.txt");
    file << "1|2|0|test\n";  // AS1 peers with AS2
    file << "2|3|0|test\n";  // AS2 peers with AS3
    file << "3|1|0|test\n";  // AS3 peers with AS1
    file.close();
    
    bool success = graph.buildFromCAIDAFile("test_peer_cycle.txt");
    
    EXPECT_TRUE(success);  // Should succeed, peer cycles are okay
    EXPECT_FALSE(graph.hasProviderCustomerCycles());
}

TEST_F(ASGraphTest, ComplexTopology) {
    // Create a more complex topology
    std::ofstream file("test_complex.txt");
    // Tier 1 providers
    file << "1|10|-1|test\n";
    file << "2|10|-1|test\n";
    file << "1|11|-1|test\n";
    
    // Mid-tier
    file << "10|20|-1|test\n";
    file << "10|21|-1|test\n";
    file << "11|21|-1|test\n";
    
    // Peers
    file << "1|2|0|test\n";
    file << "10|11|0|test\n";
    file << "20|21|0|test\n";
    file.close();
    
    bool success = graph.buildFromCAIDAFile("test_complex.txt");
    
    EXPECT_TRUE(success);
    EXPECT_FALSE(graph.hasProviderCustomerCycles());
    EXPECT_EQ(graph.getNodeCount(), 6);  // AS: 1, 2, 10, 11, 20, 21
    
    // Verify specific relationships
    auto as1 = graph.getNode(1);
    ASSERT_NE(as1, nullptr);
    EXPECT_EQ(as1->getCustomers().size(), 2);  // AS10, AS11
    EXPECT_EQ(as1->getPeers().size(), 1);      // AS2
    
    auto as10 = graph.getNode(10);
    ASSERT_NE(as10, nullptr);
    EXPECT_EQ(as10->getProviders().size(), 2);  // AS1, AS2
    EXPECT_EQ(as10->getCustomers().size(), 2);  // AS20, AS21
    EXPECT_EQ(as10->getPeers().size(), 1);      // AS11
}

TEST_F(ASGraphTest, InvalidInputHandling) {
    // Test with non-existent file
    ASGraph graph1;
    bool success1 = graph1.buildFromCAIDAFile("nonexistent_file.txt");
    EXPECT_FALSE(success1);
    
    // Test with malformed data
    std::ofstream file("test_invalid.txt");
    file << "invalid|data|here\n";
    file << "1|2|99|test\n";  // Invalid relationship type
    file.close();
    
    ASGraph graph2;
    graph2.buildFromCAIDAFile("test_invalid.txt");
    // Should handle gracefully (may succeed or fail depending on implementation)
}

// Benchmark test with real CAIDA data
// Tests full integration with 78k+ nodes and 570k+ edges
TEST(ASGraphBenchmark, LoadCAIDAFile) {
    const char* caida_file = "../caida/20250901.as-rel2.txt";
    
    ASGraph graph;
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = graph.buildFromCAIDAFile(caida_file);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    ASSERT_TRUE(success) << "Failed to load CAIDA file";
    
    std::cout << "\n========== CAIDA Benchmark Results ==========" << std::endl;
    std::cout << "File: " << caida_file << std::endl;
    std::cout << "Nodes (ASes): " << graph.getNodeCount() << std::endl;
    std::cout << "Edges: " << graph.getEdgeCount() << std::endl;
    std::cout << "Build Time: " << duration.count() << " ms" << std::endl;
    
    // Count relationship types
    size_t providerCount = 0, customerCount = 0, peerCount = 0;
    for (const auto& [asn, node] : graph.getNodes()) {
        providerCount += node->getProviders().size();
        customerCount += node->getCustomers().size();
        peerCount += node->getPeers().size();
    }
    
    std::cout << "\nRelationships:" << std::endl;
    std::cout << "  Providers: " << providerCount << std::endl;
    std::cout << "  Customers: " << customerCount << std::endl;
    std::cout << "  Peers: " << peerCount << std::endl;
    
    // Check for cycles
    auto cycle_start = std::chrono::high_resolution_clock::now();
    bool hasCycles = graph.hasProviderCustomerCycles();
    auto cycle_end = std::chrono::high_resolution_clock::now();
    auto cycle_duration = std::chrono::duration_cast<std::chrono::milliseconds>(cycle_end - cycle_start);
    
    std::cout << "\nCycle Detection: " << cycle_duration.count() << " ms" << std::endl;
    std::cout << "Has Cycles: " << (hasCycles ? "YES (ERROR!)" : "NO (OK)") << std::endl;
    std::cout << "============================================" << std::endl;
    
    EXPECT_FALSE(hasCycles) << "Provider-customer cycles should not exist in CAIDA data";
    EXPECT_GT(graph.getNodeCount(), 70000) << "Expected at least 70k nodes";
    EXPECT_GT(graph.getEdgeCount(), 500000) << "Expected at least 500k edges";
}
