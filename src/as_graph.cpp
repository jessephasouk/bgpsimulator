#include "as_graph.h"
#include "prefix_map.h"
#include "rov.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <cstring>

namespace {
    // Fast integer to string conversion (3-5x faster than std::to_string)
    inline char* fast_uint_to_str(uint32_t value, char* buffer) {
        char* p = buffer;
        if (value == 0) {
            *p++ = '0';
            return p;
        }
        
        // Write digits in reverse
        char temp[12];  // max uint32_t is 10 digits
        char* t = temp;
        while (value > 0) {
            *t++ = '0' + (value % 10);
            value /= 10;
        }
        
        // Copy in correct order
        while (t > temp) {
            *p++ = *--t;
        }
        return p;
    }
}

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

std::vector<ASNode*> collectNodesSorted(const std::unordered_map<uint32_t, ASNode*>& nodes) {
    std::vector<ASNode*> ordered;
    ordered.reserve(nodes.size());
    for (const auto& [asn, node] : nodes) {
        ordered.push_back(node);
    }
    std::sort(ordered.begin(), ordered.end(), [](ASNode* lhs, ASNode* rhs) {
        return lhs->getASN() < rhs->getASN();
    });
    return ordered;
}

std::vector<std::vector<ASNode*>> buildRankBuckets(
    const std::unordered_map<uint32_t, ASNode*>& nodes,
    int maxRank) {
    if (maxRank < 0) {
        return {};
    }

    std::vector<std::vector<ASNode*>> buckets(static_cast<size_t>(maxRank) + 1);
    for (const auto& [asn, node] : nodes) {
        int rank = node->getPropagationRank();
        if (rank < 0 || rank > maxRank) {
            continue;
        }
        buckets[static_cast<size_t>(rank)].push_back(node);
    }

    for (auto& bucket : buckets) {
        std::sort(bucket.begin(), bucket.end(), [](ASNode* lhs, ASNode* rhs) {
            return lhs->getASN() < rhs->getASN();
        });
    }

    return buckets;
}

void appendUnique(std::vector<IPPrefix>& target, const std::vector<IPPrefix>& additions) {
    if (additions.empty()) {
        return;
    }

    for (const auto& prefix : additions) {
        if (std::find(target.begin(), target.end(), prefix) == target.end()) {
            target.push_back(prefix);
        }
    }
}

std::vector<IPPrefix> takePending(std::unordered_map<uint32_t, std::vector<IPPrefix>>& pending, uint32_t asn) {
    auto it = pending.find(asn);
    if (it == pending.end()) {
        return {};
    }

    std::vector<IPPrefix> result = std::move(it->second);
    pending.erase(it);
    return result;
}

