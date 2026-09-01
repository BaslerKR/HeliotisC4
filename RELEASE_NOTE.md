# HeliotisC4 Release Note

## Unreleased

- Preserve every structurally valid multipart buffer even when `ChunkPartType` is missing or unknown, and show the first unclassified part as a raw preview until semantic mapping is available.
- Treat pixel-format metadata as a type hint: probe the SDK's typed data getters when it is missing or unsupported, keep only an unambiguous storage type, and isolate an uncopyable part without discarding other copied parts.
- Use the actual positive, sample-aligned byte count returned by a successful data copy; metadata geometry remains a warning-level shape hint when the copied sample count differs.
- Fall back from invalid Scan3d geometry, non-finite Range scaling, oversized dimensions, or an unusable typed Range part to the first displayable raw part while keeping the single `RangeFrame` host contract.
- Treat unusable part dimensions as a warning: recover the data sample count through the C API size probe and show it as a one-row raw preview when possible.
- Preserve multipart payload names such as Range, Reflectance, Amplitude, and Intensity as semantic field metadata, and keep finite validity independent for auxiliary channels.
- Keep expected stage/external-gated acquisition trigger diagnostics at informational log level while preserving the condition and guidance.
- Present all feature metadata in one connected-device tree instead of separate Device and Motion tabs.
- Remove the heuristic Device/Motion classification from the SDK-neutral feature contract.
- Add a connected-device root to the feature trees, expand only that root initially, and keep category parents collapsed while preserving tree state across refreshes.
- Keep the complete C4Utility acquisition lifecycle on one dedicated worker thread: worker-owned SDK start, software trigger, buffer receive/release, stop, and feature restoration now match the vendor sample's thread ownership while callers wait only for the arm handshake.
- Match the vendor H8 acquisition order by issuing exactly one `FrameStart` software command per request, including the `RecordingStart=On/Stage` route, before waiting for its buffer.
- Match the working vendor H8 C sample's software receive sequence: pin `TriggerSelector=FrameStart` before SDK start, perform no feature access after start and before an accepted request, and follow each `TriggerSoftware` immediately with one uninterrupted ten-second `C4Dev_getBuffer()`. This removes both the extra per-command selector write and the post-start `FrameTriggerWait` polling that are absent from the sample while preserving post-timeout diagnostics and cursor restoration.
- Preserve the device's `AcquisitionStart` trigger state instead of normalizing it during Init or StageInit.
- Terminate and disarm a software-trigger arm after its one buffer timeout so stale requests cannot block later Single or Live captures; preserve and report non-unit trigger divider/multiplier presets without host-side interpretation, and log device identity, calculated recording/motion values, exact command/buffer codes, post-wait acquisition/transfer/stage state, and a derived stall-stage diagnosis without treating a terminal FrameTriggerWait snapshot as proof that the earlier command was not consumed.
- Restore Single and Live controls after a successful feature write completes its nested feature-tree refresh and cover the ordering with a widget regression test.
- Treat disconnect as an acquisition UI session boundary: clear the Single stop icon, Live toggle, software-trigger pending state, and stale mode flags before reconnect, and ignore late worker state after the device is disconnected.
- Keep the integrated connection open-only: preserve the existing capture preset and do not initialize motion or execute StageInit during connect. The explicit Init action remains the path that applies the default H8 capture profile and initializes the stage.
- Expand the explicit Init action to apply the C4Utility H8 capture profile, including canonical `RecordingStart=On/Stage` and `FrameStart=On/Software` routing, before refreshing feature state.
- Reset explicit Init to the complete C4Utility h8SurfSimple defaults, including encoder and scan-motion values, log their final readback, and skip StageInit when those required writes are incomplete.
- Continue independent required profile groups after local failures and report one aggregate error; log the final configuration readback and keep optional Scan3d geometry, light-controller, and user-output failures warning-only.
- Run both Single and Live through device `AcquisitionMode=Continuous` to match the vendor H8 surface sequence; implement Single as host-side stop after the first copied buffer and restore a non-Continuous preset after shutdown.
- Centralize the three-selector acquisition policy, preserve enabled `AcquisitionStart` routes as device-owned gates, drive software commands only through `FrameStart=On/Software`, report and restore every selector, accept `TriggerMode=Off` when firmware hides its irrelevant source, and reject unresolved enabled sources or unsupported RecordingStart software control with actionable errors.
- Specify Single as one received buffer and Live as Continuous until stop without generating an implicit host trigger; identify all-Off free-run, canonical Software/Stage, and external/stage-gated behavior explicitly.
- Add serialized connection, initialization, arm, and synchronous-stop ownership; close now cancels and waits for active initialization before handles can be reopened. Also add ordered active/inactive status delivery, non-blocking stop requests, bounded buffer polls without misclassifying copy failures as timeouts, clean-stop recovery gating, stale discovery-identity checks, source pixel bit depth, and raw unknown-part delivery.
- Continue arming when `ChunkModeActive` is off or unavailable, use the C API part count when `ChunkPartCount` is absent or conflicts, and report missing semantic `ChunkPartType` only from the received payload.
- Keep frame diagnostics lightweight while logging untouched open configuration, Stage Init progress, arm IDs, acquisition-mode restoration, SDK timing, callback timing, trigger state, motion state, multipart metadata, and stop results; do not add a second full-sample scan after the required ownership copy.
- Log the SDK-reported C4Hdl and Diaphus versions together with the resolved producer after opening the selected runtime.
- Preserve feature-editor locking across refreshes and stop completion, keep TriggerSoftware disabled from queue acceptance through Live frame arrival or Single disarm, reject the transient second-trigger window after a Single frame, and cover initialization, asynchronous arm/refresh, rapid Single completion, trigger-in-flight, failed arm, stop, and acquisition-state transitions in tests.
- Prevent a rejected concurrent initialization request from clearing cancellation owned by the active profile or motion operation.
- Split the opt-in `HeliotisC4::QtWidget` static target from `HeliotisC4::Core`; Qt integration is now disabled by default and fails configuration explicitly when requested without Qt 6.
- Updated the optional scene adapter and its tests to consume only the neutral scene-contract target without inheriting downstream visualization implementation; frame conversion output is unchanged.
- Replace non-standalone documentation with module-local lifecycle, safety, runtime, and performance contracts.
- Clarify that opening a device does not initialize or move the stage; profile and motion initialization remain separate consumer-owned operations with explicit safety preconditions.

## v0.1.2

- Preserve native Scan3d distance units and calibrated scale/offset metadata in the organized scene adapter.
- Add adapter coverage for micrometer geometry and explicit unit conversion.

## v0.1.1

- Validate the vendor-required trailing-separator `C4UTILITY_ROOT` before opening C4HdlC.
- Verify that C4HdlC resolves the requested package-local Diaphus producer.
- Include the dynamically loaded GenICam logging backend in runtime completeness checks.
- Close a newly opened C4 handler if constructor validation or logging fails.
- Resolve the configured Qt runtime deterministically in the Windows widget test.
