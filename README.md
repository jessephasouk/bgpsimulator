# BGP Simulator

A C++ implementation of a Border Gateway Protocol (BGP) simulator project.

### AS Graph Builder

- **Graph Construction**: Builds AS topology from CAIDA AS relationship data
- **Relationship Types**: Handles provider-customer and peer-to-peer relationships
- **Cycle Detection**: Detects and rejects invalid provider-customer cycles
- **Performance**: Processes 78k+ nodes and 570k+ edges in ~1 second
- **Test Suite**: Comprehensive tests covering all major functionality

