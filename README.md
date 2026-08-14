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

`HeliotisC4Device::open()` opens the selected device but does not initialize or move the stage. `configureH8SurfaceExample()` is a separate motion-producing reference-profile operation: it writes output, chunk, encoder, scan, motion, processing, and illumination values, preserves the existing trigger configuration, and executes stage initialization. `initializeMotion()` is a separate motion-readiness operation. The consumer owns whether and when either operation is invoked and must confirm that the device is connected, known-safe, and has sufficient physical clearance; neither operation is an implicit side effect of discovery, `open()`, or acquisition.

Every `heliotis::Frame` is copied before the C4 buffer is released. Consumers may retain it independently of the SDK lifetime. Feature writes and commands recheck live type and access information.

Acquisition callbacks run on a worker. Keep them bounded, stop acquisition before closing the device, and do not update GUI objects directly. The current diagnostic path scans every part's samples for summary statistics, so high-rate deployments should disable or redesign that work before treating throughput as accepted.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for lifecycle, runtime, and validation contracts.
