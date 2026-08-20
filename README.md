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

`HeliotisC4Device::open()` opens the selected device without loading a user set, applying a capture profile, or initializing motion. The Qt control can therefore expose capture immediately from the device's existing preset. Its explicit Init action calls only `initializeMotion()`: it reads `StageInitialized` and executes `StageInit` only when false, so it may move the stage but does not write capture, processing, illumination, or trigger values. `configureH8SurfaceExample()` remains a separate consumer-owned reference-profile API and is never invoked by connection, Init, Single, or Live. The consumer must establish physical clearance before either motion-producing operation.

Every `heliotis::Frame` is copied before the C4 buffer is released. Consumers may retain it independently of the SDK lifetime. Feature writes and commands recheck live type and access information.

Acquisition callbacks run on a worker. Keep them bounded, stop acquisition before closing the device, and do not update GUI objects directly. Single temporarily applies only `AcquisitionMode=SingleFrame` and ends after one received frame; Live temporarily applies only `AcquisitionMode=Continuous` and runs until stopped. The prior mode is restored after SDK stop. Neither action implicitly triggers a frame: all-Off trigger selectors free-run, `FrameStart=On/Software` requires one explicit command per frame, and external/stage gates wait for their configured source. Full sample diagnostics run for Single, the first three live frames in each arm cycle, and every hundredth live frame; use the logged configuration, payload, copy, and callback timing before treating hardware behavior or sustained throughput as accepted.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for lifecycle, runtime, and validation contracts.
