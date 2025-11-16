#pragma once
#include "announcement.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <optional>

/**
 * @brief Abstract base class for routing policies
 * 
 * The Policy class defines the interface for routing decision-making.
 * Different routing protocols (BGP, OSPF, etc.) can implement this interface
 * with their specific routing logic.
 * 
 * Design Pattern: Strategy Pattern
 * - Each AS has a Policy object
 * - Policy determines how to store and select routes
 * - Easy to swap routing protocols (BGP vs custom)
 * 
 * Why abstract class instead of concrete?
 * - Extensibility: Can implement different routing policies
 * - Testing: Can create mock policies for unit tests
 * - Research: Can experiment with BGP variants
 */
class Policy {
public:
    virtual ~Policy() = default;

    /**
     * @brief Process an incoming announcement
     * @param ann The announcement received from a neighbor
     * 
     * This is the main entry point for route updates.
     * Implementations should:
     * 1. Add to received queue
     * 2. Run route selection
     * 3. Update local RIB if better route found
     */
    virtual void receiveAnnouncement(const Announcement& ann) = 0;

    /**
     * @brief Get the best announcement for a prefix
     * @param prefix The IP prefix to look up
     * @return The best announcement, or std::nullopt if none exists
     * 
     * Returns the announcement currently in the local RIB.
     * This is the route the AS will actually use for forwarding.
     */
    virtual std::optional<Announcement> getBestAnnouncement(const IPPrefix& prefix) const = 0;

    /**
     * @brief Check if we have any route to a prefix
     * @param prefix The IP prefix to check
     * @return true if we have at least one announcement for this prefix
     */
    virtual bool hasRoute(const IPPrefix& prefix) const = 0;

    /**
     * @brief Get all received announcements for a prefix
     * @param prefix The IP prefix
     * @return Vector of all announcements received for this prefix
     * 
     * Used for debugging and testing route selection logic.
     */
    virtual std::vector<Announcement> getReceivedAnnouncements(const IPPrefix& prefix) const = 0;
};

/**
 * @brief BGP routing policy implementation
 * 
 * Implements the Border Gateway Protocol routing policy with:
 * - Local RIB: Best route per prefix
 * - Received queue: All received routes per prefix
 * - Route selection: BGP decision process
 * 
 * Data Structures:
 * 
 * 1. Local RIB (Routing Information Base):
 *    - std::unordered_map<IPPrefix, Announcement>
 *    - One entry per prefix (the BEST route)
 *    - Used for actual packet forwarding
 *    - Example: {"8.8.8.0/24" → Announcement(...)}
 * 
 * 2. Received Queue:
 *    - std::unordered_map<IPPrefix, std::vector<Announcement>>
 *    - All announcements received per prefix
 *    - Used for route selection and comparison
 *    - Example: {"8.8.8.0/24" → [ann1, ann2, ann3]}
 * 
 * Memory Usage (typical):
 * - Local RIB: ~70 bytes × number of unique prefixes
 * - Received Queue: ~70 bytes × total announcements
 * - For 1M prefixes with avg 3 announcements each:
 *   - Local RIB: ~70 MB
 *   - Received Queue: ~210 MB
 *   - Total: ~280 MB (reasonable!)
 * 
 * Why separate RIB and received queue?
 * - RIB is for fast lookups (O(1) hash lookup)
 * - Received queue is for route comparison and debugging
 * - Mirrors real BGP implementation (Adj-RIB-In vs Loc-RIB)
 */
class BGP : public Policy {
public:
    BGP() = default;
    ~BGP() override = default;

    /**
     * @brief Process an incoming BGP announcement
     * @param ann The announcement to process
     * 
     * Algorithm:
     * 1. Add announcement to received queue
     * 2. Run BGP decision process for this prefix
     * 3. If this announcement is better than current best:
     *    - Update local RIB
     *    - Trigger announcement to neighbors (future: Section 3.3)
     * 
     * Performance: O(k) where k = number of announcements for this prefix
     * - Hash lookup: O(1) average
     * - Route selection: O(k) comparison
     * - Typical k: 1-5, so very fast
     */
    void receiveAnnouncement(const Announcement& ann) override;

    /**
     * @brief Get the best announcement for a prefix from local RIB
     * @param prefix The IP prefix
     * @return The best announcement, or std::nullopt if no route exists
     * 
     * Performance: O(1) hash lookup
     */
    std::optional<Announcement> getBestAnnouncement(const IPPrefix& prefix) const override;

    /**
     * @brief Check if we have a route to this prefix
     * @param prefix The IP prefix
     * @return true if local RIB has an entry for this prefix
     * 
     * Performance: O(1)
     */
    bool hasRoute(const IPPrefix& prefix) const override;

    /**
     * @brief Get all announcements received for a prefix
     * @param prefix The IP prefix
     * @return Vector of all announcements in received queue
     * 
     * Performance: O(1) to find + O(k) to copy vector
     * Used primarily for debugging and testing.
     */
    std::vector<Announcement> getReceivedAnnouncements(const IPPrefix& prefix) const override;

    /**
     * @brief Get the size of the local RIB
     * @return Number of prefixes in the local RIB
     * 
     * Useful for monitoring and testing.
     */
    size_t getLocalRIBSize() const { return local_rib_.size(); }

    /**
     * @brief Get the total number of received announcements
     * @return Total announcements across all prefixes
     * 
     * Useful for monitoring memory usage.
     */
    size_t getTotalReceivedCount() const;

    /**
     * @brief Clear all stored routes (for testing)
     */
    void clear();

protected:
    /**
     * @brief Run BGP route selection for a prefix
     * @param prefix The prefix to run selection for
     * @return The best announcement, or std::nullopt if no candidates
     * 
     * Implements the BGP decision process:
     * 1. Prefer customer routes over peer/provider routes
     * 2. Prefer shorter AS-Paths
     * 3. (Future: tie-breakers like origin type, MED, etc.)
     * 
     * This is where the routing policy is enforced!
     * 
     * Performance: O(k) where k = announcements for this prefix
     * - Must compare all candidates to find best
     * - Typical k: 1-5, so very fast
     */
    std::optional<Announcement> selectBestRoute(const IPPrefix& prefix) const;

private:
    /**
     * Local RIB (Routing Information Base)
     * 
     * Maps prefix → best announcement
     * This is THE routing table - used for actual forwarding decisions
     * 
     * Key: IPPrefix (e.g., "8.8.8.0/24")
     * Value: Announcement (the BEST route to that prefix)
     * 
     * Design choice: unordered_map for O(1) lookups
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
};
