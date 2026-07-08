// Phase B native OverlayAction renderer core (Spec 34 Capability 2 / chunk 34.H2).
//
// A plain host-repo library: a CoreGraphics-backed software rasterizer that
// renders OverlayAction-shaped payloads for the eight budgeted Spec 07 parity
// categories (PB-1..PB-8) at a fixed 1280x720 surface. It carries no product
// logic, no policy, and no protocol/host concerns beyond interpreting the
// action payload it is handed (ADR-0005 Decision 2). The library is deliberately
// separable from both the host dispatch (studio_host.cpp) and the future 34.H3
// in-tree OBS plugin module so either Q-121 packaging answer stays cheap.
//
// This is an EXPLICIT OPT-IN renderer. ADR-0003's browser-source default is
// untouched and nothing here graduates Phase B (Q-33 stays open).
#ifndef STREAMMATE_NATIVE_OVERLAY_RENDERER_H
#define STREAMMATE_NATIVE_OVERLAY_RENDERER_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace streammate::overlay {

inline constexpr int kOverlayWidth = 1280;
inline constexpr int kOverlayHeight = 720;

// The eight budgeted Spec 07 parity categories, in PB-1..PB-8 order.
const std::vector<std::string> &native_overlay_categories();

// The exact Spec 07 budget keys for a category (empty vector if the category is
// not one of the eight budgeted categories). These MUST match
// SPEC_07_STUDIO_HOST_PARITY_BUDGETS (packages/station/src/studio-host-parity-capture.ts).
const std::vector<std::string> &native_overlay_budget_keys(const std::string &category);

struct OverlayRasterResult {
  bool ok = false;
  std::string error;                    // sanitized reason when !ok
  std::string category;                 // resolved category (empty when !ok)
  std::map<std::string, double> timing; // keys == native_overlay_budget_keys(category)
  std::string trigger_record;           // timestamped trigger, non-empty for sound-cue
  bool empty = true;                    // true when the raster is fully transparent
  uint64_t raster_hash = 0;             // FNV-1a-64 over the RGBA surface bytes
};

// A 1280x720 RGBA (non-premultiplied on read-back) software rasterizer backed by
// a CoreGraphics CGBitmapContext surface. Not copyable; each opt-in native-overlay
// source owns one instance so a session that never opts in constructs none.
class NativeOverlayRenderer {
public:
  NativeOverlayRenderer();
  ~NativeOverlayRenderer();
  NativeOverlayRenderer(const NativeOverlayRenderer &) = delete;
  NativeOverlayRenderer &operator=(const NativeOverlayRenderer &) = delete;

  // Apply an OverlayAction-shaped JSON object. Renders the frame, measures the
  // apply->rendered timing keyed to the category's budget keys, and returns the
  // result. Unknown/unsupported types yield ok=false with a sanitized reason.
  OverlayRasterResult apply(const std::string &action_json);

  int width() const;
  int height() const;
  const std::vector<uint8_t> &rgba() const; // width*height*4, current frame
  std::vector<uint8_t> bgra() const;        // BGRA copy for the 34.H3 libobs upload path
  bool is_empty() const;
  uint64_t hash() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// FNV-1a-64 over a byte range; exposed so tests and the host share one hash.
uint64_t fnv1a64(const uint8_t *data, size_t len);

} // namespace streammate::overlay

#endif // STREAMMATE_NATIVE_OVERLAY_RENDERER_H
