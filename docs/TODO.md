## Sprint 2 — Storage Service (Single Writer + Integrity)

- [ ] Define storage service public API (`storage_service.h`)
- [ ] Implement ESP-IDF–backed persistence (NVS / partition)
- [ ] Add versioning and CRC checks for all stored objects
- [ ] Define and document cache semantics (valid / dirty / corrupt)
- [ ] Implement factory reset / erase-all path
- [ ] Emit `EVT_STORAGE_CORRUPT` on detected corruption
- [ ] Add basic host tests using simulated storage
- [ ] Verify persistence and corruption handling on ESP target
