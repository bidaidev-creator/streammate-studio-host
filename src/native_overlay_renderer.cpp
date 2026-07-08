// TEST-FIRST STUB (RED). Real CoreGraphics rasterizer lands in the implementation
// commit. This stub compiles and links so the golden/coverage/panic tests build
// and FAIL at runtime, making the test-first ordering visible in PR history.
#include "native_overlay_renderer.h"

namespace streammate::overlay {

const std::vector<std::string> &native_overlay_categories() {
  static const std::vector<std::string> kCategories; // empty in the stub
  return kCategories;
}

const std::vector<std::string> &native_overlay_budget_keys(const std::string &) {
  static const std::vector<std::string> kNone;
  return kNone;
}

uint64_t fnv1a64(const uint8_t *, size_t) { return 0; }

struct NativeOverlayRenderer::Impl {
  std::vector<uint8_t> rgba = std::vector<uint8_t>(0);
};

NativeOverlayRenderer::NativeOverlayRenderer() : impl_(std::make_unique<Impl>()) {}
NativeOverlayRenderer::~NativeOverlayRenderer() = default;

OverlayRasterResult NativeOverlayRenderer::apply(const std::string &) {
  return OverlayRasterResult{};
}

int NativeOverlayRenderer::width() const { return kOverlayWidth; }
int NativeOverlayRenderer::height() const { return kOverlayHeight; }
const std::vector<uint8_t> &NativeOverlayRenderer::rgba() const { return impl_->rgba; }
std::vector<uint8_t> NativeOverlayRenderer::bgra() const { return {}; }
bool NativeOverlayRenderer::is_empty() const { return true; }
uint64_t NativeOverlayRenderer::hash() const { return 0; }

} // namespace streammate::overlay
