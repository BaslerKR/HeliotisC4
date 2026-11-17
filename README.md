# HeliotisC4

HeliotisC4 is a C++17 device-runtime module for Heliotis heliInspect H8 systems using the C4Utility `C4HdlC` API. It owns SDK discovery, device lifecycle, feature access, acquisition, and deep-owned multipart frames.

## Targets and Options

- `HeliotisC4::Core` provides SDK-neutral frame and feature contracts.
- `HELIOTISC4_ENABLE_SDK=ON` adds the Windows C4Utility implementation.
- `HELIOTISC4_BUILD_TESTS=ON` builds the standalone contract and SDK-layout tests.
- The Playground package is widget-free: it links the widget-free runtime contract and Qt Core/Concurrent only, with no Qt Widgets, Resources widget/dialog library or OpenGL dependency.
- An optional Playground adapter (`HELIOTISC4_BUILD_PLAYGROUND_ADAPTER=ON`) can be enabled only when the neutral GraphicsEngine contract target is available; it does not require the visualization renderer. Consumers that want owned frames include `Utility/PlaygroundAdapter/HeliotisGraphicsFrameStream.h` and link `HeliotisC4::PlaygroundAdapter`; conversion stays in `HeliotisC4GraphicsFrameAdapter`. The stream header uses the SDK-neutral frame contract and does not include C4Utility headers.
- An optional Playground device plugin (`HELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON`) is a separate MODULE. Enable it only when `Playground::DeviceRuntimeContract`, the adapter, and the C4Utility SDK are available. Package identity lives in `C++/Utility/PlaygroundAdapter/Package/Package.cmake` as quoted `set(PLAYGROUND_PLUGIN_* ...)` assignments; the MODULE must set `PLAYGROUND_PLUGIN_PACKAGE` and include `DeviceRuntime/Contract/DeviceRuntime.h`. The host emits `plugin.json` from that identity plus `DeviceRuntime.h`.

No proprietary SDK binary is stored in this repository.

## Integration

```cmake
set(HELIOTISC4_ENABLE_SDK ON CACHE BOOL "" FORCE)
set(HELIOTISC4_C4UTILITY_ROOT "C:/Program Files/C4Utility" CACHE PATH "")
add_subdirectory(path/to/HeliotisC4 HeliotisC4-build)
target_link_libraries(consumer PRIVATE HeliotisC4::Core)
```

No Qt widget library is built or shipped. Device discovery, acquisition, parameters and diagnostics are driven through the session and its contracts; the Host device panel owns all presentation.

The SDK-enabled implementation is currently Windows x64 only because that is the package layout resolved by CMake. The SDK-neutral frame contract and tests can build without the SDK.

## Safety and Ownership

`HeliotisC4Device::open()` and the session connection flow preserve the selected device preset:
 they do not load a user set, apply a capture profile, initialize motion, or move the stage. The explicit Init action calls `configureH8SurfaceExample()` and resets the complete C4Utility h8SurfSimple capture defaults: surface components, chunks, canonical Software/Stage triggers, `EncoderSelector=Camera`, `EncoderInverter=1`, `ScanPosition=-1.4`, `ScanRange=0.5`, `ScanSpeed=5.0`, `GeneralSpeed=10.0`, `ScanMode=Down`, processing, and illumination. `StageInit` runs only after all required encoder and motion defaults are verified. Required groups continue independently for complete diagnostics; optional Scan3d geometry and illumination failures are warning-only. The separate `initializeMotion()` API remains an explicit consumer-owned motion operation; no connection path invokes it. The consumer must establish physical clearance before invoking Init or any direct motion operation; the declared `heliotis-c4.init` tool additionally requires its typed confirm argument carrying the stage-move warning.

Every `heliotis::Frame` is copied before the C4 buffer is released. Consumers may retain it independently of the SDK lifetime. `ChunkPartType` and pixel-format metadata are classification hints: missing or unsupported values are recovered through an unambiguous typed data probe when possible, and an uncopyable part is isolated while other safely copied parts remain deliverable. Such parts remain `FramePartKind::Unknown` until a consumer can classify them. Feature writes and commands recheck live type and access information.

