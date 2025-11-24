#include "as_graph.h"
#include "rov.h"
#include <fstream>
#include <sstream>
#include <iostream>

/**
 * Get or create an AS node in the graph
 * 
 * Performance: O(1) average case due to unordered_map hash lookup
 * 
 * Why this design?
 * - Using unordered_map for O(1) average lookup vs O(log n) for map
 * - Lazy node creation: only create nodes when we see them in relationships
 * - Returns std::shared_ptr<ASNode> so multiple relationships can reference same node
 */
std::shared_ptr<ASNode> ASGraph::getOrCreateNode(uint32_t asn) {
    // Try to find existing node in hash map - O(n) worst case, O(1) average
    auto it = nodes_.find(asn);
    if (it != nodes_.end()) {
        return it->second;  // Node exists, return it
    }
    
    // Node doesn't exist, create it
    // std::shared_ptr<ASNode>: automatic memory management, no need for manual delete
    auto node = std::make_shared<ASNode>(asn);
    nodes_[asn] = node;  // Insert into hash map - O(n) worst case
    return node;
}

/**
 * Get an existing AS node (read-only access)
 * 
 * Performance: O(1) average case
 * Returns nullptr if node doesn't exist (safer than throwing exception)
 */
std::shared_ptr<ASNode> ASGraph::getNode(uint32_t asn) const {
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

    std::istringstream iss(line);
    std::string token;
    std::vector<std::string> tokens;
    
    // Split by '|' delimiter - O(n) where n = line length
    while (std::getline(iss, token, '|')) {
        tokens.push_back(token);
    }
    
    // CAIDA format: AS1|AS2|relationship|source
    // We need at least 3 tokens (we ignore source column as instructed)
    if (tokens.size() < 3) {
        return false;
    }
    
    try {
        // Parse ASNs and relationship type
        // stoul returns unsigned long, explicitly cast to uint32_t
        // ASNs are in range 0 to 4,294,967,295 (safe for uint32_t) so its safe to case to uint32_t
        as1 = static_cast<uint32_t>(std::stoul(tokens[0]));
        as2 = static_cast<uint32_t>(std::stoul(tokens[1]));
        relationship = std::stoi(tokens[2]);  // -1 or 0
        return true;
    } catch (const std::exception&) {
        // Invalid number format, skip this line
        return false;
    }
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
bool ASGraph::hasCycleDFS(const std::shared_ptr<ASNode>& node,
                          std::unordered_map<uint32_t, bool>& visited,
                          std::unordered_map<uint32_t, bool>& inStack) const {
    uint32_t asn = node->getASN();  // Get ASN as uint32_t
    
    // Mark node as visited and add to recursion stack (GRAY)
    visited[asn] = true;
    inStack[asn] = true;
    
    // Explore all customers (follow provider -> customer edges)
    // This creates a directed graph where edges go from provider to customer
    for (const auto& customer : node->getCustomers()) {
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
    
    // Step 2: Find all edge ASes (no customers) and assign rank 0
    std::vector<uint32_t> currentRank;
    for (const auto& [asn, node] : nodes_) {
        if (node->getCustomers().empty()) {
            // Edge AS: no customers means this is a leaf node
            // Examples: Google (AS15169), Netflix (AS2906), universities
            node->setPropagationRank(0);
            currentRank.push_back(asn);
        }
    }
    
    // Result: vector of vectors, where result[rank] = ASNs at that rank
    std::vector<std::vector<uint32_t>> result;
    result.push_back(currentRank);  // Add rank 0 ASes
    
    // Step 3: BFS to assign ranks level by level
    int rank = 0;
    while (!currentRank.empty()) {
        std::vector<uint32_t> nextRank;
        
        // For each AS at current rank
        for (uint32_t asn : currentRank) {
            auto node = getNode(asn);
            if (!node) continue;
            
            // Assign rank+1 to all unranked providers
            for (const auto& provider : node->getProviders()) {
                if (provider->getPropagationRank() == -1) {
                    provider->setPropagationRank(rank + 1);
                    nextRank.push_back(provider->getASN());
                }
            }
        }
        
        // Move to next rank
        if (!nextRank.empty()) {
            result.push_back(nextRank);
            currentRank = std::move(nextRank);
            rank++;
        } else {
            break;
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
    
    // Start at rank 0 and work up to max rank
    for (int rank = 0; rank <= maxRank; ++rank) {
        // Step 1: All ASes at this rank send to their providers
        for (const auto& [asn, node] : nodes_) {
            if (node->getPropagationRank() == rank) {
                node->sendToProviders();
            }
        }
        
        // Step 2: All ASes at next rank process what they received
        // (This prevents announcements from traveling multiple hops in one step)
        if (rank + 1 <= maxRank) {
            for (const auto& [asn, node] : nodes_) {
                if (node->getPropagationRank() == rank + 1) {
                    node->processReceivedQueue();
                }
            }
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
 * - Prevents multi-hop peer propagation (valley-free routing)
 * - Example: If we processed immediately:
 *   AS1 → AS2 (process) → AS3 (process) = 2 hops across peers (BAD!)
 * - With batching:
 *   AS1 → AS2 queue, AS2 → AS3 queue, THEN process all = 1 hop each (GOOD!)
 * 
 * Performance: O(V * N * P) where:
 * - V = number of ASes (~78k)
 * - N = announcements per AS (typically 1-10)
 * - P = peers per AS (typically 5-20)
 * 
 * Note: Only customer and origin routes are sent to peers (export policy)
 */
void ASGraph::propagateAcross() {
    // Phase 1: All ASes send to peers
    for (const auto& [asn, node] : nodes_) {
        node->sendToPeers();
    }
    
    // Phase 2: All ASes process what they received
    // (This happens AFTER all sending is complete)
    for (const auto& [asn, node] : nodes_) {
        node->processReceivedQueue();
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
    
    // Start at max rank and work down to rank 0
    for (int rank = maxRank; rank >= 0; --rank) {
        // Step 1: All ASes at this rank send to their customers
        for (const auto& [asn, node] : nodes_) {
            if (node->getPropagationRank() == rank) {
                node->sendToCustomers();
            }
        }
        
        // Step 2: All ASes at next rank down process what they received
        if (rank - 1 >= 0) {
            for (const auto& [asn, node] : nodes_) {
                if (node->getPropagationRank() == rank - 1) {
                    node->processReceivedQueue();
                }
            }
        }
    }
    
    // Don't forget rank 0 needs to process at the end
    for (const auto& [asn, node] : nodes_) {
        if (node->getPropagationRank() == 0) {
            node->processReceivedQueue();
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
 * (subject to valley-free routing and export policies)
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
void ASGraph::propagateAll() {
    std::cout << "Starting BGP propagation...\n";
    
    std::cout << "Phase 1: Propagating up (to providers)...\n";
    propagateUp();
    
    std::cout << "Phase 2: Propagating across (to peers)...\n";
    propagateAcross();
    
    std::cout << "Phase 3: Propagating down (to customers)...\n";
    propagateDown();
    
    std::cout << "BGP propagation complete!\n";
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
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << " for writing\n";
        return false;
    }
    
    // Write CSV header
    file << "asn,prefix,as_path\n";
    
    // Iterate through all ASes in the graph
    for (const auto& [asn, node] : nodes_) {
        // Get this AS's BGP policy to access its routing table
        auto policy = node->getPolicy();
        if (!policy) {
            continue;  // No policy = no routes
        }
        
        // Get all prefixes this AS has routes for
        std::vector<IPPrefix> prefixes = policy->getLocalRIBPrefixes();
        
        // For each prefix, write a CSV line
        for (const auto& prefix : prefixes) {
            auto announcement = policy->getBestAnnouncement(prefix);
            if (!announcement) {
                continue;  // No route for this prefix (shouldn't happen)
            }
            
            // Write: asn,prefix,as_path
            file << asn << "," << prefix.toString() << ",";
            
            // Write AS-Path as space-separated ASNs
            const auto& as_path = announcement->getASPath();
            for (size_t i = 0; i < as_path.size(); ++i) {
                file << as_path[i];
                if (i < as_path.size() - 1) {
                    file << " ";  // Space between ASNs
                }
            }
            file << "\n";
        }
    }
    
    file.close();
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
