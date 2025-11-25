/**
 * BGP Simulator - Main Program
 * 
 * Usage: ./bgp_simulator <as_relationships_file> <announcements_file> <rov_asns_file> <output_file>
 * 
 * Input Files:
 * 1. AS Relationships: CAIDA format (ASN1|ASN2|relationship)
 * 2. Announcements CSV: asn,prefix,rov_invalid
 * 3. ROV ASNs CSV: asn (one per line)
 * 4. Output File: ribs.csv (asn,prefix,as_path)
 */

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <chrono>
#include "as_graph.h"
#include "announcement.h"

/**
 * Parse announcements CSV file
 * Format: asn,prefix,rov_invalid
 * Example: 15169,8.8.8.0/24,false
 */
struct AnnouncementRecord {
    uint32_t asn;
    std::string prefix;
    bool rov_invalid;
};

std::vector<AnnouncementRecord> parseAnnouncementsCsv(const std::string& filename) {
    std::vector<AnnouncementRecord> announcements;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open announcements file: " << filename << "\n";
        return announcements;
    }
    
    std::string line;
    bool first_line = true;
    
    while (std::getline(file, line)) {
        // Skip header line if present (check for 'asn' or 'seed_asn')
        if (first_line) {
            first_line = false;
            if (line.find("asn") != std::string::npos || 
                line.find("ASN") != std::string::npos ||
                line.find("seed_asn") != std::string::npos) {
                continue;
            }
        }
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Parse CSV: asn,prefix,rov_invalid
        std::stringstream ss(line);
        std::string asn_str, prefix_str, rov_str;
        
        if (std::getline(ss, asn_str, ',') &&
            std::getline(ss, prefix_str, ',') &&
            std::getline(ss, rov_str, ',')) {
            auto trimWhitespace = [](std::string& s) {
                s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
                s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(), s.end());
            };

            trimWhitespace(asn_str);
            trimWhitespace(prefix_str);
            trimWhitespace(rov_str);
            
            try {
                AnnouncementRecord record;
                record.asn = std::stoul(asn_str);
                record.prefix = prefix_str;
                
                // Parse boolean (true/false, 1/0, yes/no)
                std::string rov_lower = rov_str;
                std::transform(rov_lower.begin(), rov_lower.end(), rov_lower.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });

                if (rov_lower == "true" || rov_lower == "1" || rov_lower == "yes" || rov_lower == "y") {
                    record.rov_invalid = true;
                } else {
                    record.rov_invalid = false;
                }
                
                announcements.push_back(record);
            } catch (const std::exception& e) {
                std::cerr << "Warning: Skipping invalid announcement line: " << line << "\n";
            }
        }
    }
    
    file.close();
    return announcements;
}

/**
 * Parse ROV ASNs CSV file
 * Format: asn (one per line)
 * Example: 15169
 */
std::vector<uint32_t> parseRovAsnsCsv(const std::string& filename) {
    std::vector<uint32_t> rov_asns;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        std::cerr << "Error: Could not open ROV ASNs file: " << filename << "\n";
        return rov_asns;
    }
    
    std::string line;
    bool first_line = true;
    
    while (std::getline(file, line)) {
        // Skip header line if present
        if (first_line) {
            first_line = false;
            if (line.find("asn") != std::string::npos || 
                line.find("ASN") != std::string::npos) {
                continue;
            }
        }
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        
        // Parse ASN (handle comma-separated or just number)
        std::string asn_str = line;
        size_t comma_pos = line.find(',');
        if (comma_pos != std::string::npos) {
            asn_str = line.substr(0, comma_pos);
        }
        
        try {
            uint32_t asn = std::stoul(asn_str);
            rov_asns.push_back(asn);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Skipping invalid ROV ASN line: " << line << "\n";
        }
    }
    
    file.close();
    return rov_asns;
}

/**
 * Main program
 */