Acquisition callbacks run on a worker. Keep them bounded, stop acquisition before closing the device, and do not update GUI objects directly. One dedicated acquisition worker owns `C4Dev_startAcquisition`, every trigger and buffer operation, `C4Dev_stopAcquisition`, and post-stop restoration; the calling thread waits only for the worker's arm result. Single and Live both acquire through device `AcquisitionMode=Continuous`, matching the vendor H8 surface sequence. Single is a host-side completion policy that stops SDK acquisition after the first received frame; Live runs until stopped. A non-Continuous preset is restored after SDK stop. Neither action implicitly triggers a frame: all-Off trigger selectors free-run, and one `FrameStart=On/Software` request issues exactly one `TriggerSoftware` command before waiting for its frame, including when `RecordingStart=On/Stage`. The software surface path pins FrameStart before SDK start, performs no feature access after start and before an accepted request, then calls `TriggerSoftware` and immediately enters the installed C sample's uninterrupted ten-second `C4Dev_getBuffer()` wait. No feature access or log call separates those two SDK operations. A buffer timeout then records acquisition/transfer/stage state and disarms so it cannot block the next arm. Automatic and external paths use short polls for responsive cancellation. FrameStart `TriggerDivider` and `TriggerMultiplier` are preserved and reported but not interpreted by the host; device behavior for non-unit values remains hardware-dependent. The prior trigger, acquisition-status, and device-mode cursors are restored after use. Diagnostics retain worker identity, pre-arm and post-timeout state, configuration, recording plan, command/buffer SDK codes, multipart metadata, copy timing, and callback timing without a second full-sample scan after the ownership copy.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for lifecycle, runtime, and validation contracts.

## Installed consumer SDK plugin build

Build the existing plugin project using this repository, an installed Plugin
SDK, the matching Qt kit and its vendor SDK. No consumer source tree or
renderer is needed. The plugin switch enables its owning adapter
(and vendor SDK implementation where required). Theme installation remains
consumer-owned.

```sh
cmake -S . -B build/plugin -DCMAKE_PREFIX_PATH="<PluginSDK>;<Qt-prefix>" -DCMAKE_CONFIGURATION_TYPES=Release -DCMAKE_BUILD_TYPE=Release -DHELIOTISC4_BUILD_PLAYGROUND_PLUGIN=ON -DHELIOTISC4_BUILD_TESTS=ON
cmake --build build/plugin --config Release --parallel 4
ctest --test-dir build/plugin -C Release --output-on-failure
cmake --build build/plugin --config Release --target HeliotisC4PluginPackageStage
```

Supply the existing vendor SDK options when it is not in the documented default
location. The generated package is `build/plugin/packages/Release/plugins/<id>/current`
on multi-configuration generators and retains the module-owned runtime layout.
It includes the manifest, icon and SDK identity. Staging reconciles only this
generated package. Use a fresh build directory or explicitly update
`PlaygroundPluginSDK_DIR` when selecting another SDK. This Release software
gate does not establish physical-device, Debug or cross-platform acceptance.
Set `CMAKE_BUILD_TYPE=Release` for a single-configuration generator.

## Application control

The Playground source controller exposes the standard connection lifecycle and
typed parameters through `IDeviceConnectionControl` and `IDeviceParameterControl`
(Playground declares the canonical `device.*` tools), reusing the session's
view-independent flows and the module feature channel. It also declares
`heliotis-c4.init` with a typed stage-move confirmation, progress and
cancellation through the same session path. Parameter descriptors carry the
category tree and a preferred editor, and bounded change subscriptions publish
snapshots and deltas. The session keeps acquisition-error routing, the
reconnect-required latch and periodic diagnostics sampling without a view, and
Existing widget favourites are dropped; the Host panel uses its package-level
`Plugins/heliotis-c4/Favorites` namespace.
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#application-control-boundary).
