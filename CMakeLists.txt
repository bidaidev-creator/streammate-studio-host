cmake_minimum_required(VERSION 3.25)

project(streammate-studio-host VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STREAMMATE_REQUIRE_LIBOBS "Require a built libobs SDK and call obs_startup/obs_shutdown" ON)
set(STREAMMATE_OBS_INCLUDE_DIR "" CACHE PATH "Path to libobs headers, typically external/obs-studio/libobs")
set(STREAMMATE_OBS_LIBRARY "" CACHE FILEPATH "Path to the built libobs library")
set(STREAMMATE_OBS_DEPS_INCLUDE_DIR "" CACHE PATH "Path to OBS dependency headers, such as SIMDe")
set(STREAMMATE_OBS_DEPS_LIBRARY_DIR "" CACHE PATH "Path to OBS dependency libraries")
set(STREAMMATE_OBS_GENERATED_INCLUDE_DIR "" CACHE PATH "Path to generated OBS headers, such as obsconfig.h")

# Phase B native OverlayAction renderer core (Spec 34 Capability 2 / chunk 34.H2).
# A plain host-repo library, separable from the host dispatch and from the future
# 34.H3 in-tree OBS plugin module (keeps either Q-121 packaging answer cheap).
# It runs fully in scaffold mode (no libobs); on Apple it is backed by a
# CoreGraphics CGBitmapContext surface.
add_library(native-overlay-renderer STATIC src/native_overlay_renderer.cpp)
target_include_directories(native-overlay-renderer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(APPLE)
  target_link_libraries(native-overlay-renderer PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
endif()

add_executable(studio-host src/studio_host.cpp)
add_executable(studio-host-smoke src/studio_host_smoke.cpp)

target_link_libraries(studio-host PRIVATE native-overlay-renderer)

foreach(target studio-host studio-host-smoke)
  target_compile_definitions(${target} PRIVATE
    STREAMMATE_STUDIO_HOST_VERSION="${PROJECT_VERSION}"
  )
endforeach()

if(STREAMMATE_REQUIRE_LIBOBS)
  if(NOT STREAMMATE_OBS_INCLUDE_DIR OR NOT STREAMMATE_OBS_LIBRARY)
    message(FATAL_ERROR "STREAMMATE_REQUIRE_LIBOBS=ON requires STREAMMATE_OBS_INCLUDE_DIR and STREAMMATE_OBS_LIBRARY")
  endif()

  foreach(target studio-host studio-host-smoke)
    target_compile_definitions(${target} PRIVATE STREAMMATE_HAS_LIBOBS=1)
    target_include_directories(${target} PRIVATE "${STREAMMATE_OBS_INCLUDE_DIR}")
    if(STREAMMATE_OBS_DEPS_INCLUDE_DIR)
      target_include_directories(${target} PRIVATE "${STREAMMATE_OBS_DEPS_INCLUDE_DIR}")
    endif()
    if(STREAMMATE_OBS_GENERATED_INCLUDE_DIR)
      target_include_directories(${target} PRIVATE "${STREAMMATE_OBS_GENERATED_INCLUDE_DIR}")
    endif()
    target_link_libraries(${target} PRIVATE "${STREAMMATE_OBS_LIBRARY}")
    if(APPLE)
      target_link_libraries(${target} PRIVATE "-framework CoreGraphics")
    endif()
    if(APPLE AND IS_DIRECTORY "${STREAMMATE_OBS_LIBRARY}" AND STREAMMATE_OBS_LIBRARY MATCHES "\\.framework$")
      get_filename_component(STREAMMATE_OBS_FRAMEWORK_DIR "${STREAMMATE_OBS_LIBRARY}" DIRECTORY)
      set(STREAMMATE_STUDIO_HOST_RPATHS "${STREAMMATE_OBS_FRAMEWORK_DIR}")
      if(STREAMMATE_OBS_DEPS_LIBRARY_DIR)
        list(APPEND STREAMMATE_STUDIO_HOST_RPATHS "${STREAMMATE_OBS_DEPS_LIBRARY_DIR}")
      endif()
      set_target_properties(${target} PROPERTIES BUILD_RPATH "${STREAMMATE_STUDIO_HOST_RPATHS}")
    endif()
  endforeach()

  # Spec 34 Capability 3 / chunk 34.H3 — the in-tree OBS plugin module for the
  # Phase B native renderer (ADR-0005 Decision 1: modules in our own source
  # tree, loaded by studio-host; never a submodule patch). A CMake MODULE
  # library producing a <name>.plugin CFBundle that libobs loads through the
  # host's normal module load path. The 34.H2 rasterizer core stays a plain
  # shared library (native-overlay-renderer) linked here rather than forked, so
  # either Q-121 packaging answer stays cheap. Built only under HAS_LIBOBS.
  set(STREAMMATE_OBS_INCLUDE_DIRS "${STREAMMATE_OBS_INCLUDE_DIR}")
  if(STREAMMATE_OBS_DEPS_INCLUDE_DIR)
    list(APPEND STREAMMATE_OBS_INCLUDE_DIRS "${STREAMMATE_OBS_DEPS_INCLUDE_DIR}")
  endif()
  if(STREAMMATE_OBS_GENERATED_INCLUDE_DIR)
    list(APPEND STREAMMATE_OBS_INCLUDE_DIRS "${STREAMMATE_OBS_GENERATED_INCLUDE_DIR}")
  endif()

  add_library(streammate-native-overlay MODULE src/obs_module/native_overlay_obs_module.cpp)
  set_target_properties(streammate-native-overlay PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    PREFIX ""
    OUTPUT_NAME streammate-native-overlay
  )
  target_compile_definitions(streammate-native-overlay PRIVATE STREAMMATE_HAS_LIBOBS=1)
  target_include_directories(streammate-native-overlay PRIVATE ${STREAMMATE_OBS_INCLUDE_DIRS})
  target_link_libraries(streammate-native-overlay PRIVATE native-overlay-renderer "${STREAMMATE_OBS_LIBRARY}")

  # HAS_LIBOBS smoke: loads the module through libobs, creates the registered
  # source, applies a fixture toast, drives one async render cycle, asserts a
  # frame was delivered and that its hash equals the shared scaffold golden.
  add_executable(native-overlay-module-smoke src/obs_module/native_overlay_module_smoke.cpp)
  target_compile_definitions(native-overlay-module-smoke PRIVATE STREAMMATE_HAS_LIBOBS=1)
  target_include_directories(native-overlay-module-smoke PRIVATE ${STREAMMATE_OBS_INCLUDE_DIRS})
  target_link_libraries(native-overlay-module-smoke PRIVATE native-overlay-renderer "${STREAMMATE_OBS_LIBRARY}")

  if(APPLE AND STREAMMATE_STUDIO_HOST_RPATHS)
    # Both the dlopen-ed module and the smoke need to locate libobs.framework
    # (and its deps) at load time via @rpath.
    set_target_properties(streammate-native-overlay PROPERTIES BUILD_RPATH "${STREAMMATE_STUDIO_HOST_RPATHS}")
    set_target_properties(native-overlay-module-smoke PROPERTIES BUILD_RPATH "${STREAMMATE_STUDIO_HOST_RPATHS}")
  endif()

  # Spec 39 / NIF-H2 — redistributable in-tree TEST plugins (user-plugin load
  # proof) + the loads-but-registers-nothing fixture + the instance+render
  # probe smoke. These MODULE bundles are NEVER staged into the app bundle's
  # Contents/PlugIns/obs-plugins: they ship in the CI artifact's test-plugins/
  # dir and load only through the --user-plugins-manifest path.
  foreach(test_module streammate-test-source streammate-test-filter streammate-test-noop)
    string(REPLACE "-" "_" test_module_src "${test_module}")
    add_library(${test_module} MODULE src/obs_module/${test_module_src}_module.cpp)
    set_target_properties(${test_module} PROPERTIES
      BUNDLE TRUE
      BUNDLE_EXTENSION plugin
      PREFIX ""
      OUTPUT_NAME ${test_module}
    )
    target_compile_definitions(${test_module} PRIVATE STREAMMATE_HAS_LIBOBS=1)
    target_include_directories(${test_module} PRIVATE ${STREAMMATE_OBS_INCLUDE_DIRS})
    target_link_libraries(${test_module} PRIVATE "${STREAMMATE_OBS_LIBRARY}")
    if(APPLE AND STREAMMATE_STUDIO_HOST_RPATHS)
      set_target_properties(${test_module} PROPERTIES BUILD_RPATH "${STREAMMATE_STUDIO_HOST_RPATHS}")
    endif()
  endforeach()

  add_executable(user-plugin-module-smoke src/obs_module/user_plugin_module_smoke.cpp)
  target_compile_definitions(user-plugin-module-smoke PRIVATE STREAMMATE_HAS_LIBOBS=1)
  target_include_directories(user-plugin-module-smoke PRIVATE ${STREAMMATE_OBS_INCLUDE_DIRS})
  target_link_libraries(user-plugin-module-smoke PRIVATE "${STREAMMATE_OBS_LIBRARY}")
  if(APPLE AND STREAMMATE_STUDIO_HOST_RPATHS)
    set_target_properties(user-plugin-module-smoke PROPERTIES BUILD_RPATH "${STREAMMATE_STUDIO_HOST_RPATHS}")
  endif()
else()
  foreach(target studio-host studio-host-smoke)
    target_compile_definitions(${target} PRIVATE STREAMMATE_HAS_LIBOBS=0)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)

  add_executable(native-overlay-renderer-test tests/unit/test_native_overlay_renderer.cpp)
  target_link_libraries(native-overlay-renderer-test PRIVATE native-overlay-renderer)
  add_test(
    NAME native-overlay-renderer
    COMMAND native-overlay-renderer-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/native_overlay_golden.txt
  )

  add_test(
    NAME studio-host-lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_host_lifecycle.py $<TARGET_FILE:studio-host>
  )
  add_test(
    NAME record-replay
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_record_replay.py $<TARGET_FILE:studio-host>
  )
  # Chunk 37.H1: per-finding security regressions (PR07-07 / PR07-08 / NHR-01).
  add_test(
    NAME import-hardening
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_import_hardening.py $<TARGET_FILE:studio-host>
  )
  add_test(
    NAME production-control-verbs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_production_control_verbs.py $<TARGET_FILE:studio-host>
  )
  # Chunk 36.6: host.hello declares supportedCommands derived from the dispatch
  # table (capability truth, D36-2 / Capability 6).
  add_test(
    NAME hello-capabilities
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_hello_capabilities.py $<TARGET_FILE:studio-host>
  )
  # Chunk NIF-H1: user plugin discovery + static probe (manifest-driven
  # inventory; sha256 + Mach-O arch read from header bytes; no loading).
  add_test(
    NAME plugin-discovery
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_plugin_discovery.py $<TARGET_FILE:studio-host>
  )
  # NIF-H2: real user-plugin loading plan/report honesty (scaffold lane) +
  # static contracts for the in-tree test plugins and the CI loading lane.
  add_test(
    NAME user-plugin-loading
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_user_plugin_loading.py $<TARGET_FILE:studio-host>
  )
  add_test(
    NAME macos-packaging
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_macos_packaging.py
  )
  add_test(
    NAME macos-ci-workflow
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_macos_ci_workflow.py
  )
  add_test(
    NAME native-overlay-module
    COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration -p test_native_overlay_module.py
  )
  set_tests_properties(native-overlay-module PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

install(TARGETS studio-host studio-host-smoke RUNTIME DESTINATION bin)
