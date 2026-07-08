// Spec 34 Capability 3 / chunk 34.H3 — HAS_LIBOBS smoke for the in-tree OBS
// plugin module. Proves the module loads under real libobs, registers its
// source, rasterizes through the shared 34.H2 core, and delivers a frame:
//
//   obs_startup -> obs_open_module + obs_init_module (the same steps the host's
//   normal obs_load_all_modules path performs) -> assert the streammate_native_overlay
//   source id is registered as an async video source -> obs_source_create by that
//   id -> apply a fixture toast OverlayAction -> one async video render cycle
//   (obs_source_output_video) -> assert a frame was delivered, the frame is
//   non-empty, and its raster hash equals the shared scaffold-mode golden ->
//   clean obs_shutdown.
//
// The shared-golden assertion (module frame hash == the scaffold golden captured
// by the 34.H2 unit test) proves the module uploads the same plain rasterizer
// core rather than a forked copy.

#include "native_overlay_renderer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <obs.h>

namespace {

constexpr const char *kSourceId = "streammate_native_overlay";
// The exact toast fixture the 34.H2 scaffold golden test uses.
constexpr const char *kToastFixture =
    R"({"type":"toast","layer":"foreground","payload":{"message":"NOW LIVE","tone":"success"}})";

std::string hex_u64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

// Read one category's golden hash from tests/unit/native_overlay_golden.txt —
// the same file the scaffold-mode unit test asserts against.
std::string read_golden(const std::string &path, const std::string &category) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream ss(line);
    std::string cat, hash;
    ss >> cat >> hash;
    if (cat == category) {
      return hash;
    }
  }
  return "";
}

int fail(const char *msg) {
  std::cerr << "native-overlay-module-smoke FAIL: " << msg << "\n";
  return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: native-overlay-module-smoke <module-binary> <golden-file>\n";
    return EXIT_FAILURE;
  }
  const char *module_bin = argv[1];
  const std::string golden_path = argv[2];

  // Scaffold-mode reference through the plain core: the shared-golden anchor.
  streammate::overlay::NativeOverlayRenderer reference;
  streammate::overlay::OverlayRasterResult ref = reference.apply(kToastFixture);
  if (!ref.ok) {
    return fail("reference rasterize failed");
  }
  const std::string reference_hash = hex_u64(ref.raster_hash);
  const std::string golden_hash = read_golden(golden_path, "toast");
  if (golden_hash.empty()) {
    return fail("golden toast hash not found");
  }
  if (reference_hash != golden_hash) {
    return fail("shared core hash does not match scaffold golden");
  }

  if (!obs_startup("en-US", nullptr, nullptr)) {
    return fail("obs_startup failed");
  }

  obs_module_t *module = nullptr;
  int rc = obs_open_module(&module, module_bin, nullptr);
  if (rc != MODULE_SUCCESS) {
    obs_shutdown();
    return fail("obs_open_module failed");
  }
  if (!obs_init_module(module)) {
    obs_shutdown();
    return fail("obs_init_module failed");
  }

  // The module registered the source through the normal init path.
  uint32_t flags = obs_get_source_output_flags(kSourceId);
  if ((flags & OBS_SOURCE_ASYNC_VIDEO) != OBS_SOURCE_ASYNC_VIDEO) {
    obs_shutdown();
    return fail("registered source id missing or not an async video source");
  }

  obs_source_t *source = obs_source_create(kSourceId, "smoke-native-overlay", nullptr, nullptr);
  if (source == nullptr) {
    obs_shutdown();
    return fail("obs_source_create by registered id failed");
  }

  proc_handler_t *ph = obs_source_get_proc_handler(source);
  calldata_t cd;
  calldata_init(&cd);
  calldata_set_string(&cd, "action", kToastFixture);
  bool called = proc_handler_call(ph, "apply_overlay", &cd);

  int result = EXIT_SUCCESS;
  if (!called) {
    result = fail("apply_overlay proc handler missing");
  } else {
    bool ok = calldata_bool(&cd, "ok");
    long long delivered = calldata_int(&cd, "delivered");
    bool empty = calldata_bool(&cd, "empty");
    const char *hash_c = nullptr;
    calldata_get_string(&cd, "hash", &hash_c);
    std::string module_hash = hash_c != nullptr ? hash_c : "";

    if (!ok) {
      result = fail("module apply_overlay reported failure");
    } else if (delivered < 1) {
      result = fail("no frame delivered to libobs");
    } else if (empty) {
      result = fail("toast frame unexpectedly empty");
    } else if (module_hash != golden_hash) {
      result = fail("module frame hash does not match shared scaffold golden");
    } else {
      std::cout << "native-overlay-module-smoke ok (delivered=" << delivered << " hash=" << module_hash
                << ")\n";
    }
  }

  calldata_free(&cd);
  obs_source_release(source);
  obs_shutdown();
  return result;
}
