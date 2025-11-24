#include "bgp.h"
#include <algorithm>
#include <iostream>

/**
 * @file bgp.cpp
 * @brief Implementation of BGP routing policy
 * 
 * Implements standard BGP route selection following Gao-Rexford economics.
 * Core idea: Prefer routes that make money (customers) over routes that cost money (providers).
 */

void BGP::receiveAnnouncement(const Announcement& ann) {
    IPPrefix prefix = ann.getPrefix();
    
    // Store in received queue (keep all announcements)
    received_queue_[prefix].push_back(ann);
    
    // Run route selection to update local RIB
    auto best = selectBestRoute(prefix);
    if (best) {
        local_rib_[prefix] = *best;
    }
}

std::optional<Announcement> BGP::getBestAnnouncement(const IPPrefix& prefix) const {
    auto it = local_rib_.find(prefix);
    if (it != local_rib_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool BGP::hasRoute(const IPPrefix& prefix) const {
    return local_rib_.find(prefix) != local_rib_.end();
}

std::vector<Announcement> BGP::getReceivedAnnouncements(const IPPrefix& prefix) const {
    auto it = received_queue_.find(prefix);
    if (it != received_queue_.end()) {
        return it->second;
    }
    return {};
}

size_t BGP::getTotalReceivedCount() const {
    size_t total = 0;
    for (const auto& [prefix, announcements] : received_queue_) {
        total += announcements.size();
    }
    return total;
}

void BGP::clear() {
    local_rib_.clear();
    received_queue_.clear();
}

std::vector<IPPrefix> BGP::getLocalRIBPrefixes() const {
    std::vector<IPPrefix> prefixes;
    prefixes.reserve(local_rib_.size());
    
    for (const auto& [prefix, announcement] : local_rib_) {
        prefixes.push_back(prefix);
    }
    
    return prefixes;
}

std::optional<Announcement> BGP::selectBestRoute(const IPPrefix& prefix) const {
    // Get all announcements for this prefix
    auto it = received_queue_.find(prefix);
    if (it == received_queue_.end() || it->second.empty()) {
        return std::nullopt;
    }
    
    const auto& announcements = it->second;
    
    // Start with first announcement as best
    const Announcement* best = &announcements[0];
    
    // Compare with all other announcements
    for (size_t i = 1; i < announcements.size(); ++i) {
        const Announcement& candidate = announcements[i];
        
        // Get relationship types for comparison
        RelationshipType best_rel = best->getReceivedFrom();
        RelationshipType cand_rel = candidate.getReceivedFrom();
        
        // Step 1: Prefer better relationship
        // ORIGIN (3) > FROM_CUSTOMER (0) > FROM_PEER (1) > FROM_PROVIDER (2)
        // Special case: ORIGIN always wins
        if (cand_rel == RelationshipType::ORIGIN && best_rel != RelationshipType::ORIGIN) {
            best = &candidate;
            continue;
        } else if (best_rel == RelationshipType::ORIGIN && cand_rel != RelationshipType::ORIGIN) {
            continue;  // Keep ORIGIN as best
        }
        
        // For non-ORIGIN: FROM_CUSTOMER (0) > FROM_PEER (1) > FROM_PROVIDER (2)
        // Lower enum value = better relationship
        if (cand_rel != RelationshipType::ORIGIN && best_rel != RelationshipType::ORIGIN) {
            if (static_cast<int>(cand_rel) < static_cast<int>(best_rel)) {
                best = &candidate;
                continue;
            } else if (static_cast<int>(cand_rel) > static_cast<int>(best_rel)) {
                continue;  // Keep current best
            }
        }
        
        // Step 2: Same relationship - prefer shorter AS-Path
        size_t best_len = best->getASPath().size();
        size_t cand_len = candidate.getASPath().size();
        
        if (cand_len < best_len) {
            best = &candidate;
            continue;
        } else if (cand_len > best_len) {
            continue;  // Keep current best
        }
        
        // Step 3: Same path length - prefer lower next_hop ASN
        uint32_t best_nexthop = best->getNextHop();
        uint32_t cand_nexthop = candidate.getNextHop();
        
        if (cand_nexthop < best_nexthop) {
            best = &candidate;
            continue;
        } else if (cand_nexthop > best_nexthop) {
            continue;  // Keep current best
        }
        
        // Step 4: Tie - keep first seen (already in best)
        // This provides stability - prevents route flapping
    }
    
    return *best;
}
