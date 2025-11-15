#include "as_graph.h"
#include <gtest/gtest.h>
#include <chrono>

class GraphBuilderTest : public ::testing::Test {
protected:
    ASGraph graph;
};

TEST_F(GraphBuilderTest, BuildFromCAIDAFile) {
    const char* filename = ::testing::FLAGS_gtest_filter.empty() 
        ? "caida/20250901.as-rel2.txt" 
        : "caida/20250901.as-rel2.txt";
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Building AS graph from: " << filename << std::endl;
    std::cout << "========================================" << std::endl;
    
    auto start = std::chrono::high_resolution_clock::now();
    bool success = graph.buildFromCAIDAFile(filename);
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    std::cout << "Build time: " << duration.count() << " ms" << std::endl;
    
    ASSERT_TRUE(success) << "Failed to build graph from " << filename;
    
    std::cout << "\nGraph Statistics:" << std::endl;
    std::cout << "  Total ASes: " << graph.getNodeCount() << std::endl;
    std::cout << "  Total relationships: " << graph.getEdgeCount() << std::endl;
    
    // Sample a few nodes to show their relationships
    std::cout << "\nSample AS relationships:" << std::endl;
    int count = 0;
    for (const auto& [asn, node] : graph.getNodes()) {
        if (count >= 5) break;
        std::cout << "  AS" << asn << ":" << std::endl;
        std::cout << "    Providers: " << node->getProviders().size() << std::endl;
        std::cout << "    Customers: " << node->getCustomers().size() << std::endl;
        std::cout << "    Peers: " << node->getPeers().size() << std::endl;
        count++;
    }
    std::cout << "========================================" << std::endl;
    
    EXPECT_GT(graph.getNodeCount(), 0) << "Graph should have nodes";
    EXPECT_GT(graph.getEdgeCount(), 0) << "Graph should have edges";
}
