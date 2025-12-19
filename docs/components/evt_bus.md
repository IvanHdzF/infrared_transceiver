# Event Bus (evt_bus)

## Overview
The event bus provides a lightweight, deterministic publish–subscribe mechanism designed for embedded systems.

Core principles:
- **Publish = enqueue** (no callbacks in publisher/ISR context)
- **Dispatch = single worker context** (predictable callback execution)
- **O(1) unsubscribe** using handles
- **Bounded resources** (no heap, fixed limits)
- **Self-healing** subscription lists (stale handles are cleaned automatically)

This component is designed as:
- a **platform-agnostic core**, plus
- **thin platform bindings** (e.g., FreeRTOS)

This avoids locking the API to any RTOS while still providing first-class FreeRTOS ergonomics.

---

## Architecture

### Module split

#### `evt_bus_core.[ch]` (platform-agnostic)
Responsibilities:
- handle table + event subscription table
- handle validation (index + generation)
- subscribe / unsubscribe semantics
- dispatch fanout logic (evt_id → handles → callbacks)
- self-healing rules (cleanup stale handles)

Does **not** depend on:
- FreeRTOS headers/types
- dynamic allocation
- platform queues/mutexes directly

#### `evt_bus_port_freertos.[ch]` (optional binding)
Responsibilities:
- queue backend using FreeRTOS (`xQueue*` or task notifications)
- dispatcher task wrapper (`xTaskCreate`, blocking `xQueueReceive`)
- ISR-safe publish helper (`xQueueSendFromISR`)
- mutex/critical-section mapping for table mutations (if needed)

Other ports can be added similarly (Zephyr, bare-metal ringbuf, Linux-host test shim).

---

## Public API (Core)

```c
void evt_bus_init(void);

evt_handle_t evt_bus_subscribe(evt_id_t evt_id, evt_cb_t cb);

void evt_bus_unsubscribe(evt_handle_t handle);

/* Enqueue an event for later dispatch (payload model defined below). */
bool evt_bus_publish(evt_id_t evt_id, const void *payload, size_t payload_len);

/* Dispatch helpers (called by the platform binding / user loop). */
bool evt_bus_dispatch_one(void);   /* returns false if no event available */
void evt_bus_dispatch_all(void);   /* drains until empty */
```

Notes:
- The core exposes dispatch entry points so that **either**:
  - a platform task can block on its queue and call `dispatch_one()`, **or**
  - bare-metal can poll and call `dispatch_all()` in the main loop.

---

## Optional API (FreeRTOS Port)

```c
bool evt_bus_freertos_start_dispatch_task(UBaseType_t prio, uint16_t stack_words);

bool evt_bus_freertos_publish_from_isr(evt_id_t evt_id, const void *payload, size_t payload_len);
```

Notes:
- This port owns the queue object and the dispatcher task.
- The port should keep FreeRTOS types **out of the core**.

---

## Execution Model

- `publish()` **never executes callbacks**
- `publish()` enqueues `(evt_id, payload)` into a bounded queue
- A dispatcher context dequeues events and executes callbacks
- All callbacks run in the dispatcher context

ISR usage:
- Only enqueue-only functions may be ISR-safe (port dependent)
- `subscribe()` / `unsubscribe()` are **not ISR-safe**

---

## Data Model

The event bus uses three core structures:

### 1) Handle Table (global)
Indexed by handle index.

Each entry contains:
- callback function pointer
- active flag
- generation counter

Purpose:
- fast validation of handles
- prevents stale-handle reuse bugs

### 2) Event Subscription Table
Indexed by `evt_id`.

Each event contains a **fixed-size array of handles**:
```
evt_id -> [handle_0, handle_1, ... handle_N]
```
- Maximum subscribers per event is compile-time bounded
- Entries may temporarily contain stale handles

### 3) Event Queue
A bounded queue storing published events:
- `evt_id`
- payload (copied or referenced, per configuration)

