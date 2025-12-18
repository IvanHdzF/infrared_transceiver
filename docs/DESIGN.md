# Minisplit Retrofit Architecture
## Architecture Diagram
Reference the following document: ---TODO: insert PDF hyperlink--

## High-Level Module Explanation

This system is designed as an **event-driven appliance controller** for an IR-based minisplit retrofit.  
The architecture separates **protocol handling**, **system orchestration**, **device services**, and **persistence**, with a clear split between **synchronous command handling** and **asynchronous events/errors**.

### Architectural Goals
- Support raw IR capture/replay without protocol decoding
- Allow remote programming and scheduling via BLE/Wi-Fi
- Centralize system state and security decisions
- Avoid tight coupling between services
- Be OTA-ready without architectural refactors

---

### Communication Layers (BLE / Wi-Fi)
**Responsibility**
- Own transport-level concerns: GAP/GATT, bonding, encryption, MTU, Wi-Fi provisioning
- Enforce protocol-level access control (e.g., deny GATT writes if not bonded/encrypted)
- Translate incoming messages into system commands
- Subscribe to system alerts/errors and relay them outward

**Design Rationale**
- Keeps protocol complexity out of the core system
- Allows BLE/Wi-Fi to evolve independently (e.g., new app, new backend)
- Security is enforced first at protocol level, then at system level

---

### CMD Service
**Responsibility**
- Parse validated commands from communication layers
- Provide a synchronous **CMD/RSP** interface to the Orchestrator
- Return deterministic acknowledgements or rejections

**Design Rationale**
- Separates parsing/validation from decision-making
- Prevents asynchronous event noise from polluting request/response flows

---

### Orchestrator
**Responsibility**
- Central system FSM (e.g., Unauthenticated, Normal, Programming, Updating)
- Enforce system-level authorization and capability gating
- Coordinate services (IR, Scheduler, Power, Auth)
- Decide whether an action is allowed, not how it is executed

**Design Rationale**
- Single source of truth for system state
- Avoids god objects in services
- Makes OTA, lockouts, and future modes explicit and safe

---

### Event Bus (Pub/Sub Queue)
**Responsibility**
- Transport asynchronous system events:
  - Errors and alerts
  - Connectivity changes
  - Time sync events
  - Schedule triggers
  - IR learn/send results

**Publishers**
- IR Service, Scheduler, Auth, Power, Storage, Clock

**Subscribers**
- Error Manager, BLE/Wi-Fi Comms, Orchestrator (selectively)

**Design Rationale**
- Decouples producers from consumers
- Prevents bidirectional service dependencies
- Scales cleanly as features are added

---

### Error Manager
**Responsibility**
- Subscribe to internal error/events
- Classify, rate-limit, and escalate
- Publish user-visible alerts
- Forward notifications to BLE/Wi-Fi/MQTT

**Design Rationale**
- Centralized error policy
- Avoids every service needing to know how to alert the user

---

### Infrared Service
**Responsibility**
- Capture and replay raw IR data
- Manage IR slots and associated transport metadata
- Execute routines triggered by Scheduler
- Publish learn/send results and internal faults

**Internal Layers**
- High-level: abstract actions (sleep mode, routine execution)
- Low-level: raw pulse storage and hardware TX/RX

**Design Rationale**
- Protocol-agnostic by design (supports thousands of remotes)
- Keeps all IR-specific concerns localized
- Allows future layering without breaking storage format

---

### Scheduler Service
**Responsibility**
- Manage scheduled routines
- Track last_run and recurrence behavior
- Trigger execution events when schedules are due

**Design Notes**
- MVP uses coarse polling (30–60s)
- Designed to evolve into next-deadline timers without refactor
- Relies on epoch time; no HVAC state inference

---

### Clock / RTC
**Responsibility**
- Provide current epoch time
- Emit sync and time-jump events (NTP/RTC)

**Design Rationale**
- Time correctness is centralized
- Scheduler reacts to time changes instead of guessing

---

### Authentication System
**Responsibility**
- Handle application-level authentication (e.g., password characteristic)
- Publish authentication state changes
- Grant/revoke system capabilities

**Design Rationale**
- Orthogonal to BLE/Wi-Fi security
- Allows session-based privilege escalation without protocol hacks

---

### Power Management System
**Responsibility**
- Control peripheral power states
- Manage sleep/active modes
- React to connectivity and IR activity

**Design Rationale**
- Central policy avoids scattered power decisions
- Enables battery support later without redesign

---

### Config Store
**Responsibility**
- Own typed configuration data:
  - Credentials
  - Provisioning flags
  - Auth settings
- Validate and version config data

**Design Rationale**
- Separates what is stored from how it is persisted
- Avoids leaking raw storage access into services

---

### Storage Service
**Responsibility**
- Single access point to NVM
- Atomic read/write/erase operations
- Corruption detection and reporting

**Design Rationale**
- Prevents flash contention
- Enables caching, wear management, and future migration logic

