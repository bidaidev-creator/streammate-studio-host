// Spec 34 Capability 3 / chunk 34.H3 — in-tree OBS plugin module for the Phase B
// native OverlayAction renderer.
//
// A real OBS plugin module per ADR-0005 Decision 1 ("OBS plugin modules in our
// own source tree, loaded by studio-host" — never a submodule patch). It exports
// obs_module_load/obs_module_unload and registers an async video input source
// that rasterizes OverlayAction-shaped payloads through the shared 34.H2 renderer
// core (native-overlay-renderer, a plain host-repo library) and uploads BGRA
// frames via the async video path (obs_source_output_video).
//
// EXPLICIT OPT-IN: the source exists only when a caller creates it by its
// registered id. ADR-0003's browser-source default is untouched and nothing here
// graduates Phase B (Q-33 stays open). It carries no product logic — policy,
// playbooks, journal, and approval semantics stay in Station (ADR-0005
// Decision 2); the module only interprets the action payload it is handed.
//
// Module residence/packaging is Q-121 (owner-ratification-pending): the
// rasterizer-core/module split keeps a separately-distributed-artifact reversal
// cheap — this file links the core library rather than embedding a second copy.

#include "native_overlay_renderer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <obs-module.h>
#include <util/platform.h>

OBS_DECLARE_MODULE()

namespace {

// The stable registered source id callers (host RPC, CI smoke, parity driver)
// create the opt-in native overlay source by.
constexpr const char *kSourceId = "streammate_native_overlay";
constexpr const char *kDisplayName = "Stream Mate Native Overlay";
// Settings key carrying an OverlayAction-shaped JSON string.
constexpr const char *kActionSetting = "overlay_action";

struct NativeOverlaySource {
  obs_source_t *source = nullptr;
  streammate::overlay::NativeOverlayRenderer renderer;
  std::atomic<long long> delivered{0};
};

// Rasterize one OverlayAction payload via the shared core and, on success,
// upload the BGRA frame through the async video path. Returns true when a frame
// was delivered to libobs.
bool render_and_output(NativeOverlaySource *ctx, const std::string &action_json, uint64_t *hash_out,
                       bool *empty_out) {
  streammate::overlay::OverlayRasterResult res = ctx->renderer.apply(action_json);
  if (!res.ok) {
    return false;
  }
  std::vector<uint8_t> pixels = ctx->renderer.bgra();
  const uint32_t width = static_cast<uint32_t>(ctx->renderer.width());
  const uint32_t height = static_cast<uint32_t>(ctx->renderer.height());
  if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
    return false;
  }

  struct obs_source_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.format = VIDEO_FORMAT_BGRA;
  frame.width = width;
  frame.height = height;
  frame.linesize[0] = width * 4u;
  frame.data[0] = pixels.data();
  frame.full_range = true;
  frame.timestamp = os_gettime_ns();

  obs_source_output_video(ctx->source, &frame);
  ctx->delivered.fetch_add(1, std::memory_order_relaxed);

  if (hash_out != nullptr) {
    *hash_out = res.raster_hash;
  }
  if (empty_out != nullptr) {
    *empty_out = res.empty;
  }
  return true;
}

// Proc handler:
//   void apply_overlay(in string action, out bool ok, out int delivered,
//                      out string hash, out bool empty)
// Lets a caller drive one render cycle and observe delivery deterministically
// without depending on the graphics compositor tick — the CI smoke uses it.
void apply_overlay_proc(void *data, calldata_t *cd) {
  auto *ctx = static_cast<NativeOverlaySource *>(data);
  const char *action = nullptr;
  calldata_get_string(cd, "action", &action);

  uint64_t hash = 0;
  bool empty = true;
  bool ok = (action != nullptr && *action != '\0') && render_and_output(ctx, action, &hash, &empty);

  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  calldata_set_bool(cd, "ok", ok);
  calldata_set_int(cd, "delivered", ctx->delivered.load(std::memory_order_relaxed));
  calldata_set_string(cd, "hash", hex);
  calldata_set_bool(cd, "empty", empty);
}

const char *source_get_name(void *) { return kDisplayName; }

void apply_from_settings(NativeOverlaySource *ctx, obs_data_t *settings) {
  const char *action = obs_data_get_string(settings, kActionSetting);
  if (action != nullptr && *action != '\0') {
    uint64_t hash = 0;
    bool empty = true;
    render_and_output(ctx, action, &hash, &empty);
  }
}

void *source_create(obs_data_t *settings, obs_source_t *source) {
  auto *ctx = new NativeOverlaySource();
  ctx->source = source;
  proc_handler_t *ph = obs_source_get_proc_handler(source);
  proc_handler_add(
      ph, "void apply_overlay(in string action, out bool ok, out int delivered, out string hash, out bool empty)",
      apply_overlay_proc, ctx);
  apply_from_settings(ctx, settings);
  return ctx;
}

void source_destroy(void *data) { delete static_cast<NativeOverlaySource *>(data); }

void source_update(void *data, obs_data_t *settings) {
  apply_from_settings(static_cast<NativeOverlaySource *>(data), settings);
}

struct obs_source_info make_source_info() {
  struct obs_source_info info;
  std::memset(&info, 0, sizeof(info));
  info.id = kSourceId;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  info.get_name = source_get_name;
  info.create = source_create;
  info.destroy = source_destroy;
  info.update = source_update;
  return info;
}

struct obs_source_info g_native_overlay_source = make_source_info();

} // namespace

MODULE_EXPORT const char *obs_module_name(void) { return "streammate-native-overlay"; }

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate Phase B native OverlayAction renderer (explicit opt-in).";
}

bool obs_module_load(void) {
  obs_register_source(&g_native_overlay_source);
  return true;
}

void obs_module_unload(void) {}
