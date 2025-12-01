#include "bgp.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>

/**
 * @file bgp.cpp
 * @brief Implementation of BGP routing policy
 * 
 * Implements simplified BGP route selection based on relationship preferences.
 * Core idea: Prefer routes that make money (customers) over routes that cost money (providers).
 */

namespace {

bool isBetterAnnouncement(const Announcement& candidate, const Announcement& current) {
    RelationshipType cand_rel = candidate.getReceivedFrom();
    RelationshipType curr_rel = current.getReceivedFrom();

    if (cand_rel == RelationshipType::ORIGIN && curr_rel != RelationshipType::ORIGIN) {
        return true;
    }
    if (curr_rel == RelationshipType::ORIGIN && cand_rel != RelationshipType::ORIGIN) {
        return false;
    }

    if (cand_rel != RelationshipType::ORIGIN && curr_rel != RelationshipType::ORIGIN) {
        if (static_cast<int>(cand_rel) < static_cast<int>(curr_rel)) {
            return true;
        }
        if (static_cast<int>(cand_rel) > static_cast<int>(curr_rel)) {
            return false;
        }
    }

    size_t cand_len = candidate.getASPath().size();
    size_t curr_len = current.getASPath().size();
    if (cand_len < curr_len) {
        return true;
    }
    if (cand_len > curr_len) {
        return false;
    }

    uint32_t cand_next = candidate.getNextHop();
    uint32_t curr_next = current.getNextHop();
    if (cand_next < curr_next) {
        return true;
    }
    if (cand_next > curr_next) {
        return false;
    }

    return false;
}

} // namespace

