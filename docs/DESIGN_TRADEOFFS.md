# Design Tradeoffs and Alternatives Considered

This document captures key architectural and implementation tradeoffs considered during the design of the IR Minisplit Retrofit Controller.  
The goal is to make design intent explicit, document rejected alternatives, and justify decisions under system constraints.

---

## 1. IR Protocol Decoding vs Raw IR Capture

### Options Considered
- Decode known IR protocols (e.g., NEC, Daikin, Mitsubishi)
- Capture and replay raw IR pulses without semantic interpretation

### Decision
**Raw IR capture and replay was selected.**

### Rationale
- Minisplit remotes use a wide variety of proprietary and undocumented protocols.
- Supporting protocol decoding would significantly increase implementation complexity and maintenance burden.
- Raw capture guarantees compatibility with a large number of controllers without firmware changes.

### Consequences
- The system does not infer HVAC state (temperature, mode, fan).
- Slot semantics are managed externally by the mobile application.
- Absolute-state IR frames are preferred over relative commands.

---

## 2. Centralized Orchestrator FSM vs Distributed Control

### Options Considered
- Each service independently gating behavior based on local state
- Central Orchestrator owning global system state and permissions

### Decision
**A centralized Orchestrator FSM was chosen.**

### Rationale
- Makes system modes (Unauthenticated, Normal, Programming, Updating) explicit.
- Prevents inconsistent state handling across services.
- Simplifies security and update-related lockouts.

### Consequences
- Orchestrator becomes a critical coordination point.
- Requires careful interface definition to avoid becoming a “god object”.

---

## 3. Event-Driven Pub/Sub vs Direct Inter-Service Calls

### Options Considered
- Direct synchronous calls between services
- Asynchronous event bus for errors, state changes, and results

### Decision
**Hybrid approach:**
- Synchronous CMD/RSP path for user-initiated commands
- Asynchronous Event Bus for system events and errors

### Rationale
- Avoids tight coupling between producers and consumers.
- Prevents error-handling logic from leaking into all modules.
- Scales cleanly as new features are added (OTA, power management, cloud logging).

### Consequences
- Requires event classification and rate limiting.
- Debugging requires tracing both command paths and event flows.

---

## 4. Scheduler Polling vs Next-Deadline Timers

### Options Considered
- Periodic polling of schedule table (e.g., every 30–60 seconds)
- Dynamic one-shot timers scheduled to the next deadline

### Decision
**Coarse polling selected for MVP; architecture supports later upgrade.**

### Rationale
- Polling is simple, robust, and easy to reason about.
- Power impact is acceptable for mains-powered MVP.
- Next-deadline timers introduce complexity around time jumps and rescheduling.

### Consequences
- Reduced time resolution in MVP.
- Explicit upgrade path without architectural changes.

---

## 5. Single Storage Service vs Direct Flash Access

### Options Considered
- Allowing each module to access NVM directly
- Centralized Storage Service mediating all persistence

### Decision
**Single Storage Service selected.**

### Rationale
- Prevents flash contention and partial writes.
- Enables atomic updates and integrity checks.
- Simplifies future migration, caching, and wear management.

### Consequences
- Modules must define clear storage schemas.
- Storage becomes a shared dependency.

---

## 6. Application-Level Authentication vs Protocol-Only Security

### Options Considered
- Rely exclusively on BLE bonding/encryption
- Add application-level authentication and capability gating

### Decision
**Layered security approach adopted.**

### Rationale
- BLE/Wi-Fi enforce transport security.
- Application-level authentication enables session-based privileges and fine-grained capability control.
- Allows explicit lockout during sensitive modes (Programming, Updating).

### Consequences
- Authentication state must be managed and revoked on timeout/disconnect.
- Adds complexity but improves security clarity.

---

## 7. Timezone/DST Handling on Device vs External Epoch Control

### Options Considered
- Full timezone/DST handling on device
- Rely on epoch time supplied by application/backend

### Decision
**Epoch-based scheduling with external timezone management.**

### Rationale
- Simplifies device logic.
- Avoids DST-related edge cases on constrained devices.
- Delegates localization complexity to higher-level systems.

### Consequences
- Application/backend must update schedules when timezone changes.
- Device remains timezone-agnostic.

---

## 8. HVAC State Tracking vs Stateless IR Execution

### Options Considered
- Maintain internal HVAC state model
- Treat IR execution as stateless command playback

### Decision
**Stateless IR execution selected.**

### Rationale
- Minisplit units often do not expose state feedback.
- Physical remote usage can desynchronize internal state.
- Absolute-state IR frames reduce drift risk.

### Consequences
- System does not infer current temperature/mode.
- Application remains the source of semantic meaning.

---

## Summary

The system favors:
- Explicit state modeling over implicit behavior
- Simplicity and robustness over speculative features
- Clear ownership and extensibility over monolithic design

These tradeoffs were selected to support a maintainable MVP while preserving clean paths for future extension.
