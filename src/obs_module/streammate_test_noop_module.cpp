// Spec 39 / chunk NIF-H2 — in-tree TEST fixture module that loads cleanly but
// registers NOTHING. It exists to prove the `type_not_registered` compatibility
// state: obs_open_module/obs_init_module succeed, the registered-type delta is
// empty across all six kinds, and the host must report state
// "type_not_registered" (lifecycle "loaded") rather than inventing a mapping.
// Never staged into the app bundle; loads only via the user-plugins manifest.

#include <obs-module.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void) { return "streammate-test-noop"; }

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate NIF-H2 test fixture module that registers no types (type_not_registered proof).";
}

bool obs_module_load(void) {
  return true;
}

void obs_module_unload(void) {}
