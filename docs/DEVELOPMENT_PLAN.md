# Development Plan

This document defines a sprint-like plan to implement the IR Minisplit Retrofit Controller in incremental, testable milestones.  
Priority is given to **clean interfaces**, **unit-testable modules**, and **early end-to-end validation on hardware**.

---

## Guiding Principles
- Implement **interfaces first**, then internals.
- Prefer **single-writer storage** and **single-threaded service tasks** to reduce race bugs.
- Make each sprint end with a **demoable artifact** (CLI, BLE command, or hardware behavior).
- Keep Event Bus generic and small; avoid pushing large payloads through it.

---

## Sprint 0 — Repo + Scaffolding (Foundation)
**Goal:** Establish project structure, build/test harness, and coding conventions.

### Deliverables
- `docs/` skeleton: `DESIGN.md`, `SYSTEM_REQUIREMENTS.md`, `DESIGN_TRADEOFFS.md`, `DEVELOPMENT_PLAN.md`
- Component doc templates under `docs/components/`
- Project structure (suggested):
  - `main/` application glue (Orchestrator init, registrations)
  - `components/ir_service/`
  - `components/storage/`
  - `components/event_bus/`
  - `components/scheduler/`
  - `components/auth/`
  - `components/error_manager/`
  - `components/comms_ble/`, `components/comms_wifi/`
- Unit test harness on host:
  - Unity/CMock or your preferred framework
  - “HAL shims” for ESP-IDF types where needed

### Exit Criteria
- Build + run unit tests in CI/local
- Lint/format rules documented

---

## Sprint 1 — Event Bus (Core Plumbing)
**Goal:** Implement a minimal, reliable Event Bus for async events.

### Scope
- Fixed-size event struct: type + small payload (IDs/codes only)
- Publish/subscribe API
- Backpressure strategy (drop-oldest or drop-newest) documented
- Optional event tracing hook (for debugging tests)

### Deliverables
- `event_bus.h/.c` with:
  - `eventbus_publish(evt)`
  - `eventbus_subscribe(mask, callback)` OR per-subscriber queue creation
- Unit tests:
  - publish/receive ordering
  - overflow behavior
  - concurrency-safe publish from ISR-safe context (if applicable)

### Exit Criteria
- Deterministic behavior under queue pressure
- Documented constraints (max subscribers, queue depth)

---

## Sprint 2 — Storage Service (Single Writer + Integrity)
**Goal:** Centralize persistence and define atomic commit behavior.

### Scope
- Storage API supports:
  - store/load IR slot blob+header
  - store/load schedule table blob
  - store/load config blob
  - factory reset / erase all
- CRC/version checks for each stored object
- Cache model documented (valid/dirty semantics)

### Deliverables
- `storage_service.h/.c`
- Host tests using simulated flash buffer (no ESP dependencies)
- ESP integration wrapper (NVS/partition) behind `storage_hal`

### Exit Criteria
- No other module writes NVM directly
- Corruption detection paths emit `EVT_STORAGE_CORRUPT` events

---

## Sprint 3 — Infrared Service (Core + HAL Split)
**Goal:** Implement IR learn/send with a reusable middleware boundary.

### Scope
- IR public API:
  - learn start/abort
  - send slot
  - send waveform (optional)
- Internal state machine: IDLE / LEARNING / SENDING
- IR slot format:
  - header: len/crc/carrier/repeat/gap + version
  - blob: waveform data
- HAL interface targeting ESP-IDF RMT:
  - RX capture → waveform
  - TX waveform → RMT symbols

### Deliverables
- `ir_service/`:
  - `ir_service.h/.c` (service + state machine)
  - `ir_core.h/.c` (platform-agnostic waveform utilities)
  - `ir_hal_espidf_rmt.c` (ESP-only binding)
- Unit tests on host:
  - state transitions, timeouts, invalid slot handling
  - store/load integration via mock Storage Service
- Hardware smoke test:
  - learn one command, replay it

### Exit Criteria
- `EVT_IR_LEARN_RESULT`, `EVT_IR_SEND_RESULT`, `EVT_ERROR(IR_*)` emitted correctly
- No races between learn/send (policy enforced)

---

## Sprint 4 — Scheduler Service (MVP Polling)
**Goal:** Persist schedules and trigger actions with coarse resolution.

### Scope
- Schedule table stored in RAM + persisted on update
- MVP polling (30–60s)
- `last_run` markers + missed-run policy (SKIP_MISSED recommended)
- Emits `EVT_SCHEDULE_DUE(schedule_id, action)`

### Deliverables
- `scheduler_service.h/.c`
- Unit tests:
  - recurrence logic
  - reboot behavior (last_run persistence)
  - time invalid handling (emit error)
- Integration:
  - Scheduler triggers IR Service via event bus

### Exit Criteria
- No double-fire across reboot/time change under tested scenarios

---

## Sprint 5 — Authentication + Orchestrator FSM (System Control)
**Goal:** Implement capability gating and system states.

### Scope
- Orchestrator states:
  - UNAUTH, NORMAL, PROGRAMMING, UPDATING (stub)
- Capability mask gating:
  - CAN_PROGRAM, CAN_SCHEDULE, CAN_FACTORY_RESET, etc.
- Auth service:
  - verify password/token
  - session timeout + revoke on disconnect
- Events:
  - `EVT_AUTH_STATE_CHANGED`

### Deliverables
- `orchestrator.h/.c`
- `auth_system.h/.c`
- Unit tests:
  - state transitions
  - command rejects (EVT_CMD_REJECTED)
  - capability timeouts

### Exit Criteria
- Programming mode blocks scheduling and vice-versa (policy enforced)
- Clean, testable command gating

---

## Sprint 6 — BLE Comms (Minimal Vertical Slice)
**Goal:** First complete end-to-end demo path from phone to hardware.

### Scope
- Minimal GATT:
  - command write characteristic
  - response notify characteristic
  - error notify characteristic
- Transport-level security enforced (bond/encrypt)
- App-level auth characteristic (password) if desired

### Deliverables
- BLE command → CMD Service → Orchestrator → IR learn/send
- BLE notify of results/errors
- Short demo script:
  - authenticate
  - program slot
  - trigger send

### Exit Criteria
- Working demo with phone + real IR

---

## Sprint 7 — Wi-Fi Manager + MQTT (Optional MVP+)
**Goal:** Provision Wi-Fi and publish alerts.

### Scope
- Wi-Fi provisioning flow (smartconfig/captive portal/etc.)
- MQTT publish for alerts/errors (optional)
- Events:
  - `EVT_WIFI_STATE_CHANGED`, `EVT_MQTT_STATE_CHANGED`

### Exit Criteria
- Error Manager can route alerts via Wi-Fi when available

---

## Sprint 8 — OTA (Later / Post-MVP)
**Goal:** Add firmware update support with safe gating.

### Scope
- Orchestrator UPDATING state
- MCUboot-compatible flow (if chosen)
- Progress events

### Exit Criteria
- Update does not interfere with normal operation
- Rollback behavior verified

---

## Suggested Implementation Order (If You Want Fastest Value)
1) Event Bus → 2) Storage → 3) IR Service → 4) Scheduler → 5) Orchestrator/Auth → 6) BLE

Rationale:
- IR + Storage are the technical core
- Event bus makes everything composable
- Orchestrator comes after services exist to gate
- BLE last enables a real demo once internals are reliable

---

## Definition of Done (Per Sprint)
- Unit tests for core logic (host)
- Component doc updated in `docs/components/`
- One hardware smoke test (when applicable)
- All public interfaces stable and reviewed in docs
