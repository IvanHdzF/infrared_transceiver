## Sprint 2 — Storage Service (Single Writer + Integrity)

- [x] Define storage service public API (`storage_service.h`)
- [x] Implement ESP-IDF–backed persistence (NVS / partition)
- [x] Add versioning and CRC checks for all stored objects
- [x] Implement factory reset / erase-all path
- \[\] Emit `EVT_STORAGE_CORRUPT` on detected corruption
- [x] Add basic host tests using simulated storage
- [x] Verify persistence and corruption handling on ESP target