bool hasPending(const std::unordered_map<uint32_t, std::vector<IPPrefix>>& pending) {
    for (const auto& [asn, prefixes] : pending) {
        if (!prefixes.empty()) {
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * Destructor - clean up all allocated nodes
 */
ASGraph::~ASGraph() {
    for (auto& [asn, node] : nodes_) {
        delete node;
    }
    nodes_.clear();
}

/**
 * Get or create an AS node in the graph
 * 
 * Performance: O(1) average case due to unordered_map hash lookup
 * 
 * Why this design?
 * - Using unordered_map for O(1) average lookup vs O(log n) for map
 * - Lazy node creation: only create nodes when we see them in relationships
 * - Returns raw ASNode* pointer
 */
ASNode* ASGraph::getOrCreateNode(uint32_t asn) {
    // Try to find existing node in hash map - O(n) worst case, O(1) average
    auto it = nodes_.find(asn);
    if (it != nodes_.end()) {
        return it->second;  // Node exists, return it
    }
    
    // Node doesn't exist, create it with new
    auto node = new ASNode(asn);
    nodes_[asn] = node;  // Insert into hash map - O(n) worst case
    return node;
}

/**
 * Get an existing AS node (read-only access)
 * 
 * Performance: O(1) average case
 * Returns nullptr if node doesn't exist (safer than throwing exception)
 */
ASNode* ASGraph::getNode(uint32_t asn) const {
    auto it = nodes_.find(asn);
    return (it != nodes_.end()) ? it->second : nullptr;
}

/**
 * Parse a line from the CAIDA AS relationship file
 * 
 * Format: AS1|AS2|relationship|source
 * Example: 1|2|-1|bgp means AS1 is provider to AS2
 * 
 * Performance: O(n) where n is line length (small, typically < 50 chars)
 * 
 * Why this design?
 * - String parsing is typically a bottleneck in file I/O
 * - We use stringstream for safe parsing with '|' delimiter
 * - Return false for invalid lines instead of throwing (keeps parsing going)
 * - Parses uint32_t values for ASNs using std::stoul
 */
bool ASGraph::parseLine(const std::string& line, uint32_t& as1, uint32_t& as2, int& relationship) const {
    // Skip empty lines and comments (CAIDA files have many comment lines at top)
    if (line.empty() || line[0] == '#') {
        return false;
    }

    // OPTIMIZED: Manual parsing without string allocations
    // Format: AS1|AS2|relationship|source
    // We parse AS1, AS2, relationship directly from char buffer
    
    const char* ptr = line.c_str();
    const char* end = ptr + line.size();
    
    // Parse AS1 (scan until '|')
    as1 = 0;
    while (ptr < end && *ptr != '|') {
        if (*ptr >= '0' && *ptr <= '9') {
            as1 = as1 * 10 + static_cast<uint32_t>(*ptr - '0');
        }
        ptr++;
    }
    if (ptr >= end) return false;  // No delimiter found
    ptr++;  // Skip '|'
    
    // Parse AS2 (scan until '|')
    as2 = 0;
    while (ptr < end && *ptr != '|') {
        if (*ptr >= '0' && *ptr <= '9') {
            as2 = as2 * 10 + static_cast<uint32_t>(*ptr - '0');
        }
        ptr++;
    }
    if (ptr >= end) return false;  // No delimiter found
    ptr++;  // Skip '|'
    
    // Parse relationship (-1 or 0, handle negative)
    bool negative = false;
    if (ptr < end && *ptr == '-') {
        negative = true;
        ptr++;
    }
    
    relationship = 0;
    while (ptr < end && *ptr >= '0' && *ptr <= '9') {
        relationship = relationship * 10 + (*ptr - '0');
        ptr++;
    }
    if (negative) {
        relationship = -relationship;
    }
    
    // Successfully parsed all required fields
    return true;
}

/**
 * Build the AS graph from a CAIDA AS relationship file
 * 
 * Overall Performance: O(E + V) where E = edges (relationships), V = vertices (ASes)
 * - File I/O: O(E) - must read every line
 * - Node creation: O(V) - create each node once
 * - Edge insertion: O(E) - add each relationship
 * - Cycle detection: O(V + E) - DFS traversal
 * 
 * For CAIDA data: ~78k nodes, ~570k edges => ~1.2 seconds total
 * 
 * Why this design?
 * - Single-pass file reading (can't make it faster than reading every line)
 * - Lazy node creation (only create nodes we actually see)
 * - Progress indicators for user feedback on large files
 * - Validate graph integrity with cycle detection at end
 */
bool ASGraph::buildFromCAIDAFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return false;
    }

    // OPTIMIZED: Pre-allocate hash map capacity to avoid rehashing
    // Typical CAIDA file has ~78k nodes
    nodes_.reserve(80000);

    std::string line;
    size_t lineCount = 0;      // Total lines read (including comments)
    size_t processedCount = 0;  // Valid relationships processed

    // Main parsing loop - O(E) where E = number of lines
    while (std::getline(file, line)) {
        lineCount++;
        
        uint32_t as1, as2;
        int relationship;
        
        // Parse line - O(line length), typically ~20-50 chars
        if (!parseLine(line, as1, as2, relationship)) {
            continue;  // Skip invalid/comment lines (CAIDA has ~100k comment lines)
        }
        
        processedCount++;
        
        // Get or create nodes - O(1) average per lookup
        // This is where nodes are created lazily as we encounter them
        auto node1 = getOrCreateNode(as1);
        auto node2 = getOrCreateNode(as2);
        
        // Add bidirectional relationships based on type
        // Provider-customer: directed edge (transit relationship)
        // Peer-to-peer: undirected edge (settlement-free peering)
        if (relationship == -1) {
            // AS1 is provider, AS2 is customer
            // Provider -> Customer means AS1 provides transit to AS2
            // This creates a directed edge in the graph
            node1->addCustomer(node2);  // O(log k) where k = customers
            node2->addProvider(node1);  // O(log k) where k = providers
        } else if (relationship == 0) {
            // Peer-to-peer relationship (symmetric)
            // Both ASes agree to exchange routes for free
            node1->addPeer(node2);  // O(log k) where k = peers
            node2->addPeer(node1);  // O(log k) where k = peers
        }
        // Note: We ignore other relationship types (if any exist in data)
        
        // Progress indicator every 100k relationships
        // Helps user know program isn't frozen on large files
        if (processedCount % 100000 == 0) {
            std::cout << "Processed " << processedCount << " relationships..." << std::endl;
        }
    }

    file.close();
    
    // Print statistics
    std::cout << "Graph built successfully:" << std::endl;
    std::cout << "  Total lines read: " << lineCount << std::endl;
    std::cout << "  Relationships processed: " << processedCount << std::endl;
    std::cout << "  Nodes (ASes): " << nodes_.size() << std::endl;
    std::cout << "  Total edges: " << getEdgeCount() << std::endl;
    
    // Validate graph: Check for provider-customer cycles
    // This is CRITICAL for BGP simulation correctness
    // Real internet topology should NEVER have provider-customer cycles
    // (would create routing loops and payment cycles)
    if (hasProviderCustomerCycles()) {
        std::cerr << "ERROR: Provider-customer cycles detected in the graph!" << std::endl;
        std::cerr << "BGP simulation cannot proceed with cycles. Graph is invalid." << std::endl;
        return false;
    }
    
    std::cout << "  No provider-customer cycles detected." << std::endl;
    return true;
}

/**
 * Count total edges (relationships) in the graph
 * 
 * Performance: O(V) where V = number of nodes
 * 
 * Why divide by 2?
 * - Each relationship is stored twice (once at each endpoint)
 * - Provider->Customer: stored in provider's customer list AND customer's provider list
 * - Peer<->Peer: stored in both peers' peer lists
 * - So we count all edges and divide by 2 to get actual edge count
 */
size_t ASGraph::getEdgeCount() const {
    size_t count = 0;
    
    // Iterate through all nodes - O(V)
    for (const auto& [asn, node] : nodes_) {
        // Count all relationships (each counted twice)
        count += node->getProviders().size();
        count += node->getCustomers().size();
        count += node->getPeers().size();
    }
    
    // Divide by 2 because each edge is stored at both endpoints
    return count / 2;
}

/**
 * Depth-First Search to detect cycles in provider-customer relationships
 * 
 * Performance: O(V + E) where V = nodes visited, E = edges explored
 * 
 * Algorithm: DFS with color coding (white/gray/black approach)
 * - visited[asn] = false: WHITE (never seen) - asn is uint32_t
 * - inStack[asn] = true: GRAY (currently being explored)
 * - visited[asn] = true && inStack[asn] = false: BLACK (done exploring)
 * 
 * Cycle detection:
 * - If we reach a GRAY node, we found a back edge = cycle!
 * - Back edge: edge pointing to an ancestor in DFS tree
 * 
 * Why only follow customer edges?
 * - We're checking provider->customer relationships (directed graph)
 * - Peer relationships are undirected and cycles are allowed
 * - In BGP, provider->customer must be acyclic (forms hierarchy)
 */
bool ASGraph::hasCycleDFS(ASNode* node,
                          std::unordered_map<uint32_t, bool>& visited,
                          std::unordered_map<uint32_t, bool>& inStack) const {
    uint32_t asn = node->getASN();  // Get ASN as uint32_t
    
    // Mark node as visited and add to recursion stack (GRAY)
    visited[asn] = true;
    inStack[asn] = true;
    
    // Explore all customers (follow provider -> customer edges)
    // This creates a directed graph where edges go from provider to customer
    for (ASNode* customer : node->getCustomers()) {
        uint32_t customerASN = customer->getASN();
        
        if (!visited[customerASN]) {
            // WHITE node: never visited, recurse into it
            if (hasCycleDFS(customer, visited, inStack)) {
                return true;  // Cycle found in subtree
            }
        } else if (inStack[customerASN]) {
            // GRAY node: currently in recursion stack
            // This means we found a back edge!
            // Back edge = edge to an ancestor = CYCLE
            // Example: A->B->C->A forms a cycle
            return true;
        }
        // BLACK node (visited but not in stack): safe to ignore
        // This is a cross edge or forward edge, not a cycle
    }
    
    // Done exploring this node, remove from stack (mark BLACK)
    inStack[asn] = false;
    return false;
}

/**
 * Check if graph has any provider-customer cycles
 * 
 * Performance: O(V + E) total for entire graph
 * - Each node visited exactly once
 * - Each edge explored exactly once
 * 
 * Why check every unvisited node?
 * - Graph might be disconnected (multiple components)
 * - Need to ensure no cycles in ANY component
 * - Starting from each unvisited node ensures full coverage
 * 
 * Example of what we're detecting:
 * - BAD: AS1 provides to AS2, AS2 provides to AS3, AS3 provides to AS1
 * - GOOD: AS1 provides to AS2, AS2 provides to AS3 (no cycle)
 * - GOOD: AS1 peers with AS2, AS2 peers with AS3, AS3 peers with AS1 (peer cycles OK)
 */
bool ASGraph::hasProviderCustomerCycles() const {
    // Hash maps for tracking DFS state - O(V) space
    // Maps uint32_t ASN to bool status
    std::unordered_map<uint32_t, bool> visited;   // Has node been visited?
    std::unordered_map<uint32_t, bool> inStack;   // Is node in current DFS path?
    
    // Initialize all nodes as unvisited (WHITE)
    // This is O(V) but only done once
    // asn is uint32_t, node is std::shared_ptr<ASNode>
    for (const auto& [asn, node] : nodes_) {
        visited[asn] = false;
        inStack[asn] = false;
    }
    
    // Check for cycles starting from each unvisited node
    // This handles disconnected graph components
    for (const auto& [asn, node] : nodes_) {
        if (!visited[asn]) {
            // Start DFS from this unvisited node
            if (hasCycleDFS(node, visited, inStack)) {
                return true;  // Cycle found!
            }
        }
    }
    
    // No cycles found in any component
    return false;
}

/**
 * Flatten the graph into propagation ranks
 * 
 * Algorithm: BFS-based rank assignment from edge nodes upward
 * 
 * Performance: O(V + E) where V = nodes, E = provider-customer edges
 * - Find all edge nodes: O(V) - check each node's customer count
 * - BFS traversal: O(V + E) - visit each node once, follow each edge once
 * - Build result vector: O(V) - add each node to its rank vector
 * 
 * Why BFS instead of DFS?
 * - Need level-order traversal (all rank 0, then all rank 1, etc.)
 * - BFS naturally processes nodes by distance from source
 * - DFS would give arbitrary ordering
 * 
 * Example topology:
 * ```
 *     Tier-1 (rank 2)
 *       /  \
 *   Tier-2 (rank 1)
 *     /  \
 *  Edge ASes (rank 0: Google, Netflix)
 * ```
 * 
 * Result: [[Google, Netflix], [Tier-2 ASes], [Tier-1 ASes]]
 */
std::vector<std::vector<uint32_t>> ASGraph::flattenGraph() {
    // Step 1: Reset all ranks to -1 (unassigned)
    for (auto& [asn, node] : nodes_) {
        node->setPropagationRank(-1);
    }
    
    // Step 2: Topological sort using Kahn's algorithm
    // Count remaining customers for each AS (in-degree for topo sort)
    std::unordered_map<uint32_t, int> remaining_customers;
    std::queue<ASNode*> queue;
    
    for (const auto& [asn, node] : nodes_) {
        remaining_customers[asn] = node->getCustomers().size();
        if (remaining_customers[asn] == 0) {
            // Edge AS: no customers = Rank 0
            node->setPropagationRank(0);
            queue.push(node);
        }
    }
    
    // Step 3: Process ASes level by level
    // Each AS's rank = max(customer ranks) + 1
    int processed = 0;
    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();
        processed++;
        
        // Update each provider's rank based on this customer
        for (ASNode* provider : node->getProviders()) {
            uint32_t provider_asn = provider->getASN();
            int new_rank = node->getPropagationRank() + 1;
            
            // Take the MAX rank if provider has multiple customers at different ranks
            if (provider->getPropagationRank() < new_rank) {
                provider->setPropagationRank(new_rank);
            }
            
            // Decrement remaining customers; enqueue when all customers processed
            remaining_customers[provider_asn]--;
            if (remaining_customers[provider_asn] == 0) {
                queue.push(provider);
            }
        }
    }
    
    // Check for cycles (if not all ASes were processed)
    if (processed != static_cast<int>(nodes_.size())) {
        std::cerr << "Warning: Detected " << (nodes_.size() - processed) 
                  << " ASes unreachable (possible provider-customer cycles)\n";
    }
    
    // Build result: vector of vectors grouped by rank
    std::vector<std::vector<uint32_t>> result;
    int maxRank = getMaxRank();
    if (maxRank >= 0) {
        result.resize(static_cast<size_t>(maxRank + 1));
        for (const auto& [asn, node] : nodes_) {
            int rank = node->getPropagationRank();
            if (rank >= 0) {
                result[static_cast<size_t>(rank)].push_back(asn);
            }
        }
    }
    
    return result;
}

