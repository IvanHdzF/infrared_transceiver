# Control Plane & Async Workflow Decisions (MVP)

This document freezes the **MVP-level architectural decisions** for the Infrared Transceiver Platform,
specifically around **commands, asynchronous results, the orchestrator, and the event bus**.

The goal is to:
- Avoid over‑engineering
- Preserve a clean upgrade path
- Make implementation decisions explicit and non-ambiguous

---

## 1. Core Architectural Principles

### 1.1 Control Plane vs Async Results
- **Commands (CMD_*)** are synchronous requests handled via the CMD Service and Orchestrator.
- **Events (EVT_*)** are asynchronous facts published by the module that performed the action.
- **Commands NEVER traverse the Event Bus.**
- **Async results are events.**

This separation is absolute.

---

### 1.2 Admission vs Completion
All commands follow a two‑phase model:

1. **Admission (synchronous)**
   - Auth, state, argument validation
   - Resource availability checks
   - Immediate ACK / REJECT

2. **Completion (asynchronous)**
   - Actual execution result
   - Delivered as EVT_*_RESULT events

---

## 2. Orchestrator (MVP)

### 2.1 Orchestrator States (Frozen)

- `ORCH_BOOT`
- `ORCH_LOCKED` (unauthenticated)
- `ORCH_NORMAL`

Future (not implemented yet):
- `ORCH_UPDATING`
- `ORCH_PROGRAMMING`
- `ORCH_DEGRADED`

---

### 2.2 Orchestrator Responsibilities
- Gate all commands based on:
  - Current state
  - Authentication
  - Capability rules
- Allocate `op_id` for accepted commands
- Invoke service APIs (non‑blocking)
- Publish `EVT_ORCH_STATE_CHANGED`

The orchestrator **does not publish service lifecycle events**.

---

## 3. Commands (MVP Surface)

### 3.1 Supported Commands (Frozen)

- `CMD_AUTH_LOGIN`
- `CMD_AUTH_LOGOUT`
- `CMD_IR_LEARN_START(slot_id)`
- `CMD_IR_LEARN_CANCEL(op_id)`
- `CMD_IR_SEND(slot_id)`
- `CMD_SCHED_SET_TABLE(...)` (optional MVP)
- `CMD_FACTORY_RESET` (optional MVP)

---

### 3.2 Command Response Model

All commands return synchronously:

```
RSP_ACCEPTED(op_id)
RSP_REJECTED(reason)
```

Where `reason` ∈:
- `REJECTED_AUTH`
- `REJECTED_STATE`
- `INVALID_ARG`
- `BUSY`
- `NOT_SUPPORTED`
- `INTERNAL_ERR`

`RSP_ACCEPTED(op_id)` acts as a **"waiting_rsp"** acknowledgement.

---

## 4. op_id (Correlation ID)

### 4.1 Rules
- Every accepted command allocates an `op_id` (u32).
- All async result events **must carry op_id**.
- op_id is used by Comms to route async results back to the initiating client.

### 4.2 op_id Lifetime
- Created at admission
- Retired after final RESULT event
- Fixed-size table (no heap)

---

## 5. Event Bus Usage (MVP)

### 5.1 What Goes on the Event Bus
- System state changes
- Auth changes
- Async service results
- Scheduler triggers

### 5.2 What Does NOT Go on the Event Bus
- Commands
- Synchronous command responses

---

## 6. MVP Event Set (Frozen)

### Core
- `EVT_ORCH_STATE_CHANGED(old, new, reason)`
- `EVT_AUTH_STATE_CHANGED(authed, reason, op_id?)`

### IR
- `EVT_IR_LEARN_RESULT(op_id, slot_id, ok, err)`
- `EVT_IR_SEND_RESULT(op_id, slot_id, ok, err)`

### Scheduler (if enabled)
- `EVT_SCHEDULE_TABLE_COMMITTED(op_id, version/hash)`
- `EVT_SCHEDULE_DUE(schedule_id)`

### Storage (minimal)
- `EVT_STORAGE_STATUS(domain, status, severity)`

---

## 7. IR Learn Workflow (Canonical)

1. Client sends `CMD_IR_LEARN_START(slot)`
2. CMD Service → Orchestrator
3. Orchestrator:
   - validates auth/state/slot/busy
   - allocates op_id
   - calls `ir_learn_start(op_id, slot)`
4. Returns `RSP_ACCEPTED(op_id)`
5. IR Service performs capture
6. IR Service publishes:
   - `EVT_IR_LEARN_RESULT(op_id, slot, ok, err)`
7. Comms routes result to originating client using op_id

Cancel:
- `CMD_IR_LEARN_CANCEL(op_id)`
- Result event returns `err=CANCELLED`

---

## 8. Scheduler Workflow (MVP)

- `CMD_SCHED_SET_TABLE(...)`:
  - Admission via orchestrator
  - Async commit result via `EVT_SCHEDULE_TABLE_COMMITTED(op_id, ...)`
- Scheduler emits:
  - `EVT_SCHEDULE_DUE(schedule_id)`
- Orchestrator may gate execution before invoking IR

---

## 9. Comms Layer Responsibilities

- Enforce transport security (bonding/encryption)
- Reject writes before CMD layer if security fails
- Maintain `op_id → client` routing table
- Subscribe to EVT_*_RESULT events
- Forward async results via notify/indication / socket

No service needs to know BLE vs Wi‑Fi.

---

## 10. Explicit Non‑Goals (MVP)

Not implemented yet:
- Dedicated Error Manager service
- Power Management arbitration
- OTA lifecycle
- Multi‑queue event bus
- Event flooding protection beyond basic rate limiting

These can be added **without changing any frozen interface** above.

---

## 11. Key Invariants (Do Not Break)

- Commands are synchronous and never go on the event bus
- Services publish their own result events
- Orchestrator never fakes service lifecycle
- op_id is mandatory for async completion
- Event bus is control‑plane, not request‑plane

---

**This document represents the MVP architectural freeze point.**
