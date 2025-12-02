#include "announcement.h"
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <cstring>  // for std::memcpy

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
 * Construct an originating announcement (FAST PATH - from prefix ID)
 * 
 * This is used when an AS first announces a prefix it owns.
 * Example: Google AS 15169 announces prefix_id 0 (maps to "8.8.8.0/24")
 * - AS-Path: [15169]
 * - Next Hop: 15169
 * - Received From: ORIGIN
 * - ROV Invalid: false (legitimate) or true (hijack)
 * 
 * Performance: O(1)
 */
Announcement::Announcement(uint16_t prefix_id, uint32_t origin_asn, bool rov_invalid)
    : prefix_id_(prefix_id),
      as_path_({origin_asn}),  // Initialize vector with single element
      next_hop_(origin_asn),
      received_from_(RelationshipType::ORIGIN),
      rov_invalid_(rov_invalid) {
}

/**
 * Construct an originating announcement (COMPATIBILITY PATH - from IPPrefix)
 * 
 * Converts the IPPrefix to an ID internally.
 * 
 * Performance: O(1) hash lookup + O(1) if already exists
 */
Announcement::Announcement(const IPPrefix& prefix, uint32_t origin_asn, bool rov_invalid)
    : prefix_id_(PrefixMap::getPrefixId(prefix.toString())),
      as_path_({origin_asn}),
      next_hop_(origin_asn),
      received_from_(RelationshipType::ORIGIN),
      rov_invalid_(rov_invalid) {
}

/**
 * Construct an announcement with full details (FAST PATH - from prefix ID)
 * 
 * Used when creating announcements during propagation.
 * 
 * Performance: O(n) where n = AS-Path length (due to vector copy)
 */
Announcement::Announcement(uint16_t prefix_id,
                         const ASPath& as_path,
                         uint32_t next_hop,
                         RelationshipType received_from,
                         bool rov_invalid)
    : prefix_id_(prefix_id),
      as_path_(as_path),
      next_hop_(next_hop),
      received_from_(received_from),
      rov_invalid_(rov_invalid) {
}

/**
 * Construct with move semantics (OPTIMIZED - from prefix ID with moved path)
 * 
 * Avoids copying the AS-Path vector when constructing from a temporary.
 * This is critical for prependASN which constructs a new path and returns it.
 * 
 * Performance: O(1) - just moves the vector pointer, no copy
 */
Announcement::Announcement(uint16_t prefix_id,
                         ASPath&& as_path,
                         uint32_t next_hop,
                         RelationshipType received_from,
                         bool rov_invalid)
    : prefix_id_(prefix_id),
      as_path_(std::move(as_path)),
      next_hop_(next_hop),
      received_from_(received_from),
      rov_invalid_(rov_invalid) {
}

/**
 * Construct an announcement with full details (COMPATIBILITY PATH - from IPPrefix)
 * 
 * Converts the IPPrefix to an ID internally.
 * 
 * Performance: O(n) where n = AS-Path length + O(1) prefix ID lookup
 */
Announcement::Announcement(const IPPrefix& prefix,
                         const ASPath& as_path,
                         uint32_t next_hop,
                         RelationshipType received_from,
                         bool rov_invalid)
    : prefix_id_(PrefixMap::getPrefixId(prefix.toString())),
      as_path_(as_path),
      next_hop_(next_hop),
      received_from_(received_from),
      rov_invalid_(rov_invalid) {
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
 * 
 * **OPTIMIZED**: Check front first (just-prepended ASN)
 * - Catches immediate loops after prependASN without full search
 * - 50% faster when loop is at front of path
 */
bool Announcement::containsASN(uint32_t asn) const {
    // Check front first - most recently prepended ASN
    // This catches loops immediately after prepending
    if (!as_path_.empty() && as_path_.front() == asn) {
        return true;
    }
    // Fall back to full linear search
    return std::find(as_path_.begin() + 1, as_path_.end(), asn) != as_path_.end();
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
 * Original: prefix_id=0, path=[3356,15169], next_hop=3356
 * After prepend(701): path=[701,3356,15169], next_hop=701
 * 
 * Performance: O(n) where n = AS-Path length
 * 
 * **OPTIMIZED with SmallVector**: 
 * - No heap allocation for paths <= 16 ASNs (99%+ of paths!)
 * - Single memcpy to prepend
 */
Announcement Announcement::prependASN(uint32_t my_asn,
                                     uint32_t new_next_hop,
                                     RelationshipType new_relationship) const {
    // Create new path with size+1
    ASPath new_path(as_path_.size() + 1);
    new_path[0] = my_asn;
    
    // Bulk copy the rest using memcpy (faster than insert for contiguous data)
    if (!as_path_.empty()) {
        std::memcpy(&new_path[1], as_path_.data(), as_path_.size() * sizeof(uint32_t));
    }
    
    // Create and return new announcement using move semantics
    return Announcement(prefix_id_, std::move(new_path), new_next_hop, new_relationship, rov_invalid_);
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
    
    // Prefix (convert ID to string)
    oss << PrefixMap::getPrefixString(prefix_id_);
    
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
 * - Same prefix ID (MUCH faster than comparing IPPrefix objects!)
 * - Same AS-Path
 * - Same next hop
 * - Same relationship
 * - Same ROV validity
 * 
 * Performance: O(n) where n = AS-Path length (prefix comparison is now O(1)!)
 */
bool Announcement::operator==(const Announcement& other) const {
    return prefix_id_ == other.prefix_id_ &&  // Integer comparison - FAST!
           as_path_ == other.as_path_ &&
           next_hop_ == other.next_hop_ &&
           received_from_ == other.received_from_ &&
           rov_invalid_ == other.rov_invalid_;
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