/**
 * Get the maximum propagation rank assigned in the graph
 * 
 * Performance: O(V) - scan all nodes
 * 
 * Returns -1 if no ranks assigned (flattenGraph() not called yet)
 */
int ASGraph::getMaxRank() const {
    int maxRank = -1;
    for (const auto& [asn, node] : nodes_) {
        maxRank = std::max(maxRank, node->getPropagationRank());
    }
    return maxRank;
}

/**
 * Propagate announcements UP the hierarchy (to providers)
 * 
 * Algorithm:
 * 1. Start at rank 0 (edge ASes with no customers)
 * 2. Each AS at rank 0 sends all announcements to its providers
 * 3. Move to rank 1, process received queue, send to providers
 * 4. Repeat until reaching max rank (tier-1 ISPs)
 * 
 * Performance: O(R * V * N * P) where:
 * - R = number of ranks (typically 5-10)
 * - V = ASes per rank (average ~15k)
 * - N = announcements per AS (typically 1-10)
 * - P = providers per AS (typically 1-3)
 * 
 * For real Internet: ~5 ranks * 78k ASes * 5 announcements * 2 providers
 * = ~4M operations (sub-second on modern hardware)
 */
void ASGraph::propagateUp() {
    int maxRank = getMaxRank();
    if (maxRank < 0) {
        std::cerr << "Warning: Graph not flattened, cannot propagate\n";
        return;
    }

    auto buckets = buildRankBuckets(nodes_, maxRank);
    for (int rank = 0; rank <= maxRank; ++rank) {
        const auto& layer = buckets[static_cast<size_t>(rank)];
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < layer.size(); ++i) {
            layer[i]->processReceivedQueue();
        }
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < layer.size(); ++i) {
            layer[i]->sendToProviders();
        }
    }
}