Overflow policy is defined by configuration (see below).

---

## Handle Model (Index + Generation)

Handles are encoded as:
```
handle = { index, generation }
```
(typically packed into a `uint16_t`/`uint32_t`)

Rules:
- Each handle slot has a generation counter
- Generation increments whenever the slot is freed/reused
- A handle is valid **only if index exists AND generation matches**

This prevents:
- old handles unsubscribing new subscribers after slot reuse
- dispatch calling the wrong callback after reuse

---

## Subscribe Semantics

- Allocates a free handle slot
- **Repairs** the event’s subscription list before inserting:
  - clears stale or invalid handles
- Inserts handle into the first free slot
- Returns an opaque handle to the caller

Failure cases:
- no free handle slots
- event subscription list full (after repair)

Duplicates:
- By default, duplicates for the same `(evt_id, handle)` should be rejected (bounded scan).

---

## Unsubscribe Semantics (Lazy)

- `unsubscribe(handle)`:
  - marks handle slot inactive
  - increments generation counter
- Does **not** immediately remove handle from event lists

Rationale:
- O(1) unsubscribe
- avoids scanning event tables
- safe with generation validation

---

## Self-Healing Strategy

Stale handles are cleaned automatically in two places:

### During Dispatch
For each handle in `evt_id -> handles[]`:
- validate `(index, generation)` against handle table
- if invalid:
  - skip callback
  - clear the handle slot in the event list (self-heal)

### During Subscribe
Before inserting into `evt_id -> handles[]`:
- scan and remove invalid/stale handles
- ensures you can insert even if the list was previously filled by dead entries

Guarantees:
- event lists do not permanently fill with dead entries
- no incorrect callback execution

---

## Publish Semantics

- `publish()` enqueues an event
- returns failure if queue is full (policy-dependent)
- does not interact with subscription tables

Queue overflow policy (compile-time; pick one):
- **DROP_NEW**: fail the publish if full
- **DROP_OLD**: overwrite oldest
- **COALESCE_PER_EVT** (optional): keep only latest per `evt_id`

Recommended default for embedded determinism: **DROP_NEW** + explicit error counter.

---

## Payload Ownership

Choose one payload model (this is part of the contract):

1) **Copy-in** (recommended)
- queue stores bytes inline (fixed max size)
- safest and easiest to reason about
- bounded memory

2) **Pointer**
- queue stores pointer + length
- publisher must guarantee lifetime until dispatch
- fastest but easiest to misuse

3) **Pool-backed**
- queue stores pointer to pool block
- publish allocates from a static pool, dispatch releases
- safe + fast, slightly more code

---

## Threading and Safety Rules

Core expectations:
- callbacks always run in dispatcher context
- publish path is thread-safe (via queue backend)
- subscribe/unsubscribe must not race unless protected by the port’s lock/critical section

Reentrancy:
- callbacks may call `unsubscribe(self)`
- dispatch should use either:
  - per-event handle snapshot (local copy), or
  - tolerate tombstoning while scanning (with generation validation)

---

## Configuration and Limits

All limits are compile-time constants:
- `MAX_EVT`
- `MAX_HANDLES`
- `MAX_SUBS_PER_EVT`
- `QUEUE_DEPTH`
- `MAX_PAYLOAD_SIZE` (if copy-in)

Complexity:
- `publish()` → O(1)
- `unsubscribe()` → O(1)
- `dispatch(evt)` → O(MAX_SUBS_PER_EVT)

---

## Behavioral Guarantees

- no callbacks after successful unsubscribe (generation validation)
- no stale handle can execute a new subscriber’s callback
- deterministic execution time per event
- no heap allocation in core
- platform portability (core has no RTOS dependencies)

---

## Non-Goals

- priority-based dispatch
- dynamic resizing
- topic strings / wildcard routing
- broadcast to unbounded subscribers
- executing callbacks in ISR context