# HeliotisC4

HeliotisC4 is a C++17 device-runtime module for Heliotis heliInspect H8 systems using the C4Utility `C4HdlC` API. It owns SDK discovery, device lifecycle, feature access, acquisition, and deep-owned multipart frames.

## Targets and Options

- `HeliotisC4::Core` provides SDK-neutral frame and feature contracts.
- `HELIOTISC4_ENABLE_SDK=ON` adds the Windows C4Utility implementation.
- `HELIOTISC4_BUILD_QT_UI=ON` adds the optional `HeliotisC4::QtWidget` static target and requires Qt 6 Widgets.
- `HELIOTISC4_BUILD_TESTS=ON` builds the standalone contract and SDK-layout tests.
- An optional scene adapter can be enabled only when a neutral scene-contract target is already available; it does not require the visualization renderer.

No proprietary SDK binary is stored in this repository.

## Integration

```cmake
set(HELIOTISC4_ENABLE_SDK ON CACHE BOOL "" FORCE)
set(HELIOTISC4_C4UTILITY_ROOT "C:/Program Files/C4Utility" CACHE PATH "")
add_subdirectory(path/to/HeliotisC4 HeliotisC4-build)
target_link_libraries(consumer PRIVATE HeliotisC4::Core)
```

Qt integration is disabled by default. A Qt consumer enables it explicitly and links only the widget target, which brings in `HeliotisC4::Core` and Qt Widgets transitively:

```cmake
set(HELIOTISC4_BUILD_QT_UI ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/HeliotisC4 HeliotisC4-build)
target_link_libraries(qt_consumer PRIVATE HeliotisC4::QtWidget)
```

Requesting the UI without Qt 6 Widgets is a configure error. Enabling it does not add widget sources, Qt libraries, or UI compile definitions to `HeliotisC4::Core`.

The SDK-enabled implementation is currently Windows x64 only because that is the package layout resolved by CMake. The SDK-neutral frame contract and tests can build without the SDK.

## Safety and Ownership

`HeliotisC4Device::open()` and the integrated Qt connection flow preserve the selected device preset: they do not load a user set, apply a capture profile, initialize motion, or move the stage. The explicit Init action calls `configureH8SurfaceExample()` and resets the complete C4Utility h8SurfSimple capture defaults: surface components, chunks, canonical Software/Stage triggers, `EncoderSelector=Camera`, `EncoderInverter=1`, `ScanPosition=-1.4`, `ScanRange=0.5`, `ScanSpeed=5.0`, `GeneralSpeed=10.0`, `ScanMode=Down`, processing, and illumination. `StageInit` runs only after all required encoder and motion defaults are verified. Required groups continue independently for complete diagnostics; optional Scan3d geometry and illumination failures are warning-only. The separate `initializeMotion()` API remains an explicit consumer-owned motion operation; no connection path invokes it. The consumer must establish physical clearance before invoking Init or any direct motion operation.

Every `heliotis::Frame` is copied before the C4 buffer is released. Consumers may retain it independently of the SDK lifetime. Feature writes and commands recheck live type and access information.

Acquisition callbacks run on a worker. Keep them bounded, stop acquisition before closing the device, and do not update GUI objects directly. One dedicated acquisition worker owns `C4Dev_startAcquisition`, every trigger and buffer operation, `C4Dev_stopAcquisition`, and post-stop restoration; the calling thread waits only for the worker's arm result. Single and Live both acquire through device `AcquisitionMode=Continuous`, matching the vendor H8 surface sequence. Single is a host-side completion policy that stops SDK acquisition after the first received frame; Live runs until stopped. A non-Continuous preset is restored after SDK stop. Neither action implicitly triggers a frame: all-Off trigger selectors free-run, and one `FrameStart=On/Software` request issues exactly one `TriggerSoftware` command before waiting for its frame, including when `RecordingStart=On/Stage`. The software surface path pins FrameStart before SDK start, performs no feature access after start and before an accepted request, then calls `TriggerSoftware` and immediately enters the installed C sample's uninterrupted ten-second `C4Dev_getBuffer()` wait. No feature access or log call separates those two SDK operations. A buffer timeout then records acquisition/transfer/stage state and disarms so it cannot block the next arm. Automatic and external paths use short polls for responsive cancellation. The one-command-per-frame path requires FrameStart `TriggerDivider=1` and `TriggerMultiplier=1`; the prior trigger, acquisition-status, and device-mode cursors are restored after use. Diagnostics retain worker identity, pre-arm and post-timeout state, configuration, recording plan, command/buffer SDK codes, multipart metadata, copy timing, and callback timing without a second full-sample scan after the ownership copy.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for lifecycle, runtime, and validation contracts.