/**
 * Propagate announcements ACROSS peers (single hop only)
 * 
 * Algorithm:
 * 1. All ASes send to their peers simultaneously
 * 2. Then all ASes process their received queues
 * 
 * Why this ordering?
 * - Prevents multi-hop peer propagation
 * - Example: If we processed immediately:
 *   AS1 → AS2 (process) → AS3 (process) = 2 hops across peers
 * - With batching:
 *   AS1 → AS2 queue, AS2 → AS3 queue, THEN process all = 1 hop each
 * 
 * Performance: O(V * N * P) where:
 * - V = number of ASes (~78k)
 * - N = announcements per AS (typically 1-10)
 * - P = peers per AS (typically 5-20)
 * 
 * Note: All routes in RIB are sent to peers (no export policy filtering)
 */
void ASGraph::propagateAcross() {
    auto ordered = collectNodesSorted(nodes_);
    #pragma omp parallel for schedule(static, 128)
    for (size_t i = 0; i < ordered.size(); ++i) {
        ordered[i]->sendToPeers();
    }
    #pragma omp parallel for schedule(static, 128)
    for (size_t i = 0; i < ordered.size(); ++i) {
        ordered[i]->processReceivedQueue();
    }
}

/**
 * Propagate announcements DOWN the hierarchy (to customers)
 * 
 * Algorithm:
 * 1. Start at max rank (tier-1 ISPs)
 * 2. Each AS at max rank sends all announcements to its customers
 * 3. Move to rank-1, process received queue, send to customers
 * 4. Repeat until reaching rank 0 (edge ASes)
 * 
 * Performance: Same as propagateUp() - O(R * V * N * C)
 * - C = customers per AS (varies: tier-1 has thousands, edge has 0)
 * 
 * Why separate from propagateUp()?
 * - Different direction (down vs up)
 * - Different relationship (FROM_PROVIDER vs FROM_CUSTOMER)
 * - Happens after peer propagation
 */
