// Spec 39 / chunk NIF-H2 — redistributable in-tree TEST source plugin.
//
// A deliberately-minimal real OBS module used to prove the native host's
// USER-plugin loading path (obs_open_module/obs_init_module over manifest
// roots) with a genuine registered source type. It ships in the CI artifact
// under test-plugins/ and is NEVER staged into the app bundle's
// Contents/PlugIns/obs-plugins — it must only ever load through the
// user-plugins manifest path. It is a test fixture, not a product feature,
// and is always labeled moduleClass "in-tree-test-module" downstream
// (never "vendor" — Q-139).
//
// The source is an explicit-opt-in async video input that fills a fixed 8x8
// BGRA frame with a color taken from its settings ("color", uint32 BGRA),
// so the instance+render probe can verify settings preservation and frame
// delivery deterministically via the emit_test_frame proc — no compositor
// tick required.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <obs-module.h>

OBS_DECLARE_MODULE()

namespace {

constexpr const char *kSourceId = "streammate_test_source";
constexpr const char *kDisplayName = "Stream Mate Test Source (fixture)";
constexpr const char *kColorSetting = "color";
constexpr uint32_t kDefaultColor = 0xFF00FF00; // opaque green (BGRA)
constexpr uint32_t kSide = 8;

struct TestSource {
  obs_source_t *source = nullptr;
  std::atomic<uint32_t> color{kDefaultColor};
  std::atomic<long long> delivered{0};
};

bool emit_frame(TestSource *ctx) {
  std::vector<uint32_t> pixels(kSide * kSide, ctx->color.load(std::memory_order_relaxed));
  struct obs_source_frame frame;
  std::memset(&frame, 0, sizeof(frame));
  frame.format = VIDEO_FORMAT_BGRA;
  frame.width = kSide;
  frame.height = kSide;
  frame.linesize[0] = kSide * 4u;
  frame.data[0] = reinterpret_cast<uint8_t *>(pixels.data());
  frame.full_range = true;
  frame.timestamp = 0;
  obs_source_output_video(ctx->source, &frame);
  ctx->delivered.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// Proc: void emit_test_frame(out bool ok, out int delivered, out int color)
// Drives one deterministic frame delivery and echoes the settings-derived
// color so the smoke can assert settings preservation end to end.
void emit_test_frame_proc(void *data, calldata_t *cd) {
  auto *ctx = static_cast<TestSource *>(data);
  bool ok = emit_frame(ctx);
  calldata_set_bool(cd, "ok", ok);
  calldata_set_int(cd, "delivered", ctx->delivered.load(std::memory_order_relaxed));
  calldata_set_int(cd, "color", static_cast<long long>(ctx->color.load(std::memory_order_relaxed)));
}

const char *source_get_name(void *) { return kDisplayName; }

void apply_settings(TestSource *ctx, obs_data_t *settings) {
  const long long color = obs_data_get_int(settings, kColorSetting);
  ctx->color.store(color != 0 ? static_cast<uint32_t>(color) : kDefaultColor,
                   std::memory_order_relaxed);
}

void *source_create(obs_data_t *settings, obs_source_t *source) {
  auto *ctx = new TestSource();
  ctx->source = source;
  apply_settings(ctx, settings);
  proc_handler_t *ph = obs_source_get_proc_handler(source);
  proc_handler_add(ph, "void emit_test_frame(out bool ok, out int delivered, out int color)",
                   emit_test_frame_proc, ctx);
  return ctx;
}

void source_destroy(void *data) { delete static_cast<TestSource *>(data); }

void source_update(void *data, obs_data_t *settings) {
  apply_settings(static_cast<TestSource *>(data), settings);
}

// NIF-V1: auto-emit one deterministic frame per compositor tick so a packaged
// host can observe the pattern via source.captureFrame without calling the
// emit_test_frame proc (which remains for the smoke's settings echo).
void source_tick(void *data, float) { emit_frame(static_cast<TestSource *>(data)); }

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
  info.video_tick = source_tick;
  return info;
}

struct obs_source_info g_test_source = make_source_info();

} // namespace

MODULE_EXPORT const char *obs_module_name(void) { return "streammate-test-source"; }

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate NIF-H2 test fixture source plugin (user-plugin load proof; not a product feature).";
}

bool obs_module_load(void) {
  obs_register_source(&g_test_source);
  return true;
}

void obs_module_unload(void) {}
