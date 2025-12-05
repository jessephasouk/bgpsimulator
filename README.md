# BGP Simulator

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/std/the-standard)
[![CMake](https://img.shields.io/badge/CMake-3.10%2B-blue.svg)](https://cmake.org/)

A high-performance C++ implementation of a Border Gateway Protocol (BGP) simulator for modeling Internet routing behavior. Designed for speed and accuracy on multi-core systems.

## Features

- **Fast Propagation**: Simulates BGP route propagation across 78k+ ASes in ~2 seconds
- **Gao-Rexford Model**: Implements industry-standard valley-free routing policies
- **ROV Support**: Route Origin Validation for security analysis
- **CAIDA Compatible**: Parses standard CAIDA AS relationship datasets
- **Parallel Processing**: OpenMP-based parallelization for multi-core systems
- **Comprehensive Testing**: Full test suite with Google Test

## Quick Start

### Prerequisites

- C++17 compatible compiler (GCC 7.0+ or Clang 5.0+)
- CMake 3.10 or higher
- OpenMP support (included with most modern compilers)

### Building

```bash
# Clone the repository
git clone https://github.com/yourusername/bgpsimulator.git
cd bgpsimulator

# Configure and build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Running

```bash
# Basic usage
./build/bgp_simulator \
  --relationships <caida-relationships.txt> \
  --announcements <announcements.csv> \
  --rov-asns <rov-asns.csv> \
  --output <output.csv>

# Example with benchmark data
./build/bgp_simulator \
  --relationships bench/many/CAIDAASGraphCollector_2025.10.16.txt \
  --announcements bench/many/anns.csv \
  --rov-asns bench/many/rov_asns.csv \
  --output ribs.csv
```

### Testing

```bash
cd build
ctest --output-on-failure
```

## Input File Formats

### AS Relationships (CAIDA Format)

```
# Comments start with #
AS1|AS2|relationship|source
```

Where `relationship` is:
- `-1`: AS1 is provider to AS2 (customer-provider)
- `0`: AS1 and AS2 are peers

### Announcements CSV

```csv
prefix,origin_asn,rov_invalid
10.0.0.0/8,12345,false
192.168.0.0/16,67890,true
```

### ROV ASNs

One ASN per line specifying which ASes have ROV enabled:

```
13335
15169
16509
```

## Output Format

The simulator outputs a CSV with the routing table for each AS:

```csv
asn,prefix,as_path
1234,10.0.0.0/8,"(1234, 5678, 12345)"
5678,10.0.0.0/8,"(5678, 12345)"
```

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        ASGraph                               │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐                      │
│  │ ASNode  │──│ ASNode  │──│ ASNode  │  ...                 │
│  │ (AS123) │  │ (AS456) │  │ (AS789) │                      │
│  └────┬────┘  └────┬────┘  └────┬────┘                      │
│       │            │            │                            │
│  ┌────▼────┐  ┌────▼────┐  ┌────▼────┐                      │
│  │  BGP/   │  │  BGP/   │  │  BGP/   │  Policy Layer        │
│  │  ROV    │  │  ROV    │  │  ROV    │                      │
│  └────┬────┘  └────┬────┘  └────┬────┘                      │
│       │            │            │                            │
│  ┌────▼────────────▼────────────▼────┐                      │
│  │           Local RIB               │  Announcement Store  │
│  │    prefix_id → Announcement       │                      │
│  └───────────────────────────────────┘                      │
└─────────────────────────────────────────────────────────────┘
```

### Core Components

| Component | Description |
|-----------|-------------|
| `ASGraph` | Manages the AS topology and orchestrates propagation |
| `ASNode` | Represents a single Autonomous System with neighbors |
| `Announcement` | BGP route announcement with AS path and attributes |
| `BGP` | Standard BGP policy (Gao-Rexford model) |
| `ROV` | Route Origin Validation policy (extends BGP) |
| `PrefixMap` | Efficient string↔ID mapping for IP prefixes |

### Propagation Model

The simulator follows the Gao-Rexford valley-free routing model:

1. **Up Phase**: Routes propagate from customers to providers
2. **Across Phase**: Routes propagate between peers (one hop only)
3. **Down Phase**: Routes propagate from providers to customers

```
         ┌─────────┐
         │ Tier-1  │  ← Rank 2
         └────┬────┘
              │
    ┌─────────┼─────────┐
    ▼         ▼         ▼
┌───────┐ ┌───────┐ ┌───────┐
│Tier-2 │─│Tier-2 │─│Tier-2 │  ← Rank 1 (peers)
└───┬───┘ └───┬───┘ └───┬───┘
    │         │         │
    ▼         ▼         ▼
┌───────┐ ┌───────┐ ┌───────┐
│ Edge  │ │ Edge  │ │ Edge  │  ← Rank 0
└───────┘ └───────┘ └───────┘
```

## Performance

**Benchmark Results (2 CPU cores, 78,370 ASes, 40 announcements):**

| Phase | Time |
|-------|------|
| Graph Loading | ~300ms |
| Flattening | ~60ms |
| Propagation | ~1.6s |
| CSV Output | ~450ms |
| **Total** | **~2.1s** |

### Key Optimizations

- **Integer Prefix IDs**: 10-20x faster prefix comparisons
- **Static OpenMP Scheduling**: Eliminated work-stealing overhead
- **Pre-allocated Queues**: Reduced memory reallocation
- **SIMD Loop Detection**: AVX2-accelerated AS path checking
- **Batched Mutex Locking**: Reduced lock contention 10x
- **Direct Buffer Writing**: Custom integer-to-string conversion

## Project Structure

```
bgpsimulator/
├── include/           # Header files
│   ├── announcement.h # BGP announcement class
│   ├── as_graph.h     # AS topology graph
│   ├── as_node.h      # Individual AS node
│   ├── bgp.h          # BGP routing policy
│   ├── rov.h          # ROV security policy
│   └── prefix_map.h   # Prefix string↔ID mapping
├── src/               # Implementation files
│   ├── main.cpp       # CLI entry point
│   ├── as_graph.cpp   # Graph operations
│   ├── as_node.cpp    # Node operations
│   ├── bgp.cpp        # BGP policy logic
│   └── rov.cpp        # ROV policy logic
├── tests/             # Google Test suite
├── bench/             # Benchmark datasets
└── CMakeLists.txt     # Build configuration
```

## API Reference

### Creating and Loading a Graph

```cpp
#include "as_graph.h"

ASGraph graph;

// Load from CAIDA relationship file
graph.buildFromCAIDAFile("caida_relationships.txt");

// Flatten for propagation
graph.flattenGraph();

// Deploy ROV to specific ASes
graph.deployROV("rov_asns.txt");
```

### Seeding Announcements

```cpp
#include "announcement.h"

// Get an AS node
ASNode* node = graph.getNode(12345);

// Create and seed an announcement
Announcement ann(IPPrefix("10.0.0.0/8"), 12345, false);
node->seedAnnouncement(ann);
```

### Running Propagation

```cpp
// Run full propagation (up → across → down)
graph.propagateAll();

// Or run phases individually
graph.propagateUp();
graph.propagateAcross();
graph.propagateDown();
```

### Exporting Results

```cpp
// Dump all routing tables to CSV
graph.dumpToCSV("output.csv");
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the MIT License - see the [LICENSE.txt](LICENSE.txt) file for details.

## Acknowledgments

- [CAIDA](https://www.caida.org/) for AS relationship datasets
- [Google Test](https://github.com/google/googletest) for testing framework
- BGPStream and related research for algorithm inspiration

## References

- Gao, L. (2001). On inferring autonomous system relationships in the Internet
- Gill, P., Schapira, M., & Goldberg, S. (2011). A Survey of Interdomain Routing Policies
- RPKI and Route Origin Validation (RFC 6480, RFC 6811)




