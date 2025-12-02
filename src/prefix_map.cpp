#include "prefix_map.h"
#include <stdexcept>

namespace PrefixMap {

// Global storage
std::vector<std::string> id_to_string;
std::unordered_map<std::string, uint16_t> string_to_id;

uint16_t getPrefixId(const std::string& prefix) {
    auto it = string_to_id.find(prefix);
    if (it != string_to_id.end()) {
        return it->second;
    }
    
    // Create new ID
    uint16_t id = static_cast<uint16_t>(id_to_string.size());
    id_to_string.push_back(prefix);
    string_to_id[prefix] = id;
    return id;
}

const std::string& getPrefixString(uint16_t id) {
    if (id >= id_to_string.size()) {
        throw std::out_of_range("Invalid prefix ID");
    }
    return id_to_string[id];
}

void reset() {
    id_to_string.clear();
    string_to_id.clear();
}

size_t size() {
    return id_to_string.size();
}

} // namespace PrefixMap