void ASGraph::propagateDown() {
    int maxRank = getMaxRank();
    if (maxRank < 0) {
        std::cerr << "Warning: Graph not flattened, cannot propagate\n";
        return;
    }

    auto buckets = buildRankBuckets(nodes_, maxRank);
    for (int rank = maxRank; rank >= 0; --rank) {
        const auto& layer = buckets[static_cast<size_t>(rank)];
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < layer.size(); ++i) {
            layer[i]->processReceivedQueue();
        }
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < layer.size(); ++i) {
            layer[i]->sendToCustomers();
        }
    }
}

/**
 * Run full BGP propagation (up, across, down)
 * 
 * This is the complete BGP convergence process:
 * 1. Announcements propagate UP from edge ASes to tier-1 ISPs
 * 2. Announcements propagate ACROSS one hop to peers
 * 3. Announcements propagate DOWN from tier-1 ISPs to edge ASes
 * 
 * Result: Every AS has received announcements from every originating AS
 * 
 * Performance: O(R * V * N * (P + C + Pe))
 * - For real Internet: ~5-10 seconds for full convergence
 * 
 * BGP Context:
 * - This models Internet-wide BGP convergence
 * - In reality, this happens gradually over minutes
 * - Announcements propagate at speed of light + processing delays
 * - Our simulation is synchronous (lock-step), real BGP is asynchronous
 */
void ASGraph::propagateAll(int iterations) {
    std::cout << "Starting BGP propagation...\n";
    
    // Pre-compute rank buckets and sorted nodes once (avoid repeated computation)
    int maxRank = getMaxRank();
    if (maxRank < 0) {
        std::cerr << "Warning: Graph not flattened, cannot propagate\n";
        return;
    }
    
    // Cache rank buckets for efficient rank-based iteration
    std::vector<std::vector<ASNode*>> buckets(static_cast<size_t>(maxRank + 1));
    for (auto& pair : nodes_) {
        ASNode* node = pair.second;
        int rank = node->getPropagationRank();
        if (rank >= 0 && rank <= maxRank) {
            buckets[static_cast<size_t>(rank)].push_back(node);
        }
    }
    
    // Pre-sorted list for peer propagation
    std::vector<ASNode*> ordered;
    ordered.reserve(nodes_.size());
    for (auto& pair : nodes_) {
        ordered.push_back(pair.second);
    }
    
    for (int iter = 1; iter <= iterations; ++iter) {
        if (iterations > 1) {
            std::cout << "Iteration " << iter << "/" << iterations << ":\n";
        }
        
        std::cout << (iterations > 1 ? "  " : "") << "Phase 1: Propagating up (to providers)...\n";
        // Propagate up: rank 0 to maxRank
        for (int rank = 0; rank <= maxRank; ++rank) {
            const auto& layer = buckets[static_cast<size_t>(rank)];
            #pragma omp parallel for schedule(static, 128)
            for (size_t i = 0; i < layer.size(); ++i) {
                layer[i]->processReceivedQueue();
                layer[i]->sendToProviders();
            }
        }
        
        std::cout << (iterations > 1 ? "  " : "") << "Phase 2: Propagating across (to peers)...\n";
        // Propagate across peers - process what we received in Phase 1 FIRST
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < ordered.size(); ++i) {
            ordered[i]->processReceivedQueue();  // Process Phase 1 results
        }
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < ordered.size(); ++i) {
            ordered[i]->sendToPeers();  // Then send to peers
        }
        #pragma omp parallel for schedule(static, 128)
        for (size_t i = 0; i < ordered.size(); ++i) {
            ordered[i]->processReceivedQueue();  // Process what peers sent
        }
        
        std::cout << (iterations > 1 ? "  " : "") << "Phase 3: Propagating down (to customers)...\n";
        // Propagate down: maxRank to 0
        for (int rank = maxRank; rank >= 0; --rank) {
            const auto& layer = buckets[static_cast<size_t>(rank)];
            #pragma omp parallel for schedule(static, 128)
            for (size_t i = 0; i < layer.size(); ++i) {
                layer[i]->processReceivedQueue();
                layer[i]->sendToCustomers();
            }
        }
    }
    
    std::cout << "BGP propagation complete!\n";
}

