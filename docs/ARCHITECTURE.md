# HeliotisC4 Architecture

## Scope

HeliotisC4 owns the C4Utility boundary for heliInspect H8 devices: SDK discovery, interface and device lifecycle, live feature access, acquisition, and deep-owned payloads. It does not own application session routing, global theme installation, or product packaging.

The public boundary is split into:

- `HeliotisC4::Core`: SDK-neutral values plus the optional C4 implementation.
- `heliotis::Frame`: deep-owned multipart data and Scan3d metadata.
- `HeliotisC4::QtWidget`: opt-in static Qt presentation target over SDK-neutral feature descriptors; it links `HeliotisC4::Core` without changing the core target's sources or dependencies.
- An optional renderer adapter that is not required for discovery, control, or acquisition.

## SDK and Runtime Contract

The validated C4Utility 1.12.0 / C4HdlC 1.4.2 development layout is:

```text
<C4Utility root>/
|-- c4hdl/win64-x64/c/cmake/C4HdlCConfig.cmake
|-- c4hdl/win64-x64/c/{include,lib,bin}
|-- c4hdl/win64-x64/genicam/bin
`-- diaphus/win64-x64/diaphus.cti
```

This package exposes a Windows x64 Release import target. Other operating systems or architectures require a vendor package with a matching CMake and runtime layout; they are not established by the current build.

A staged runtime must preserve the relative C4Hdl, GenICam, logging-backend, `NodeMapData`, and Diaphus producer layout. Before `C4Hdl_open`, `C4UTILITY_ROOT` must identify that package root with the vendor-required trailing directory separator. Runtime validation must also confirm that `C4Hdl_getDiaphusLocation` resolves the intended producer. DLL presence alone is insufficient because delayed dependencies and the producer lookup are runtime contracts.

## Device and Motion Lifecycle

`HeliotisC4System` owns one C4 handler. `HeliotisC4Device` owns one selected interface/device pair. Device descriptors are discovery snapshots, so `open()` refreshes the interface/device lists and verifies both names as well as indices before opening. A reordered list is rejected instead of opening a different physical device.

Connection replacement is serialized. `close()` first requests initialization cancellation and waits for the profile/motion operation to release the SDK channel before acquisition teardown or handle release; a later `open()` cannot publish a new device handle to the old initialization thread. Calls made from the active initialization or acquisition callback thread are deferred to avoid self-join/self-wait and require a later teardown-owner call.

Opening a device preserves the current device preset: it does not load a user set, write capture/processing/illumination/trigger values, initialize motion, or move the stage. Motion-producing setup is an explicit operation:

- `configureH8SurfaceExample()` writes the vendor example's output components, chunk selection, scan position/range/speed, general speed, processing values, illumination, and related controls, then executes stage initialization.
- `initializeMotion()` reads `StageInitialized`; it executes only `StageInit` when that value is false and otherwise performs no device mutation.
- Both operations require a connected, known-safe device and sufficient physical clearance.

A consumer must not treat either operation as an implicit side effect of connection. If a consumer workflow invokes one after connection, that call policy must be explicit and must enforce the connected, known-safe, and clearance preconditions. Discovery, Single, and Live never initialize motion.

The Qt widget treats connection, capture availability, and Stage Init as separate states. A successful open enables capture from the existing device configuration. Stage Init temporarily locks competing SDK controls but does not become a capture-readiness gate; success or failure returns to the connected state with non-stage capture available. The integrated Init action calls only `initializeMotion()` and never calls `configureH8SurfaceExample()`.

The reference profile treats Range/Reflectance, multipart `PartCount`/`PartType`, motion, and processing controls as acquisition prerequisites. Scan3d physical-coordinate chunks, light-controller selection/source/brightness, and user-output selection/value are separate optional selector-dependent groups. Failure stops the remainder of that group so a stale selector cannot redirect a dependent write; the warning does not block Range capture. Frames without complete Scan3d geometry remain valid but cannot advertise physical grid reconstruction.

The feature snapshot retains unavailable entries so a UI can explain them. Typed writes and commands recheck the live SDK type and access mode. Full feature reads, ordinary writes, and ordinary commands are rejected while acquisition is armed; `triggerSoftware()` is the dedicated armed command path. Motion features remain classified under `FeatureSection::Motion`; normal configuration remains under `FeatureSection::Device`. A Qt consumer executes SDK work away from its event thread.

## Acquisition Boundary

The required C API order is:

```text
C4Hdl_open
  -> update/open interface
  -> update/open device
  -> start acquisition
  -> receive a buffer
  -> copy every required part and chunk while the buffer is alive
  -> release the buffer
  -> stop acquisition
  -> release device/interface
  -> close handler
