# HeliotisC4 Release Note

## v0.1.1

- Validate the vendor-required trailing-separator `C4UTILITY_ROOT` before opening C4HdlC.
- Verify that C4HdlC resolves the requested package-local Diaphus producer.
- Include the dynamically loaded GenICam logging backend in runtime completeness checks.
- Close a newly opened C4 handler if constructor validation or logging fails.
- Resolve the configured Qt runtime deterministically in the Windows widget test.
