#include "as_node.h"

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
void ASNode::addProvider(std::shared_ptr<ASNode> provider) {
    if (provider) {
        providers_.insert(provider);
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
void ASNode::addCustomer(std::shared_ptr<ASNode> customer) {
    if (customer) {
        customers_.insert(customer);
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
void ASNode::addPeer(std::shared_ptr<ASNode> peer) {
    if (peer) {
        peers_.insert(peer);
    }
}
