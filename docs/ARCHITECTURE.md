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
Motion tab; no device-feature write, command, or motion action is exposed.
Readable nodes and execution-only command nodes appear in the tree: read-only
and command nodes are disabled, while read/write nodes remain enabled for a
future editor. The host refreshes this snapshot after connection and explicit
device refresh. Future acquisition and feature-write transitions must use the
same refresh path because C4 access modes can change with selector and
acquisition state.

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
both `uint16` and floating-point payload access. The module publishes that data
as a deep-owned `heliotis::Frame`: each part has an explicit H8 semantic,
identity, pixel format, dimensions, bit depth, and `uint16` or `double`
samples. It must not expose an SDK buffer after `C4Buf_release`.

`HeliotisC4::Core` has no GraphicsEngine dependency. Its acquisition worker
arms the current device configuration, waits for buffers, deep-copies every
part, releases the C4 buffer, and stops cleanly. It never changes trigger,
output-component, stage, or scan features. Before arming, it reads and requires
`ChunkModeActive=1`; every delivered buffer must provide matching
`ChunkPartCount`, a selected `ChunkPartType` for every part, and Scan3d chunk
metadata. The required H8 configuration must already enable `ChunkPartCount`,
`ChunkPartType`, `ChunkScan3dDistanceUnit`, `ChunkScan3dOutputMode`,
`ChunkScan3dCoordinateScale`, and `ChunkScan3dCoordinateOffset`; the plugin
reports a configuration error instead of guessing a part semantic or geometry.
`ChunkPartSelector` and `ChunkScan3dCoordinateSelector` are written only on a
received C4 buffer to read that buffer's metadata; neither writes a device
feature. Hosts may opt into the module-owned static
`HeliotisC4::GraphicsEngineAdapter`, which maps an organized
Range part and compatible Intensity/Confidence parts to `GraphicsScene3D` and
`RangeFrame`. The adapter is not required for discovery, control, or
acquisition-only hosts. The C4 reader must classify parts before invoking it.
It converts Scan3d units (`nm`, `um`, or `mm`) plus the C scale/offset to
physical millimeter Z values before the RangeFrame reaches Range2D, analysis,
or 3D consumers. `RectifiedC`
provides a uniform A/B grid and can therefore derive PointCloud/Surface views;
`CalibratedC` remains Range2D-only because its 2.5D Surface payload contains no
X/Y axes. `ChunkFrameID` and `ChunkTimestamp` are captured when configured;
they remain optional metadata so an existing read-only configuration without
them can still acquire frames. `ChunkPartFixpointScaling` is preserved per part
and applied by the GraphicsEngine adapter.

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
3. Add optional module-owned GraphicsEngine conversion and the parent
   `HeliotisC4Plugin`, controller, and minimal H8 control widget. Completed.
4. Validate discovery plus a capture/stop cycle on an H8, including both
   `RectifiedC` and `CalibratedC` Scan3d chunk values, before treating the
   current buffer-part classification and geometry mapping as hardware-complete.
   Motion remains disabled.
