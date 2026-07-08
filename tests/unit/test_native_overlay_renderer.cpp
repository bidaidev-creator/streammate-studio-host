// Phase B native OverlayAction renderer core — golden / coverage / panic tests
// (Spec 34 Capability 2 / chunk 34.H2). Dependency-free, exercises the
// native_overlay_renderer library directly (no libobs, scaffold mode).
//
// Golden hashes live in tests/unit/native_overlay_golden.txt and are captured
// from the verified implementation via `--update-golden <file>`. The structural
// assertions (category coverage, budget-key-exact timing, clear/panic states,
// determinism) are fixed and fail against the test-first stub.
#include "native_overlay_renderer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using streammate::overlay::NativeOverlayRenderer;
using streammate::overlay::native_overlay_budget_keys;
using streammate::overlay::native_overlay_categories;
using streammate::overlay::OverlayRasterResult;

namespace {

int g_failures = 0;

void check(bool cond, const std::string &label) {
  if (!cond) {
    std::cerr << "FAIL: " << label << "\n";
    ++g_failures;
  }
}

// Fixed fixture payloads (OverlayAction-shaped) per category. Stable across runs.
std::map<std::string, std::string> fixtures() {
  return {
      {"toast", R"({"type":"toast","layer":"foreground","payload":{"message":"NOW LIVE","tone":"success"}})"},
      {"lower-third", R"({"type":"lower-third","layer":"foreground","payload":{"title":"ADA LOVELACE","subtitle":"HOST"}})"},
      {"vote-callout", R"({"type":"vote-callout","layer":"foreground","payload":{"question":"NEXT GAME?","options":[{"id":"a","label":"CHESS"},{"id":"b","label":"GO"}]}})"},
      {"vote-result", R"({"type":"vote-result","layer":"foreground","payload":{"question":"NEXT GAME?","winnerLabel":"GO","responseCount":42,"breakdown":[{"optionId":"a","label":"CHESS","count":18,"percentage":43},{"optionId":"b","label":"GO","count":24,"percentage":57}]}})"},
      {"celebration-burst", R"({"type":"celebration-burst","layer":"foreground","payload":{"label":"NEW SUB","intensity":"high"}})"},
      {"generated-image", R"({"type":"generated-image","layer":"foreground","payload":{"assetId":"img-1","alt":"A CASTLE","caption":"CASTLE AT DUSK","url":"https://station.localhost/asset?token=preload-secret"}})"},
      {"sound-cue", R"({"type":"sound-cue","layer":"foreground","payload":{"cueId":"cue-77","label":"APPLAUSE","volume":80}})"},
      {"clear", R"({"type":"clear","layer":"panic","payload":{"reason":"panic"}})"},
  };
}

// Independent restatement of the Spec 07 budget keys (a contract, not copied code).
std::map<std::string, std::set<std::string>> expected_budget_keys() {
  return {
      {"toast", {"enterMs", "exitMs"}},
      {"lower-third", {"enterMs", "exitMs"}},
      {"vote-callout", {"enterMs", "exitMs"}},
      {"vote-result", {"enterMs", "exitMs"}},
      {"celebration-burst", {"enterMs", "sustainedFrameSkipPercent", "exitMs"}},
      {"generated-image", {"preloadMs", "enterMs", "exitMs"}},
      {"sound-cue", {"latencyMs"}},
      {"clear", {"appliesMs"}},
  };
}

std::set<std::string> keys_of(const std::map<std::string, double> &m) {
  std::set<std::string> out;
  for (const auto &kv : m) out.insert(kv.first);
  return out;
}

std::map<std::string, std::string> read_golden(const std::string &path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    std::string cat, hash;
    ss >> cat >> hash;
    if (!cat.empty()) out[cat] = hash;
  }
  return out;
}

std::string hex(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

} // namespace

int main(int argc, char **argv) {
  const std::string golden_path = (argc > 1 && std::strcmp(argv[1], "--update-golden") != 0)
                                      ? argv[1]
                                      : (argc > 2 ? argv[2] : std::string());
  const bool update = (argc > 1 && std::strcmp(argv[1], "--update-golden") == 0);

  const auto fx = fixtures();
  const auto expected_keys = expected_budget_keys();
  const std::vector<std::string> expected_order = {
      "toast", "lower-third", "vote-callout", "vote-result",
      "celebration-burst", "generated-image", "sound-cue", "clear"};

  // (a) category coverage: the library exposes exactly the eight PB-1..PB-8 categories in order.
  check(native_overlay_categories() == expected_order, "categories are PB-1..PB-8 in order");

  // Update-golden mode: capture hashes and exit.
  if (update) {
    const std::string out_path = argv[2];
    std::ofstream out(out_path);
    out << "# native overlay golden hashes (FNV-1a-64 over RGBA surface) — captured from verified impl\n";
    for (const auto &cat : expected_order) {
      NativeOverlayRenderer r;
      auto res = r.apply(fx.at(cat));
      out << cat << " " << hex(res.raster_hash) << "\n";
    }
    return 0;
  }

  const auto golden = read_golden(golden_path);

  for (const auto &cat : expected_order) {
    NativeOverlayRenderer r;
    OverlayRasterResult res = r.apply(fx.at(cat));
    check(res.ok, cat + ": apply ok");
    check(res.category == cat, cat + ": resolved category matches");

    // (b) budget-key-exact timing records.
    check(keys_of(res.timing) == expected_keys.at(cat), cat + ": timing keys match Spec 07 budget keys");
    check(native_overlay_budget_keys(cat).size() == expected_keys.at(cat).size(),
          cat + ": library budget-key count matches");

    // (c) golden raster hash (deterministic across runs).
    auto git = golden.find(cat);
    check(git != golden.end(), cat + ": golden entry present");
    if (git != golden.end()) {
      check(hex(res.raster_hash) == git->second, cat + ": golden raster hash matches");
    }

    // (d) determinism: a fresh renderer produces the identical hash.
    NativeOverlayRenderer r2;
    check(r2.apply(fx.at(cat)).raster_hash == res.raster_hash, cat + ": hash is deterministic");

    if (cat == "clear") {
      check(res.empty, "clear: raster is empty");
    } else {
      check(!res.empty, cat + ": raster is non-empty");
    }
  }

  // (e) clear empties the raster within one apply cycle.
  {
    NativeOverlayRenderer r;
    auto toast = r.apply(fx.at("toast"));
    check(!toast.empty && !r.is_empty(), "clear-cycle: toast leaves a non-empty raster");
    auto cleared = r.apply(fx.at("clear"));
    check(cleared.empty && r.is_empty(), "clear-cycle: clear empties within one apply");
    NativeOverlayRenderer empty;
    check(cleared.raster_hash == empty.hash(), "clear-cycle: cleared hash equals a fresh empty raster");
  }

  // (f) sound-cue emits a timestamped trigger record (latency-only category).
  {
    NativeOverlayRenderer r;
    auto cue = r.apply(fx.at("sound-cue"));
    check(!cue.trigger_record.empty(), "sound-cue: trigger record emitted");
    check(cue.trigger_record.find("cue-77") != std::string::npos, "sound-cue: trigger record carries cueId");
  }

  // (g) panic-shaped fixtures produce the Spec 07 visual states.
  {
    // panic-mute: a muted sound-cue renders a distinct (muted) indicator raster.
    NativeOverlayRenderer normal;
    auto loud = normal.apply(fx.at("sound-cue"));
    NativeOverlayRenderer muted;
    auto quiet = muted.apply(R"({"type":"sound-cue","layer":"panic","payload":{"cueId":"cue-77","label":"APPLAUSE","volume":0}})");
    check(!quiet.empty, "panic-mute: muted sound-cue raster is non-empty");
    check(quiet.raster_hash != loud.raster_hash, "panic-mute: muted indicator differs from unmuted");

    // panic-freeze: a freeze clear holds the last rendered frame.
    NativeOverlayRenderer frz;
    auto lt = frz.apply(fx.at("lower-third"));
    auto frozen = frz.apply(R"({"type":"clear","layer":"panic","payload":{"reason":"panic-freeze","freeze":true}})");
    check(!frozen.empty, "panic-freeze: raster is held (non-empty)");
    check(frozen.raster_hash == lt.raster_hash, "panic-freeze: frozen frame equals the held frame");
  }

  // (h) secret hygiene: a payload url/secret never appears in the trigger record or error text.
  {
    NativeOverlayRenderer r;
    auto gen = r.apply(fx.at("generated-image"));
    check(gen.trigger_record.find("preload-secret") == std::string::npos, "generated-image: no secret in trigger record");
    check(gen.error.find("preload-secret") == std::string::npos, "generated-image: no secret in error text");
  }

  if (g_failures == 0) {
    std::cout << "native_overlay_renderer: all assertions passed\n";
    return 0;
  }
  std::cerr << g_failures << " assertion(s) failed\n";
  return 1;
}
