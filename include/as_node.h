#pragma once
#include <memory>
#include <set>
#include <cstdint>
#include "policy.h"
#include "announcement.h"

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
     * @brief Get the propagation rank of this AS
     * @return Propagation rank (0 = leaf/edge, higher = more upstream)
     * 
     * Rank meaning:
     * - 0: Edge AS (no customers) - e.g., Google, Netflix, universities
     * - 1: Tier-2 AS with only edge customers
     * - 2+: Higher tiers up the provider chain
     * - Used to determine announcement propagation order
     */
    int getPropagationRank() const { return propagation_rank_; }
    
    /**
     * @brief Set the propagation rank of this AS
     * @param rank The rank to assign (0 = edge AS)
     */
    void setPropagationRank(int rank) { propagation_rank_ = rank; }
    
    /**
     * @brief Get the routing policy for this AS
     * @return Pointer to the policy (may be nullptr if not set)
     */
    Policy* getPolicy() const { return policy_.get(); }
    
    /**
     * @brief Set the routing policy for this AS
     * @param policy The policy to use (takes ownership)
     * 
     * Example:
     *   node->setPolicy(std::make_unique<BGP>());
     */
    void setPolicy(std::unique_ptr<Policy> policy);
    
    /**
     * @brief Seed this AS with an origin announcement
     * @param prefix The IP prefix to announce (e.g., "1.2.0.0/16")
     * 
     * Creates an origin announcement with:
     * - AS-Path: [this AS]
     * - Next hop: this AS
     * - Relationship: ORIGIN (highest preference)
     * 
     * This is how announcements start - an AS originates a prefix it owns.
     * 
     * Example:
     *   node->seedAnnouncement(IPPrefix("1.2.0.0/16"));           // Legitimate
     *   node->seedAnnouncement(IPPrefix("8.8.8.0/24"), true);     // Hijack!
     */
    void seedAnnouncement(const IPPrefix& prefix, bool rov_invalid = false);
    
    /**
     * @brief Add an announcement to the received queue
     * @param ann The announcement received from a neighbor
     * 
     * Stores announcements temporarily before processing.
     * This allows batch processing to prevent multi-hop propagation in single step.
     */
    void addToReceivedQueue(const Announcement& ann);
    
    /**
     * @brief Process all announcements in the received queue
     * 
     * For each announcement:
     * 1. Check for loops (ignore if this AS is already in AS-Path)
     * 2. Prepend this AS to the AS-Path
     * 3. Update next_hop to this AS
     * 4. Store in local RIB via policy
     * 5. Clear the received queue
     * 
     * This is called after all ASes at a rank have sent their announcements.
     */
    void processReceivedQueue();

    /**
     * @brief Process received queue and report which prefixes changed
     *
     * Returns the list of prefixes whose best announcement changed as a
     * result of processing the queue. Useful for event-driven propagation
     * where we only need to re-announce when local state actually updates.
     */
    std::vector<IPPrefix> processReceivedQueueWithDiff();
    
    /**
     * @brief Send all announcements in local RIB to providers
     * 
     * For each announcement in local RIB:
     * - Update relationship to FROM_CUSTOMER (provider sees us as customer)
     * - Update next_hop to this AS
     * - Add to each provider's received queue
     */
    void sendToProviders();
    void sendToProviders(const std::vector<IPPrefix>& prefixes);
    
    /**
     * @brief Send all announcements in local RIB to customers
     * 
     * For each announcement in local RIB:
     * - Update relationship to FROM_PROVIDER (customer sees us as provider)
     * - Update next_hop to this AS
     * - Add to each customer's received queue
     */
    void sendToCustomers();
    void sendToCustomers(const std::vector<IPPrefix>& prefixes);
    
    /**
     * @brief Send all announcements in local RIB to peers
     * 
     * For each announcement in local RIB:
     * - Only send if received from customer or originated (valley-free routing)
     * - Update relationship to FROM_PEER
     * - Update next_hop to this AS
     * - Add to each peer's received queue
     * 
     * Export policy: Don't send provider/peer routes to peers
     * (This prevents being used as free transit)
     */
    void sendToPeers();
    void sendToPeers(const std::vector<IPPrefix>& prefixes);
    
    /**
     * @brief Get all prefixes in the local RIB
     * @return Vector of prefixes that this AS has routes for
     */
    std::vector<IPPrefix> getLocalRIBPrefixes() const;

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

    /**
     * @brief Get the received queue (for debugging/introspection)
     * @return Reference to the received queue
     */
    const std::vector<Announcement>& getReceivedQueue() const { return received_queue_; }

private:
    uint32_t asn_;  // Autonomous System Number (unique ID)
    int propagation_rank_ = -1;  // Propagation rank (-1 = unassigned, 0+ = rank)
    std::unique_ptr<Policy> policy_;  // Routing policy (BGP route selection)
    std::vector<Announcement> received_queue_;  // Temporary storage for announcements before processing
    
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

