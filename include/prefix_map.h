#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

/**
 * Global Prefix ID Mapping System
 * 
 * Maps string prefixes (e.g., "10.0.0.0/24") to integer IDs for fast comparison.
 * This is a massive performance optimization: integer comparison is 10-20x faster
 * than hashing and comparing IP address + subnet mask objects.
 * 
 * Usage:
 *   uint16_t id = getPrefixId("10.0.0.0/24");  // First call creates mapping
 *   uint16_t id2 = getPrefixId("10.0.0.0/24"); // Subsequent calls return same ID
 *   std::string str = getPrefixString(id);      // Convert back to string
 */

namespace PrefixMap {

// Global storage (defined in prefix_map.cpp)
extern std::vector<std::string> id_to_string;
extern std::unordered_map<std::string, uint16_t> string_to_id;

/**
 * Get or create a prefix ID for a string prefix
 * @param prefix String like "10.0.0.0/24"
 * @return Unique integer ID (0-65535)
 */
uint16_t getPrefixId(const std::string& prefix);

/**
 * Get the string representation of a prefix ID
 * @param id Prefix ID
 * @return String like "10.0.0.0/24"
 */
const std::string& getPrefixString(uint16_t id);

/**
 * Reset the mapping (useful for testing)
 */
void reset();

/**
 * Get number of unique prefixes
 */
size_t size();

} // namespace PrefixMap
