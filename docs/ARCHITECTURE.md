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

The selected runtime, whether installed or staged, must preserve the relative C4Hdl, GenICam, logging-backend, `NodeMapData`, and Diaphus producer layout. Before `C4Hdl_open`, `C4UTILITY_ROOT` must identify that selected root with the vendor-required trailing directory separator. Runtime validation must also confirm that `C4Hdl_getDiaphusLocation` resolves the intended producer. After open, the module logs the SDK-reported C4Hdl and Diaphus versions and the resolved producer. DLL presence alone is insufficient because delayed dependencies, version compatibility, and producer lookup are runtime contracts.

## Device and Motion Lifecycle

`HeliotisC4System` owns one C4 handler. `HeliotisC4Device` owns one selected interface/device pair. Device descriptors are discovery snapshots, so `open()` refreshes the interface/device lists and verifies both names as well as indices before opening. A reordered list is rejected instead of opening a different physical device.

Connection replacement is serialized. `close()` first requests initialization cancellation and waits for the profile/motion operation to release the SDK channel before acquisition teardown or handle release; a later `open()` cannot publish a new device handle to the old initialization thread. Calls made from the active initialization or acquisition callback thread are deferred to avoid self-join/self-wait and require a later teardown-owner call.

The device facade's `open()` preserves the current device preset: it does not load a user set, write capture/processing/illumination/trigger values, initialize motion, or move the stage. The integrated Qt connection flow performs only that open operation and a feature refresh; it does not initialize motion or apply a profile. The explicit operations are:

- `configureH8SurfaceExample()` resets the complete C4Utility h8SurfSimple capture defaults: output components, chunks, `RecordingStart=On/Stage`, `FrameStart=On/Software`, `EncoderSelector=Camera`, `EncoderInverter=1`, `ScanPosition=-1.4`, `ScanRange=0.5`, `ScanSpeed=5.0`, `GeneralSpeed=10.0`, `ScanMode=Down`, processing, and illumination, while preserving the device's `AcquisitionStart` route. It verifies required encoder and motion writes and then executes stage initialization.
- `initializeMotion()` remains an explicit consumer-owned operation. It logs the pre-command `StageInitialized` state and always executes `StageInit` so the device's `StageInitMode` controls reinitialization. It performs no other device mutation and preserves trigger values. No connection path invokes it.
- Both operations require a connected, known-safe device and sufficient physical clearance.

A consumer must not treat either operation as an implicit side effect of connection. If a consumer workflow invokes one after connection, that call policy must be explicit and must enforce the connected, known-safe, and clearance preconditions. Discovery, connection, Single, and Live never initialize motion.

The Qt widget keeps connection and capture availability independent from explicit initialization results. Connection opens the device and refreshes features away from the GUI thread while preserving the existing preset. The integrated Init action is the explicit preset-changing path: it calls `configureH8SurfaceExample()` away from the GUI thread, refreshes the feature tree afterward, and leaves connection and capture controls available for diagnosis or retry when a required group fails. Disconnect is an acquisition-presentation session boundary: active and pending trigger flags, Single's Stop presentation, and Live's checked state are cleared before reconnect. Acquisition status delivered after disconnect is ignored and cannot relabel the closed session.

The reference profile treats Range/Reflectance, multipart `PartCount`/`PartType`, encoder, scan motion, processing controls, and canonical Software/Stage trigger routing as acquisition prerequisites while preserving the device's `AcquisitionStart` state. Selector-dependent required groups stop locally after failure while independent groups continue, producing one aggregate error after all safe work is attempted; StageInit is skipped when required encoder or motion defaults are incomplete. Scan3d physical-coordinate chunks, light-controller selection/source/brightness, and user-output selection/value are separate optional selector-dependent groups whose warnings do not block Range capture. Frames without complete Scan3d geometry remain valid but cannot advertise physical grid reconstruction.

The feature snapshot retains unavailable entries so a UI can explain them. Typed writes and commands recheck the live SDK type and access mode. Full feature reads, ordinary writes, and ordinary commands are rejected while acquisition is armed; `triggerSoftware()` is the dedicated armed command path. The Qt feature surface presents the complete snapshot in one connected-device tree using each feature's SDK category path. Same-structure refreshes update existing editors in place so the viewport stays put; selector-driven add, remove, type, or enum-list changes rebuild the tree while preserving expansion, selection, and the top-visible viewport anchor. A Qt consumer executes SDK work away from its event thread.

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

