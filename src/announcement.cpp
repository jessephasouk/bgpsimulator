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
      path_len_(1),
      next_hop_(origin_asn),
      received_from_(RelationshipType::ORIGIN),
      rov_invalid_(rov_invalid),
      comparison_score_(0) {
    path_[0] = origin_asn;
    updateComparisonScore();
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
      path_len_(1),
      next_hop_(origin_asn),
      received_from_(RelationshipType::ORIGIN),
      rov_invalid_(rov_invalid),
      comparison_score_(0) {
    path_[0] = origin_asn;
    updateComparisonScore();
}

/**
 * Construct an announcement with full details (FAST PATH - from prefix ID)
 * 
 * Used when creating announcements during propagation.
 * 
 * Performance: O(n) where n = AS-Path length (memcpy)
 */
Announcement::Announcement(uint16_t prefix_id,
                         const uint32_t* path_data,
                         uint8_t path_len,
                         uint32_t next_hop,
                         RelationshipType received_from,
                         bool rov_invalid)
    : prefix_id_(prefix_id),
      path_len_(path_len > MAX_PATH_LEN ? MAX_PATH_LEN : path_len),
      next_hop_(next_hop),
      received_from_(received_from),
      rov_invalid_(rov_invalid),
      comparison_score_(0) {
    std::memcpy(path_, path_data, path_len_ * sizeof(uint32_t));
    updateComparisonScore();
}

/**
 * Construct an announcement with full details (COMPATIBILITY PATH - from IPPrefix)
 * 
 * Converts the IPPrefix to an ID internally.
 * 
 * Performance: O(n) where n = AS-Path length + O(1) prefix ID lookup
 */
Announcement::Announcement(const IPPrefix& prefix,
                         const uint32_t* path_data,
                         uint8_t path_len,
                         uint32_t next_hop,
                         RelationshipType received_from,
                         bool rov_invalid)
    : prefix_id_(PrefixMap::getPrefixId(prefix.toString())),
      path_len_(path_len > MAX_PATH_LEN ? MAX_PATH_LEN : path_len),
      next_hop_(next_hop),
      received_from_(received_from),
      rov_invalid_(rov_invalid),
      comparison_score_(0) {
    std::memcpy(path_, path_data, path_len_ * sizeof(uint32_t));
    updateComparisonScore();
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
    if (path_len_ == 0) {
        throw std::runtime_error("AS-Path is empty, no origin ASN");
    }
    return path_[path_len_ - 1];
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
 * - Worst case (rare): 16 ASes (max)
 * 
 * **OPTIMIZED**: Uses AVX2 SIMD when available to check 8 ASNs at once
 */
bool Announcement::containsASN(uint32_t asn) const {
    if (path_len_ == 0) return false;
    
#ifdef USE_SIMD_LOOP_DETECTION
    // SIMD for paths >= 8 elements (check first 8)
    if (path_len_ >= 8) {
        __m256i target = _mm256_set1_epi32(static_cast<int>(asn));
        __m256i path_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(path_));
        __m256i cmp = _mm256_cmpeq_epi32(path_vec, target);
        if (_mm256_movemask_epi8(cmp) != 0) return true;
        
        // Check elements 8-15 if path is that long
        if (path_len_ >= 16) {
            path_vec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(path_ + 8));
            cmp = _mm256_cmpeq_epi32(path_vec, target);
            return _mm256_movemask_epi8(cmp) != 0;
        } else {
            // Scalar for elements 8 to path_len_-1
            for (uint8_t i = 8; i < path_len_; ++i) {
                if (path_[i] == asn) return true;
            }
        }
        return false;
    }
#endif
    
    // Scalar path for short paths (most common)
    for (uint8_t i = 0; i < path_len_; ++i) {
        if (path_[i] == asn) return true;
    }
    return false;
}

/**
 * Update the pre-computed comparison score
 * Score format: (relationship << 56) | (path_len << 40) | next_hop
 */
void Announcement::updateComparisonScore() {
    uint64_t rel_val;
    switch (received_from_) {
        case RelationshipType::ORIGIN: rel_val = 0; break;
        case RelationshipType::FROM_CUSTOMER: rel_val = 1; break;
        case RelationshipType::FROM_PEER: rel_val = 2; break;
        case RelationshipType::FROM_PROVIDER: rel_val = 3; break;
        default: rel_val = 4; break;
    }
    comparison_score_ = (rel_val << 56) | 
                        (static_cast<uint64_t>(path_len_) << 40) | 
                        next_hop_;
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
 * Performance: O(1) - fixed array copy
 * 
 * **OPTIMIZED with fixed array**: 
 * - Zero heap allocation!
 * - Single memcpy to prepend
 */
Announcement Announcement::prependASN(uint32_t my_asn,
                                     uint32_t new_next_hop,
                                     RelationshipType new_relationship) const {
    // Build new path in-place
    uint32_t new_path[MAX_PATH_LEN];
    new_path[0] = my_asn;
    
    uint8_t new_len = path_len_ + 1;
    if (new_len > MAX_PATH_LEN) new_len = MAX_PATH_LEN;
    
    // Copy existing path (shift by 1)
    if (path_len_ > 0) {
        uint8_t copy_len = (path_len_ < MAX_PATH_LEN) ? path_len_ : (MAX_PATH_LEN - 1);
        std::memcpy(&new_path[1], path_, copy_len * sizeof(uint32_t));
    }
    
    return Announcement(prefix_id_, new_path, new_len, new_next_hop, new_relationship, rov_invalid_);
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
    for (uint8_t i = 0; i < path_len_; ++i) {
        if (i > 0) oss << ", ";
        oss << path_[i];
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
 * Performance: O(n) where n = AS-Path length
 */
bool Announcement::operator==(const Announcement& other) const {
    if (prefix_id_ != other.prefix_id_ ||
        path_len_ != other.path_len_ ||
        next_hop_ != other.next_hop_ ||
        received_from_ != other.received_from_ ||
        rov_invalid_ != other.rov_invalid_) {
        return false;
    }
    // Compare paths
    return std::memcmp(path_, other.path_, path_len_ * sizeof(uint32_t)) == 0;
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
