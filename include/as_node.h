#pragma once
#include <memory>
#include <set>
#include <cstdint>

/**
 * @brief Represents an Autonomous System (AS) node in the BGP graph
 * 
 * Design Decisions:
 * 
 * 1. Why std::set for relationships?
 *    - Automatic deduplication (no duplicate relationships)
 *    - Ordered by ASN (helpful for debugging and deterministic behavior)
 *    - O(log n) insert/lookup (good enough for typical AS degree)
 *    - Alternative: std::unordered_set for O(1) but loses ordering
 * 
 * 2. Why shared_ptr?
 *    - Multiple nodes can reference the same AS (circular references)
 *    - Automatic memory management (no manual delete)
 *    - Safer than raw pointers (prevents dangling pointers)
 *    - Trade-off: slightly slower than raw pointers, uses more memory
 * 
 * 3. Why uint32_t for ASN?
 *    - ASNs range from 0 to 4,294,967,295 (32-bit unsigned)
 *    - Current max ASN in use: ~400,000 (but growing)
 *    - Fixed-size type ensures consistency across platforms
 * 
 * Memory per node (approximate):
 * - ASN: 4 bytes
 * - 3 std::sets: ~48 bytes overhead + (8 bytes × number of relationships)
 * - For avg AS with 10 relationships: ~130 bytes
 * - 78k nodes: ~10 MB total (very reasonable!)
 */
class ASNode {
public:
    /**
     * @brief Construct a new ASNode
     * @param asn The Autonomous System Number (unique identifier)
     * 
     * Performance: O(1) - just initializes member variables
     */
    explicit ASNode(uint32_t asn);

    // Getters - all O(1) operations
    // Declare const because they do not modify the object
    uint32_t getASN() const { return asn_; }
    const std::set<std::shared_ptr<ASNode>>& getProviders() const { return providers_; }
    const std::set<std::shared_ptr<ASNode>>& getCustomers() const { return customers_; }
    const std::set<std::shared_ptr<ASNode>>& getPeers() const { return peers_; }

    /**
     * Relationship management
     * 
     * Performance: O(log k) where k = number of relationships of that type
     * - std::set insert is O(log k)
     * - For typical AS: k < 100, so log k ≈ 6-7 comparisons
     * 
     * Why separate functions instead of one addRelationship(type)?
     * - Type safety: can't accidentally mix relationship types
     * - Clearer intent in calling code
     * - No need for enum/switch statement
     */
    void addProvider(std::shared_ptr<ASNode> provider);
    void addCustomer(std::shared_ptr<ASNode> customer);
    void addPeer(std::shared_ptr<ASNode> peer);

    /**
     * Comparison operators for using ASNode in containers
     * 
     * Why compare by ASN only?
     * - ASN is unique identifier (like a primary key)
     * - Makes ASNodes sortable and usable in std::set
     * - Relationships don't affect equality (same AS = same node)
     */
    bool operator<(const ASNode& other) const { return asn_ < other.asn_; }
    bool operator==(const ASNode& other) const { return asn_ == other.asn_; }

private:
    uint32_t asn_;  // Autonomous System Number (unique ID)
    
    /**
     * Relationship storage using std::set
     * 
     * Why std::set<std::shared_ptr<ASNode>> instead of std::set<uint32_t>?
     * - Direct access to related nodes without hash lookup
     * - Can traverse graph by following pointers
     * - Trade-off: uses more memory (pointer + node) vs just storing ASN (uint32_t)
     * 
     * Relationships explained:
     * - Providers: ASes that provide transit (upstream connectivity)
     * - Customers: ASes that this AS provides transit to (downstream)
     * - Peers: ASes with settlement-free peering (lateral connectivity)
     */
    std::set<std::shared_ptr<ASNode>> providers_;  // Upstream: ASes that provide transit TO this AS
    std::set<std::shared_ptr<ASNode>> customers_;  // Downstream: ASes that this AS provides transit TO
    std::set<std::shared_ptr<ASNode>> peers_;      // Lateral: ASes with peer-to-peer relationship
};

/**
 * Comparator for shared_ptr<ASNode> - needed for sets of pointers
 * 
 * Why needed?
 * - Default shared_ptr comparison compares pointer addresses (not ASN values)
 * - We want to compare by ASN value (uint32_t) for logical ordering
 * - Used when storing std::shared_ptr<ASNode> in containers that need ordering
 */
struct ASNodePtrComparator {
    bool operator()(const std::shared_ptr<ASNode>& a, const std::shared_ptr<ASNode>& b) const {
        return a->getASN() < b->getASN();
    }
};

