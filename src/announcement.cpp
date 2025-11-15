#include "announcement.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>

// ============================================================================
// IPPrefix Implementation
// ============================================================================

/**
 * Parse an IP prefix string into components
 * 
 * Format: "address/prefix_length"
 * Examples:
 * - "8.8.8.0/24" → address="8.8.8.0", prefix_length=24
 * - "2001:4860::/32" → address="2001:4860::", prefix_length=32
 * 
 * Performance: O(n) where n = string length (typically < 50 chars)
 */
void IPPrefix::parse() {
    // Find the '/' separator
    size_t slash_pos = prefix_.find('/');
    
    if (slash_pos == std::string::npos) {
        throw std::invalid_argument("Invalid prefix format: missing '/' separator");
    }
    
    // Extract address part (before '/')
    address_ = prefix_.substr(0, slash_pos);
    
    // Extract and parse prefix length (after '/')
    std::string length_str = prefix_.substr(slash_pos + 1);
    try {
        prefix_length_ = static_cast<uint8_t>(std::stoi(length_str));
    } catch (const std::exception&) {
        throw std::invalid_argument("Invalid prefix length: " + length_str);
    }
    
    // Validate prefix length based on IP version
    if (isIPv4() && prefix_length_ > 32) {
        throw std::invalid_argument("IPv4 prefix length cannot exceed 32");
    }
    if (isIPv6() && prefix_length_ > 128) {
        throw std::invalid_argument("IPv6 prefix length cannot exceed 128");
    }
}

IPPrefix::IPPrefix(const std::string& prefix) 
    : prefix_(prefix), prefix_length_(0) {
    parse();
}

/**
 * Detect if this is an IPv4 prefix
 * 
 * Simple heuristic: IPv4 addresses contain dots ('.')
 * More robust: could parse and validate octets
 * 
 * Performance: O(n) where n = address length
 */
bool IPPrefix::isIPv4() const {
    return address_.find('.') != std::string::npos;
}

/**
 * Detect if this is an IPv6 prefix
 * 
 * Simple heuristic: IPv6 addresses contain colons (':')
 * More robust: could parse and validate hex groups
 * 
 * Performance: O(n) where n = address length
 */
bool IPPrefix::isIPv6() const {
    return address_.find(':') != std::string::npos;
}

// ============================================================================
// Announcement Implementation
// ============================================================================

/**
 * Construct an originating announcement
 * 
 * This is used when an AS first announces a prefix it owns.
 * Example: Google AS 15169 announces "8.8.8.0/24"
 * - AS-Path: [15169]
 * - Next Hop: 15169
 * - Received From: ORIGIN
 * 
 * Performance: O(1)
 */
Announcement::Announcement(const IPPrefix& prefix, uint32_t origin_asn)
    : prefix_(prefix),
      as_path_({origin_asn}),  // Initialize vector with single element
      next_hop_(origin_asn),
      received_from_(RelationshipType::ORIGIN) {
}

/**
 * Construct an announcement with full details
 * 
 * Used when creating announcements during propagation.
 * 
 * Performance: O(n) where n = AS-Path length (due to vector copy)
 * Optimization: could use move semantics if as_path is temporary
 */
Announcement::Announcement(const IPPrefix& prefix,
                         const std::vector<uint32_t>& as_path,
                         uint32_t next_hop,
                         RelationshipType received_from)
    : prefix_(prefix),
      as_path_(as_path),
      next_hop_(next_hop),
      received_from_(received_from) {
}

/**
 * Get the origin ASN (last ASN in the path)
 * 
 * The origin is the AS that first announced this prefix.
 * It's always at the end of the AS-Path.
 * 
 * Why at the end?
 * - Each AS prepends itself to the front
 * - So the origin stays at the back
 * - Example progression: [15169] → [3356,15169] → [701,3356,15169]
 * 
 * Performance: O(1)
 */
uint32_t Announcement::getOriginASN() const {
    if (as_path_.empty()) {
        throw std::runtime_error("AS-Path is empty, no origin ASN");
    }
    return as_path_.back();
}

