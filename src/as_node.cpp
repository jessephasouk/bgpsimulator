#include "as_node.h"
#include "bgp.h"
#include <unordered_map>
#include <optional>
#include <iostream>
#include <algorithm>

/**
 * Constructor: Initialize an AS node with its unique identifier
 * 
 * Performance: O(1)
 * - Member initializer list (efficient)
 * - Sets initialize empty (no allocation until first insert)
 * 
 * Memory: ~130 bytes per node (with typical 10 relationships)
 * - 4 bytes: ASN
 * - ~48 bytes: 3 empty std::set overhead
 * - ~8 bytes per relationship stored
 */
ASNode::ASNode(uint32_t asn) : asn_(asn) {}

void ASNode::setPolicy(std::unique_ptr<Policy> policy) {
    if (!policy) {
        policy_.reset();
        return;
    }

    if (auto* bgp_policy = dynamic_cast<BGP*>(policy.get())) {
        bgp_policy->setOwnerASN(asn_);
    }

    policy_ = std::move(policy);
}

/**
 * Add a provider relationship
 * 
 * Performance: O(log k) where k = number of existing providers
 * - std::set::insert is O(log k)
 * - Typical AS has 1-5 providers → log k ≈ 2-3 comparisons
 * - Null check is O(1) safety measure
 * 
 * Why null check?
 * - Prevents crashes if caller passes nullptr
 * - Defensive programming (better safe than sorry)
 * - Nearly zero cost (pointer comparison)
 * 
 * BGP Context:
 * - Provider = upstream AS that gives you Internet connectivity
 * - Most ASes have 1-2 providers (redundancy)
 * - Tier-1 ISPs have 0 providers (they ARE the Internet backbone)
 */
void ASNode::addProvider(ASNode* provider) {
    if (provider) {
        providers_.push_back(provider);
    }
}

/**
 * Add a customer relationship
 * 
 * Performance: O(log k) where k = number of existing customers
 * - std::set::insert is O(log k)
 * - Large ISPs can have thousands of customers
 * - Even with 10,000 customers: log k ≈ 13 comparisons (very fast!)
 * 
 * BGP Context:
 * - Customer = downstream AS that you provide connectivity to
 * - Tier-1/Tier-2 ISPs have many customers
 * - Small companies/universities have 0 customers
 * 
 * Why std::set doesn't slow down with many customers?
 * - Balanced binary tree (Red-Black tree typically)
 * - Insertion stays O(log k) even with 1000s of customers
 * - Alternative: vector would be O(1) insert but O(n) lookup
 */
void ASNode::addCustomer(ASNode* customer) {
    if (customer) {
        customers_.push_back(customer);
    }
}

/**
 * Add a peer relationship
 * 
 * Performance: O(log k) where k = number of existing peers
 * - std::set::insert is O(log k)
 * - Typical AS has 5-20 peers
 * - Internet Exchange Points (IXPs) enable many peers
 * 
 * BGP Context:
 * - Peer = lateral relationship (settlement-free peering)
 * - "I'll route your traffic if you route mine"
 * - Common at Internet Exchange Points (IXPs)
 * - Major ISPs often peer with each other
 * 
 * Why peers matter for BGP:
 * - Peer routes preferred over provider routes (saves money)
 * - Peering reduces latency (direct connection)
 * - BGP policy: prefer customer > peer > provider
 */
void ASNode::addPeer(ASNode* peer) {
    if (peer) {
        peers_.push_back(peer);
    }
}

/**
 * Seed this AS with an origin announcement
 * 
 * Creates an announcement for a prefix that this AS owns/originates.
 * This is how routes enter the BGP system - someone has to own the prefix!
 * 
 * Performance: O(1) - just creates announcement and stores in policy
 * 
 * BGP Context:
 * - Origin announcements have highest preference (ORIGIN > customer > peer > provider)
 * - AS-Path starts with just this AS: [this_asn]
 * - Next hop is this AS (we are the source)
 * 
 * Example:
 *   Google (AS15169) announces 8.8.8.0/24:
 *   - AS-Path: [15169]
 *   - Next hop: 15169
 *   - Relationship: ORIGIN
 * 
 * Why ORIGIN is preferred most?
 * - This AS literally owns the IP space
 * - Most trustworthy source for this prefix
 * - Any other route is a longer path to get here
 */
