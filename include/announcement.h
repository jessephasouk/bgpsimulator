#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

/**
 * @brief Relationship type for BGP announcements
 * 
 * Represents where an announcement was received from:
 * - FROM_CUSTOMER: Announcement came from a customer AS
 * - FROM_PEER: Announcement came from a peer AS
 * - FROM_PROVIDER: Announcement came from a provider AS
 * - ORIGIN: This is the originating announcement (not received from anyone)
 * 
 * Why does this matter?
 * BGP route selection follows the Gao-Rexford model:
 * - Prefer customer routes (you get paid!)
 * - Then peer routes (free)
 * - Then provider routes (you pay)
 */
enum class RelationshipType {
    FROM_CUSTOMER,
    FROM_PEER,
    FROM_PROVIDER,
    ORIGIN  // For announcements originated by this AS
};

/**
 * @brief Represents an IP prefix (IPv4 or IPv6)
 * 
 * Format examples:
 * - IPv4: "8.8.8.0/24" (Google DNS range)
 * - IPv6: "2001:4860:4860::/48" (Google DNS IPv6 range)
 * 
 * The /24 means the first 24 bits are the network prefix,
 * leaving 8 bits (256 addresses) for hosts.
 * 
 * Design decisions:
 * - Store as string for simplicity and debugging
 * - Could optimize later with binary representation if needed
 * - Supports both IPv4 (32-bit) and IPv6 (128-bit) addresses
 */
class IPPrefix {
public:
    /**
     * @brief Construct an IP prefix from string representation
     * @param prefix String like "8.8.8.0/24" or "2001:4860::/32"
     * 
     * Examples:
     * - "192.168.1.0/24" = 192.168.1.0 through 192.168.1.255
     * - "10.0.0.0/8" = 10.0.0.0 through 10.255.255.255
     * - "2001:db8::/32" = IPv6 range
     */
    explicit IPPrefix(const std::string& prefix);

    // Default constructor for empty prefix
    IPPrefix() = default;

    // Getters
    const std::string& toString() const { return prefix_; }
    bool isIPv4() const;
    bool isIPv6() const;
    uint8_t getPrefixLength() const { return prefix_length_; }
    const std::string& getAddress() const { return address_; }

    // Comparison operators (needed for using as map keys)
    bool operator==(const IPPrefix& other) const { return prefix_ == other.prefix_; }
    bool operator!=(const IPPrefix& other) const { return prefix_ != other.prefix_; }
    bool operator<(const IPPrefix& other) const { return prefix_ < other.prefix_; }

private:
    std::string prefix_;         // Full prefix string "8.8.8.0/24"
    std::string address_;        // Just the address part "8.8.8.0"
    uint8_t prefix_length_;      // Just the length part: 24
    
    void parse();  // Parse prefix string into components
};

/**
 * @brief Represents a BGP announcement
 * 
 * A BGP announcement contains all the information needed to route to a prefix:
 * 
 * 1. Prefix: The IP address range being announced (e.g., "8.8.8.0/24")
 * 2. AS-Path: The sequence of ASes the announcement has traversed
 *    - Starts with just the origin AS
 *    - Each AS prepends itself to the path
 *    - Example: [15169] → [3356, 15169] → [701, 3356, 15169]
 *    - Used for loop detection and path preference
 * 
 * 3. Next Hop: The ASN where this announcement came from
 *    - Used to determine which neighbor sent this route
 *    - Important for forwarding decisions
 * 
 * 4. Received From: The relationship type (customer/peer/provider)
 *    - Affects route preference (prefer customer > peer > provider)
 *    - Affects export policy (don't announce provider routes to peers)
 * 
 * Memory usage per announcement:
 * - IPPrefix: ~32 bytes (string overhead + data)
 * - AS-Path: ~8 bytes overhead + (4 bytes × path length)
 * - Next hop: 4 bytes
 * - Relationship: 4 bytes (enum)
 * - Total: ~50 bytes + (4 bytes × AS-Path length)
 * - For typical path length of 5: ~70 bytes per announcement
 */
class Announcement {
public:
    /**
     * @brief Construct an announcement (typically for origination)
     * @param prefix The IP prefix being announced
     * @param origin_asn The ASN originating this announcement
     * 
     * Creates an announcement with:
     * - AS-Path: [origin_asn]
     * - Next Hop: origin_asn
     * - Relationship: ORIGIN
     */
    Announcement(const IPPrefix& prefix, uint32_t origin_asn);

    /**
     * @brief Construct an announcement with full details (for propagation)
     * @param prefix The IP prefix
     * @param as_path The sequence of ASNs traversed
     * @param next_hop The ASN this announcement came from
     * @param received_from The relationship type
     */
    Announcement(const IPPrefix& prefix,
                 const std::vector<uint32_t>& as_path,
                 uint32_t next_hop,
                 RelationshipType received_from);

    // Default constructor
    Announcement() = default;

    // Getters
    const IPPrefix& getPrefix() const { return prefix_; }
    const std::vector<uint32_t>& getASPath() const { return as_path_; }
    uint32_t getNextHop() const { return next_hop_; }
    RelationshipType getReceivedFrom() const { return received_from_; }

    /**
     * @brief Get the origin ASN (last ASN in the path)
     * @return The ASN that originated this prefix
     * 
     * The origin is always the last ASN in the AS-Path.
     * Example: for path [701, 3356, 15169], origin is 15169
     */
    uint32_t getOriginASN() const;

    /**
     * @brief Get the path length
     * @return Number of ASes in the AS-Path
     * 
     * Shorter paths are generally preferred in BGP route selection.
     */
    size_t getPathLength() const { return as_path_.size(); }

    /**
     * @brief Check if this AS is in the path (for loop detection)
     * @param asn The ASN to check
     * @return true if ASN is in the AS-Path
     * 
     * BGP loop prevention: never accept an announcement that already
     * contains your own ASN in the path (would create routing loop).
     */
    bool containsASN(uint32_t asn) const;

    /**
     * @brief Create a new announcement with this AS prepended to the path
     * @param my_asn The ASN to prepend
     * @param new_next_hop The new next hop
     * @param new_relationship The new relationship type
     * @return A new Announcement with updated AS-Path
     * 
     * When an AS receives and forwards an announcement, it:
     * 1. Prepends its own ASN to the AS-Path
     * 2. Updates the next hop to itself
     * 3. Updates the relationship based on who it's sending to
     * 
     * Example:
     * - Received: prefix=8.8.8.0/24, path=[15169], next_hop=15169
     * - After prepend(3356): path=[3356, 15169], next_hop=3356
     */
    Announcement prependASN(uint32_t my_asn,
                           uint32_t new_next_hop,
                           RelationshipType new_relationship) const;

    /**
     * @brief Convert announcement to string for debugging
     * @return Human-readable representation
     * 
     * Example: "8.8.8.0/24 via [3356, 15169] from customer"
     */
    std::string toString() const;

    // Comparison operators
    bool operator==(const Announcement& other) const;
    bool operator!=(const Announcement& other) const { return !(*this == other); }

private:
    IPPrefix prefix_;                    // The IP prefix being announced
    std::vector<uint32_t> as_path_;      // Sequence of ASNs (path traversed)
    uint32_t next_hop_;                  // ASN where this came from
    RelationshipType received_from_;     // Customer/Peer/Provider relationship
};

/**
 * @brief Convert RelationshipType to string for debugging
 */
std::string relationshipToString(RelationshipType type);