H8 payloads are multipart. `heliotis::FramePart` records semantic kind, pixel format, source bit depth, dimensions, scale, and either `uint16_t` or `double` converted samples. The reader retries metadata and data reads using the byte capacity returned by the C ABI; it must not assume two dimensions or a fixed part size. `ChunkPartType` and pixel-format metadata are hints, not receive gates: missing or unsupported pixel formats are accepted only when exactly one typed data getter provides an unambiguous positive sample size, and a failed part copy does not discard other safely copied parts. Every safely copied part is retained, including parts with missing or unknown `ChunkPartType`, which are marked `Unknown`. A delivered frame must contain at least one structurally valid part; it does not require a recognized Range/Surface part.

Arming does not require or write `ChunkModeActive`; its current value is logged so the existing preset can be tested without hidden initialization. `C4Buf_getNumParts()` is authoritative; a conflicting `ChunkPartCount` is reported and does not block copying. `ChunkPartType` is classification metadata, not a receive gate: missing or unrecognized values do not discard the raw part. When no part is recognized as Range, the GraphicsEngine adapter presents the first displayable part as a raw preview and labels it as unclassified; this is not a calibrated semantic claim. Dimensions are best-effort. Positive dimensions are used when available; a non-integral reported capacity is rounded for a safe retry, and unusable dimensions fall back to the data API's required sample size as a one-row raw preview. A successful data copy uses its positive sample-aligned returned byte count as authoritative; a mismatch with metadata geometry becomes a one-row preview warning. Only failure to obtain a safely typed data copy prevents that part's delivery, and at least one safely copied part is required for frame delivery. Scan3d output mode, distance unit, coordinate scale, coordinate offset, fixed-point scaling, frame ID, and timestamp are retained when present. Invalid geometry or an unrepresentable typed Range part falls back to a raw displayable part when available. `RectifiedC` supplies physical A/B grid spacing. `CalibratedC` has no physical X/Y axes and therefore cannot be advertised as metrically calibrated XYZ geometry.

`C4Dev_startAcquisition` arms the C4 receive path and four SDK-managed buffer slots; it does not itself create a frame trigger. One dedicated acquisition worker owns SDK start, every trigger and buffer call, SDK stop, and post-stop restoration. `startAcquisition()` waits for that worker's arm handshake but does not transfer SDK lifecycle ownership to the caller. The host never generates an implicit software trigger from either capture action. Both Single and Live select device `AcquisitionMode=Continuous`, matching the vendor H8 surface sequence. Single remains a host-side policy that stops after the first copied buffer; Live continues until stopped. This is the only persistent feature value changed by either action. Trigger/status selector cursors are inspected before arming. A software route pins the `TriggerSelector` cursor to `FrameStart` before SDK start, keeps it unchanged through every software command and buffer wait, and restores the prior cursor only after SDK stop; trigger modes and sources are not changed. The mode write is read back before arming, and a non-Continuous original value is restored after `C4Dev_stopAcquisition`. Receive metadata is advisory: non-finite Range fixed-point scaling is retained with scale `1.0` and the part is reclassified as `Unknown` so the raw buffer remains displayable without claiming calibration.

| Request | Device mode | `FrameStart=Off` | `FrameStart=On/Software` | `FrameStart=On/<external>` | Completion |
| --- | --- | --- | --- | --- | --- |
| Single | `Continuous` | Wait for one automatic frame | Issue exactly one `TriggerSoftware` command, then wait for one frame | Wait for one external event | Deliver one frame, stop SDK acquisition, restore prior mode when changed, report inactive |
| Live | `Continuous` | Poll automatic frames continuously | Issue exactly one `TriggerSoftware` command per request, then wait for its frame | Each external event requests a frame | Remain armed until stop, fatal error, or disconnect |

Automatic and external-trigger Single or Live requests have no host-side frame deadline; short buffer polls keep them cancellable and report the preserved arm configuration every ten seconds. One accepted software request instead owns a single ten-second vendor buffer wait. Its timeout is terminal for that arm and reports inactive with an error, preventing a completed request from permanently blocking a later Single or Live arm. The Single control becomes a Stop action while its one-frame request is armed. Live remains checked until worker-owned shutdown reports inactive.

The three trigger selectors are independent gates:

- `AcquisitionStart` is preserved as a device-owned gate. An enabled route is accepted but is not free-run; the host does not reinterpret or rewrite it.
- `FrameStart=Off` is automatic. `FrameStart=On/Software` exposes the host command. Any other readable enabled source is external.
- `RecordingStart` starts sensor data recording for the surface; it is not host file/video recording. `Off` opens that gate automatically after FrameStart, `On/Stage` waits for stage motion, and another readable enabled source is external. `On/Software` is rejected because the host owns only the FrameStart command path.
- `TriggerSource=Auto` remains accepted for compatibility but is logged as deprecated because the documented equivalent is `TriggerMode=Off`.

