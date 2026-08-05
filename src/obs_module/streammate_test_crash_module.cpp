// NIF-H3 seeded-failure fixture: an OBS module whose obs_module_load crashes
// the process. Built ONLY in the HAS_LIBOBS lane and NEVER staged into the app
// bundle or the CI artifact's test-plugins/ dir consumers select by default:
// it exists so CI can prove crash attribution + bounded containment against a
// real dlopen/obs_module_load path. Deliberately not a fake: the abort() is a
// genuine mid-load process death, exactly what a broken third-party plugin
// does in the field.
#include <cstdlib>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

#include <obs-module.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate seeded CRASHING test module (containment CI fixture)";
}

bool obs_module_load(void) {
  // Crash mid-load: the host's plugin-load sentinel still names this module,
  // and the next boot must refuse it as crash-suspected.
#if defined(_WIN32)
  // On Windows a bare abort() routes through the CRT abort message and
  // Windows Error Reporting, which can stall the dying process long enough
  // for the load watchdog to fire FIRST — the sentinel then records
  // deadline-exceeded and the module is mis-attributed as a hang. Suppress
  // only the reporting detour; the death itself stays a genuine abort.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, 0);
  _CrtSetReportMode(_CRT_ERROR, 0);
#endif
  abort();
}