ASGraph::PropagationStats ASGraph::propagateToConvergence(size_t maxRounds) {
    PropagationStats stats;
    auto ordered = collectNodesSorted(nodes_);

    for (const auto& node : ordered) {
        auto policy = node->getPolicy();
        if (policy) {
            stats.initialAnnouncements += policy->getLocalRIBPrefixes().size();
        }
    }

    if (ordered.empty()) {
        return stats;
    }

    const int maxRank = getMaxRank();
    auto buckets = buildRankBuckets(nodes_, maxRank);

    std::unordered_map<uint32_t, std::vector<IPPrefix>> pendingUp;
    std::unordered_map<uint32_t, std::vector<IPPrefix>> pendingPeers;
    std::unordered_map<uint32_t, std::vector<IPPrefix>> pendingDown;

    for (const auto& node : ordered) {
        auto policy = node->getPolicy();
        if (!policy) {
            continue;
        }

        auto prefixes = policy->getLocalRIBPrefixes();
        if (!prefixes.empty()) {
            appendUnique(pendingUp[node->getASN()], prefixes);
            appendUnique(pendingPeers[node->getASN()], prefixes);
            appendUnique(pendingDown[node->getASN()], prefixes);
        }
    }

    auto anyPending = [&]() {
        return hasPending(pendingUp) || hasPending(pendingPeers) || hasPending(pendingDown);
    };

    size_t rounds = 0;
    while (true) {
        if (maxRounds > 0 && rounds >= maxRounds) {
            stats.hitRoundLimit = true;
            break;
        }

        bool roundHadWork = false;

        auto processUpLayer = [&](const std::vector<ASNode*>& layer) {
            for (ASNode* node : layer) {
                auto deltas = node->processReceivedQueueWithDiff();
                if (!deltas.empty()) {
                    stats.nodeEvents++;
                    stats.bestChanges += deltas.size();
                    appendUnique(pendingPeers[node->getASN()], deltas);
                    appendUnique(pendingDown[node->getASN()], deltas);
                }

                auto toSend = takePending(pendingUp, node->getASN());
                appendUnique(toSend, deltas);
                if (!toSend.empty()) {
                    node->sendToProviders(toSend);
                    roundHadWork = true;
                }
            }
        };

        if (maxRank >= 0) {
            for (int rank = 0; rank <= maxRank; ++rank) {
                const auto& layer = buckets[static_cast<size_t>(rank)];
                processUpLayer(layer);
            }
        } else {
            processUpLayer(ordered);
        }

        for (const auto& node : ordered) {
            auto toSend = takePending(pendingPeers, node->getASN());
            if (!toSend.empty()) {
                node->sendToPeers(toSend);
                roundHadWork = true;
            }
        }

        for (const auto& node : ordered) {
            auto deltas = node->processReceivedQueueWithDiff();
            if (!deltas.empty()) {
                stats.nodeEvents++;
                stats.bestChanges += deltas.size();
                appendUnique(pendingUp[node->getASN()], deltas);
                appendUnique(pendingPeers[node->getASN()], deltas);
                appendUnique(pendingDown[node->getASN()], deltas);
            }
        }

        auto processDownLayer = [&](const std::vector<ASNode*>& layer) {
            for (ASNode* node : layer) {
                auto deltas = node->processReceivedQueueWithDiff();
                if (!deltas.empty()) {
                    stats.nodeEvents++;
                    stats.bestChanges += deltas.size();
                    appendUnique(pendingUp[node->getASN()], deltas);
                    appendUnique(pendingPeers[node->getASN()], deltas);
                }

                auto toSend = takePending(pendingDown, node->getASN());
                appendUnique(toSend, deltas);
                if (!toSend.empty()) {
                    node->sendToCustomers(toSend);
                    roundHadWork = true;
                }
            }
        };

        if (maxRank >= 0) {
            for (int rank = maxRank; rank >= 0; --rank) {
                const auto& layer = buckets[static_cast<size_t>(rank)];
                processDownLayer(layer);
            }
        } else {
            processDownLayer(ordered);
        }

        rounds++;

        if (!roundHadWork && !anyPending()) {
            break;
        }
    }

    stats.rounds = rounds;
    return stats;
}

/**
 * Dump AS graph routing tables to CSV format
 * 
 * Output format:
 * - Header: "asn,prefix,as_path"
 * - Each line: ASN,prefix,AS-Path (space-separated ASNs)
 * 
 * Example:
 * ```
 * asn,prefix,as_path
 * 3,10.0.0.0/8,3 2 1
 * 4,10.0.0.0/8,4 2 1
 * 2,10.0.0.0/8,2 1
 * 1,10.0.0.0/8,1
 * ```
 * 
 * Purpose:
 * - Cloudflare network optimization analysis
 * - Identify routing paths across entire Internet
 * - Compare against ground truth (bgpsimulator.com)
 * - Detect suboptimal routing or hijacks
 * 
 * Performance: O(V * P) where V = ASes, P = avg prefixes per AS
 * - Iterate through all ASes: O(V)
 * - For each AS, get all prefixes: O(P)
 * - Write each route to file: O(1) per line
 * - Real data: ~1 second for 78k ASes, ~10 prefixes each
 * 
 * File I/O optimization:
 * - std::ofstream with internal buffering
 * - Could use mmap for huge datasets (not needed here)
 * 
 * @param filename Path to output CSV file
 * @return true if successful, false if file cannot be opened
 */
