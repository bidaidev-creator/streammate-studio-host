cmake_minimum_required(VERSION 3.25)

project(streammate-studio-host VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STREAMMATE_REQUIRE_LIBOBS "Require a built libobs SDK and call obs_startup/obs_shutdown" ON)
set(STREAMMATE_OBS_INCLUDE_DIR "" CACHE PATH "Path to libobs headers, typically external/obs-studio/libobs")
set(STREAMMATE_OBS_LIBRARY "" CACHE FILEPATH "Path to the built libobs library")
set(STREAMMATE_OBS_DEPS_INCLUDE_DIR "" CACHE PATH "Path to OBS dependency headers, such as SIMDe")
set(STREAMMATE_OBS_GENERATED_INCLUDE_DIR "" CACHE PATH "Path to generated OBS headers, such as obsconfig.h")

add_executable(studio-host-smoke src/studio_host_smoke.cpp)
target_compile_definitions(studio-host-smoke PRIVATE
  STREAMMATE_STUDIO_HOST_VERSION="${PROJECT_VERSION}"
)

if(STREAMMATE_REQUIRE_LIBOBS)
  if(NOT STREAMMATE_OBS_INCLUDE_DIR OR NOT STREAMMATE_OBS_LIBRARY)
    message(FATAL_ERROR "STREAMMATE_REQUIRE_LIBOBS=ON requires STREAMMATE_OBS_INCLUDE_DIR and STREAMMATE_OBS_LIBRARY")
  endif()

  target_compile_definitions(studio-host-smoke PRIVATE STREAMMATE_HAS_LIBOBS=1)
  target_include_directories(studio-host-smoke PRIVATE "${STREAMMATE_OBS_INCLUDE_DIR}")
  if(STREAMMATE_OBS_DEPS_INCLUDE_DIR)
    target_include_directories(studio-host-smoke PRIVATE "${STREAMMATE_OBS_DEPS_INCLUDE_DIR}")
  endif()
  if(STREAMMATE_OBS_GENERATED_INCLUDE_DIR)
    target_include_directories(studio-host-smoke PRIVATE "${STREAMMATE_OBS_GENERATED_INCLUDE_DIR}")
  endif()
  target_link_libraries(studio-host-smoke PRIVATE "${STREAMMATE_OBS_LIBRARY}")
  if(APPLE AND IS_DIRECTORY "${STREAMMATE_OBS_LIBRARY}" AND STREAMMATE_OBS_LIBRARY MATCHES "\\.framework$")
    get_filename_component(STREAMMATE_OBS_FRAMEWORK_DIR "${STREAMMATE_OBS_LIBRARY}" DIRECTORY)
    set_target_properties(studio-host-smoke PROPERTIES BUILD_RPATH "${STREAMMATE_OBS_FRAMEWORK_DIR}")
  endif()
else()
  target_compile_definitions(studio-host-smoke PRIVATE STREAMMATE_HAS_LIBOBS=0)
endif()

install(TARGETS studio-host-smoke RUNTIME DESTINATION bin)
