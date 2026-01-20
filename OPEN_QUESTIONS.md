# Open Questions

- Should integration tests be allowed to run against an existing DISPLAY by default, or remain Xvfb-only unless `LWM_TEST_ALLOW_EXISTING_DISPLAY=1` is set?
- Should CI install/run Xvfb so the integration tests run automatically?
- Do we want a minimal test config file to lock workspace/bar settings for integration tests instead of relying on defaults?
