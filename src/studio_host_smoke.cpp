#include <cstdlib>
#include <iostream>
#include <string>

#if STREAMMATE_HAS_LIBOBS
#include <obs.h>
#endif

namespace {
constexpr const char *kVersion = STREAMMATE_STUDIO_HOST_VERSION;
}

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "streammate-studio-host-smoke " << kVersion << "\n";
    return EXIT_SUCCESS;
  }

#if STREAMMATE_HAS_LIBOBS
  if (!obs_startup("en-US", nullptr, nullptr)) {
    std::cerr << "obs_startup failed\n";
    return EXIT_FAILURE;
  }

  obs_shutdown();
  std::cout << "obs_startup_shutdown_smoke ok\n";
  return EXIT_SUCCESS;
#else
  std::cout << "scaffold smoke ok (libobs disabled for local configure)\n";
  return EXIT_SUCCESS;
#endif
}
