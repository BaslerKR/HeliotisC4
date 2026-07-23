# HeliotisC4

Reusable Heliotis heliInspect H8 device-runtime module for Playground hosts.

The current milestone uses the C4Utility `C4HdlC` C API for SDK-layout
validation, device discovery, selected-device connection, and disconnect. It
does not start acquisition, write a feature, or issue a motion command.

The module keeps SDK discovery, device lifecycle, feature access, acquisition,
and payload ownership inside this repository. Host-specific plugin wiring
belongs in Playground. No C4Utility binary is stored in this repository.

See [the architecture note](docs/ARCHITECTURE.md) for the validated SDK layout
and phased implementation contract.

`C++/Utility/Qt/QHeliotisC4Widget` is an optional, Resources-independent Qt
feature-tree scaffold. It uses the shared `DeviceFeatureTree` semantic role when
a host installs Resources, while remaining a functional plain Qt tree otherwise.
The widget separates normal device features from H8 internal-motion features in
Device and Motion tabs; it does not issue motion commands by itself.
