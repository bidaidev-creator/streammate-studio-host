// Spec 39 / chunk NIF-H2 — redistributable in-tree TEST filter plugin.
//
// The filter-kind counterpart of streammate-test-source: a real OBS async
// video passthrough filter whose only job is to prove that a USER-loaded
// module can register a filter type and that a created filter instance sits
// in a parent source's chain and sees frames. Ships in the CI artifact under
// test-plugins/, never in the app bundle; moduleClass "in-tree-test-module".

#include <atomic>
#include <cstring>

#include <obs-module.h>

OBS_DECLARE_MODULE()

namespace {

constexpr const char *kFilterId = "streammate_test_filter";
constexpr const char *kDisplayName = "Stream Mate Test Filter (fixture)";

struct TestFilter {
  obs_source_t *source = nullptr;
  std::atomic<long long> frames{0};
};

// Async passthrough: count the frame and hand it back untouched.
struct obs_source_frame *filter_video(void *data, struct obs_source_frame *frame) {
  static_cast<TestFilter *>(data)->frames.fetch_add(1, std::memory_order_relaxed);
  return frame;
}

// Proc: void get_test_filter_stats(out int frames, out bool has_parent)
// Lets the probe smoke observe frame flow and parent wiring deterministically.
void get_stats_proc(void *data, calldata_t *cd) {
  auto *ctx = static_cast<TestFilter *>(data);
  calldata_set_int(cd, "frames", ctx->frames.load(std::memory_order_relaxed));
  calldata_set_bool(cd, "has_parent", obs_filter_get_parent(ctx->source) != nullptr);
}

const char *filter_get_name(void *) { return kDisplayName; }

void *filter_create(obs_data_t *, obs_source_t *source) {
  auto *ctx = new TestFilter();
  ctx->source = source;
  proc_handler_t *ph = obs_source_get_proc_handler(source);
  proc_handler_add(ph, "void get_test_filter_stats(out int frames, out bool has_parent)",
                   get_stats_proc, ctx);
  return ctx;
}

void filter_destroy(void *data) { delete static_cast<TestFilter *>(data); }

struct obs_source_info make_filter_info() {
  struct obs_source_info info;
  std::memset(&info, 0, sizeof(info));
  info.id = kFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  info.get_name = filter_get_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.filter_video = filter_video;
  return info;
}

struct obs_source_info g_test_filter = make_filter_info();

// F1 fixture (opus review): a deliberately NON-portable type id (leading
// underscore + > 64 chars). The host must drop it from the emitted
// registered-type delta while this module still reports loaded.
constexpr const char *kNonPortableFilterId =
    "_streammate_nonportable_type_id_deliberately_longer_than_sixty_four_characters";

struct obs_source_info make_nonportable_filter_info() {
  struct obs_source_info info;
  std::memset(&info, 0, sizeof(info));
  info.id = kNonPortableFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
  info.get_name = filter_get_name;
  info.create = filter_create;
  info.destroy = filter_destroy;
  info.filter_video = filter_video;
  return info;
}

struct obs_source_info g_nonportable_filter = make_nonportable_filter_info();

} // namespace

MODULE_EXPORT const char *obs_module_name(void) { return "streammate-test-filter"; }

MODULE_EXPORT const char *obs_module_description(void) {
  return "Stream Mate NIF-H2 test fixture filter plugin (user-plugin load proof; not a product feature).";
}

bool obs_module_load(void) {
  obs_register_source(&g_test_filter);
  obs_register_source(&g_nonportable_filter);
  return true;
}

void obs_module_unload(void) {}