void ASNode::seedAnnouncement(const IPPrefix& prefix, bool rov_invalid) {
    // Ensure this AS has a policy (create BGP if not set)
    if (!policy_) {
        setPolicy(std::make_unique<BGP>());
    }
    
    // Create origin announcement
    // AS-Path contains just this AS
    std::vector<uint32_t> as_path = {asn_};
    
    // Create the announcement with ORIGIN relationship (highest preference)
    // Include rov_invalid flag for ROV testing (hijack simulation)
    Announcement origin_announcement(
        prefix,
        as_path,
        asn_,  // next_hop is this AS
        RelationshipType::ORIGIN,
        rov_invalid  // Mark as invalid if this is a hijack
    );
    
    // Store in local RIB via policy
    policy_->receiveAnnouncement(origin_announcement);
}

/**
 * Add an announcement to the received queue
 * 
 * This doesn't process the announcement immediately - just stores it.
 * Processing happens later in processReceivedQueue() after all ASes have sent.
 * 
 * Why delay processing?
 * - Prevents announcements from traveling multiple hops in one step
 * - Example: If we processed immediately during peer propagation:
 *   AS1 → AS2 (processed) → AS3 (processed) = 2 hops in one step!
 * - By queuing: AS1 → AS2 queue, AS1 → AS3 queue, THEN process all
 */
void ASNode::addToReceivedQueue(const Announcement& ann) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    received_queue_.push_back(ann);
}

/**
 * Process all announcements in the received queue
 * 
 * For each announcement:
 * 1. Check for loops (reject if we're already in AS-Path)
 * 2. Prepend our ASN to the AS-Path
 * 3. Update next_hop to this AS
 * 4. Store in local RIB via policy
 * 
 * Performance: O(k * m) where k = queue size, m = avg AS-Path length
 * - Loop detection: O(m) for each announcement
 * - Prepend: O(m) to copy path + 1 element
 * - Policy store: O(1) average
 * 
 * BGP Context:
 * - Loop prevention: Never accept announcement containing our ASN
 * - AS-Path prepending: Each AS adds itself to show path taken
 * - Route selection: Policy chooses best among all received
 */
void ASNode::processReceivedQueue() {
    if (!policy_) {
        setPolicy(std::make_unique<BGP>());
    }

    if (received_queue_.empty()) {
        return;
    }

    processReceivedQueueWithDiff();
}

std::vector<IPPrefix> ASNode::processReceivedQueueWithDiff() {
    if (received_queue_.empty()) {
        return {};
    }

    // Ensure policy exists before querying current best routes
    if (!policy_) {
        setPolicy(std::make_unique<BGP>());
    }

    std::unordered_map<IPPrefix, std::optional<Announcement>> previous_best;
    previous_best.reserve(received_queue_.size());
    std::vector<IPPrefix> touched_prefixes;
    touched_prefixes.reserve(received_queue_.size());

    for (const Announcement& ann : received_queue_) {
        const IPPrefix& prefix = ann.getPrefix();
        if (previous_best.find(prefix) == previous_best.end()) {
            previous_best.emplace(prefix, policy_->getBestAnnouncement(prefix));
            touched_prefixes.push_back(prefix);
        }
    }

    for (const Announcement& ann : received_queue_) {
        if (ann.containsASN(asn_)) {
            continue;
        }

        Announcement processed = ann.prependASN(
            asn_,
            ann.getNextHop(),
            ann.getReceivedFrom()
        );

        policy_->receiveAnnouncement(processed);
    }

    received_queue_.clear();

    std::vector<IPPrefix> changed_prefixes;
    changed_prefixes.reserve(touched_prefixes.size());

    for (const IPPrefix& prefix : touched_prefixes) {
        auto it = previous_best.find(prefix);
        std::optional<Announcement> before = (it != previous_best.end()) ? it->second : std::nullopt;
        std::optional<Announcement> after = policy_->getBestAnnouncement(prefix);

        bool changed = false;
        if (!before && after) {
            changed = true;
        } else if (before && !after) {
            changed = true;
        } else if (before && after && *before != *after) {
            changed = true;
        }

        if (changed) {
            changed_prefixes.push_back(prefix);
        }
    }

    return changed_prefixes;
}

/**
 * Send all announcements in local RIB to providers
 * 
 * For each route we have, send it to our providers.
 * They will see it as coming FROM_CUSTOMER (we are their customer).
 * 
 * BGP Context:
 * - Only send routes we originated or learned from customers
 * - Providers prefer customer routes (they make money from us)
 * - This is how announcements propagate "up" the hierarchy
 * 
 * Performance: O(p * r) where p = providers, r = routes in RIB
 */
void ASNode::sendToProviders() {
    if (!policy_) return;

    auto prefixes = getLocalRIBPrefixes();
    sendToProviders(prefixes);
}

