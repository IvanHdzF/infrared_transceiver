cmake_minimum_required(VERSION 3.16)

# Choose which app to build
set(APP_NAME "test_ir_ctrl" CACHE STRING "Select which application to build")
message(STATUS "Building application: ${APP_NAME}")

# Paths
set(APP_PATH ${CMAKE_SOURCE_DIR}/apps/${APP_NAME})
set(CUSTOM_ROOT_PATH ${CMAKE_SOURCE_DIR})
message(STATUS "Root path: ${CUSTOM_ROOT_PATH}")

# Validate app folder
if(NOT EXISTS ${APP_PATH}/CMakeLists.txt)
    message(FATAL_ERROR "Invalid APP_NAME: ${APP_NAME}. Folder not found at ${APP_PATH}")
endif()

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# -------------------- SDKCONFIG DEFAULTS (PER APP) --------------------
# Always include root defaults if you have them, then app-specific defaults if present.
set(_root_defaults "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
set(_app_defaults  "${APP_PATH}/sdkconfig.defaults")

set(SDKCONFIG_DEFAULTS "")

if(EXISTS "${_root_defaults}")
    list(APPEND SDKCONFIG_DEFAULTS "${_root_defaults}")
endif()

if(EXISTS "${_app_defaults}")
    list(APPEND SDKCONFIG_DEFAULTS "${_app_defaults}")
endif()

# Helpful debug
message(STATUS "SDKCONFIG_DEFAULTS: ${SDKCONFIG_DEFAULTS}")
# ---------------------------------------------------------------------

# Integrate Middlewares
# Use separate variables per app
# Declared here because it gives compilation errors when being included in apps folder CMakeLists.txt
set(APP_COMPONENTS_infrared_test "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")
set(APP_COMPONENTS_test_evt_bus "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")
set(APP_COMPONENTS_system_demo "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")
set(APP_COMPONENTS_test_orchestrator "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")
set(APP_COMPONENTS_test_stg_man "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")
set(APP_COMPONENTS_test_ir_ctrl "${CUSTOM_ROOT_PATH}/externals/embedded_evt_bus/ports/esp-idf/evt_bus")

# Function to get components for app
function(get_app_components app_name out_var)
    set(var_name APP_COMPONENTS_${app_name})
    if(DEFINED ${var_name})
        set(components ${${var_name}})
        set(${out_var} ${components} PARENT_SCOPE)
        message(STATUS "Found special requirements for app ${app_name}: ${components}")
    else()
        message(WARNING "No special requirements defined for app ${app_name}")
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# Get middleware/components for this app
get_app_components(${APP_NAME} APP_REQUIREMENTS)
message(STATUS "App requirements: ${APP_REQUIREMENTS}")

# Include the app itself in EXTRA_COMPONENT_DIRS
set(EXTRA_COMPONENT_DIRS ${APP_PATH} ${APP_REQUIREMENTS})
message(STATUS "App - Extra component dirs: ${EXTRA_COMPONENT_DIRS}")

# Include ESP-IDF project
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fw)
