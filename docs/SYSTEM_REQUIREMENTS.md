# System Requirements

## 1. Scope

This system is an **IR-based minisplit retrofit controller** implemented on an ESP32-S3.  
It allows capturing and replaying raw IR commands, remote programming via BLE/Wi-Fi, time-based scheduling, and secure operation with error reporting.

The system is **protocol-agnostic**: it does not decode or interpret HVAC IR protocols and instead operates on raw captured IR data.

---

## 2. High-Level Functional Requirements

### FR-1: IR Command Capture and Replay
- The system shall capture raw IR signals during a dedicated *programming mode*.
- The system shall store captured IR data into indexed slots.
- The system shall replay stored IR slots on demand or via scheduled routines.
- The system shall not attempt to decode or infer IR protocol semantics.

### FR-2: Programming Mode
- The system shall support an explicit programming mode entered via authenticated command.
- While in programming mode:
  - IR reception shall be enabled.
  - Normal schedule execution shall be blocked.
- The system shall exit programming mode automatically on completion, timeout, or error.

### FR-3: Slot Management
- The system shall support multiple IR slots.
- Each slot shall include transport metadata (length, CRC, carrier frequency, timing).
- Slots shall be validated before being committed to non-volatile storage.
- Slot writes shall be atomic.

---

## 3. Scheduling Requirements

### FR-4: Time-Based Scheduling
- The system shall support scheduling IR slot execution based on epoch time.
- Schedules may be one-shot or recurring.
- The scheduler shall prevent double execution across reboots or time jumps.

### FR-5: Schedule Persistence
- Schedule definitions shall be persisted in non-volatile storage.
- Each schedule entry shall track `last_run` metadata.
- Schedule table integrity shall be protected via CRC/versioning.

### FR-6: Clock Dependency
- The system shall rely on RTC/NTP-provided epoch time.
- If time is invalid, schedules shall not execute and an error shall be emitted.

---

## 4. Communication Requirements

### FR-7: BLE Communication
- The system shall expose a BLE GATT interface for:
  - Command execution
  - Programming mode control
  - Status and error reporting
- BLE security (bonding/encryption) shall be enforced at the protocol level.
- Unauthorized GATT writes shall be rejected before reaching system logic.

### FR-8: Wi-Fi Communication
- The system shall support Wi-Fi for:
  - Status reporting
  - Error/alert forwarding
  - OTA updates (future)
- Wi-Fi provisioning shall be handled outside the core orchestrator.

---

## 5. Authentication and Authorization

### FR-9: Application-Level Authentication
- The system shall support an application-level authentication mechanism.
- Authentication state shall gate access to sensitive operations:
  - Programming mode
  - Slot erase/write
  - Schedule modification
- Authentication shall be session-based and revocable.

### FR-10: Capability Gating
- The orchestrator shall enable or disable system capabilities based on auth state.
- Authentication shall be independent of BLE/Wi-Fi protocol security.

---

## 6. Error Handling and Events

### FR-11: Centralized Error Reporting
- All modules shall publish internal errors/events to a shared event bus.
- Errors shall be classified and relayed by a dedicated Error Manager.
- User-visible alerts shall be delivered via BLE/Wi-Fi.

### FR-12: Event-Driven Architecture
- The system shall use asynchronous events for:
  - Errors and alerts
  - Connectivity changes
  - Time synchronization
  - Schedule triggers
  - IR operation results

---

## 7. Power Management

### FR-13: Power Control
- The system shall centrally manage peripheral power states.
- IR hardware shall only be powered when required.
- The system shall support idle and active power modes.

### FR-14: Extensibility
- The power management design shall support future battery operation without redesign.

---

## 8. Storage Requirements

### FR-15: Storage Ownership
- All non-volatile memory access shall be mediated by a single Storage Service.
- No module shall access flash/NVS directly.

### FR-16: Data Integrity
- Stored data shall include integrity checks (CRC/version).
- Corruption or storage failures shall generate system errors.

---

## 9. Configuration Management

### FR-17: Configuration Store
- Device configuration (credentials, provisioning state, auth settings) shall be stored separately from operational data.
- Configuration schema shall be validated and versioned.

---

## 10. OTA and Update Readiness

### FR-18: Update State
- The system shall support a dedicated update state in the orchestrator FSM.
- Normal operations shall be suspended during firmware update.

### FR-19: Bootloader Compatibility
- The system shall be compatible with a dual-image bootloader (e.g., MCUboot).
- Rollback shall be supported in case of update failure.

---

## 11. Non-Goals / Explicit Exclusions

- The system shall not infer or track HVAC internal state (temperature, mode, fan).
- The system shall not interpret IR protocol semantics.
- Relative IR commands (e.g., temp up/down) are not guaranteed to be supported.
- The system shall not attempt to synchronize with physical remote controls.

---

## 12. MVP Constraints

- Scheduler may use coarse polling (≥30s resolution).
- Event bus may be implemented with a single queue.
- Cloud/MQTT integration is optional for MVP.

---

## 13. Future Extensions (Out of Scope for MVP)

- Next-deadline scheduling timers
- Battery-backed operation
- Rich HVAC state abstraction
- Cloud-based automation rules
- Multi-device coordination

---
