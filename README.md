# HeliotisC4

Reusable Heliotis heliInspect H8 device-runtime module for Playground hosts.

This initial skeleton establishes the module boundary only. It contains no device
implementation, no Playground plugin, and no C4Utility binaries.

The future implementation will use the C4Utility `C4HdlC` C API and keep SDK
discovery, device lifecycle, feature access, acquisition, and payload ownership
inside this module. Host-specific plugin wiring belongs in Playground.

See [the architecture note](docs/ARCHITECTURE.md) for the validated SDK layout
and phased implementation contract.
