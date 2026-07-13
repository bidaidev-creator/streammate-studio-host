// NIF-H3 seeded-failure fixture: an OBS module whose obs_module_load never
// returns. Built ONLY in the HAS_LIBOBS lane; never staged into the app
// bundle. It exists so CI can prove the plugin-load watchdog converts an
// in-process hang into a bounded, attributable exit (deadline-exceeded
// sentinel + kPluginWatchdogExit) instead of an unbounded startup hang.
#include <chrono>
#include <thread>

#include <obs-module.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate seeded HANGING test module (containment CI fixture)";
}

bool obs_module_load(void) {
  // Hang mid-load forever; the watchdog deadline must fire.
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
