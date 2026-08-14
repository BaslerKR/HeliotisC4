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

`HeliotisC4System` owns one C4 handler. `HeliotisC4Device` owns one selected interface/device pair. Device descriptors are discovery snapshots, so `open()` refreshes the interface list and validates the indices before opening.

Opening a device does not initialize or move the stage. Motion-producing setup is an explicit operation:

- `configureH8SurfaceExample()` writes the vendor example's output components, chunk selection, scan position/range/speed, general speed, processing values, illumination, and related controls, then executes stage initialization.
- `initializeMotion()` executes the dedicated motion initialization path.
- Both operations require a connected, known-safe device and sufficient physical clearance.

A consumer must not treat either operation as an implicit side effect of connection. If a consumer workflow invokes one after connection, that call policy must be explicit and must enforce the connected, known-safe, and clearance preconditions. Discovery and acquisition never initialize motion.

The feature snapshot retains unavailable entries so a UI can explain them. Typed writes and commands recheck the live SDK type and access mode. Motion features remain classified under `FeatureSection::Motion`; normal configuration remains under `FeatureSection::Device`. A Qt consumer executes SDK work away from its event thread.

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

H8 payloads are multipart. `heliotis::FramePart` records semantic kind, pixel format, dimensions, bit depth, scale, and either `uint16_t` or `double` samples. The reader retries metadata and data reads using the byte capacity returned by the C ABI; it must not assume two dimensions or a fixed part size.

Before arming, acquisition requires `ChunkModeActive=1`. Each buffer must provide a matching part count and part type. Scan3d output mode, distance unit, coordinate scale, coordinate offset, fixed-point scaling, frame ID, and timestamp are retained when present. `RectifiedC` supplies physical A/B grid spacing. `CalibratedC` has no physical X/Y axes and therefore cannot be advertised as metrically calibrated XYZ geometry.

Single-frame mode stops after one copied frame. Continuous mode receives until stopped. Software trigger requests are serialized so only one request can be in flight. Acquisition preserves the device's trigger policy and does not silently issue a trigger or rewrite acquisition mode.

## Performance Contract

The delivery-critical path already deep-copies every part because the C4 buffer is released immediately. That ownership copy is required unless the SDK provides a proven lease contract.

The current implementation also computes min/max/invalid diagnostics by scanning all samples for every received part. This is an additional O(sample count) pass before delivery and remains a high-impact throughput risk. Move it behind an explicit diagnostic mode, replace it with bounded sampling, or collect counters outside the critical path before accepting high-resolution continuous acquisition.

Logging of idle waits is sampled, but message preparation and device logging still need measurement under sustained acquisition. Acceptance should record payload bytes, copy time, diagnostic time, callback time, queue depth, and p95 frame-delivery latency.

## Validation Gates

Automated gates cover SDK layout, runtime open, frame invariants, optional Qt behavior, and optional adapter contracts. They do not establish device acceptance.

Physical validation must cover discovery, safe open/close, explicit motion initialization with clearance, writable feature changes, Single cancellation without a trigger, continuous capture/stop, software trigger behavior, `RectifiedC` and `CalibratedC` chunk values, retained frame lifetime, delayed runtime dependencies, and sustained throughput.
