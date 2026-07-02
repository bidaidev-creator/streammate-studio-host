# Included by OBS Studio's top-level project() via CMAKE_PROJECT_INCLUDE.
# OBS 32.1.2's macOS libobs-metal target is Swift-only; enabling Swift here
# lets CMake infer the target linker language without patching the upstream
# obs-studio submodule.
enable_language(Swift)
