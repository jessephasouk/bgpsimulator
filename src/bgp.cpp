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
        
        // Step 1: Prefer better relationship (customer > peer > provider)
        // Lower enum value = better relationship
        if (static_cast<int>(cand_rel) < static_cast<int>(best_rel)) {
            best = &candidate;
            continue;
        } else if (static_cast<int>(cand_rel) > static_cast<int>(best_rel)) {
            continue;  // Keep current best
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
        
        // Step 3: Tie - keep first seen (already in best)
        // This provides stability - prevents route flapping
    }
    
    return *best;
}