```

H8 payloads are multipart. `heliotis::FramePart` records semantic kind, pixel format, source bit depth, dimensions, scale, and either `uint16_t` or `double` converted samples. The reader retries metadata and data reads using the byte capacity returned by the C ABI; it must not assume two dimensions or a fixed part size. Unknown extra parts are skipped, but a delivered frame must contain a supported Range/Surface part.

Arming does not require or write `ChunkModeActive`; its current value is logged so the existing preset can be tested without hidden initialization. `C4Buf_getNumParts()` is authoritative, and `ChunkPartCount` is cross-checked only when present. `ChunkPartType` remains required because the C API exposes no other reliable mapping from a payload part to Range, Reflectance, or Confidence; its absence is reported as a payload-contract error after the buffer arrives. Scan3d output mode, distance unit, coordinate scale, coordinate offset, fixed-point scaling, frame ID, and timestamp are retained when present. `RectifiedC` supplies physical A/B grid spacing. `CalibratedC` has no physical X/Y axes and therefore cannot be advertised as metrically calibrated XYZ geometry.

`C4Dev_startAcquisition` arms the C4 receive path and four SDK-managed buffer slots; it does not itself create a frame trigger. The host never generates an implicit software trigger from either capture action. Single requests temporarily select device `AcquisitionMode=SingleFrame`; Live requests select `Continuous`. This is the only persistent feature value changed by either action. Trigger/status selector cursors are temporarily inspected and restored for routing and diagnostics; trigger modes and sources are not changed. The mode write is read back before arming, and the worker restores the original value after `C4Dev_stopAcquisition`.

| Request | Device mode | `FrameStart=Off` | `FrameStart=On/Software` | `FrameStart=On/<external>` | Completion |
| --- | --- | --- | --- | --- | --- |
| Single | `SingleFrame` | Wait for one automatic frame | Wait for one explicit `TriggerSoftware` command | Wait for one external event | Deliver one frame, stop SDK acquisition, restore mode, report inactive |
| Live | `Continuous` | Poll automatic frames continuously | Each explicit `TriggerSoftware` command requests one frame | Each external event requests a frame | Remain armed until stop, fatal error, or disconnect |

An armed Single or Live request has no host-side frame deadline. It remains cancellable and reports a diagnostic wait snapshot every ten seconds until a frame/trigger arrives or the operator stops it. The Single control becomes a Stop action while that one-frame request is armed. Live remains checked until worker-owned shutdown reports inactive.

The three trigger selectors are independent gates:

- `AcquisitionStart` must be `Off`. C4Utility documents the selector as deprecated and directs consumers to `FrameStart`; every enabled source is rejected.
- `FrameStart=Off` is automatic. `FrameStart=On/Software` exposes the host command. Any other readable enabled source is external.
- `RecordingStart` starts sensor data recording for the surface; it is not host file/video recording. `Off` opens that gate automatically after FrameStart, `On/Stage` waits for stage motion, and another readable enabled source is external. `On/Software` is rejected because the host owns only the FrameStart command path.
- `TriggerSource=Auto` remains accepted for compatibility but is logged as deprecated because the documented equivalent is `TriggerMode=Off`.

True free-run is `AcquisitionStart=Off`, `FrameStart=Off`, and `RecordingStart=Off`. `FrameStart=Off` with `RecordingStart=On/Stage` is not free-run: frame start is automatic, but sensor recording still depends on stage activity. The C4Utility H8 surface example instead uses `AcquisitionStart=Off`, `FrameStart=On/Software`, and `RecordingStart=On/Stage`; one FrameStart software command requests one stage-backed surface.

Software trigger requests are serialized so only one request can be in flight. The UI disables the command after queue acceptance. Live enables the next command after that frame arrives; Single keeps the command in flight until its one-frame acquisition disarms, so no unexecutable second command can be accepted. Before each arm, the implementation records the selected cursor plus independent AcquisitionStart, FrameStart, and RecordingStart mode/source pairs, then restores the cursor. The worker selects FrameStart for the command and restores the previous cursor. A selector with `TriggerMode=Off` remains usable when firmware hides its irrelevant source, while an enabled selector with an unreadable source is rejected. Trigger modes/sources are otherwise preserved.

The acquisition worker owns bounded buffer polling, final SDK stop, mode restoration, and disarm notification. Only failed `C4Dev_getBuffer` waits are classified as timeouts; payload copy, metadata, release, and trigger-command failures remain terminal errors. Arm requests and synchronous teardown ownership are serialized, and the receive loop is released only after active status callbacks complete so a fast Single frame cannot report inactive before active. Interactive callers may request stop without joining; teardown callers join through `stopAcquisition()`. A failed SDK stop marks the device as requiring close/open before another arm.

## Performance Contract

The delivery-critical path already deep-copies every part because the C4 buffer is released immediately. That ownership copy is required unless the SDK provides a proven lease contract.

Full sample diagnostics run for every Single frame, the first three Continuous frames, and each hundredth Continuous frame. Other live frames avoid the additional O(sample count) diagnostic pass. The required ownership copy remains on every frame.

Logs correlate one arm cycle by ID and include the untouched open-time configuration, Stage Init before/progress/after snapshots, requested/original/restored acquisition mode, SDK start/stop time, logical buffer wait, multipart/chunk metadata, sample summaries, copy time, callback time, trigger snapshots, motion/recording state, and raw C4 error codes. Message preparation and device logging still need measurement under sustained acquisition.

## Validation Gates

Automated gates cover SDK layout, runtime open, frame invariants, optional Qt behavior, and optional adapter contracts. They do not establish device acceptance.

Physical validation must first capture an open-only snapshot and prove whether the selected device preset already supplies Range and `ChunkPartType`. It must then cover discovery identity changes, safe open/close, optional Stage Init with clearance and non-blocking failure, writable feature changes, all-Off free-run repeated Single and Live stop/restart, canonical Software/Stage Single and Live triggering, external-trigger waits, Single cancellation without a trigger, deprecated AcquisitionStart rejection, RecordingStart gate diagnostics, `RectifiedC` and `CalibratedC` chunk values, acquisition-mode restoration, retained frame lifetime, delayed runtime dependencies, and sustained throughput.