---

### Summary
This architecture intentionally favors:
- Clear ownership
- Event-driven decoupling
- Explicit system state
- Protocol agnosticism

It supports the current MVP goals while leaving clean extension points for OTA, power optimization, richer scheduling, and cloud integration — without requiring structural rewrites.


## Evt Table

| Event                                                                       | Published by                      | Subscribed by                          | Why / used for                                                              |
| --------------------------------------------------------------------------- | --------------------------------- | -------------------------------------- | --------------------------------------------------------------------------- |
| `EVT_AUTH_STATE_CHANGED`                                                    | Auth System                       | Orchestrator, BLE/Wi-Fi Comms          | Gate commands/capabilities; update UI; revoke session on logout/timeout     |
| `EVT_BLE_CONN_CHANGED` (up/down)                                            | BLE Comms                         | Orchestrator, Power Mgmt, Error/Alert  | Know when to allow interactive modes; adjust power policy; route alerts     |
| `EVT_BLE_SEC_CHANGED` (bonded/encrypted/MTU)                                | BLE Comms                         | Orchestrator, CMD Service              | Enforce “programming allowed only if encrypted”; pick payload chunk sizes   |
| `EVT_WIFI_STATE_CHANGED` (up/down/IP)                                       | Wi-Fi Manager                     | Orchestrator, Power Mgmt, Error/Alert  | Enable MQTT/OTA; adjust sleep; notify app/cloud availability                |
| `EVT_MQTT_STATE_CHANGED`                                                    | MQTT Client                       | Error/Alert, Orchestrator              | Decide if alerts can be published; backoff/retry policy                     |
| `EVT_TIME_SYNCED`                                                           | Clock Service (NTP/RTC)           | Scheduler, Orchestrator                | Recompute next deadlines; clear “clock invalid” errors                      |
| `EVT_TIME_JUMPED` (Δseconds)                                                | Clock Service                     | Scheduler, Orchestrator, Error/Alert   | Prevent double-fires; apply missed-run policy; optionally alert if big jump |
| `EVT_SCHEDULE_TABLE_UPDATED`                                                | Scheduler Service (or Config API) | Scheduler (self), Orchestrator         | Reload/rehash cache; persist; acknowledge to requester                      |
| `EVT_SCHEDULE_DUE` (schedule_id)                                            | Scheduler Service                 | IR Service, Orchestrator               | Trigger action execution; orchestrator can block if state disallows         |
| `EVT_IR_LEARN_STARTED`                                                      | IR Service (or Orchestrator)      | Power Mgmt, Error/Alert                | Ensure RX path powered; UI feedback “learning…”                             |
| `EVT_IR_LEARN_RESULT` (ok/fail + slot)                                      | IR Service                        | Orchestrator, Error/Alert, CMD Service | Commit slot / rollback; notify user; return status to requester             |
| `EVT_IR_SLOT_WRITTEN` (slot, crc)                                           | IR Service (after storage commit) | Scheduler, Error/Alert                 | Confirms slot is valid before schedules can reference it                    |
| `EVT_IR_SEND_STARTED`                                                       | IR Service                        | Power Mgmt                             | Keep rails/peripherals on during TX burst                                   |
| `EVT_IR_SEND_RESULT` (ok/fail)                                              | IR Service                        | Error/Alert, Orchestrator              | Detect TX hardware faults; update reliability metrics                       |
| `EVT_STORAGE_CORRUPT` / `EVT_STORAGE_FULL`                                  | Storage Service                   | Orchestrator, Error/Alert              | Escalate: factory reset prompt, block programming, degrade features         |
| `EVT_FACTORY_RESET_DONE`                                                    | Storage Service / Orchestrator    | All (BLE/Wi-Fi, Scheduler, IR, Auth)   | Everyone clears caches, re-inits defaults                                   |
| `EVT_POWER_MODE_CHANGED` (active/idle/sleep)                                | Power Mgmt                        | Orchestrator, IR, Scheduler, Comms     | Modules adjust behavior (e.g., stop learning, defer heavy ops)              |
| `EVT_BATTERY_STATE` (if battery)                                            | Power Mgmt                        | Orchestrator, Error/Alert              | Low-batt inhibits IR TX/OTA; alert user                                     |
| `EVT_OTA_AVAILABLE` / `EVT_OTA_START` / `EVT_OTA_PROGRESS` / `EVT_OTA_DONE` | OTA/Update Service                | Orchestrator, Error/Alert, Comms       | Drive updating state + user feedback + rollback handling                    |
| `EVT_CMD_REJECTED` (reason)                                                 | CMD Service / Orchestrator        | Error/Alert, Comms                     | Consistent user-visible errors for “denied by auth/state” cases             |
| `EVT_WATCHDOG_WARNING` / `EVT_HEALTH_TICK`                                  | System Monitor                    | Error/Alert, Orchestrator              | Detect stalls, queue overflows, heap low; improve field reliability         |
