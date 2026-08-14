# HeliotisC4 Release Note

## Unreleased

- Correct the acquisition buffer timeout unit and poll the SDK in bounded slices so stop and close can interrupt a pending buffer wait without changing trigger or motion behavior.
- Split the opt-in `HeliotisC4::QtWidget` static target from `HeliotisC4::Core`; Qt integration is now disabled by default and fails configuration explicitly when requested without Qt 6.
- Updated the optional scene adapter to consume a neutral scene-contract target without inheriting the visualization runtime; frame conversion output is unchanged.
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
