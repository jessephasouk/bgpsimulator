#pragma once
#include "policy.h"
#include "announcement.h"
#include <unordered_map>
#include <vector>
#include <optional>

/**
 * @brief BGP (Border Gateway Protocol) routing policy
 * 
 * Implements standard BGP route selection algorithm following the Gao-Rexford model.
 * 
 * Route Selection Process:
 * 1. Prefer customer routes (FROM_CUSTOMER) - makes money!
 * 2. Then prefer peer routes (FROM_PEER) - free transit
 * 3. Then prefer provider routes (FROM_PROVIDER) - costs money
 * 4. Within same relationship, prefer shorter AS-Path
 * 5. Tie-breaker: first-seen wins (stable routing)
 * 
 * Data Structures:
 * - Local RIB: One best route per prefix (O(1) lookup)
 * - Received Queue: All routes per prefix (for re-evaluation)
 * 
 * Why this design?
 * - Economics: Customer routes make money, provider routes cost money
 * - Performance: Hash maps for O(1) lookups during forwarding
 * - Correctness: Store all announcements for proper route selection
 * 
 * Real BGP has more tie-breakers (router ID, IGP cost, etc.), but this
 * simplified version captures the core valley-free routing economics.
 */
class BGP : public Policy {
public:
    /**
     * @brief Default constructor - initializes empty RIB
     */
    BGP() = default;

    /**
     * @brief Receive and process a new BGP announcement
     * 
     * Stores announcement in received queue and runs route selection.
     * If this announcement is better than current best, updates local RIB.
     * 
     * @param ann The announcement to receive
     * 
     * Time Complexity: O(k) where k = number of announcements for this prefix
     * Space Complexity: O(1) per announcement stored
     * 
     * Example:
     *   BGP bgp;
     *   Announcement google("8.8.8.0/24", {15169}, 15169, RelationshipType::ORIGIN);
     *   bgp.receiveAnnouncement(google);
     *   // Now getBestAnnouncement("8.8.8.0/24") returns google's route
     */
    void receiveAnnouncement(const Announcement& ann) override;

    /**
     * @brief Get the best route for a prefix
     * 
     * Returns the best announcement from the local RIB (O(1) lookup).
     * 
     * @param prefix The IP prefix to lookup
     * @return The best announcement, or std::nullopt if no route exists
     * 
     * Time Complexity: O(1) - hash map lookup
     * 
     * Example:
     *   auto best = bgp.getBestAnnouncement(IPPrefix("8.8.8.0/24"));
     *   if (best) {
     *       std::cout << "Best route: " << best->toString() << "\n";
     *   }
     */
    std::optional<Announcement> getBestAnnouncement(const IPPrefix& prefix) const override;

    /**
     * @brief Check if we have any route to a prefix
     * 
     * @param prefix The prefix to check
     * @return true if we have a route, false otherwise
     * 
     * Time Complexity: O(1)
     */
    bool hasRoute(const IPPrefix& prefix) const override;

    /**
     * @brief Get all received announcements for a prefix
     * 
     * Returns all announcements from the received queue, not just the best one.
     * Useful for debugging route selection or implementing backup routes.
     * 
     * @param prefix The prefix to query
     * @return Vector of all received announcements (may be empty)
     * 
     * Time Complexity: O(1) to find prefix, O(k) to copy k announcements
     * 
     * Example:
     *   auto all = bgp.getReceivedAnnouncements(IPPrefix("8.8.8.0/24"));
     *   std::cout << "Received " << all.size() << " routes\n";
     */
    std::vector<Announcement> getReceivedAnnouncements(const IPPrefix& prefix) const override;

    /**
     * @brief Get the size of the local RIB
     * @return Number of prefixes in local RIB
     */
    size_t getLocalRIBSize() const { return local_rib_.size(); }
    
    /**
     * @brief Get all prefixes in the local RIB
     * @return Vector of all prefixes we have routes for
     * 
     * Used for announcement propagation - we need to know what to send.
     */
    std::vector<IPPrefix> getLocalRIBPrefixes() const;

    /**
     * @brief Get total number of announcements received (across all prefixes)
     * 
     * Counts all announcements in received queue, useful for statistics.
     * 
     * @return Total count of received announcements
     * 
     * Time Complexity: O(n) where n = number of prefixes
     */
    size_t getTotalReceivedCount() const;

    /**
     * @brief Clear all state (reset RIB and received queue)
     * 
     * Useful for testing or simulating routing table resets.
     */
    void clear();

    /**
     * @brief Set the owning ASN for this policy instance (for debugging/logging)
     * @param asn ASN that owns this BGP instance
     */
    void setOwnerASN(uint32_t asn) { owner_asn_ = asn; }

    /**
     * @brief Get the owning ASN if one has been set
     */
    std::optional<uint32_t> getOwnerASN() const { return owner_asn_; }

    /**
     * @brief Internal helper for hot-path export loops
     * @return Pointer to best announcement or nullptr if none exists
     */
    const Announcement* findAnnouncement(const IPPrefix& prefix) const;

private:
    /**
     * @brief Select the best route from all received announcements
     * 
     * Implements BGP route selection algorithm:
     * 1. Prefer customer > peer > provider (economics!)
     * 2. Then prefer shorter AS-Path (performance)
     * 3. Then first-seen wins (stability)
     * 
     * @param prefix The prefix to select best route for
     * @return The best announcement, or std::nullopt if no routes
     * 
     * Time Complexity: O(k) where k = announcements for this prefix
     * 
     * Why this algorithm?
     * - Economics: Prioritize profitable routes (customer pays you)
     * - Valley-free routing: Prevents transit through peers/providers
     * - Stability: First-seen tie-breaker prevents route flapping
     */
    std::optional<Announcement> selectBestRoute(const IPPrefix& prefix) const;

    /**
     * Local RIB (Routing Information Base)
     * 
     * Maps prefix → best announcement
     * Stores only ONE route per prefix (the current best)
     * 
     * Key: IPPrefix
     * Value: Announcement (the best one)
     * 
     * Why unordered_map (hash map)?
     * - O(1) average lookups vs O(log n) for std::map
     * - Most common operation: "What's my route to prefix X?"
     * - Need fast lookups during packet forwarding simulation
     * 
     * Alternative: std::map for ordered iteration
     * - Would be O(log n) lookups
     * - Only useful if we need prefixes sorted
     */
    std::unordered_map<IPPrefix, Announcement> local_rib_;

    /**
     * Received Queue (Adj-RIB-In in real BGP)
     * 
     * Maps prefix → all received announcements
     * Stores ALL announcements, not just the best one
     * 
     * Key: IPPrefix
     * Value: std::vector<Announcement> (all received for this prefix)
     * 
     * Why store all announcements?
     * 1. Route selection: Need to compare all options
     * 2. Backup routes: If best fails, can switch to next best
     * 3. Debugging: Can see why a route was chosen
     * 4. Policy changes: Can re-run selection without re-receiving
     * 
     * Design choice: vector instead of set
     * - Order doesn't matter for route selection
     * - Vector has better cache locality
     * - Small k (typically < 5) so linear search is fine
     */
    std::unordered_map<IPPrefix, std::vector<Announcement>> received_queue_;

    std::optional<uint32_t> owner_asn_{};  // Owning ASN for trace context
};