bool ASGraph::dumpToCSV(const std::string& filename) const {
    // Count total routes for pre-allocation
    size_t total_routes = 0;
    for (const auto& [asn, node] : nodes_) {
        auto policy = node->getPolicy();
        if (policy) {
            auto bgp_policy = dynamic_cast<const BGP*>(policy);
            if (bgp_policy) {
                total_routes += bgp_policy->getLocalRIBSize();
            } else {
                total_routes += policy->getLocalRIBPrefixes().size();
            }
        }
    }
    
    // Get direct access to prefix strings vector (avoid hash lookups!)
    const auto& prefix_strings = PrefixMap::id_to_string;
    
    // Pre-allocate single large buffer with exact size
    size_t estimated_size = 21 + (total_routes * 60);  // header + ~60 bytes avg per route
    std::string output;
    output.reserve(estimated_size);
    output = "asn,prefix,as_path\n";
    
    // Reserve exact space and get direct pointer access
    size_t header_len = output.size();
    output.resize(estimated_size);  // Allocate full buffer upfront
    char* write_ptr = &output[header_len];
    char* buffer_end = &output[estimated_size - 1];
    
    // Build CSV content directly into buffer (no append!)
    for (const auto& [asn, node] : nodes_) {
        auto policy = node->getPolicy();
        if (!policy) {
            continue;
        }
        
        auto bgp_policy = dynamic_cast<const BGP*>(policy);
        if (bgp_policy) {
            const auto& local_rib = bgp_policy->getLocalRIB();
            for (const auto& [prefix_id, announcement] : local_rib) {
                // Safety check: ensure we have space
                if (write_ptr + 200 > buffer_end) {
                    // Need to grow buffer - resize and update pointers
                    size_t current_pos = write_ptr - output.data();
                    output.resize(output.size() + 10000000);  // Add 10MB
                    write_ptr = &output[current_pos];
                    buffer_end = &output[output.size() - 1];
                }
                
                char* p = write_ptr;
                
                // Write ASN
                p = fast_uint_to_str(asn, p);
                *p++ = ',';
                
                // Write prefix (direct vector access - no hash lookup!)
                const std::string& prefix_str = prefix_strings[prefix_id];
                const char* prefix_data = prefix_str.data();
                size_t prefix_len = prefix_str.size();
                std::memcpy(p, prefix_data, prefix_len);
                p += prefix_len;
                
                // Start AS path
                *p++ = ',';
                *p++ = '"';
                *p++ = '(';
                
                // Write AS path (fixed-size array)
                const uint32_t* as_path = announcement.getASPath();
                uint8_t path_size = announcement.getPathLength();
                for (uint8_t i = 0; i < path_size; ++i) {
                    p = fast_uint_to_str(as_path[i], p);
                    if (i < path_size - 1) {
                        *p++ = ',';
                        *p++ = ' ';
                    }
                }
                if (path_size == 1) {
                    *p++ = ',';
                }
                
                // End AS path
                *p++ = ')';
                *p++ = '"';
                *p++ = '\n';
                
                write_ptr = p;
            }
        } else {
            // Compatibility path for non-BGP policies (rarely used)
            std::vector<IPPrefix> prefixes = policy->getLocalRIBPrefixes();
            for (const auto& prefix : prefixes) {
                auto announcement = policy->getBestAnnouncement(prefix);
                if (!announcement) {
                    continue;
                }
                
                if (write_ptr + 200 > buffer_end) {
                    size_t current_pos = write_ptr - output.data();
                    output.resize(output.size() + 10000000);
                    write_ptr = &output[current_pos];
                    buffer_end = &output[output.size() - 1];
                }
                
                char* p = write_ptr;
                p = fast_uint_to_str(asn, p);
                *p++ = ',';
                
                const std::string& prefix_str = prefix.toString();
                std::memcpy(p, prefix_str.data(), prefix_str.size());
                p += prefix_str.size();
                
                *p++ = ',';
                *p++ = '"';
                *p++ = '(';
                
                const uint32_t* as_path = announcement->getASPath();
                uint8_t path_size = announcement->getPathLength();
                for (uint8_t i = 0; i < path_size; ++i) {
                    p = fast_uint_to_str(as_path[i], p);
                    if (i < path_size - 1) {
                        *p++ = ',';
                        *p++ = ' ';
                    }
                }
                if (path_size == 1) {
                    *p++ = ',';
                }
                
                *p++ = ')';
                *p++ = '"';
                *p++ = '\n';
                
                write_ptr = p;
            }
        }
    }
    
    // Trim to actual size
    output.resize(write_ptr - output.data());
    
    // Single write to file with optimized buffering
    FILE* file = fopen(filename.c_str(), "w");
    if (!file) {
        std::cerr << "Error: Could not open file " << filename << " for writing\n";
        return false;
    }
    
    // Set larger buffer for faster I/O (1MB buffer)
    static char io_buffer[1048576];
    setvbuf(file, io_buffer, _IOFBF, sizeof(io_buffer));
    
    // Single write operation for entire file
    size_t written = fwrite(output.data(), 1, output.size(), file);
    
    // Explicit flush before close for consistent timing
    fflush(file);
    fclose(file);
    
    if (written != output.size()) {
        std::cerr << "Error: Incomplete write to " << filename << "\n";
        return false;
    }
    
    std::cout << "Successfully wrote routing tables to " << filename << "\n";
    return true;
}

