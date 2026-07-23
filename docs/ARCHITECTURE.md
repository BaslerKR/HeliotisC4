# Heliotis C4 Architecture

## Scope

`HeliotisC4` owns the Heliotis C4Utility SDK boundary for heliInspect H8:
SDK discovery, device discovery, connection, GenICam feature access, acquisition,
and deep-owned payloads. It must not depend on Playground, its plugin ABI, or
the host application's UI theme.

Playground will later own a `HeliotisC4Plugin`, its imaging controller, session
creation, and deployment registration. This separation follows the existing
device-plugin boundary.

## Validated C4Utility installation

The development machine has C4Utility 1.12.0 installed at
`C:/Program Files/C4Utility`. Its C API package is C4HdlC 1.4.2:

- CMake package: `c4hdl/win64-x64/c/cmake/C4HdlCConfig.cmake`
- API header/import library/DLL: `c4hdl/win64-x64/c/{include,lib,bin}`
- GenICam dependencies: `c4hdl/win64-x64/genicam/bin`
- Heliotis GenTL producer: `diaphus/win64-x64/diaphus.cti`

The installed package exposes a Windows x64 Release import target only. SDK
discovery must therefore be optional and Windows-gated until supported packages
for other host platforms are supplied. No SDK binary is copied into this module.
Playground stages the required runtime subset in its private plugin runtime;
distribution remains subject to the C4Utility license terms.

`Internal/C4UtilitySdkRuntime` is the first implementation unit. It verifies the
known installation layout without loading a DLL or discovering hardware. Its
standalone CTest is the precondition for the later C4HdlC lifecycle layer.

`HeliotisC4System` now owns one C4 handler and discovers interfaces/devices.
`HeliotisC4Device` owns one selected interface/device pair. After connection it
builds a deep-owned, read-only feature snapshot from C4Utility metadata and
current readable values. Stage/scan/motion/encoder categories remain in the
Motion tab; no write, command, acquisition, or motion action is exposed.

## Acquisition boundary

The C API expresses the lifecycle as:

```text
C4Hdl_open
  -> update/open interface
  -> update/open device
  -> configure features and commands
  -> start acquisition
  -> get buffer(s)
  -> copy every required part while the C4 buffer is alive
  -> release buffer
  -> stop acquisition, release device/interface, close handler
```

H8 data is multipart. The vendor examples configure `Range`/surface and
reflectance-style components, identify parts through chunk metadata, and expose
both `uint16` and floating-point payload access. The module must publish a
device-neutral, deep-owned frame containing: part identity, pixel format,
dimensions, calibration/chunk metadata, and samples. It must not expose an SDK
buffer after `C4Buf_release`.

For the first Playground integration, map the height/surface part to the 3D
path and intensity/reflectance parts to auxiliary images. Keep the module's
payload independent of `GraphicsEngine`; perform UI/GraphicsEngine conversion
in the parent plugin/controller. Measure the cost of the SDK floating-point
conversion and any down-conversion before choosing the long-lived frame type.

## Control boundary

Use the C API's feature metadata (type, access, min/max/increment, enum values,
category, and visibility) to validate all reads and writes. Begin with a small,
H8-specific control set: connection, acquisition mode, output components,
trigger, stage initialization/position/speed, scan configuration, and
illumination. Do not expose an unrestricted raw-feature editor in the first
plugin milestone.

`C++/Utility/Qt/QHeliotisC4Widget` owns the plain Qt feature-tree presentation.
It accepts only SDK-neutral `FeatureDescriptor` values and exposes the existing
`treeRole="DeviceFeatureTree"` contract; Resources may theme it, but this module
must remain usable without Resources. Device and Motion tabs keep internal H8
stage features separate from normal device configuration. The trees are
read-only at skeleton stage.

## H8 internal motion contract

H8 motion is controlled through C4 feature reads, writes, and commands, not by
an external motion-controller integration. The module must never run
`StageInit`, change position, or start a stage-triggered scan while discovering
or connecting a device. A future motion API must require an open, idle device;
validate writable feature access and limits; apply one explicit user request;
then read the affected features back. Motion feature descriptors use the Motion
section so `QHeliotisC4Widget` renders them only on its Motion tab.

## Runtime and deployment gate

The Windows plugin runtime belongs below `plugins/heliotis-c4/runtime`,
preserving the C4Utility-relative layout above. It stages `C4HdlC.dll`, its
delayed GenApi/GCBase dependencies, and `diaphus.cti`; `PluginManager` registers
only their private DLL directories and sets the private Diaphus producer path.
Before release, validate a clean staged copy with the SDK license/distribution
terms confirmed, direct plugin loading, H8 discovery, and a capture/stop cycle.
Do not add Heliotis paths to the host-wide GenTL environment without proving
they do not change ownership of existing devices.

## Implementation sequence

1. Add a Windows-only C4Utility discovery and runtime-layout test.
2. Implement the module lifecycle and deep-owned multipart frame without Qt UI.
3. Add the parent `HeliotisC4Plugin`, controller, and minimal H8 control widget.
4. Stage and verify the bundle before enabling acquisition or motion controls.