/**
 * Check if an ASN is in the path (loop detection)
 * 
 * Used to prevent routing loops:
 * - If you receive an announcement with your ASN already in the path,
 *   reject it (accepting would create a loop)
 * 
 * Performance: O(n) where n = AS-Path length
 * - Typical path length: 3-5 ASes
 * - Worst case (rare): 20+ ASes
 * 
 * Optimization ideas:
 * - Could use std::unordered_set for O(1) lookup if paths get very long
 * - But linear search is fine for typical path lengths
 */
bool Announcement::containsASN(uint32_t asn) const {
    return std::find(as_path_.begin(), as_path_.end(), asn) != as_path_.end();
}

/**
 * Prepend this AS to the path and create a new announcement
 * 
 * This is the core of BGP path construction:
 * 1. Copy the existing path
 * 2. Insert my ASN at the front
 * 3. Update next hop and relationship
 * 4. Return new announcement
 * 
 * Example:
 * Original: prefix=8.8.8.0/24, path=[3356,15169], next_hop=3356
 * After prepend(701): path=[701,3356,15169], next_hop=701
 * 
 * Performance: O(n) where n = AS-Path length
 * - Vector copy: O(n)
 * - Insert at front: O(n) due to shifting elements
 * - Could optimize with std::deque if prepending is frequent
 * 
 * Why return new announcement instead of modifying?
 * - Immutability: safer, easier to reason about
 * - Can keep original announcement if needed
 * - Prevents accidental modification bugs
 */
Announcement Announcement::prependASN(uint32_t my_asn,
                                     uint32_t new_next_hop,
                                     RelationshipType new_relationship) const {
    // Create new AS-Path with my ASN prepended
    std::vector<uint32_t> new_path;
    new_path.reserve(as_path_.size() + 1);  // Pre-allocate to avoid reallocation
    
    new_path.push_back(my_asn);  // Add my ASN at the front
    
    // Copy the rest of the path
    new_path.insert(new_path.end(), as_path_.begin(), as_path_.end());
    
    // Create and return new announcement
    return Announcement(prefix_, new_path, new_next_hop, new_relationship);
}

/**
 * Convert announcement to human-readable string
 * 
 * Format: "prefix via [AS-Path] from relationship"
 * Example: "8.8.8.0/24 via [701, 3356, 15169] from customer"
 * 
 * Used for debugging and logging.
 * 
 * Performance: O(n) where n = AS-Path length
 */
std::string Announcement::toString() const {
    std::ostringstream oss;
    
    // Prefix
    oss << prefix_.toString();
    
    // AS-Path
    oss << " via [";
    for (size_t i = 0; i < as_path_.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << as_path_[i];
    }
    oss << "]";
    
    // Relationship
    oss << " from " << relationshipToString(received_from_);
    
    // Next hop
    oss << " (next_hop=" << next_hop_ << ")";
    
    return oss.str();
}

/**
 * Compare two announcements for equality
 * 
 * Two announcements are equal if they have:
 * - Same prefix
 * - Same AS-Path
 * - Same next hop
 * - Same relationship
 * 
 * Performance: O(n) where n = AS-Path length
 */
bool Announcement::operator==(const Announcement& other) const {
    return prefix_ == other.prefix_ &&
           as_path_ == other.as_path_ &&
           next_hop_ == other.next_hop_ &&
           received_from_ == other.received_from_;
}

/**
 * Convert RelationshipType enum to string
 * 
 * Used for debugging and logging.
 * 
 * Performance: O(1)
 */
std::string relationshipToString(RelationshipType type) {
    switch (type) {
        case RelationshipType::FROM_CUSTOMER:
            return "customer";
        case RelationshipType::FROM_PEER:
            return "peer";
        case RelationshipType::FROM_PROVIDER:
            return "provider";
        case RelationshipType::ORIGIN:
            return "origin";
        default:
            return "unknown";
    }
}