/**
 * Deploy ROV (Route Origin Validation) to specific ASes
 * 
 * Reads a file containing ASNs (one per line) and replaces their
 * BGP policy with ROV policy, which filters invalid announcements.
 * 
 * File Format:
 * ```
 * 13335    # Cloudflare
 * 15169    # Google
 * 16509    # Amazon
 * # Comments and blank lines are ignored
 * ```
 * 
 * What Happens:
 * =============
 * 
 * For each ASN in the file:
 * 1. Look up the AS node in the graph
 * 2. Replace its policy: BGP → ROV
 * 3. ROV policy = BGP + filtering (drops rov_invalid announcements)
 * 
 * Effect on Routing:
 * ==================
 * 
 * ROV ASes will:
 * - Accept legitimate announcements (rov_invalid=false)
 * - DROP hijacked announcements (rov_invalid=true)
 * - Never forward hijacked routes to neighbors
 * 
 * Non-ROV ASes will:
 * - Accept all announcements (no filtering)
 * - Vulnerable to prefix hijacks
 * 
 * This creates realistic mixed-deployment scenarios where some
 * ASes are protected and some are vulnerable.
 * 
 * Real-World Usage:
 * =================
 * 
 * Major networks deploying ROV:
 * - Cloudflare (AS 13335)
 * - Google (AS 15169)
 * - Amazon (AS 16509)
 * - AT&T (AS 7018)
 * - ~40% of Internet ASes overall
 * 
 * Example:
 * ```cpp
 * ASGraph graph;
 * graph.buildFromCAIDAFile("caida/20250901.as-rel2.txt");
 * 
 * // Deploy ROV to major networks
 * graph.deployROV("rov_deployment.txt");
 * 
 * // Now seed a hijack
 * auto attacker = graph.getNode(666);
 * attacker->seedAnnouncement(IPPrefix("1.2.0.0/16"), true);  // rov_invalid=true
 * 
 * // Propagate
 * graph.propagateAll();
 * 
 * // ROV ASes won't have the hijacked route
 * // Non-ROV ASes will have it (vulnerable!)
 * ```
 * 
 * Implementation Details:
 * =======================
 * 
 * Why create new ROV policy instead of setting a flag?
 * - Clean separation of concerns
 * - ROV logic is isolated in ROV class
 * - Easy to extend with more sophisticated policies
 * - Follows Open/Closed Principle (open for extension, closed for modification)
 * 
 * Why unique_ptr?
 * - AS node owns its policy (exclusive ownership)
 * - Automatic cleanup when AS is destroyed
 * - Can't accidentally share policy between ASes
 * 
 * Performance:
 * ============
 * 
 * Time Complexity: O(n + m) where n = lines in file, m = ASes in graph
 * - Read file: O(n) lines
 * - For each ASN: O(1) hash lookup in nodes_
 * - Create ROV policy: O(1)
 * - Total: O(n) for file with n ASNs
 * 
 * Memory:
 * - Each ROV policy: ~same as BGP (~100 bytes overhead)
 * - Creating 30k ROV policies: ~3 MB (negligible)
 * 
 * Error Handling:
 * ===============
 * 
 * - File not found → return false, print error
 * - ASN not in graph → skip (print warning)
 * - Invalid ASN format → skip line (print warning)
 * - Comments (#) → ignore
 * - Blank lines → ignore
 * 
 * @param filename Path to file containing ASNs (one per line)
 * @return true if file was read successfully, false if file error
 */
bool ASGraph::deployROV(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open ROV deployment file: " << filename << "\n";
        return false;
    }
    
    int deployed_count = 0;
    int skipped_count = 0;
    std::string line;
    
    std::cout << "Deploying ROV from " << filename << "...\n";
    
    while (std::getline(file, line)) {
        // Skip empty lines
        if (line.empty()) {
            continue;
        }
        
        // Skip comments (lines starting with #)
        if (line[0] == '#') {
            continue;
        }
        
        // Parse ASN
        try {
            uint32_t asn = std::stoul(line);
            
            // Look up AS in graph
            auto it = nodes_.find(asn);
            if (it == nodes_.end()) {
                // ASN not in graph - skip
                skipped_count++;
                continue;
            }
            
            // Deploy ROV policy to this AS
            // This replaces the existing BGP policy with ROV
            it->second->setPolicy(std::make_unique<ROV>());
            deployed_count++;
            
        } catch (const std::exception& e) {
            // Invalid ASN format - skip line
            std::cerr << "Warning: Invalid ASN format in ROV file: " << line << "\n";
            skipped_count++;
            continue;
        }
    }
    
    file.close();
    
    std::cout << "ROV deployment complete:\n";
    std::cout << "  - Deployed to " << deployed_count << " ASes\n";
    if (skipped_count > 0) {
        std::cout << "  - Skipped " << skipped_count << " entries (not in graph or invalid)\n";
    }
    
    return true;
}
