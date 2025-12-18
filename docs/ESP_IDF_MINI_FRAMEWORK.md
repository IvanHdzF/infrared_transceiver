# ESP-IDF Mini Framework

## Overview

This project uses a lightweight **ESP-IDF mini-framework** to support multiple independent applications within a single firmware repository.

The framework is intentionally simple:
- A single top-level `CMakeLists.txt` selects which application to build
- Applications are isolated in `apps/<app_name>/`
- Core system components are always compiled
- Optional middleware components are included only when required by an application

The goal is to:
- Enable rapid prototyping of system components
- Allow vertical “demo apps” for verification
- Keep ESP-IDF build behavior predictable
- Avoid fragile cross-app dependencies

---

## Design Goals

- Single firmware repository with multiple applications
- Explicit application selection at build time
- Reusable system components
- Optional middleware inclusion
- Minimal ESP-IDF build hacks
- Clear separation between system, middleware, and application code

This is not a general-purpose framework; it is a pragmatic structure optimized for ESP-IDF quirks.

---

## Repository Structure
```
apps/
  system_demo/
  infrared_test/
components/
  event_bus/
  storage/
  ir_service/
  scheduler/
  orchestrator/
  auth/
  error_manager/
middlewares/
  ir_generic/
  rtc_generic/
managed_components/
  <esp-idf managed libraries>
```
---

## Application Selection Mechanism

The top-level `CMakeLists.txt` selects which application to build using a cache variable:

set(APP_NAME "infrared_test" CACHE STRING "Select which application to build")

Applications are selected at build time:

idf.py -DAPP_NAME=system_demo build

The build system validates that:
```
apps/${APP_NAME}/CMakeLists.txt
```

exists, otherwise the build fails early.

---

## Component Inclusion Model

### System Components

All modules under `components/` are always compiled.

These include:
- Orchestrator
- Event Bus
- Storage Service
- Scheduler
- Authentication System
- Error Manager

This simplifies linking and avoids conditional compilation across system boundaries.

Unused functionality may later be optimized out via:
- link-time optimization
- Kconfig feature flags

---

### Middleware Components

Middleware modules are:
- Optional
- Selected per application
- Ideally independent of ESP-IDF and system logic

Middleware inclusion is declared centrally in the root `CMakeLists.txt`, not inside application CMake files.

Example:

```
set(APP_COMPONENTS_infrared_test "")
```
A helper function resolves middleware for the selected application:

```
get_app_components(${APP_NAME} APP_REQUIREMENTS)
set(EXTRA_COMPONENT_DIRS ${APP_PATH} ${APP_REQUIREMENTS})
```
---

## Application CMakeLists.txt Responsibilities

Each application:
- Declares its source files
- Declares ESP-IDF–specific dependencies
- Does not modify global build configuration

Typical responsibilities:

```
idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS "."
  REQUIRES esp_driver_rmt
)
```

---

## Application Entry Point

Each application provides its own `app_main()`.

This enables:
- Hardware bring-up tests
- Vertical verification of individual services
- Minimal demo applications
- A future integration-oriented `system_demo`

---

## Design Rationale

### Why multiple applications instead of runtime selection?

- ESP-IDF build times are significant
- Static linking simplifies reasoning
- Each application remains self-contained
- Reduces conditional runtime logic

---

### Why always compile system components?

- Avoids complex dependency graphs
- Prevents fragile `#ifdef`-driven designs
- Improves testability and reuse
- Keeps module interfaces stable

---

### Why middleware selection at the root level?

- ESP-IDF build quirks with nested component directories
- Centralized visibility of application dependencies
- Easier auditing of what each app pulls in

---

## Current State vs Future Enhancements

### Current (Intentional MVP)

- Manual middleware registration in root `CMakeLists.txt`
- All system components compiled
- Application-level entry points

---

### Potential Future Enhancements

- Kconfig-driven feature enable/disable
- Per-application build presets
- Middleware dependency auto-resolution
- Common `system_init()` helper for integration apps
- CI matrix builds for multiple applications

These are deferred intentionally to keep the framework simple and robust.

---

## Summary

This mini-framework prioritizes:
- Predictability over flexibility
- Explicit configuration over automation
- Reusability over clever abstractions

It enables fast iteration on embedded system components while staying within ESP-IDF’s constraints and avoiding build-system fragility.
