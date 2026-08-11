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
connection applies the C4Utility 1.12 `h8SurfSimple` measurement/motion profile: Range
and Reflectance components, its `PartCount`/`PartType` multipart chunks, example
encoder/motion values, `StageInit`, 3D/processing values, and
`UserOutput0`-controlled illumination. It preserves the device's existing
RecordingStart, AcquisitionStart, and FrameStart trigger configuration. It writes the example
`ScanPosition=-1.4`, `ScanRange=0.5`, `ScanSpeed=5.0`, and
`GeneralSpeed=10.0`; this can move hardware, so connection requires a cleared,
known-safe H8. `StageInit` never runs during discovery or acquisition.

The device builds a deep-owned feature snapshot from C4Utility metadata and
current readable values. It retains every implemented feature, including those
currently unavailable, so the Qt tree can show unavailable entries disabled.
The widget uses typed editors only for entries whose current C4 access mode is
WO or RW. Every write/command rechecks the live C4 type and access mode. When
acquisition starts or stops, the module refreshes this access snapshot: only
features that C4 currently reports writable remain enabled. `TriggerSoftware`
is additionally enabled only while an acquisition is armed.
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
The C4 C ABI reports required capacity in **bytes** through its `bufferSize`
arguments; the reader must retry multipart data and dimension reads with that
returned byte capacity instead of assuming two dimensions or a fixed payload
size.

`HeliotisC4::Core` has no GraphicsEngine dependency. Acquisition is limited to
grab lifecycle: it arms the current device configuration, waits for buffers,
deep-copies every part, releases the C4 buffer, and stops cleanly. It never
initializes or moves the stage, or changes trigger, output-component, stage, or
scan features. Motion operations are separate from the acquisition controller.
Before arming, it reads and requires
`ChunkModeActive=1`; every delivered buffer must provide matching
`ChunkPartCount` and a selected `ChunkPartType` for every part. The reference
profile enables the `Scan3dDistanceUnit`, `Scan3dOutputMode`,
`Scan3dCoordinateScale`, and `Scan3dCoordinateOffset` chunks in addition to
multipart identity. Scan3d geometry is required for 3D presentation: a
complete Range/Reflectance frame without it remains Range2D, and the
GraphicsEngine adapter preserves its native Range/Reflectance/Confidence
16-bit codes without inferring a calibrated 3D grid. The
shared Mono MSB/LSB window selects the visible 8-bit range; it does not alter
the retained device codes or physical analysis values. Both Single and Live
start the vendor example's four C4Utility receive slots. The arm log
records the device-reported `PayloadSize` when it is available.
`ChunkPartSelector` and `ChunkScan3dCoordinateSelector` are written only on a
received C4 buffer to read that buffer's metadata; neither writes a device
feature. Hosts may opt into the module-owned static
`HeliotisC4::GraphicsEngineAdapter`, which maps an organized
Range part and compatible Intensity/Confidence parts to `GraphicsScene3D` and
`RangeFrame`. The adapter is not required for discovery, control, or
acquisition-only hosts. The C4 reader must classify parts before invoking it.
It converts Scan3d units (`nm`, `um`, or `mm`) plus the C scale/offset to
physical millimeter Z values before the RangeFrame reaches Range2D, analysis,
or 3D consumers. `RectifiedC` provides a uniform physical A/B grid and derives
PointCloud/Surface views. `CalibratedC` contains no physical X/Y axes, so its
PointCloud/Surface views use an explicitly labelled pixel grid while retaining
physical millimeter Z for Range2D and analysis. Matching heliViewer, the 3D
preview uses a 1 micrometer pixel-grid pitch while leaving camera direction
under GraphicsEngine/user control. It is not metrically calibrated 3D data;
`RectifiedC` remains required for physical XYZ geometry.
`ChunkFrameID` and `ChunkTimestamp` are captured when configured;
they remain optional metadata so an existing read-only configuration without
them can still acquire frames. `ChunkPartFixpointScaling` is preserved per part
and applied by the GraphicsEngine adapter.

The host's **Single** action arms one incoming frame and stops after it is
copied; its action becomes **Stop** while it remains armed, so a missing
trigger can be cancelled. **Live** arms continuous reception. The host does
not auto-trigger Single. `TriggerSoftware` remains available while armed and
is forwarded to C4Utility without a host selector/source policy. TriggerMode
and TriggerSource are selector-scoped: choosing a different selector reveals
that selector's saved values and does not change the FrameStart configuration.
They are host grab policies and do not write the device's GenICam
`AcquisitionMode`.
While acquisition is armed, the module does not enumerate feature metadata.
A software trigger is a single
in-flight request; a second request is rejected until the prior request has
received, failed, or been stopped.
The device determines whether an explicit `TriggerSoftware` command applies
to its current trigger configuration.

## Control boundary

Use the C API's feature metadata (type, access, enum values, category, and
visibility) to validate all reads and writes. The generic feature tree presents
the values currently advertised by the connected H8; SDK access checks remain
the authority. The host executes all writes and commands off the GUI thread.

Keep these operation paths separate:

- **Motion:** connection-time C4Utility `h8SurfSimple` profile plus
  user-requested stage, scan, speed, and encoder changes in the Motion tab.
- **Acquisition:** only arm, receive, deep-copy, release, and stop buffers.
  Single does not issue an automatic trigger. An explicit `TriggerSoftware`
  command is queued to the acquisition worker, which executes it immediately
  before one 10-second C4Utility `getBuffer` wait.

`C++/Utility/Qt/QHeliotisC4Widget` owns the plain Qt feature-tree presentation.
It accepts only SDK-neutral `FeatureDescriptor` values and exposes the existing
`treeRole="DeviceFeatureTree"` contract; Resources may theme it, but this module
must remain usable without Resources. Device and Motion tabs keep internal H8
stage features separate from normal device configuration. The trees preserve
their current item, scroll position, and category expansion state across a
metadata refresh. On the first population, each top-level category is expanded
one level so its direct children are visible while deeper category levels remain
collapsed.
They expose current writable feature editors but never own the SDK call; their
host wires the request to the device boundary.

## H8 internal motion contract

H8 motion is controlled through C4 feature reads, writes, and commands, not by
an external motion-controller integration. Discovery never runs `StageInit`,
changes position, or starts a stage-triggered scan. Connection runs the vendor
reference profile, including its `StageInit` command and motion values; user
motion writes and commands require an open device and use the live SDK access
check. After every successful user operation except `TriggerSoftware`, the
host refreshes the affected feature tree. Motion feature descriptors use the Motion section so
`QHeliotisC4Widget` renders them only on its Motion tab.

## Runtime and deployment gate

The Windows plugin runtime belongs below `plugins/heliotis-c4/runtime`,
preserving the C4Utility-relative layout above. It stages `C4HdlC.dll`, the
complete delayed GenICam closure (`GenApi`, `GCBase`, `MathParser`, `XmlParser`,
`Log`, its dynamically loaded `log4cpp` backend, and `NodeMapData`), and
`diaphus.cti`. Before `C4Hdl_open`, the host must point the vendor-required
`C4UTILITY_ROOT` at this runtime with a trailing directory separator. The module
validates that process contract and the producer reported by
`C4Hdl_getDiaphusLocation`; the host registers only the package's private DLL
directories and supplies the package root.
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