void ASNode::sendToProviders(const std::vector<IPPrefix>& prefixes) {
    if (!policy_ || prefixes.empty()) {
        return;
    }

    for (const IPPrefix& prefix : prefixes) {
        const Announcement* bestPtr = nullptr;
        std::optional<Announcement> fallback;
        if (auto* bgp = dynamic_cast<BGP*>(policy_.get())) {
            bestPtr = bgp->findAnnouncement(prefix);
        }
        if (!bestPtr) {
            fallback = policy_->getBestAnnouncement(prefix);
            if (!fallback) {
                continue;
            }
            bestPtr = &fallback.value();
        }

        const Announcement& best = *bestPtr;

        // Assignment spec: "send those announcements to their providers"
        // No export policy filtering required - send ALL announcements from RIB

        Announcement to_send(
            best.getPrefix(),
            best.getASPath(),
            asn_,
            RelationshipType::FROM_CUSTOMER,
            best.isROVInvalid()
        );

        for (const auto& provider : providers_) {
            provider->addToReceivedQueue(to_send);
        }
    }
}

/**
 * Send all announcements in local RIB to customers
 * 
 * For each route we have, send it to our customers.
 * They will see it as coming FROM_PROVIDER (we are their provider).
 * 
 * BGP Context:
 * - Always send everything to customers (they pay us for connectivity)
 * - Customers accept provider routes (they need Internet access)
 * - This is how announcements propagate "down" the hierarchy
 * 
 * Performance: O(c * r) where c = customers, r = routes in RIB
 */
void ASNode::sendToCustomers() {
    if (!policy_) return;

    auto prefixes = getLocalRIBPrefixes();
    sendToCustomers(prefixes);
}

void ASNode::sendToCustomers(const std::vector<IPPrefix>& prefixes) {
    if (!policy_ || prefixes.empty()) {
        return;
    }

    for (const IPPrefix& prefix : prefixes) {
        const Announcement* bestPtr = nullptr;
        std::optional<Announcement> fallback;
        if (auto* bgp = dynamic_cast<BGP*>(policy_.get())) {
            bestPtr = bgp->findAnnouncement(prefix);
        }
        if (!bestPtr) {
            fallback = policy_->getBestAnnouncement(prefix);
            if (!fallback) {
                continue;
            }
            bestPtr = &fallback.value();
        }

        const Announcement& best = *bestPtr;
        
        Announcement to_send(
            best.getPrefix(),
            best.getASPath(),
            asn_,
            RelationshipType::FROM_PROVIDER,
            best.isROVInvalid()
        );
        
        for (const auto& customer : customers_) {
            customer->addToReceivedQueue(to_send);
        }
    }
}

/**
 * Send announcements to peers
 * 
 * Per assignment specification: "send all announcements" without filtering.
 * All routes in the local RIB are sent to all peers.
 * 
 * Performance: O(p * r) where p = peers, r = routes in RIB
 */
void ASNode::sendToPeers() {
    if (!policy_) return;

    auto prefixes = getLocalRIBPrefixes();
    sendToPeers(prefixes);
}

void ASNode::sendToPeers(const std::vector<IPPrefix>& prefixes) {
    if (!policy_ || prefixes.empty()) {
        return;
    }

    for (const IPPrefix& prefix : prefixes) {
        const Announcement* bestPtr = nullptr;
        std::optional<Announcement> fallback;
        if (auto* bgp = dynamic_cast<BGP*>(policy_.get())) {
            bestPtr = bgp->findAnnouncement(prefix);
        }
        if (!bestPtr) {
            fallback = policy_->getBestAnnouncement(prefix);
            if (!fallback) {
                continue;
            }
            bestPtr = &fallback.value();
        }

        const Announcement& best = *bestPtr;

        // Assignment spec: send announcements to peers (one hop only)
        // No export policy filtering required - send ALL announcements from RIB
        
        Announcement to_send(
            best.getPrefix(),
            best.getASPath(),
            asn_,
            RelationshipType::FROM_PEER,
            best.isROVInvalid()
        );
        
        for (const auto& peer : peers_) {
            peer->addToReceivedQueue(to_send);
        }
    }
}

/**
 * Get all prefixes in the local RIB
 * 
 * Returns a list of all prefixes this AS currently has routes for.
 * Used when sending announcements to neighbors.
 * 
 * Performance: O(r) where r = routes in local RIB
 */
std::vector<IPPrefix> ASNode::getLocalRIBPrefixes() const {
    if (!policy_) return {};  // No policy = no routes
    
    // Get the BGP policy (downcast from Policy*)
    BGP* bgp = dynamic_cast<BGP*>(policy_.get());
    if (!bgp) return {};  // Not a BGP policy (shouldn't happen)
    
    // Get all prefixes from BGP's local RIB
    return bgp->getLocalRIBPrefixes();
}

