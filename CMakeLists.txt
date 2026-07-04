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

add_executable(studio-host src/studio_host.cpp)
add_executable(studio-host-smoke src/studio_host_smoke.cpp)

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
else()
  foreach(target studio-host studio-host-smoke)
    target_compile_definitions(${target} PRIVATE STREAMMATE_HAS_LIBOBS=0)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME studio-host-lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_host_lifecycle.py $<TARGET_FILE:studio-host>
  )
  add_test(
    NAME macos-packaging
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_macos_packaging.py
  )
  add_test(
    NAME macos-ci-workflow
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_macos_ci_workflow.py
  )
endif()

install(TARGETS studio-host studio-host-smoke RUNTIME DESTINATION bin)
