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
`HeliotisC4Device` owns one selected interface/device pair. A successful H8
connection applies the C4Utility 1.12 `h8SurfSimple` reference profile: Range
and Reflectance components, multipart and Scan3d geometry chunks, stage/software
trigger selectors, example encoder/motion values, `StageInit`, 3D/processing
values, and `UserOutput0`-controlled illumination. It writes the example
`ScanPosition=-1.4`, `ScanRange=0.5`, `ScanSpeed=5.0`, and
`GeneralSpeed=10.0`; this can move hardware, so connection requires a cleared,
known-safe H8. `StageInit` never runs during discovery or acquisition.

The device builds a deep-owned feature snapshot from C4Utility metadata and
current readable values. It shows RO, RW, and WO features and command nodes;
the Qt widget uses typed editors for writable values and an Execute action for
writable commands. Every write/command rechecks the live C4 type and access
mode, rejects acquisition-time changes, and refreshes the feature tree after a
successful operation because selectors can change values and permissions. The
only acquisition-time command exception is `TriggerSoftware`: the widget
enables it only while an acquisition is armed, and the device accepts it only
when C4 reports the command writable.
Stage/scan/motion/encoder categories remain in the Motion tab; all other
features remain in Device.

Device descriptors are discovery snapshots. `HeliotisC4Device::open()` must
refresh the newly opened interface list and validate the selected index before
opening it, because C4Utility does not retain the enumeration performed by a
previous interface handle. Host connection attempts run off the GUI thread;
the control widget enters a pending state until a success or failure result
returns, and a failed attempt always clears the connect toggle.

Every lifecycle operation writes a concise `[Heliotis C4]` diagnostic to the
standard output/error streams: SDK open/discovery, selected-device connection,
motion readiness, feature writes/commands, acquisition arm/disarm, trigger
configuration, software-trigger acceptance, buffer waits, received frames, and
SDK failures. Playground redirects those streams into System Logs; standalone
hosts retain normal console output. Repeated idle buffer waits are sampled
(first wait and then every 20 waits) to keep an external-trigger session
diagnosable without flooding logs.

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

`HeliotisC4::Core` has no GraphicsEngine dependency. Acquisition is limited to
grab lifecycle: it arms the current device configuration, waits for buffers,
deep-copies every part, releases the C4 buffer, and stops cleanly. It never
initializes or moves the stage, or changes trigger, output-component, stage, or
scan features. Motion operations are separate from the acquisition controller.
Before arming, it reads and requires
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

The host's **Single** action arms one incoming frame, issues one
`TriggerSoftware` command for the reference profile's `FrameStart=Software`,
and stops after the frame is copied; **Live** arms continuous reception. They
are host grab policies and do not write the device's GenICam `AcquisitionMode`.
Live accepts explicit `TriggerSoftware` commands from the feature tree. With
FrameStart configured for a physical input, the device instead supplies frames
from that input. `RecordingStart` and `FrameStart` remain distinct H8 trigger
selectors; `AcquisitionStart` is configured only as part of the vendor
reference profile.

## Control boundary

Use the C API's feature metadata (type, access, enum values, category, and
visibility) to validate all reads and writes. The generic feature tree presents
the values currently advertised by the connected H8; SDK access checks remain
the authority. The host executes all writes and commands off the GUI thread.

Keep these operation paths separate:

- **Motion:** connection-time C4Utility `h8SurfSimple` profile plus
  user-requested stage, scan, speed, and encoder changes in the Motion tab.
- **Acquisition:** only arm, receive, deep-copy, release, and stop buffers.
  It consumes the already configured trigger source and does not create a
  stage or external-trigger policy. `TriggerSoftware` is an explicit armed
  command rather than an implicit side effect of Start or Live.

`C++/Utility/Qt/QHeliotisC4Widget` owns the plain Qt feature-tree presentation.
It accepts only SDK-neutral `FeatureDescriptor` values and exposes the existing
`treeRole="DeviceFeatureTree"` contract; Resources may theme it, but this module
must remain usable without Resources. Device and Motion tabs keep internal H8
stage features separate from normal device configuration. The trees expose
current writable feature editors but never own the SDK call; their host wires
the request to the device boundary.

## H8 internal motion contract

H8 motion is controlled through C4 feature reads, writes, and commands, not by
an external motion-controller integration. Discovery never runs `StageInit`,
changes position, or starts a stage-triggered scan. Connection runs the vendor
reference profile, including its `StageInit` command and motion values; user
motion writes and commands require an open, idle device and use the live SDK
access check. After every successful user operation, the host refreshes the
affected feature tree. Motion feature descriptors use the Motion section so
`QHeliotisC4Widget` renders them only on its Motion tab.

## Runtime and deployment gate

The Windows plugin runtime belongs below `plugins/heliotis-c4/runtime`,
preserving the C4Utility-relative layout above. It stages `C4HdlC.dll`, the
complete delayed GenICam closure (`GenApi`, `GCBase`, `MathParser`, `XmlParser`,
`Log`, and `NodeMapData`), and `diaphus.cti`; `PluginManager` registers only
their private DLL directories and sets the private Diaphus producer path.
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
   Also validate the connection-time StageInit path and writable feature changes
   against an H8 with safe motion clearance.
