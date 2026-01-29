# Storage Manager (`stg_man`)

## Supported Targets

This component is **target-agnostic** and works on **all ESP-IDF
supported targets**:

- ESP32
- ESP32-S2
- ESP32-S3
- ESP32-C2
- ESP32-C3
- ESP32-C6
- ESP32-H2
- ESP32-P4

Any target that supports: - SPI flash - ESP-IDF FATFS - Wear Leveling
(WL)

---

## Overview

`stg_man` is a small storage abstraction built on top of **ESP-IDF
FATFS + wear leveling**.\
It provides **safe, binary-friendly, versioned storage** for small
payloads (≈260 bytes), with a focus on:

- Robustness against power loss
- Corruption detection
- Simple synchronous APIs
- Predictable behavior

This component is intended for **configuration blobs and small
persistent state**, not high-throughput logging.

---

## Design Goals

- Simple, synchronous API
- Safe against power loss during writes
- Binary-safe (no `printf`-style writes)
- Detectable corruption and version mismatch
- Minimal flash wear for small, infrequent updates
- No background tasks or queues

### Non-goals

- High-frequency logging
- Transactional databases
- Multi-writer concurrency
- Journaling filesystems

---

## Architecture Overview

```
Application
    |
    v
stg_man API
    |
    v
VFS (stdio / POSIX)
    |
    v
FATFS
    |
    v
Wear Leveling (WL)
    |
    v
SPI Flash
```

---

## File Format

Each managed file has the following layout:

```
+------------------+
| Header           |
|  - magic         |
|  - version       |
|  - payload_len   |
+------------------+
| Payload          |
+------------------+
```

---

## Write Strategy (Temp + Rename)

All writes use a **commit-style write**:

1. Write full `[header + payload]` to a temporary file
2. Close the temporary file
3. Replace the real file using `rename()`

```{=html}
<!-- -->
```

```
/spiflash/UTSTG.TMP   -->   /spiflash/stg.dat
```

---

## Temporary File and 8.3 Filenames

The temporary file name is fixed to:

```
UTSTG.TMP
```

ESP-IDF FATFS may be built without Long File Name (LFN) support.\
Using an 8.3 filename guarantees compatibility.

---

## Thread Safety Model

- All public APIs are synchronous
- Internal access is serialized with a mutex
- No background task or queue

---

## Corruption Handling Policy

Operation   Short header   Bad magic/version

---

init        N/A            N/A
write       auto-heal      error
format      auto-heal      error
check       error          error
read        error          error

---

## Summary

`stg_man` provides predictable, safe storage semantics for small
persistent data.
