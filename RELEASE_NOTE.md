# HeliotisC4 Release Note

## Unreleased

- Enable capture immediately after open from the existing device preset; connection no longer applies or requires the H8 reference profile.
- Make the integrated Init action Stage-only: inspect `StageInitialized`, execute `StageInit` only when needed, and keep connection and non-stage capture available after failure.
- Keep the full motion-producing reference profile as a separate consumer-owned API, with optional Scan3d geometry, light-controller, and user-output groups that do not prevent Range capture.
- Map Single and Continuous requests to the device's `AcquisitionMode`, verify readback, and restore the original mode after worker-owned SDK shutdown.
- Centralize the three-selector acquisition policy, drive software commands only through `FrameStart=On/Software`, report and restore every selector, accept `TriggerMode=Off` when firmware hides its irrelevant source, and reject every enabled deprecated AcquisitionStart route, unresolved enabled sources, or unsupported RecordingStart software control with actionable errors.
- Specify Single as one received `SingleFrame` and Live as `Continuous` until stop without generating an implicit host trigger; identify all-Off free-run, canonical Software/Stage, and external/stage-gated behavior explicitly.
- Add serialized connection, initialization, arm, and synchronous-stop ownership; close now cancels and waits for active initialization before handles can be reopened. Also add ordered active/inactive status delivery, non-blocking stop requests, bounded buffer polls without misclassifying copy failures as timeouts, clean-stop recovery gating, stale discovery-identity checks, source pixel bit depth, unknown-part tolerance, and required Range-part validation.
- Continue arming when `ChunkModeActive` is off or unavailable, use the C API part count when `ChunkPartCount` is absent, and report missing semantic `ChunkPartType` only from the received payload.
- Sample full payload diagnostics for Single, the first three live frames, and every hundredth live frame while logging untouched open configuration, Stage Init progress, arm IDs, acquisition-mode restoration, SDK timing, callback timing, trigger state, motion state, multipart metadata, and stop results.
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
