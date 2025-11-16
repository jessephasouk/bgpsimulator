#include "policy.h"
#include <algorithm>

// ============================================================================
// BGP Implementation
// ============================================================================

/**
 * Process incoming announcement
 * 
 * Algorithm:
 * 1. Add to received queue for this prefix
 * 2. Run route selection to find best route
 * 3. Update local RIB if we found a best route
 * 
 * Performance: O(k) where k = announcements for this prefix
 * - Queue insertion: O(1) amortized (vector push_back)
 * - Route selection: O(k) to compare all announcements
 * - RIB update: O(1) hash table insert/update
 */
void BGP::receiveAnnouncement(const Announcement& ann) {
    const IPPrefix& prefix = ann.getPrefix();
    
    // Add to received queue
    // Creates empty vector if prefix doesn't exist
    received_queue_[prefix].push_back(ann);
    
    // Run route selection for this prefix
    auto best = selectBestRoute(prefix);
    
    // Update local RIB if we have a best route
    if (best.has_value()) {
        local_rib_[prefix] = best.value();
    }
}

/**
 * Get best announcement from local RIB
 * 
 * Performance: O(1) average case (hash lookup)
 */
std::optional<Announcement> BGP::getBestAnnouncement(const IPPrefix& prefix) const {
    auto it = local_rib_.find(prefix);
    if (it != local_rib_.end()) {
        return it->second;
    }
    return std::nullopt;
}

/**
 * Check if we have a route to this prefix
 * 
 * Performance: O(1)
 */
bool BGP::hasRoute(const IPPrefix& prefix) const {
    return local_rib_.find(prefix) != local_rib_.end();
}

/**
 * Get all received announcements for a prefix
 * 
 * Performance: O(1) + O(k) to copy vector
 */
std::vector<Announcement> BGP::getReceivedAnnouncements(const IPPrefix& prefix) const {
    auto it = received_queue_.find(prefix);
    if (it != received_queue_.end()) {
        return it->second;  // Return copy of vector
    }
    return {};  // Empty vector if no announcements
}

/**
 * Get total number of received announcements
 * 
 * Performance: O(p) where p = number of prefixes
 */
size_t BGP::getTotalReceivedCount() const {
    size_t count = 0;
    for (const auto& [prefix, announcements] : received_queue_) {
        count += announcements.size();
    }
    return count;
}

/**
 * Clear all stored routes
 * 
 * Useful for testing and resetting state.
 */
void BGP::clear() {
    local_rib_.clear();
    received_queue_.clear();
}

/**
 * BGP Route Selection Algorithm
 * 
 * Implements the BGP decision process to choose the best route
 * among all received announcements for a prefix.
 * 
 * BGP Decision Process (simplified):
 * 1. **Prefer customer routes** (you get paid!)
 *    - Customer > Peer > Provider
 * 2. **Prefer shorter AS-Paths** (fewer hops = faster, more reliable)
 *    - Shorter path wins
 * 3. **Tie-breaker**: First received (stable routing)
 * 
 * Full BGP has ~10 tie-breaker rules, but these 2 are most important.
 * 
 * Why this order?
 * - Economics: Customer routes make money (they pay you for transit)
 * - Performance: Shorter paths are faster and more reliable
 * - Stability: Consistent tie-breaking prevents route flapping
 * 
 * Performance: O(k) where k = number of announcements for this prefix
 * - Must examine all announcements to find best
 * - Typical k: 1-5 (most prefixes have few announcements)
 * - Worst case k: ~20 (multi-homed organizations)
 */
std::optional<Announcement> BGP::selectBestRoute(const IPPrefix& prefix) const {
    // Get all announcements for this prefix
    auto it = received_queue_.find(prefix);
    if (it == received_queue_.end() || it->second.empty()) {
        return std::nullopt;  // No announcements
    }
    
    const std::vector<Announcement>& candidates = it->second;
    
    // If only one announcement, it's automatically the best
    if (candidates.size() == 1) {
        return candidates[0];
    }
    
    // Find best announcement using BGP decision process
    const Announcement* best = &candidates[0];
    
    for (size_t i = 1; i < candidates.size(); ++i) {
        const Announcement& candidate = candidates[i];
        
        // Rule 1: Prefer customer routes over peer/provider routes
        // Customer routes are most profitable (you get paid to carry traffic)
        RelationshipType best_rel = best->getReceivedFrom();
        RelationshipType cand_rel = candidate.getReceivedFrom();
        
        // Customer < Peer < Provider (lower is better)
        int best_pref = (best_rel == RelationshipType::FROM_CUSTOMER) ? 0 :
                       (best_rel == RelationshipType::FROM_PEER) ? 1 : 2;
        int cand_pref = (cand_rel == RelationshipType::FROM_CUSTOMER) ? 0 :
                       (cand_rel == RelationshipType::FROM_PEER) ? 1 : 2;
        
        if (cand_pref < best_pref) {
            // Candidate has better relationship preference
            best = &candidate;
            continue;
        } else if (cand_pref > best_pref) {
            // Current best has better relationship
            continue;
        }
        
        // Rule 2: Prefer shorter AS-Paths
        // Shorter paths are:
        // - Faster (fewer hops)
        // - More reliable (fewer points of failure)
        // - Lower latency
        if (candidate.getPathLength() < best->getPathLength()) {
            best = &candidate;
            continue;
        } else if (candidate.getPathLength() > best->getPathLength()) {
            continue;
        }
        
        // Rule 3: Tie-breaker - keep first seen (stable routing)
        // In a real implementation, we might use:
        // - Origin type (IGP > EGP > INCOMPLETE)
        // - MED (Multi-Exit Discriminator)
        // - eBGP over iBGP
        // - Lowest router ID
        // But for simulation, first-seen is sufficient
        
        // Keep current best (first-seen wins)
    }
    
    return *best;
}