void BGP::receiveAnnouncement(const Announcement& ann) {
    IPPrefix prefix = ann.getPrefix();

    static const char* trace_prefix_env = std::getenv("BGP_TRACE_PREFIX");
    static const bool trace_all = std::getenv("BGP_TRACE_ALL") != nullptr;
    static const std::optional<uint32_t> trace_asn = []() -> std::optional<uint32_t> {
        const char* env = std::getenv("BGP_TRACE_ASN");
        if (!env || *env == '\0') {
            return std::nullopt;
        }
        char* end = nullptr;
        errno = 0;
        unsigned long value = std::strtoul(env, &end, 10);
        if (errno != 0 || end == env || value > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(value);
    }();

    const std::string& prefix_str = prefix.toString();
    const bool prefix_match = (trace_prefix_env && prefix_str == trace_prefix_env);
    const bool base_enabled = trace_all || prefix_match;
    const bool asn_match = !trace_asn.has_value() || (owner_asn_ && *owner_asn_ == *trace_asn);
    const bool trace_enabled = base_enabled && asn_match;

    auto traceReceive = [&](const std::string& label, const Announcement& announcement) {
        if (!trace_enabled) {
            return;
        }
        std::cerr << "[BGP::receive] ";
        if (owner_asn_) {
            std::cerr << "asn=" << *owner_asn_ << " ";
        }
        std::cerr << prefix_str << " " << label
                  << " | rel=" << relationshipToString(announcement.getReceivedFrom())
                  << " len=" << announcement.getASPath().size()
                  << " next_hop=" << announcement.getNextHop()
                  << (announcement.isROVInvalid() ? " rov=invalid" : " rov=valid")
                  << " | " << announcement.toString() << "\n";
    };

    traceReceive("incoming", ann);

    auto& queue = received_queue_[prefix];

    bool replaced = false;
    bool replacedBest = false;
    Announcement* stored = nullptr;

    auto bestIt = local_rib_.find(prefix);
    bool hadBest = (bestIt != local_rib_.end());
    Announcement currentBestSnapshot;
    if (hadBest) {
        currentBestSnapshot = bestIt->second;
    }

    for (auto& existing : queue) {
        if (existing.getNextHop() == ann.getNextHop()) {
            existing = ann;
            stored = &existing;
            replaced = true;
            if (hadBest && currentBestSnapshot.getNextHop() == ann.getNextHop()) {
                replacedBest = true;
            }
            break;
        }
    }

    if (!stored) {
        queue.push_back(ann);
        stored = &queue.back();
    }

    const Announcement& candidate = *stored;

    if (!hadBest) {
        local_rib_[prefix] = candidate;
        traceReceive("install", candidate);
        return;
    }

    Announcement& currentBest = bestIt->second;
    if (isBetterAnnouncement(candidate, currentBest)) {
        currentBest = candidate;
        traceReceive("promote", candidate);
        return;
    }

    if (replaced && replacedBest) {
        auto best = selectBestRoute(prefix);
        if (best) {
            currentBest = *best;
            traceReceive("reselect", currentBest);
        } else {
            local_rib_.erase(bestIt);
        }
    }
}

std::optional<Announcement> BGP::getBestAnnouncement(const IPPrefix& prefix) const {
    auto it = local_rib_.find(prefix);
    if (it != local_rib_.end()) {
        return it->second;
    }
    return std::nullopt;
}

const Announcement* BGP::findAnnouncement(const IPPrefix& prefix) const {
    auto it = local_rib_.find(prefix);
    return (it != local_rib_.end()) ? &it->second : nullptr;
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

    // Optional trace logging controlled by environment variables
    static const char* trace_prefix_env = std::getenv("BGP_TRACE_PREFIX");
    static const bool trace_all = std::getenv("BGP_TRACE_ALL") != nullptr;
    static const std::optional<uint32_t> trace_asn = []() -> std::optional<uint32_t> {
        const char* env = std::getenv("BGP_TRACE_ASN");
        if (!env || *env == '\0') {
            return std::nullopt;
        }
        char* end = nullptr;
        errno = 0;
        unsigned long value = std::strtoul(env, &end, 10);
        if (errno != 0 || end == env || value > std::numeric_limits<uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(value);
    }();
    const std::string& prefix_str = prefix.toString();
    const bool prefix_match = (trace_prefix_env && prefix_str == trace_prefix_env);
    const bool base_enabled = trace_all || prefix_match;
    const bool asn_match = !trace_asn.has_value() || (owner_asn_ && *owner_asn_ == *trace_asn);
    const bool trace_enabled = base_enabled && asn_match;

    auto traceCandidate = [&](const std::string& label, const Announcement& ann) {
        if (!trace_enabled) {
            return;
        }
        std::cerr << "[BGP::select] ";
        if (owner_asn_) {
            std::cerr << "asn=" << *owner_asn_ << " ";
        }
        std::cerr << prefix_str << " " << label
                  << " | rel=" << relationshipToString(ann.getReceivedFrom())
                  << " len=" << ann.getASPath().size()
                  << " origin=" << ann.getOriginASN()
                  << " next_hop=" << ann.getNextHop()
                  << (ann.isROVInvalid() ? " rov=invalid" : " rov=valid")
                  << " | " << ann.toString() << "\n";
    };

    if (trace_enabled) {
        std::cerr << "[BGP::select] evaluating prefix " << prefix_str
                  << " with " << announcements.size() << " candidates" << "\n";
        for (const auto& ann : announcements) {
            traceCandidate("candidate", ann);
        }
    }
    
    // Start with first announcement as best
    const Announcement* best = &announcements[0];
    traceCandidate("initial-best", *best);

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
            traceCandidate("promote-origin", candidate);
            best = &candidate;
            continue;
        } else if (best_rel == RelationshipType::ORIGIN && cand_rel != RelationshipType::ORIGIN) {
            traceCandidate("keep-origin", *best);
            continue;  // Keep ORIGIN as best
        }
        
        // For non-ORIGIN: FROM_CUSTOMER (0) > FROM_PEER (1) > FROM_PROVIDER (2)
        // Lower enum value = better relationship
        if (cand_rel != RelationshipType::ORIGIN && best_rel != RelationshipType::ORIGIN) {
            if (static_cast<int>(cand_rel) < static_cast<int>(best_rel)) {
                traceCandidate("better-relationship", candidate);
                best = &candidate;
                continue;
            } else if (static_cast<int>(cand_rel) > static_cast<int>(best_rel)) {
                traceCandidate("worse-relationship", candidate);
                continue;  // Keep current best
            }
        }
        
        // Step 2: Same relationship - prefer shorter AS-Path
        size_t best_len = best->getASPath().size();
        size_t cand_len = candidate.getASPath().size();
        
        if (cand_len < best_len) {
            traceCandidate("shorter-path", candidate);
            best = &candidate;
            continue;
        } else if (cand_len > best_len) {
            traceCandidate("longer-path", candidate);
            continue;  // Keep current best
        }
        
        // Step 3: Same path length - prefer lower next_hop ASN
        uint32_t best_nexthop = best->getNextHop();
        uint32_t cand_nexthop = candidate.getNextHop();
        
        if (cand_nexthop < best_nexthop) {
            traceCandidate("lower-next-hop", candidate);
            best = &candidate;
            continue;
        } else if (cand_nexthop > best_nexthop) {
            traceCandidate("higher-next-hop", candidate);
            continue;  // Keep current best
        }

        // Step 4: Complete tie - keep first seen (already in best)
        // This provides stability - prevents route flapping
    }
    if (trace_enabled) {
        traceCandidate("final-best", *best);
    }

    return *best;
}