int main(int argc, char* argv[]) {
    // Check command line arguments
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <as_relationships_file> <announcements_file> <rov_asns_file> <output_file>\n";
        std::cerr << "\nExample:\n";
        std::cerr << "  " << argv[0] << " caida/20250901.as-rel2.txt announcements.csv rov_asns.csv ribs.csv\n";
        return 1;
    }
    
    std::string as_rel_file = argv[1];
    std::string announcements_file = argv[2];
    std::string rov_asns_file = argv[3];
    std::string output_file = argv[4];
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::cout << "=== BGP Simulator ===" << std::endl;
    std::cout << "AS Relationships: " << as_rel_file << std::endl;
    std::cout << "Announcements: " << announcements_file << std::endl;
    std::cout << "ROV ASNs: " << rov_asns_file << std::endl;
    std::cout << "Output: " << output_file << std::endl;
    std::cout << std::endl;
    
    // Step 1: Build AS topology
    std::cout << "Step 1: Loading AS topology..." << std::endl;
    ASGraph graph;
    
    if (!graph.buildFromCAIDAFile(as_rel_file)) {
        std::cerr << "Error: Failed to load AS relationships" << std::endl;
        return 1;
    }
    
    std::cout << "  Loaded " << graph.getNodeCount() << " ASes" << std::endl;
    
    // Check for cycles (will print error and return false if detected)
    // The loadFromFile already does this, so we're covered
    
    auto topo_time = std::chrono::high_resolution_clock::now();
    auto topo_duration = std::chrono::duration_cast<std::chrono::milliseconds>(topo_time - start_time);
    std::cout << "  Time: " << topo_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Step 2: Flatten graph for propagation
    std::cout << "Step 2: Flattening AS graph..." << std::endl;
    graph.flattenGraph();
    std::cout << "  Graph flattened (ranks assigned)" << std::endl;
    
    auto flatten_time = std::chrono::high_resolution_clock::now();
    auto flatten_duration = std::chrono::duration_cast<std::chrono::milliseconds>(flatten_time - topo_time);
    std::cout << "  Time: " << flatten_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Step 3: Deploy ROV
    std::cout << "Step 3: Deploying ROV..." << std::endl;
    std::vector<uint32_t> rov_asns = parseRovAsnsCsv(rov_asns_file);
    
    if (rov_asns.empty()) {
        std::cout << "  No ROV ASNs to deploy (file empty or missing)" << std::endl;
    } else {
        std::cout << "  Deploying ROV to " << rov_asns.size() << " ASes..." << std::endl;
        
        // Create temporary file with ROV ASNs
        std::ofstream temp_rov_file("temp_rov_deployment.txt");
        for (uint32_t asn : rov_asns) {
            temp_rov_file << asn << "\n";
        }
        temp_rov_file.close();
        
        graph.deployROV("temp_rov_deployment.txt");
        
        // Clean up temp file
        std::remove("temp_rov_deployment.txt");
    }
    
    auto rov_time = std::chrono::high_resolution_clock::now();
    auto rov_duration = std::chrono::duration_cast<std::chrono::milliseconds>(rov_time - flatten_time);
    std::cout << "  Time: " << rov_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Step 4: Load and seed announcements
    std::cout << "Step 4: Loading announcements..." << std::endl;
    std::vector<AnnouncementRecord> announcements = parseAnnouncementsCsv(announcements_file);
    
    if (announcements.empty()) {
        std::cerr << "Error: No announcements loaded" << std::endl;
        return 1;
    }
    
    std::cout << "  Loaded " << announcements.size() << " announcements" << std::endl;
    std::cout << "  Seeding announcements..." << std::endl;
    
    int seeded_count = 0;
    int skipped_count = 0;
    
    for (const auto& ann : announcements) {
        auto node = graph.getNode(ann.asn);
        if (!node) {
            std::cerr << "  Warning: AS " << ann.asn << " not in topology, skipping" << std::endl;
            skipped_count++;
            continue;
        }
        
        try {
            IPPrefix prefix(ann.prefix);
            node->seedAnnouncement(prefix, ann.rov_invalid);
            seeded_count++;
        } catch (const std::exception& e) {
            std::cerr << "  Warning: Invalid prefix " << ann.prefix << ", skipping" << std::endl;
            skipped_count++;
        }
    }
    
    std::cout << "  Seeded " << seeded_count << " announcements";
    if (skipped_count > 0) {
        std::cout << " (skipped " << skipped_count << ")";
    }
    std::cout << std::endl;
    
    auto seed_time = std::chrono::high_resolution_clock::now();
    auto seed_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seed_time - rov_time);
    std::cout << "  Time: " << seed_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Step 5: Propagate announcements
    std::cout << "Step 5: Propagating announcements..." << std::endl;
    // Use multiple iterations so announcements traverse providers, peers, and customers fully
    // Run enough iterations for large topologies to converge fully
    graph.propagateAll(8);
    
    auto propagate_time = std::chrono::high_resolution_clock::now();
    auto propagate_duration = std::chrono::duration_cast<std::chrono::milliseconds>(propagate_time - seed_time);
    std::cout << "  Time: " << propagate_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Step 6: Output results
    std::cout << "Step 6: Writing output to " << output_file << "..." << std::endl;
    graph.dumpToCSV(output_file);
    
    auto output_time = std::chrono::high_resolution_clock::now();
    auto output_duration = std::chrono::duration_cast<std::chrono::milliseconds>(output_time - propagate_time);
    std::cout << "  Time: " << output_duration.count() << " ms" << std::endl;
    std::cout << std::endl;
    
    // Final summary
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(output_time - start_time);
    std::cout << "=== Simulation Complete ===" << std::endl;
    std::cout << "Total time: " << total_duration.count() << " ms" << std::endl;
    
    return 0;
}