True free-run is `AcquisitionStart=Off`, `FrameStart=Off`, and `RecordingStart=Off`. `FrameStart=Off` with `RecordingStart=On/Stage` is not free-run: frame start is automatic, but sensor recording still depends on stage activity. The C4Utility H8 surface example instead uses `AcquisitionStart=Off`, `FrameStart=On/Software`, and `RecordingStart=On/Stage`; the worker follows the example by issuing exactly one FrameStart `TriggerSoftware` command before waiting for the buffer.

Software trigger requests are serialized so only one request can be in flight. The UI disables the command after queue acceptance. Before SDK start, the implementation records all three trigger gates and pins `TriggerSelector=FrameStart`. FrameStart `TriggerDivider` and `TriggerMultiplier` are logged and preserved without host interpretation. After SDK start, the worker performs no feature access while waiting for a user request. Once a request arrives, it leaves `TriggerSelector=FrameStart` unchanged, calls `C4Dev_executeCommand("TriggerSoftware")`, and immediately calls `C4Dev_getBuffer(..., 10000)` under the same SDK lock. This direct sequence matches the working installed C `h8SurfSimple` sample: no readiness poll, feature read, write, or selector operation occurs after start and before the command, and no operation or log call separates the command from the buffer operation. The worker later calls `C4Dev_stopAcquisition` and restores changed cursors and mode without changing threads. General feature diagnostics remain suspended until buffer completion; post-wait diagnostics are allowed after the atomic sequence. A successful Live frame enables the next request; Single disarms after its first frame. A software-buffer timeout captures acquisition, transfer, stage, and trigger state, then stops the arm so no stale request remains in flight. Automatic and external paths retain short buffer polls without active selector diagnostics. The device owns any `RecordingStart=On/Stage` motion and recording sequence. The prior TriggerSelector cursor, AcquisitionStatusSelector cursor, and non-Continuous acquisition mode are restored after use. A selector with `TriggerMode=Off` remains usable when firmware hides its irrelevant source, while an enabled selector with an unreadable source is rejected. Trigger modes and sources are otherwise preserved.

The acquisition worker owns SDK start, bounded buffer waits, final SDK stop, trigger-cursor and mode restoration, and disarm notification. A startup handshake publishes active state only after worker-owned SDK start succeeds. A software-triggered buffer timeout is terminal for the current arm; automatic and external timeout polls remain non-terminal. Payload copy, metadata, release, and trigger-command failures are always terminal. Arm requests and synchronous teardown ownership are serialized, and the receive loop is released only after active status callbacks complete so a fast Single frame cannot report inactive before active. Interactive callers may request stop without joining; teardown callers join through `stopAcquisition()`. A failed SDK stop marks the device as requiring close/open before another arm.

## Performance Contract

The delivery-critical path already deep-copies every part because the C4 buffer is released immediately. That ownership copy is required unless the SDK provides a proven lease contract.

Frame diagnostics reuse metadata and timing gathered by the required ownership copy. They do not perform a second O(sample count) scan. The ownership copy remains on every frame.

Logs correlate one arm cycle by ID and include the SDK lifecycle worker thread ID, same-thread assertions for start/trigger/getBuffer/stop, SDK-reported runtime versions and producer, device model/serial/firmware identity, the untouched open-time configuration, explicit Init's encoder/motion target writes and final readback, Stage Init before/progress/after snapshots, calculated recording-frame and stage-position plan, requested/original/restored acquisition mode and trigger cursor, pre-arm and post-timeout acquisition status, SDK start/stop time, atomic TriggerSoftware/getBuffer SDK codes and elapsed time, logical buffer wait, multipart/chunk metadata, copy time, callback time, trigger snapshots and pulse-ratio controls, and raw C4 error codes. On a terminal software timeout, post-wait diagnostics add acquisition, transfer-streaming, stage-motion, and trigger state. A final FrameTriggerWait state is classified as command-consumption unknown because it is also the normal state after a completed or aborted frame cycle; other states can still identify a pending RecordingStart gate, active measurement without streaming, or streaming without host delivery. Message preparation and device logging still need measurement under sustained acquisition.

## Validation Gates

Automated gates cover SDK layout, runtime open, frame invariants, optional Qt behavior, and optional adapter contracts. They do not establish device acceptance.

Physical validation must first capture an open-only snapshot and prove whether the selected device preset already supplies Range and `ChunkPartType`. It must then cover discovery identity changes, safe open/close without connection-time motion, explicit Init profile writes and final readback with clearance, writable feature changes, all-Off free-run repeated Single and Live stop/restart, canonical Software/Stage Single and Live triggering after Init, external-trigger waits, Single cancellation without a trigger, enabled AcquisitionStart preservation and device-trigger behavior, RecordingStart gate diagnostics, `RectifiedC` and `CalibratedC` chunk values, acquisition-mode restoration, retained frame lifetime, installed and fallback runtime provenance, delayed runtime dependencies, and sustained throughput.
