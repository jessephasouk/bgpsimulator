# BGP Simulator

A C++ implementation of a Border Gateway Protocol (BGP) simulator for CSE3150 course project.

## Current Implementation Status

### ✅ Section 2.3: AS Graph Builder

Successfully implemented the AS-level topology graph builder with the following features:

- **Graph Construction**: Builds AS topology from CAIDA AS relationship data
- **Relationship Types**: Handles provider-customer and peer-to-peer relationships
- **Cycle Detection**: Detects and rejects invalid provider-customer cycles
- **Performance**: Processes 78k+ nodes and 570k+ edges in ~1 second
- **Test Suite**: Comprehensive tests covering all major functionality

See [detailed documentation](docs/SECTION_2.3_GRAPH_BUILDER.md) for more information.

## Building the Project

```bash
mkdir build
cd build
cmake ..
make
```

## Running Tests

```bash
# Run the test suite
./build/test_as_graph

# Test with CAIDA data
./build/test_graph_builder caida/20250901.as-rel2.txt
```

## Project Structure

```
bgpsimulator/
├── include/           # Header files
│   ├── as_node.hpp
│   └── as_graph.hpp
├── src/              # Source files
│   ├── as_node.cpp
│   ├── as_graph.cpp
│   └── test_graph_builder.cpp
├── tests/            # Test files
│   └── test_as_graph.cpp
├── docs/             # Documentation
│   └── SECTION_2.3_GRAPH_BUILDER.md
├── caida/            # CAIDA AS relationship data
├── build/            # Build directory (generated)
└── CMakeLists.txt    # Build configuration
```

## Test Results

All tests passing ✅

```
Testing basic node creation...
  ✓ Basic node creation test passed
Testing provider-customer relationships...
  ✓ Provider-customer relationship test passed
Testing peer relationships...
  ✓ Peer relationship test passed
Testing cycle detection (no cycles)...
  ✓ No cycles test passed
Testing cycle detection (with cycle)...
  ✓ Cycle detection test passed
Testing peer cycles (should be allowed)...
  ✓ Peer cycles allowed test passed
Testing complex topology...
  ✓ Complex topology test passed
Testing invalid input handling...
  ✓ Invalid input handling test passed

✓ All tests passed!
```

## Performance Metrics

- **Build Time**: ~1.1 seconds for full CAIDA dataset
- **Nodes**: 78,643 Autonomous Systems
- **Edges**: 570,696 relationships
- **Memory**: Efficient with smart pointer-based graph structure
