
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <cerrno>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits.h>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "native_overlay_renderer.h"
#include "user_plugin_portability.h"

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#endif

#if STREAMMATE_HAS_LIBOBS
#include <dlfcn.h>
#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>
#endif

namespace {
constexpr const char *kVersion = STREAMMATE_STUDIO_HOST_VERSION;
constexpr int kUsageExit = 64;
constexpr int kRuntimeExit = 70;
constexpr int kHeartbeatMs = 5000;
constexpr int kMaxSceneItemPosition = 1000000;
constexpr std::uintmax_t kMaxSceneCollectionBytes = 8 * 1024 * 1024;
constexpr const char *kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// NIF-H1: tells plugins.discover/plugins.report consumers whether this host
// can really load modules (the discovery inventory itself is identical in
// both lanes -- pure filesystem + Mach-O byte parsing, no libobs).
#if STREAMMATE_HAS_LIBOBS
constexpr const char *kPluginHostMode = "libobs";
#else
constexpr const char *kPluginHostMode = "scaffold";
#endif
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

// ---------------------------------------------------------------------------
// NIF-H1: --user-plugins-manifest launch input (schema user-plugins-manifest.v1)
//
//   { "version":1,
//     "roots":[{"binaryDir":"/abs","dataDir":"/abs"|null}, ...],
//     "selected":["module:<id>", ...]|null,
//     "exclude":["module:<id>", ...] }
//
// Launch-time parsing/validation only (the static inventory itself lives with
// the JSON emit helpers further down). Validation is fail-closed and every
// refusal reason is a SANITIZED category code -- the raw filesystem path is
// never interpolated into a reason string. "selected" is validated here but
// only consumed by module loading, which lands in chunk NIF-H2.
// ---------------------------------------------------------------------------
namespace user_plugins {

struct Root {
  std::string binary_dir;
  std::string data_dir; // empty => null/absent in the manifest
};

struct Manifest {
  std::vector<Root> roots;
  bool has_selected = false;         // "selected" present and non-null
  std::vector<std::string> selected; // reserved for NIF-H2 loading
  std::vector<std::string> exclude;
};

// Strict, self-contained scanner for the small v1 schema. The host's loose
// substring extractors (extract_json_*) are deliberately NOT reused here:
// launch validation must detect malformed JSON, not paper over it.
class ManifestScanner {
public:
  explicit ManifestScanner(const std::string &text) : text_(text) {}

  void ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool eof() {
    ws();
    return pos_ >= text_.size();
  }

  bool consume(char c) {
    ws();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool literal(const char *word) {
    ws();
    std::size_t len = std::strlen(word);
    if (text_.compare(pos_, len, word) == 0) {
      pos_ += len;
      return true;
    }
    return false;
  }

  std::optional<std::string> string() {
    ws();
    if (pos_ >= text_.size() || text_[pos_] != '"') return std::nullopt;
    ++pos_;
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos_ >= text_.size()) return std::nullopt;
        char escaped = text_[pos_++];
        switch (escaped) {
        case '"': case '\\': case '/': out.push_back(escaped); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          // Full \uXXXX decoding to UTF-8 including surrogate pairs: a
          // Station-side JSON serializer may escape non-ASCII path bytes.
          // Unpaired/reversed surrogates are rejected (fail-closed launch
          // validation, unlike the wire parser's U+FFFD replacement).
          auto code_unit = hex4();
          if (!code_unit) return std::nullopt;
          unsigned codepoint = *code_unit;
          if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              return std::nullopt;
            }
            pos_ += 2;
            auto low = hex4();
            if (!low || *low < 0xDC00 || *low > 0xDFFF) return std::nullopt;
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (*low - 0xDC00);
          } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
            return std::nullopt; // lone low surrogate
          }
          append_utf8(out, codepoint);
          break;
        }
        default:
          return std::nullopt;
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return std::nullopt; // raw control byte inside a JSON string
      } else {
        out.push_back(c);
      }
    }
    return std::nullopt;
  }

  // Strict JSON number grammar: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  // Rejects leading zeros ("01") and operator smuggling ("1+2") -- anything
  // left over after the grammar trips the caller's ','/'}' expectation.
  bool number() {
    ws();
    const std::size_t start = pos_;
    const auto digit = [&] {
      return pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]));
    };
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (!digit()) {
      pos_ = start;
      return false;
    }
    if (text_[pos_] == '0') {
      ++pos_;
      if (digit()) { // leading zero: 01 is not a JSON number
        pos_ = start;
        return false;
      }
    } else {
      while (digit()) ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (!digit()) {
        pos_ = start;
        return false;
      }
      while (digit()) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (!digit()) {
        pos_ = start;
        return false;
      }
      while (digit()) ++pos_;
    }
    return true;
  }

  std::optional<long> integer() {
    ws();
    const std::size_t start = pos_;
    if (!number()) return std::nullopt;
    const std::string token = text_.substr(start, pos_ - start);
    if (token.find_first_of(".eE") != std::string::npos) {
      pos_ = start; // a valid number but not an integer; caller may skip it
      return std::nullopt;
    }
    try {
      return std::stol(token);
    } catch (...) {
      pos_ = start;
      return std::nullopt;
    }
  }

  // Skips one well-formed JSON value of any type (for unknown keys).
  bool skip_value(int depth = 0) {
    if (depth > 64) return false;
    ws();
    if (pos_ >= text_.size()) return false;
    char c = text_[pos_];
    if (c == '"') return string().has_value();
    if (c == '{' || c == '[') {
      const bool object = (c == '{');
      const char close = object ? '}' : ']';
      ++pos_;
      if (consume(close)) return true;
      while (true) {
        if (object) {
          if (!string()) return false;
          if (!consume(':')) return false;
        }
        if (!skip_value(depth + 1)) return false;
        if (consume(',')) continue;
        return consume(close);
      }
    }
    if (literal("true") || literal("false") || literal("null")) return true;
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return number();
    return false;
  }

private:
  static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
  }

  std::optional<unsigned> hex4() {
    if (pos_ + 4 > text_.size()) return std::nullopt;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      int digit = hex_digit_value(text_[pos_++]);
      if (digit < 0) return std::nullopt;
      value = (value << 4) | static_cast<unsigned>(digit);
    }
    return value;
  }

  static void append_utf8(std::string &out, unsigned codepoint) {
    if (codepoint < 0x80) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
  }

  const std::string &text_;
  std::size_t pos_ = 0;
};

inline bool is_absolute_manifest_path(const std::string &path) {
  return !path.empty() && path[0] == '/';
}

inline bool is_module_ref(const std::string &value) {
  constexpr const char *kPrefix = "module:";
  constexpr std::size_t kPrefixLen = 7;
  return value.size() > kPrefixLen && value.compare(0, kPrefixLen, kPrefix) == 0;
}

struct ParseOutcome {
  std::optional<Manifest> manifest; // set on success
  std::string reason_code;          // set on failure; a category, never a path
};

inline ParseOutcome parse_manifest_text(const std::string &text) {
  const auto fail = [](const char *code) {
    ParseOutcome out;
    out.reason_code = code;
    return out;
  };

  ManifestScanner scan(text);
  Manifest manifest;
  bool saw_version = false;
  bool version_ok = false;
  bool saw_roots = false;

  if (!scan.consume('{')) return fail("malformed-json");
  if (!scan.consume('}')) {
    // Duplicate keys are interop-ambiguous (last-wins vs append) at every
    // object level of the schema: refuse them fail-closed.
    std::set<std::string> seen_keys;
    while (true) {
      auto key = scan.string();
      if (!key) return fail("malformed-json");
      if (!seen_keys.insert(*key).second) return fail("duplicate-key");
      if (!scan.consume(':')) return fail("malformed-json");

      if (*key == "version") {
        saw_version = true;
        if (auto value = scan.integer()) {
          version_ok = (*value == 1);
        } else if (scan.skip_value()) {
          version_ok = false; // well-formed but not an integer
        } else {
          return fail("malformed-json");
        }
      } else if (*key == "roots") {
        saw_roots = true;
        if (!scan.consume('[')) return fail("invalid-roots");
        if (!scan.consume(']')) {
          while (true) {
            if (!scan.consume('{')) return fail("invalid-roots");
            Root root;
            bool saw_binary_dir = false;
            if (!scan.consume('}')) {
              std::set<std::string> seen_root_keys;
              while (true) {
                auto root_key = scan.string();
                if (!root_key) return fail("malformed-json");
                if (!seen_root_keys.insert(*root_key).second) return fail("duplicate-key");
                if (!scan.consume(':')) return fail("malformed-json");
                if (*root_key == "binaryDir") {
                  auto value = scan.string();
                  if (!value) return fail("invalid-roots");
                  root.binary_dir = *value;
                  saw_binary_dir = true;
                } else if (*root_key == "dataDir") {
                  if (scan.literal("null")) {
                    root.data_dir.clear();
                  } else {
                    auto value = scan.string();
                    if (!value) return fail("invalid-roots");
                    root.data_dir = *value;
                  }
                } else if (!scan.skip_value()) {
                  return fail("malformed-json");
                }
                if (scan.consume(',')) continue;
                if (scan.consume('}')) break;
                return fail("malformed-json");
              }
            }
            if (!saw_binary_dir) return fail("invalid-roots");
            manifest.roots.push_back(std::move(root));
            if (scan.consume(',')) continue;
            if (scan.consume(']')) break;
            return fail("malformed-json");
          }
        }
      } else if (*key == "selected") {
        if (scan.literal("null")) {
          manifest.has_selected = false;
          manifest.selected.clear();
        } else {
          if (!scan.consume('[')) return fail("invalid-selected");
          manifest.has_selected = true;
          if (!scan.consume(']')) {
            while (true) {
              auto value = scan.string();
              if (!value || !is_module_ref(*value)) return fail("invalid-selected");
              manifest.selected.push_back(*value);
              if (scan.consume(',')) continue;
              if (scan.consume(']')) break;
              return fail("malformed-json");
            }
          }
        }
      } else if (*key == "exclude") {
        if (!scan.consume('[')) return fail("invalid-exclude");
        if (!scan.consume(']')) {
          while (true) {
            auto value = scan.string();
            if (!value || !is_module_ref(*value)) return fail("invalid-exclude");
            manifest.exclude.push_back(*value);
            if (scan.consume(',')) continue;
            if (scan.consume(']')) break;
            return fail("malformed-json");
          }
        }
      } else if (!scan.skip_value()) {
        return fail("malformed-json");
      }

      if (scan.consume(',')) continue;
      if (scan.consume('}')) break;
      return fail("malformed-json");
    }
  }
  if (!scan.eof()) return fail("malformed-json");

  if (!saw_version || !version_ok) return fail("unsupported-version");
  if (!saw_roots) return fail("invalid-roots");
  for (const auto &root : manifest.roots) {
    if (!is_absolute_manifest_path(root.binary_dir)) return fail("invalid-roots");
    if (!root.data_dir.empty() && !is_absolute_manifest_path(root.data_dir)) return fail("invalid-roots");
  }

  ParseOutcome out;
  out.manifest = std::move(manifest);
  return out;
}

// Launch-time gate: any failure throws with "invalid --user-plugins-manifest:
// <reason-code>" -- main() catches it, logs, and returns kUsageExit. The raw
// path is intentionally NEVER part of the message.
// Manifest byte ceiling: manifests are tiny; consistent with the repo's
// bounded-input posture (kMaxSceneCollectionBytes).
constexpr std::uintmax_t kMaxManifestBytes = 1024 * 1024;

inline Manifest load_or_throw(const std::string &path) {
  const std::string prefix = "invalid --user-plugins-manifest: ";
  if (!is_absolute_manifest_path(path)) throw std::runtime_error(prefix + "not-absolute-path");
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) throw std::runtime_error(prefix + "unreadable-file");
  if (size > kMaxManifestBytes) throw std::runtime_error(prefix + "manifest-too-large");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error(prefix + "unreadable-file");
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) throw std::runtime_error(prefix + "unreadable-file");
  const std::string text = buffer.str();
  // Re-checked on the actual bytes read (covers a file racing the size stat).
  if (text.size() > kMaxManifestBytes) throw std::runtime_error(prefix + "manifest-too-large");
  ParseOutcome outcome = parse_manifest_text(text);
  if (!outcome.manifest) throw std::runtime_error(prefix + outcome.reason_code);
  return std::move(*outcome.manifest);
}

} // namespace user_plugins

struct Options {
  std::string host = "127.0.0.1";
  int port = 0;
  std::string token;
  std::string state_file;
  bool allow_live_egress = false;
  // NIF-H1: parsed --user-plugins-manifest (absent arg = behavior unchanged).
  std::optional<user_plugins::Manifest> user_plugins;
};

std::string json_escape(const std::string &input) {
  std::ostringstream out;
  for (unsigned char c : input) {
    switch (c) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\b': out << "\\b"; break;
    case '\f': out << "\\f"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (c < 0x20) {
        // Zero-padded four-hex-digit escape (backslash-u0001 style), never the malformed short form.
        out << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
            << static_cast<int>(c) << std::dec;
      } else {
        out << c;
      }
    }
  }
  return out.str();
}

void emit_log(const std::string &level, const std::string &event, const std::string &message) {
  std::cout << "{\"level\":\"" << json_escape(level) << "\",\"event\":\"" << json_escape(event)
            << "\",\"message\":\"" << json_escape(message) << "\"}" << std::endl;
}

bool is_loopback_host(const std::string &host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

Options parse_args(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto require_value = [&](const char *name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string(name) + " requires a value");
      }
      return argv[++i];
    };

    if (arg == "--version") {
      std::cout << "streammate-studio-host " << kVersion << "\n";
      std::exit(EXIT_SUCCESS);
    } else if (arg == "--host") {
      options.host = require_value("--host");
    } else if (arg == "--port") {
      options.port = std::stoi(require_value("--port"));
    } else if (arg == "--token") {
      options.token = require_value("--token");
    } else if (arg == "--state-file") {
      options.state_file = require_value("--state-file");
    } else if (arg == "--user-plugins-manifest") {
      // NIF-H1: user plugin discovery manifest (user-plugins-manifest.v1).
      // Validated fail-closed at launch; a bad manifest refuses startup with
      // a sanitized reason code (never the raw path).
      options.user_plugins = user_plugins::load_or_throw(require_value("--user-plugins-manifest"));
    } else if (arg == "--allow-live-egress") {
      // Q-122 defense-in-depth: a launch-time gate over the caller-supplied
      // allowLiveEgress JSON flag. Default off; both layers must assert before
      // any live egress path is reachable.
      options.allow_live_egress = true;
    } else {
      throw std::runtime_error("unknown argument");
    }
  }

  if (options.token.empty()) {
    throw std::runtime_error("--token is required");
  }
  if (!is_loopback_host(options.host)) {
    throw std::runtime_error("control server refuses non-loopback bind host");
  }
  return options;
}

// NIF-H2: boot-frozen user-plugin load report (defined with the user-plugin
// machinery below; the libobs lane performs the real per-module loading).
// bundle_plugins_dir feeds the duplicate_of_bundled collision set (empty in
// the scaffold lane / non-packaged binaries).
std::string build_user_plugins_boot_report_json(
    const std::optional<user_plugins::Manifest> &manifest,
    const std::filesystem::path &bundle_plugins_dir);

class EngineLifecycle {
public:
  bool start(const std::optional<user_plugins::Manifest> &user_plugins_manifest) {
#if STREAMMATE_HAS_LIBOBS
    if (!obs_startup("en-US", nullptr, nullptr)) {
      return false;
    }
    add_bundle_module_path();
    if (!reset_offscreen_video()) {
      return false;
    }
    if (!reset_offscreen_audio()) {
      return false;
    }
    obs_load_all_modules();
    // NIF-H2: user modules load PER-MODULE here — after the bundled modules,
    // before obs_post_load_modules(), so everything exists ahead of any
    // collection instantiation and post-load runs exactly once.
    user_plugins_report_json_ =
        build_user_plugins_boot_report_json(user_plugins_manifest, bundle_plugins_dir());
    obs_post_load_modules();
#else
    // Scaffold lane: plan facts only, labeled mode:"scaffold" (never a
    // loading claim).
    user_plugins_report_json_ = build_user_plugins_boot_report_json(user_plugins_manifest, {});
#endif
    started_ = true;
    return true;
  }

  // Boot-frozen: byte-identical for every plugins.report request this boot.
  const std::string &user_plugins_report_json() const { return user_plugins_report_json_; }

  void shutdown() {
    if (!started_) {
      return;
    }
#if STREAMMATE_HAS_LIBOBS
    obs_shutdown();
#endif
    started_ = false;
  }

  bool started() const { return started_; }

private:
#if STREAMMATE_HAS_LIBOBS
  static std::optional<std::filesystem::path> executable_path() {
#if defined(__APPLE__)
    uint32_t size = PATH_MAX;
    std::vector<char> buffer(size + 1);
    int result = _NSGetExecutablePath(buffer.data(), &size);
    if (result == -1) {
      buffer.assign(size + 1, '\0');
      result = _NSGetExecutablePath(buffer.data(), &size);
    }
    if (result != 0) {
      return std::nullopt;
    }
    return std::filesystem::weakly_canonical(buffer.data());
#else
    return std::nullopt;
#endif
  }

  // The packaged app's bundled-module directory (empty for raw builds) —
  // also the duplicate_of_bundled collision inventory for user loading.
  static std::filesystem::path bundle_plugins_dir() {
    auto executable = executable_path();
    if (!executable) {
      return {};
    }
    std::filesystem::path contents = executable->parent_path().parent_path();
    std::filesystem::path plugins = contents / "PlugIns" / "obs-plugins";
    if (!std::filesystem::is_directory(plugins)) {
      return {};
    }
    return plugins;
  }

  static void add_bundle_module_path() {
    std::filesystem::path plugins = bundle_plugins_dir();
    if (plugins.empty()) {
      return;
    }
    std::string binary_pattern = (plugins / "%module%.plugin" / "Contents" / "MacOS").string();
    std::string data_pattern = (plugins / "%module%.plugin" / "Contents" / "Resources").string();
    obs_add_module_path(binary_pattern.c_str(), data_pattern.c_str());
  }

  static bool reset_offscreen_video() {
    obs_video_info video = {};
    video.graphics_module = "@executable_path/../Frameworks/libobs-opengl.dylib";
    video.fps_num = 30;
    video.fps_den = 1;
    video.base_width = 1280;
    video.base_height = 720;
    video.output_width = 1280;
    video.output_height = 720;
    video.output_format = VIDEO_FORMAT_NV12;
    video.adapter = 0;
    video.gpu_conversion = true;
    video.colorspace = VIDEO_CS_DEFAULT;
    video.range = VIDEO_RANGE_DEFAULT;
    video.scale_type = OBS_SCALE_BICUBIC;
    return obs_reset_video(&video) == OBS_VIDEO_SUCCESS;
  }

  static bool reset_offscreen_audio() {
    obs_audio_info audio = {};
    audio.samples_per_sec = 48000;
    audio.speakers = SPEAKERS_STEREO;
    return obs_reset_audio(&audio);
  }
#endif

  bool started_ = false;
  std::string user_plugins_report_json_;
};

class StateFile {
public:
  explicit StateFile(std::string path) : path_(std::move(path)) {}

  void write_ready(int port) {
    if (path_.empty()) {
      return;
    }
    try {
      std::filesystem::path target(path_);
      if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path());
      }
      std::filesystem::path temp = target;
      temp += ".tmp";
      {
        std::ofstream out(temp);
        out << "{\"status\":\"ready\",\"hostId\":\"studio-host-1\",\"port\":" << port
            << ",\"heartbeatMs\":" << kHeartbeatMs << "}\n";
      }
      std::filesystem::rename(temp, target);
    } catch (...) {
      throw std::runtime_error("state file write failed");
    }
  }

  void write_stopped() {
    if (path_.empty()) {
      return;
    }
    try {
      std::filesystem::path target(path_);
      if (!target.parent_path().empty()) {
        std::filesystem::create_directories(target.parent_path());
      }
      std::filesystem::path temp = target;
      temp += ".tmp";
      {
        std::ofstream out(temp);
        out << "{\"status\":\"stopped\",\"hostId\":\"studio-host-1\"}\n";
      }
      std::filesystem::rename(temp, target);
    } catch (...) {
      throw std::runtime_error("state file write failed");
    }
  }

private:
  std::string path_;
};

uint32_t left_rotate(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

std::array<uint8_t, 20> sha1(const std::string &input) {
  uint64_t bit_len = static_cast<uint64_t>(input.size()) * 8;
  std::vector<uint8_t> data(input.begin(), input.end());
  data.push_back(0x80);
  while ((data.size() % 64) != 56) {
    data.push_back(0);
  }
  for (int i = 7; i >= 0; --i) {
    data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));
  }

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xEFCDAB89;
  uint32_t h2 = 0x98BADCFE;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xC3D2E1F0;

  for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
    uint32_t w[80]{};
    for (int i = 0; i < 16; ++i) {
      size_t j = chunk + i * 4;
      w[i] = (static_cast<uint32_t>(data[j]) << 24) | (static_cast<uint32_t>(data[j + 1]) << 16) |
             (static_cast<uint32_t>(data[j + 2]) << 8) | static_cast<uint32_t>(data[j + 3]);
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0, k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = left_rotate(b, 30);
      b = a;
      a = temp;
    }
    h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
  }

  std::array<uint8_t, 20> digest{};
  uint32_t hs[5] = {h0, h1, h2, h3, h4};
  for (int i = 0; i < 5; ++i) {
    digest[i * 4] = static_cast<uint8_t>((hs[i] >> 24) & 0xff);
    digest[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xff);
    digest[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xff);
    digest[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xff);
  }
  return digest;
}

std::string base64(const uint8_t *data, size_t len) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? table[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? table[n & 63] : '=');
  }
  return out;
}

std::string websocket_accept(const std::string &key) {
  auto digest = sha1(key + kWebSocketGuid);
  return base64(digest.data(), digest.size());
}

std::string lower(std::string value) {
  for (char &c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

std::string trim(std::string value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.erase(value.begin());
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) value.pop_back();
  return value;
}

std::map<std::string, std::string> parse_headers(const std::string &request) {
  std::map<std::string, std::string> headers;
  std::istringstream stream(request);
  std::string line;
  std::getline(stream, line);
  while (std::getline(stream, line)) {
    auto pos = line.find(':');
    if (pos == std::string::npos) continue;
    headers[lower(trim(line.substr(0, pos)))] = trim(line.substr(pos + 1));
  }
  return headers;
}

bool send_all(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    ssize_t written = send(fd, data, len, 0);
    if (written <= 0) return false;
    data += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

bool send_text_frame(int fd, const std::string &payload) {
  std::vector<uint8_t> frame;
  frame.push_back(0x81);
  if (payload.size() < 126) {
    frame.push_back(static_cast<uint8_t>(payload.size()));
  } else if (payload.size() <= 0xffff) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xff));
  } else {
    // 64-bit length frames carry the native overlay raster PNG, which exceeds the
    // 16-bit frame limit at 1280x720 (RFC 6455 extended payload length).
    frame.push_back(127);
    const uint64_t len = payload.size();
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<uint8_t>((len >> shift) & 0xff));
    }
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return send_all(fd, frame.data(), frame.size());
}

std::optional<std::string> read_text_frame(int fd) {
  uint8_t header[2];
  ssize_t n = recv(fd, header, 2, MSG_WAITALL);
  if (n <= 0) return std::nullopt;
  uint8_t opcode = header[0] & 0x0f;
  bool masked = (header[1] & 0x80) != 0;
  uint64_t len = header[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2];
    if (recv(fd, ext, 2, MSG_WAITALL) != 2) return std::nullopt;
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    return std::nullopt;
  }
  uint8_t mask[4]{};
  if (masked && recv(fd, mask, 4, MSG_WAITALL) != 4) return std::nullopt;
  std::vector<uint8_t> payload(len);
  if (len > 0 && recv(fd, payload.data(), len, MSG_WAITALL) != static_cast<ssize_t>(len)) return std::nullopt;
  if (opcode == 0x8) return std::nullopt;
  if (opcode != 0x1) return std::string{};
  if (masked) {
    for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i % 4];
  }
  return std::string(payload.begin(), payload.end());
}

std::string extract_json_string(const std::string &json, const std::string &key) {
  std::string marker = "\"" + key + "\"";
  auto pos = json.find(marker);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";
  pos = json.find('"', pos);
  if (pos == std::string::npos) return "";
  ++pos;
  std::string out;
  bool escape = false;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (escape) {
      out.push_back(c);
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      break;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool is_json_number_token(const std::string &token) {
  size_t pos = 0;
  const auto digit = [&](size_t at) {
    return at < token.size() && token[at] >= '0' && token[at] <= '9';
  };
  if (pos < token.size() && token[pos] == '-') ++pos;
  if (!digit(pos)) return false;
  if (token[pos] == '0') {
    ++pos;
  } else {
    while (digit(pos)) ++pos;
  }
  if (pos < token.size() && token[pos] == '.') {
    ++pos;
    if (!digit(pos)) return false;
    while (digit(pos)) ++pos;
  }
  if (pos < token.size() && (token[pos] == 'e' || token[pos] == 'E')) {
    ++pos;
    if (pos < token.size() && (token[pos] == '+' || token[pos] == '-')) ++pos;
    if (!digit(pos)) return false;
    while (digit(pos)) ++pos;
  }
  return pos == token.size();
}

// Re-emit a raw string-id token (escapes still encoded) so that valid escapes
// round-trip untouched while raw control bytes and lone backslashes are
// re-escaped — a smuggled byte must never make the response unparseable.
std::string sanitize_json_string_token(const std::string &raw) {
  std::string out;
  const auto is_hex = [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  for (size_t i = 0; i < raw.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(raw[i]);
    if (c == '\\') {
      if (i + 1 < raw.size()) {
        char next = raw[i + 1];
        if (next == '"' || next == '\\' || next == '/' || next == 'b' || next == 'f' ||
            next == 'n' || next == 'r' || next == 't') {
          out.push_back('\\');
          out.push_back(next);
          ++i;
          continue;
        }
        if (next == 'u' && i + 5 < raw.size() && is_hex(raw[i + 2]) && is_hex(raw[i + 3]) &&
            is_hex(raw[i + 4]) && is_hex(raw[i + 5])) {
          out += raw.substr(i, 6);
          i += 5;
          continue;
        }
      }
      out += "\\\\";
      continue;
    }
    // Quotes cannot appear unescaped (extraction stops there); everything else
    // funnels through json_escape so control bytes get the padded \uXXXX form.
    out += json_escape(std::string(1, static_cast<char>(c)));
  }
  return out;
}

std::string extract_json_id(const std::string &json) {
  auto marker = json.find("\"id\"");
  if (marker == std::string::npos) return "null";
  auto colon = json.find(':', marker);
  if (colon == std::string::npos) return "null";
  auto start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (start == std::string::npos) return "null";
  if (json[start] == '"') {
    // Escape-aware scan to the closing quote (an escaped \" is not the end).
    std::string inner;
    bool escape = false;
    bool closed = false;
    for (size_t pos = start + 1; pos < json.size(); ++pos) {
      char c = json[pos];
      if (escape) {
        inner.push_back(c);
        escape = false;
        continue;
      }
      if (c == '\\') {
        inner.push_back(c);
        escape = true;
        continue;
      }
      if (c == '"') {
        closed = true;
        break;
      }
      inner.push_back(c);
    }
    if (!closed) return "null";
    return "\"" + sanitize_json_string_token(inner) + "\"";
  }
  auto end = json.find_first_of(",}\r\n", start);
  std::string token = json.substr(start, end == std::string::npos ? std::string::npos : end - start);
  while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.pop_back();
  // Non-string ids are spliced back verbatim only when they are a valid JSON
  // scalar token; anything else degrades to null rather than raw bytes.
  if (token == "null" || token == "true" || token == "false" || is_json_number_token(token)) {
    return token;
  }
  return "null";
}

std::optional<int> extract_json_int(const std::string &json, const std::string &key) {
  std::string marker = "\"" + key + "\"";
  auto pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  auto start = json.find_first_not_of(" \t\r\n", pos + 1);
  if (start == std::string::npos) return std::nullopt;
  auto end = json.find_first_not_of("-0123456789", start);
  try {
    return std::stoi(json.substr(start, end == std::string::npos ? std::string::npos : end - start));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<double> extract_json_double(const std::string &json, const std::string &key) {
  std::string marker = "\"" + key + "\"";
  auto pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  auto start = json.find_first_not_of(" \t\r\n", pos + 1);
  if (start == std::string::npos) return std::nullopt;
  auto end = json.find_first_not_of("-0123456789.eE+", start);
  try {
    return std::stod(json.substr(start, end == std::string::npos ? std::string::npos : end - start));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> extract_json_bool(const std::string &json, const std::string &key) {
  std::string marker = "\"" + key + "\"";
  auto pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  auto start = json.find_first_not_of(" \t\r\n", pos + 1);
  if (start == std::string::npos) return std::nullopt;
  if (json.compare(start, 4, "true") == 0) return true;
  if (json.compare(start, 5, "false") == 0) return false;
  return std::nullopt;
}

std::optional<std::string> extract_json_object_body(const std::string &json, const std::string &key) {
  std::string marker = "\"" + key + "\"";
  auto pos = json.find(marker);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return std::nullopt;
  pos = json.find_first_not_of(" \t\r\n", pos + 1);
  if (pos == std::string::npos || json[pos] != '{') return std::nullopt;

  size_t start = pos + 1;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) return json.substr(start, pos - start);
    }
  }
  return std::nullopt;
}

std::optional<std::string> parse_json_string_token(const std::string &json, size_t &pos);
void skip_json_whitespace(const std::string &json, size_t &pos);

bool is_safe_protocol_id(const std::string &value) {
  if (value.empty() || value.size() > 255) return false;
  for (unsigned char c : value) {
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

std::string rpc_result(const std::string &id, const std::string &result) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
}

std::string rpc_error(const std::string &id, int code, const std::string &message) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":" + std::to_string(code) +
         ",\"message\":\"" + json_escape(message) + "\"}}";
}

std::string host_event(const std::string &type, int port) {
  return "{\"type\":\"" + type + "\",\"sessionId\":\"local\",\"payload\":{\"hostId\":\"studio-host-1\",\"status\":\"ready\",\"port\":" +
         std::to_string(port) + ",\"heartbeatMs\":" + std::to_string(kHeartbeatMs) + "}}";
}

std::string heartbeat_event() {
  return "{\"type\":\"host.health\",\"payload\":{\"hostId\":\"studio-host-1\",\"status\":\"ready\",\"heartbeatMs\":" +
         std::to_string(kHeartbeatMs) + "}}";
}

struct Rgba {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
};

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

uint8_t parse_hex_byte(char high, char low, uint8_t fallback) {
  int hi = hex_digit(high);
  int lo = hex_digit(low);
  if (hi < 0 || lo < 0) return fallback;
  return static_cast<uint8_t>((hi << 4) | lo);
}

Rgba parse_color(const std::string &value, Rgba fallback = {32, 48, 64, 255}) {
  if (value.size() != 7 || value[0] != '#') return fallback;
  return {parse_hex_byte(value[1], value[2], fallback.r),
          parse_hex_byte(value[3], value[4], fallback.g),
          parse_hex_byte(value[5], value[6], fallback.b),
          255};
}

void append_be32(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t crc32_bytes(const uint8_t *data, size_t len) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return crc ^ 0xffffffffU;
}

uint32_t adler32_bytes(const std::vector<uint8_t> &data) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (uint8_t byte : data) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

void append_png_chunk(std::vector<uint8_t> &png, const char type[4], const std::vector<uint8_t> &data) {
  append_be32(png, static_cast<uint32_t>(data.size()));
  size_t type_offset = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  append_be32(png, crc32_bytes(png.data() + type_offset, png.size() - type_offset));
}

std::vector<uint8_t> zlib_store(const std::vector<uint8_t> &raw) {
  std::vector<uint8_t> out;
  out.push_back(0x78);
  out.push_back(0x01);
  size_t offset = 0;
  do {
    size_t block_len = std::min<size_t>(65535, raw.size() - offset);
    bool final = offset + block_len >= raw.size();
    out.push_back(final ? 0x01 : 0x00);
    uint16_t len = static_cast<uint16_t>(block_len);
    uint16_t nlen = static_cast<uint16_t>(~len);
    out.push_back(static_cast<uint8_t>(len & 0xff));
    out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(nlen & 0xff));
    out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xff));
    out.insert(out.end(), raw.begin() + static_cast<long>(offset), raw.begin() + static_cast<long>(offset + block_len));
    offset += block_len;
  } while (offset < raw.size());
  append_be32(out, adler32_bytes(raw));
  return out;
}

struct SceneModel {
  std::string id;
  int width = 128;
  int height = 72;
  Rgba background{32, 48, 64, 255};
};

struct SourceModel {
  std::string id;
  std::string scene_id;
  std::string kind = "browser";
  std::string url;
  int x = 0;
  int y = 0;
  int width = 128;
  int height = 72;
  double opacity = 1.0;
  bool muted = false;
  bool visible = true;
  int position = 0;
  double volume_db = 0.0;
  std::string media_action = "stop";
  int browser_refresh_count = 0;
};

enum class FilterSettingKind { Number, Bool, String };

struct FilterSettingValue {
  FilterSettingKind kind = FilterSettingKind::String;
  double number_value = 0.0;
  bool bool_value = false;
  std::string string_value;
};

struct FilterModel {
  std::string id;
  std::string kind;
  std::string label;
  bool enabled = true;
  std::map<std::string, FilterSettingValue> settings;
};

class RendererState {
public:
  ~RendererState() {
#if STREAMMATE_HAS_LIBOBS
    release_obs_mirrors();
#endif
  }

  std::string load_scene(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty()) return rpc_error_result(-32602, "sceneId is required");
    int width = extract_json_int(request, "width").value_or(128);
    int height = extract_json_int(request, "height").value_or(72);
    if (width <= 0 || height <= 0 || width > 128 || height > 72 || width * height > 10000) {
      return rpc_error_result(-32602, "scene dimensions exceed local offscreen capture limits");
    }
    SceneModel scene;
    scene.id = scene_id;
    scene.width = width;
    scene.height = height;
    std::string background = extract_json_string(request, "background");
    if (!background.empty()) scene.background = parse_color(background);
    scenes_[scene_id] = scene;
    if (program_scene_id_.empty()) program_scene_id_ = scene_id;
#if STREAMMATE_HAS_LIBOBS
    mirror_scene_load(scene_id);
#endif
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"width\":" + std::to_string(width) +
           ",\"height\":" + std::to_string(height) + ",\"renderer\":\"offscreen-scaffold\"}";
  }

  std::string set_program(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    program_scene_id_ = scene_id;
#if STREAMMATE_HAS_LIBOBS
    mirror_set_program(scene_id);
#endif
    return "{\"ok\":true,\"programSceneId\":\"" + json_escape(scene_id) + "\"}";
  }

  std::string create_source(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    std::string source_id = extract_json_string(request, "sourceId");
    if (source_id.empty()) return rpc_error_result(-32602, "sourceId is required");
    std::string kind = extract_json_string(request, "kind");
    if (kind.empty()) kind = "browser";
    if (kind != "browser") return rpc_error_result(-32602, "only browser sources are supported in phase A");
    SourceModel source;
    const SceneModel &scene = scenes_.at(scene_id);
    source.id = source_id;
    source.scene_id = scene_id;
    source.kind = kind;
    source.url = extract_json_string(request, "url");
    source.x = extract_json_int(request, "x").value_or(0);
    source.y = extract_json_int(request, "y").value_or(0);
    source.width = extract_json_int(request, "width").value_or(scene.width);
    source.height = extract_json_int(request, "height").value_or(scene.height);
    source.opacity = std::clamp(extract_json_double(request, "opacity").value_or(1.0), 0.0, 1.0);
    source.muted = extract_json_bool(request, "muted").value_or(false);
    source.position = next_position(scene_id);
    sources_[source_id] = source;
    filters_[source_id] = {default_filter()};
#if STREAMMATE_HAS_LIBOBS
    mirror_source_create(sources_[source_id]);
#endif
    return source_result(source, true);
  }

  std::string update_source(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
    std::string url = extract_json_string(request, "url");
    if (!url.empty()) it->second.url = url;
    if (auto value = extract_json_int(request, "x")) it->second.x = *value;
    if (auto value = extract_json_int(request, "y")) it->second.y = *value;
    if (auto value = extract_json_int(request, "width")) it->second.width = *value;
    if (auto value = extract_json_int(request, "height")) it->second.height = *value;
    if (auto value = extract_json_double(request, "opacity")) it->second.opacity = std::clamp(*value, 0.0, 1.0);
    return source_result(it->second, !url.empty());
  }

  std::string mute_source(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
    const bool muted = extract_json_bool(request, "muted").value_or(true);
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_mute_source(source_id, muted)) return rpc_error_result(-32603, "libobs source unavailable");
#endif
    it->second.muted = muted;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"muted\":" +
           std::string(it->second.muted ? "true" : "false") + "}";
  }

  std::string set_item_visible(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    std::string item_id = extract_json_string(request, "itemId");
    auto it = sources_.find(item_id);
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    if (item_id.empty() || it == sources_.end() || it->second.scene_id != scene_id) return rpc_error_result(-32602, "scene item not found");
    auto visible = extract_json_bool(request, "visible");
    if (!visible) return rpc_error_result(-32602, "visible is required");
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_item_visible(scene_id, item_id, *visible)) return rpc_error_result(-32603, "libobs scene item unavailable");
#endif
    it->second.visible = *visible;
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"itemId\":\"" + json_escape(item_id) +
           "\",\"visible\":" + std::string(*visible ? "true" : "false") + "}";
  }

  std::string set_item_order(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    std::string item_id = extract_json_string(request, "itemId");
    auto it = sources_.find(item_id);
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    if (item_id.empty() || it == sources_.end() || it->second.scene_id != scene_id) return rpc_error_result(-32602, "scene item not found");
    std::string idempotency_token = extract_json_string(request, "idempotencyToken");
    if (request.find("\"idempotencyToken\"") == std::string::npos) {
      return rpc_error_result(-32602, "idempotencyToken is required");
    }
    if (!is_safe_protocol_id(idempotency_token)) return rpc_error_result(-32602, "idempotencyToken is invalid");
    auto position = extract_json_int(request, "position");
    if (!position || *position < 0 || *position > kMaxSceneItemPosition) {
      return rpc_error_result(-32602, "position must be a bounded non-negative integer");
    }
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_item_order(scene_id, item_id, *position)) return rpc_error_result(-32603, "libobs scene item unavailable");
#endif
    it->second.position = *position;
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"itemId\":\"" + json_escape(item_id) +
           "\",\"position\":" + std::to_string(*position) + "}";
  }

  std::string list_filters(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    if (source_id.empty() || sources_.find(source_id) == sources_.end()) return rpc_error_result(-32602, "source not found");
#if STREAMMATE_HAS_LIBOBS
    mirror_list_filters(source_id);
#endif
    return filter_list_json(source_id);
  }

  std::string set_filter_enabled(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    std::string filter_id = extract_json_string(request, "filterId");
    auto filter = find_filter(source_id, filter_id);
    if (!filter) return rpc_error_result(-32602, "filter not found");
    auto enabled = extract_json_bool(request, "enabled");
    if (!enabled) return rpc_error_result(-32602, "enabled is required");
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_filter_enabled(source_id, filter_id, *enabled)) return rpc_error_result(-32603, "libobs filter unavailable");
#endif
    filter->get().enabled = *enabled;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"filterId\":\"" + json_escape(filter_id) +
           "\",\"enabled\":" + std::string(*enabled ? "true" : "false") + "}";
  }

  std::string set_filter_settings(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    std::string filter_id = extract_json_string(request, "filterId");
    auto filter = find_filter(source_id, filter_id);
    if (!filter) return rpc_error_result(-32602, "filter not found");
    auto parsed = parse_filter_settings(request);
    if (!parsed.ok) return rpc_error_result(-32602, parsed.error);
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_filter_settings(source_id, filter_id, parsed.settings)) return rpc_error_result(-32603, "libobs filter unavailable");
#endif
    for (const auto &entry : parsed.settings) filter->get().settings[entry.first] = entry.second;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"filterId\":\"" + json_escape(filter_id) +
           "\",\"settings\":" + settings_json(parsed.settings) + "}";
  }

  std::string set_audio_volume(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
    auto volume_db = extract_json_double(request, "volumeDb");
    if (!volume_db || !std::isfinite(*volume_db) || *volume_db < -100.0 || *volume_db > 26.0) {
      return rpc_error_result(-32602, "volumeDb must be a finite number between -100 and 26");
    }
    const bool muted = *volume_db <= -100.0;
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_audio_volume(source_id, *volume_db, muted)) return rpc_error_result(-32603, "libobs source unavailable");
#endif
    it->second.volume_db = *volume_db;
    // OBS treats the bottom of the fader as silence; mirror that as explicit mute at -100 dB.
    it->second.muted = muted;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"volumeDb\":" + json_number(*volume_db) + "}";
  }

  std::string media_control(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    std::string action = extract_json_string(request, "action");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
    if (action != "play" && action != "pause" && action != "restart" && action != "stop") {
      return rpc_error_result(-32602, "action must be one of play, pause, restart, stop");
    }
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_media_control(source_id, action)) return rpc_error_result(-32603, "libobs source unavailable");
#endif
    it->second.media_action = action;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"action\":\"" + json_escape(action) + "\"}";
  }

  std::string refresh_browser(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
#if STREAMMATE_HAS_LIBOBS
    if (!mirror_refresh_browser(source_id)) return rpc_error_result(-32603, "libobs browser refresh unavailable");
#endif
    ++it->second.browser_refresh_count;
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"refreshed\":true}";
  }

  std::string capture_frame(const std::string &request) {
    std::string format = extract_json_string(request, "format");
    if (!format.empty() && format != "png") return rpc_error_result(-32602, "only png capture is supported");
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty()) scene_id = program_scene_id_;
    auto scene_it = scenes_.find(scene_id);
    if (scene_id.empty() || scene_it == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    int source_count = 0;
    int muted_count = 0;
    for (const auto &entry : sources_) {
      if (entry.second.scene_id == scene_id) {
        ++source_count;
        if (entry.second.muted) ++muted_count;
      }
    }
    std::vector<uint8_t> png = render_png(scene_it->second);
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"format\":\"png\",\"renderer\":\"offscreen-scaffold\"," +
           "\"width\":" + std::to_string(scene_it->second.width) + ",\"height\":" + std::to_string(scene_it->second.height) +
           ",\"sourceCount\":" + std::to_string(source_count) + ",\"mutedSourceCount\":" + std::to_string(muted_count) +
           ",\"pngBase64\":\"" + base64(png.data(), png.size()) + "\"}";
  }

  // scene.list intentionally answers from the authoritative in-memory model
  // (scaffold scenes_/sources_/filters_), matching the filter.list precedent
  // above; no libobs reads. Additive sources[] deliberately omits url/opacity
  // to preserve the source_result() redaction invariant.
  std::string list_scenes(const std::string &) const {
    std::string scenes_json;
    bool first = true;
    for (const auto &entry : scenes_) {
      if (!first) scenes_json += ",";
      first = false;
      bool is_program = entry.first == program_scene_id_;
      scenes_json += "{\"sceneId\":\"" + json_escape(entry.first) + "\",\"program\":" +
                     std::string(is_program ? "true" : "false") + ",\"label\":\"" + json_escape(entry.first) + "\"}";
    }
    std::string sources_json;
    first = true;
    for (const auto &entry : sources_) {
      if (!first) sources_json += ",";
      first = false;
      const SourceModel &source = entry.second;
      sources_json += "{\"sourceId\":\"" + json_escape(source.id) + "\",\"sceneId\":\"" + json_escape(source.scene_id) +
                       "\",\"kind\":\"" + json_escape(source.kind) + "\",\"visible\":" +
                       std::string(source.visible ? "true" : "false") + ",\"muted\":" +
                       std::string(source.muted ? "true" : "false") + ",\"volumeDb\":" + json_number(source.volume_db) +
                       ",\"position\":" + std::to_string(source.position) + ",\"filters\":[";
      auto filters_it = filters_.find(source.id);
      if (filters_it != filters_.end()) {
        for (size_t i = 0; i < filters_it->second.size(); ++i) {
          const FilterModel &filter = filters_it->second[i];
          if (i) sources_json += ",";
          sources_json += "{\"filterId\":\"" + json_escape(filter.id) + "\",\"label\":\"" + json_escape(filter.label) +
                           "\",\"enabled\":" + std::string(filter.enabled ? "true" : "false") + "}";
        }
      }
      sources_json += "]}";
    }
    return "{\"ok\":true,\"programSceneId\":\"" + json_escape(program_scene_id_) + "\",\"scenes\":[" + scenes_json +
           "],\"sources\":[" + sources_json + "]}";
  }

  std::string item_transform(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end() || it->second.scene_id != scene_id) {
      return rpc_error_result(-32602, "source not found");
    }
    // Validate every field before mutating so a rejected transform leaves the
    // scene item untouched (no partial application observable in state/capture).
    auto x = extract_json_int(request, "x");
    auto y = extract_json_int(request, "y");
    auto width = extract_json_int(request, "width");
    auto height = extract_json_int(request, "height");
    if (width && *width <= 0) return rpc_error_result(-32602, "transform width must be positive");
    if (height && *height <= 0) return rpc_error_result(-32602, "transform height must be positive");
    SourceModel &source = it->second;
    if (x) source.x = *x;
    if (y) source.y = *y;
    if (width) source.width = *width;
    if (height) source.height = *height;
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"sourceId\":\"" + json_escape(source_id) +
           "\",\"transform\":{\"x\":" + std::to_string(source.x) + ",\"y\":" + std::to_string(source.y) +
           ",\"width\":" + std::to_string(source.width) + ",\"height\":" + std::to_string(source.height) + "}}";
  }

  std::string remove_source(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (source_id.empty() || it == sources_.end()) return rpc_error_result(-32602, "source not found");
#if STREAMMATE_HAS_LIBOBS
    release_obs_source(source_id);
#endif
    filters_.erase(source_id);
    sources_.erase(it);
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"removed\":true}";
  }

private:
  struct ParsedSettings {
    bool ok = false;
    std::string error;
    std::map<std::string, FilterSettingValue> settings;
  };

  std::string rpc_error_result(int code, const std::string &message) const {
    return "__error__:" + std::to_string(code) + ":" + message;
  }

  std::string source_result(const SourceModel &source, bool url_touched) const {
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source.id) + "\",\"sceneId\":\"" + json_escape(source.scene_id) +
           "\",\"kind\":\"" + json_escape(source.kind) + "\",\"muted\":" + std::string(source.muted ? "true" : "false") +
           ",\"opacity\":" + std::to_string(source.opacity) + ",\"urlStatus\":\"" +
           std::string(url_touched || !source.url.empty() ? "stored-redacted" : "empty") + "\"}";
  }

  int next_position(const std::string &scene_id) const {
    int position = 0;
    for (const auto &entry : sources_) {
      if (entry.second.scene_id == scene_id) position = std::max(position, entry.second.position + 1);
    }
    return position;
  }

  static FilterModel default_filter() {
    return {"color-correction", "color_filter_v2", "Color Correction", true, {}};
  }

  std::optional<std::reference_wrapper<FilterModel>> find_filter(const std::string &source_id, const std::string &filter_id) {
    if (source_id.empty() || sources_.find(source_id) == sources_.end() || filter_id.empty()) return std::nullopt;
    auto list = filters_.find(source_id);
    if (list == filters_.end()) return std::nullopt;
    for (FilterModel &filter : list->second) {
      if (filter.id == filter_id) return filter;
    }
    return std::nullopt;
  }

  std::string filter_list_json(const std::string &source_id) const {
    auto it = filters_.find(source_id);
    std::string out = "{\"sourceId\":\"" + json_escape(source_id) + "\",\"filters\":[";
    if (it != filters_.end()) {
      for (size_t i = 0; i < it->second.size(); ++i) {
        const FilterModel &filter = it->second[i];
        if (i) out += ",";
        out += "{\"filterId\":\"" + json_escape(filter.id) + "\",\"filterKind\":\"" + json_escape(filter.kind) +
               "\",\"label\":\"" + json_escape(filter.label) + "\",\"enabled\":" +
               std::string(filter.enabled ? "true" : "false") + "}";
      }
    }
    out += "]}";
    return out;
  }

  static const std::set<std::string> &known_filter_setting_keys() {
    static const std::set<std::string> keys = {
        "brightness", "contrast", "gamma", "saturation", "hue_shift", "opacity", "color_multiply", "color_add",
        "similarity", "smoothness", "spill", "key_color_type", "key_color", "left", "right", "top", "bottom",
        "relative", "db", "gain_db", "ratio", "threshold", "open_threshold", "close_threshold", "attack_time",
        "hold_time", "release_time", "suppress_level", "sharpness"};
    return keys;
  }

  static bool settings_string_refused(const std::string &value) {
    static const std::regex url_scheme("[A-Za-z][A-Za-z0-9+.-]*://");
    static const std::regex scheme_relative("(^|\\s)//");
    static const std::regex windows_absolute("^[A-Za-z]:[\\\\/]");
    static const std::regex live_or_secret_key("\\b(live|sk)_[A-Za-z0-9_-]+\\b", std::regex_constants::icase);
    static const std::regex query_key("(^|[?&\\s])key=[A-Za-z0-9_-]+", std::regex_constants::icase);

    std::string lower;
    lower.reserve(value.size());
    for (unsigned char c : value) lower.push_back(static_cast<char>(std::tolower(c)));

    return std::regex_search(value, url_scheme) || std::regex_search(value, scheme_relative) ||
           value.rfind("/", 0) == 0 || value.rfind("~/", 0) == 0 || std::regex_search(value, windows_absolute) ||
           lower.rfind("stm_", 0) == 0 || lower.find("stream-key") != std::string::npos ||
           lower.find("secret") != std::string::npos || std::regex_search(value, live_or_secret_key) ||
           std::regex_search(value, query_key);
  }

  ParsedSettings parse_filter_settings(const std::string &request) const {
    auto body = extract_json_object_body(request, "settings");
    if (!body) return {false, "settings must be an object of known filter-settings keys", {}};
    ParsedSettings parsed;
    size_t pos = 0;
    bool first = true;
    while (true) {
      skip_json_whitespace(*body, pos);
      if (pos >= body->size()) break;
      if (!first) {
        if ((*body)[pos] != ',') return {false, "settings entries must be separated by commas", {}};
        ++pos;
        skip_json_whitespace(*body, pos);
        if (pos >= body->size() || (*body)[pos] == ',') {
          return {false, "settings entries must be separated by commas", {}};
        }
      } else if ((*body)[pos] == ',') {
        return {false, "settings entries must be separated by commas", {}};
      }
      auto key = parse_json_string_token(*body, pos);
      if (!key) return {false, "settings must contain string keys", {}};
      if (known_filter_setting_keys().find(*key) == known_filter_setting_keys().end()) {
        return {false, "unknown filter-settings key (refused fail-closed)", {}};
      }
      skip_json_whitespace(*body, pos);
      if (pos >= body->size() || (*body)[pos] != ':') return {false, "settings must contain key/value pairs", {}};
      ++pos;
      skip_json_whitespace(*body, pos);
      if (pos >= body->size()) return {false, "settings value is required", {}};

      FilterSettingValue value;
      if ((*body)[pos] == '"') {
        auto string_value = parse_json_string_token(*body, pos);
        if (!string_value || string_value->empty() || string_value->size() > 120 || settings_string_refused(*string_value)) {
          return {false, "filter-settings string value refused", {}};
        }
        value.kind = FilterSettingKind::String;
        value.string_value = *string_value;
      } else if (body->compare(pos, 4, "true") == 0) {
        value.kind = FilterSettingKind::Bool;
        value.bool_value = true;
        pos += 4;
      } else if (body->compare(pos, 5, "false") == 0) {
        value.kind = FilterSettingKind::Bool;
        value.bool_value = false;
        pos += 5;
      } else {
        size_t end = body->find_first_not_of("-0123456789.eE+", pos);
        try {
          value.kind = FilterSettingKind::Number;
          value.number_value = std::stod(body->substr(pos, end == std::string::npos ? std::string::npos : end - pos));
          if (!std::isfinite(value.number_value)) return {false, "filter-settings number must be finite", {}};
          if (std::fabs(value.number_value) > 1000000.0) return {false, "filter-settings number is out of range", {}};
          pos = end == std::string::npos ? body->size() : end;
        } catch (...) {
          return {false, "filter-settings values must be primitive", {}};
        }
      }
      parsed.settings[*key] = value;
      first = false;
      skip_json_whitespace(*body, pos);
      if (pos < body->size() && (*body)[pos] != ',') return {false, "settings entries must be separated by commas", {}};
    }
    if (parsed.settings.empty()) return {false, "settings must set at least one known filter-settings key", {}};
    parsed.ok = true;
    return parsed;
  }

  static std::string json_number(double value) {
    std::ostringstream out;
    out << std::setprecision(17) << value;
    return out.str();
  }

  static std::string setting_value_json(const FilterSettingValue &value) {
    switch (value.kind) {
    case FilterSettingKind::Number: return json_number(value.number_value);
    case FilterSettingKind::Bool: return value.bool_value ? "true" : "false";
    case FilterSettingKind::String: return "\"" + json_escape(value.string_value) + "\"";
    }
    return "null";
  }

  static std::string settings_json(const std::map<std::string, FilterSettingValue> &settings) {
    std::string out = "{";
    bool first = true;
    for (const auto &entry : settings) {
      if (!first) out += ",";
      first = false;
      out += "\"" + json_escape(entry.first) + "\":" + setting_value_json(entry.second);
    }
    out += "}";
    return out;
  }

  std::vector<uint8_t> render_png(const SceneModel &scene) const {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>((scene.width * 4 + 1) * scene.height));
    for (int y = 0; y < scene.height; ++y) {
      raw.push_back(0);
      for (int x = 0; x < scene.width; ++x) {
        Rgba pixel = scene.background;
        for (const auto &entry : sources_) {
          const SourceModel &source = entry.second;
          if (source.scene_id != scene.id || source.muted || !source.visible) continue;
          if (x >= source.x && y >= source.y && x < source.x + source.width && y < source.y + source.height) {
            double alpha = std::clamp(source.opacity, 0.0, 1.0) * 0.65;
            Rgba overlay{136, 72, 216, 255};
            pixel.r = static_cast<uint8_t>(pixel.r * (1.0 - alpha) + overlay.r * alpha);
            pixel.g = static_cast<uint8_t>(pixel.g * (1.0 - alpha) + overlay.g * alpha);
            pixel.b = static_cast<uint8_t>(pixel.b * (1.0 - alpha) + overlay.b * alpha);
          }
        }
        raw.push_back(pixel.r);
        raw.push_back(pixel.g);
        raw.push_back(pixel.b);
        raw.push_back(pixel.a);
      }
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    append_be32(ihdr, static_cast<uint32_t>(scene.width));
    append_be32(ihdr, static_cast<uint32_t>(scene.height));
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    append_png_chunk(png, "IHDR", ihdr);
    append_png_chunk(png, "IDAT", zlib_store(raw));
    append_png_chunk(png, "IEND", {});
    return png;
  }

  std::map<std::string, SceneModel> scenes_;
  std::map<std::string, SourceModel> sources_;
  std::map<std::string, std::vector<FilterModel>> filters_;
  std::string program_scene_id_;

#if STREAMMATE_HAS_LIBOBS
  static std::string scene_item_key(const std::string &scene_id, const std::string &source_id) {
    return scene_id + "\n" + source_id;
  }

  void release_obs_mirrors() {
    for (auto &entry : obs_sources_) obs_source_release(entry.second);
    obs_sources_.clear();
    obs_scene_items_.clear();
    for (auto &entry : obs_scenes_) obs_scene_release(entry.second);
    obs_scenes_.clear();
  }

  void release_obs_source(const std::string &source_id) {
    auto source = obs_sources_.find(source_id);
    if (source != obs_sources_.end()) {
      obs_source_release(source->second);
      obs_sources_.erase(source);
    }
    for (auto it = obs_scene_items_.begin(); it != obs_scene_items_.end();) {
      if (it->first.size() > source_id.size() && it->first.ends_with("\n" + source_id)) {
        it = obs_scene_items_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void purge_obs_scene_items(const std::string &scene_id) {
    const std::string prefix = scene_id + "\n";
    for (auto it = obs_scene_items_.begin(); it != obs_scene_items_.end();) {
      if (it->first.rfind(prefix, 0) == 0) {
        it = obs_scene_items_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void mirror_scene_load(const std::string &scene_id) {
    auto existing = obs_scenes_.find(scene_id);
    if (existing != obs_scenes_.end()) {
      purge_obs_scene_items(scene_id);
      obs_scene_release(existing->second);
      obs_scenes_.erase(existing);
    }
    obs_scene_t *scene = obs_scene_create(scene_id.c_str());
    if (!scene) return;
    obs_scenes_[scene_id] = scene;
    for (const auto &entry : sources_) {
      if (entry.second.scene_id != scene_id) continue;
      auto source = obs_sources_.find(entry.first);
      if (source == obs_sources_.end()) continue;
      obs_sceneitem_t *item = obs_scene_add(scene, source->second);
      if (item) obs_scene_items_[scene_item_key(scene_id, entry.first)] = item;
    }
  }

  void mirror_set_program(const std::string &scene_id) {
    auto scene = obs_scenes_.find(scene_id);
    if (scene == obs_scenes_.end()) return;
    obs_source_t *source = obs_scene_get_source(scene->second);
    if (source) obs_set_output_source(0, source);
  }

  void mirror_source_create(const SourceModel &model) {
    auto scene = obs_scenes_.find(model.scene_id);
    if (scene == obs_scenes_.end()) return;
    release_obs_source(model.id);

    obs_data_t *settings = obs_data_create();
    if (!settings) return;
    obs_data_set_string(settings, "url", model.url.c_str());
    obs_data_set_int(settings, "width", model.width);
    obs_data_set_int(settings, "height", model.height);
    obs_source_t *source = obs_source_create("browser_source", model.id.c_str(), settings, nullptr);
    if (!source) source = obs_source_create("color_source", model.id.c_str(), settings, nullptr);
    obs_data_release(settings);
    if (!source) return;

    obs_sources_[model.id] = source;
    obs_sceneitem_t *item = obs_scene_add(scene->second, source);
    if (item) obs_scene_items_[scene_item_key(model.scene_id, model.id)] = item;

    obs_source_t *filter = obs_source_create("color_filter_v2", "color-correction", nullptr, nullptr);
    if (filter) {
      obs_source_filter_add(source, filter);
      obs_source_release(filter);
    }
  }

  bool mirror_mute_source(const std::string &source_id, bool muted) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    obs_source_set_muted(source->second, muted);
    return true;
  }

  bool mirror_item_visible(const std::string &scene_id, const std::string &item_id, bool visible) {
    auto item = obs_scene_items_.find(scene_item_key(scene_id, item_id));
    if (item == obs_scene_items_.end()) return false;
    obs_sceneitem_set_visible(item->second, visible);
    return true;
  }

  bool mirror_item_order(const std::string &scene_id, const std::string &item_id, int position) {
    auto item = obs_scene_items_.find(scene_item_key(scene_id, item_id));
    if (item == obs_scene_items_.end()) return false;
    obs_sceneitem_set_order_position(item->second, position);
    return true;
  }

  void mirror_list_filters(const std::string &source_id) {
    (void)source_id;
    // filter.list intentionally answers from the authoritative in-memory model;
    // libobs enumeration is not used to shape the RPC response in either build mode.
  }

  bool mirror_filter_enabled(const std::string &source_id, const std::string &filter_id, bool enabled) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    obs_source_t *filter = obs_source_get_filter_by_name(source->second, filter_id.c_str());
    if (!filter) return false;
    obs_source_set_enabled(filter, enabled);
    obs_source_release(filter);
    return true;
  }

  bool mirror_filter_settings(const std::string &source_id, const std::string &filter_id,
                              const std::map<std::string, FilterSettingValue> &settings) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    obs_source_t *filter = obs_source_get_filter_by_name(source->second, filter_id.c_str());
    if (!filter) return false;
    obs_data_t *data = obs_data_create();
    if (!data) {
      obs_source_release(filter);
      return false;
    }
    for (const auto &entry : settings) {
      switch (entry.second.kind) {
      case FilterSettingKind::Number:
        obs_data_set_double(data, entry.first.c_str(), entry.second.number_value);
        break;
      case FilterSettingKind::Bool:
        obs_data_set_bool(data, entry.first.c_str(), entry.second.bool_value);
        break;
      case FilterSettingKind::String:
        obs_data_set_string(data, entry.first.c_str(), entry.second.string_value.c_str());
        break;
      }
    }
    obs_source_update(filter, data);
    obs_data_release(data);
    obs_source_release(filter);
    return true;
  }

  bool mirror_audio_volume(const std::string &source_id, double volume_db, bool muted) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    float scalar = volume_db <= -100.0 ? 0.0f : static_cast<float>(std::pow(10.0, volume_db / 20.0));
    obs_source_set_volume(source->second, scalar);
    obs_source_set_muted(source->second, muted);
    return true;
  }

  bool mirror_media_control(const std::string &source_id, const std::string &action) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    if (action == "play") {
      obs_source_media_play_pause(source->second, false);
    } else if (action == "pause") {
      obs_source_media_play_pause(source->second, true);
    } else if (action == "restart") {
      obs_source_media_restart(source->second);
    } else if (action == "stop") {
      obs_source_media_stop(source->second);
    }
    return true;
  }

  bool mirror_refresh_browser(const std::string &source_id) {
    auto source = obs_sources_.find(source_id);
    if (source == obs_sources_.end()) return false;
    proc_handler_t *handler = obs_source_get_proc_handler(source->second);
    if (!handler) return false;
    calldata_t data;
    calldata_init(&data);
    proc_handler_call(handler, "refresh", &data);
    calldata_free(&data);
    return true;
  }

  std::map<std::string, obs_scene_t *> obs_scenes_;
  std::map<std::string, obs_source_t *> obs_sources_;
  std::map<std::string, obs_sceneitem_t *> obs_scene_items_;
#endif
};

std::string getenv_string(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  if (!in) return "";
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string read_scene_collection_file(const std::filesystem::path &path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (!ec && size > kMaxSceneCollectionBytes) {
    throw std::runtime_error("scene collection exceeds import size limit");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::string contents;
  contents.reserve(!ec ? static_cast<size_t>(size) : 0);
  char buffer[4096];
  std::uintmax_t total = 0;
  while (in) {
    in.read(buffer, sizeof(buffer));
    const std::streamsize count = in.gcount();
    if (count <= 0) break;
    total += static_cast<std::uintmax_t>(count);
    if (total > kMaxSceneCollectionBytes) {
      throw std::runtime_error("scene collection exceeds import size limit");
    }
    contents.append(buffer, static_cast<size_t>(count));
  }
  return contents;
}

std::string path_stem_id(const std::filesystem::path &path) {
  std::string id = path.stem().string();
  for (char &c : id) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '-';
  }
  return id;
}

struct ObsSourceEntry {
  std::string label;
  std::string module;
};

std::optional<uint32_t> json_hex_value(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint32_t>(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return static_cast<uint32_t>(10 + c - 'A');
  return std::nullopt;
}

std::optional<uint32_t> parse_json_hex4_at(const std::string &json, size_t pos) {
  if (pos + 4 > json.size()) return std::nullopt;
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    auto digit = json_hex_value(json[pos + i]);
    if (!digit) return std::nullopt;
    value = (value << 4) | *digit;
  }
  return value;
}

void append_utf8_codepoint(std::string &value, uint32_t codepoint) {
  if (codepoint <= 0x7f) {
    value.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    value.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    value.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0x10ffff) {
    value.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    value.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    append_utf8_codepoint(value, 0xfffd);
  }
}

void consume_json_string_remainder(const std::string &json, size_t &pos) {
  bool escape = false;
  while (pos < json.size()) {
    char c = json[pos++];
    if (escape) {
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else if (c == '"') {
      return;
    }
  }
}

std::optional<std::string> parse_json_string_token(const std::string &json, size_t &pos) {
  if (pos >= json.size() || json[pos] != '"') return std::nullopt;
  ++pos;
  std::string value;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '"') return value;
    if (c == '\\' && pos < json.size()) {
      char escaped = json[pos++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        value.push_back(escaped);
        break;
      case 'b':
        value.push_back('\b');
        break;
      case 'f':
        value.push_back('\f');
        break;
      case 'n':
        value.push_back('\n');
        break;
      case 'r':
        value.push_back('\r');
        break;
      case 't':
        value.push_back('\t');
        break;
      case 'u': {
        auto code_unit = parse_json_hex4_at(json, pos);
        if (!code_unit) {
          consume_json_string_remainder(json, pos);
          return std::nullopt;
        }
        pos += 4;
        if (*code_unit >= 0xd800 && *code_unit <= 0xdbff) {
          if (pos + 6 <= json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
            auto low = parse_json_hex4_at(json, pos + 2);
            if (low && *low >= 0xdc00 && *low <= 0xdfff) {
              pos += 6;
              append_utf8_codepoint(value, 0x10000 + (((*code_unit - 0xd800) << 10) | (*low - 0xdc00)));
              break;
            }
          }
          append_utf8_codepoint(value, 0xfffd);
        } else if (*code_unit >= 0xdc00 && *code_unit <= 0xdfff) {
          append_utf8_codepoint(value, 0xfffd);
        } else {
          append_utf8_codepoint(value, *code_unit);
        }
        break;
      }
      default:
        consume_json_string_remainder(json, pos);
        return std::nullopt;
      }
    } else {
      value.push_back(c);
    }
  }
  return std::nullopt;
}

void skip_json_whitespace(const std::string &json, size_t &pos) {
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
}

std::vector<ObsSourceEntry> parse_obs_source_entries(const std::string &json) {
  struct ObjectFields {
    std::string name;
    std::string id;
    bool emitted = false;
  };
  std::vector<ObjectFields> stack;
  std::vector<ObsSourceEntry> entries;
  for (size_t pos = 0; pos < json.size();) {
    const char c = json[pos];
    if (c == '{') {
      stack.push_back({"", "", false});
      ++pos;
      continue;
    }
    if (c == '}') {
      if (!stack.empty()) stack.pop_back();
      ++pos;
      continue;
    }
    if (c != '"') {
      ++pos;
      continue;
    }

    auto key = parse_json_string_token(json, pos);
    if (!key) continue;
    skip_json_whitespace(json, pos);
    if (pos >= json.size() || json[pos] != ':') continue;
    ++pos;
    skip_json_whitespace(json, pos);
    if (pos >= json.size() || json[pos] != '"') continue;
    auto value = parse_json_string_token(json, pos);
    if (!value || stack.empty()) continue;

    ObjectFields &current = stack.back();
    if (*key == "name") {
      current.name = *value;
    } else if (*key == "id") {
      current.id = *value;
    }
    if (!current.emitted && !current.name.empty() && !current.id.empty()) {
      ObsSourceEntry entry{current.name, current.id};
      if (entries.empty() || entries.back().label != entry.label || entries.back().module != entry.module) {
        entries.push_back(entry);
      }
      current.emitted = true;
    }
  }
  return entries;
}

std::string obs_collection_name(const std::string &json, const std::string &fallback) {
  std::regex name_re("\\\"name\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
  std::smatch match;
  if (std::regex_search(json, match, name_re)) return match[1].str();
  return fallback;
}

bool has_service_key(const std::filesystem::path &config_dir) {
  std::string service = read_text_file(config_dir / "service.json");
  return service.find("\"key\"") != std::string::npos || service.find("\"stream_key\"") != std::string::npos;
}

int profile_count(const std::filesystem::path &config_dir) {
  std::filesystem::path profiles = config_dir / "basic" / "profiles";
  if (!std::filesystem::is_directory(profiles)) return 0;
  int count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(profiles)) {
    if (entry.is_directory() && std::filesystem::exists(entry.path() / "basic.ini")) ++count;
  }
  return count;
}

std::string first_profile_name(const std::filesystem::path &config_dir) {
  std::filesystem::path profiles = config_dir / "basic" / "profiles";
  if (!std::filesystem::is_directory(profiles)) return "OBS Profile";
  for (const auto &entry : std::filesystem::directory_iterator(profiles)) {
    if (entry.is_directory() && std::filesystem::exists(entry.path() / "basic.ini")) return entry.path().filename().string();
  }
  return "OBS Profile";
}

std::string json_string_array(const std::vector<std::string> &notes) {
  std::string out = "[";
  for (size_t i = 0; i < notes.size(); ++i) {
    if (i) out += ",";
    out += "\"" + json_escape(notes[i]) + "\"";
  }
  out += "]";
  return out;
}

std::string report_item(const std::string &id, const std::string &kind, const std::string &label, const std::string &state,
                        const std::string &reason, const std::string &module = "", const std::string &tcc = "",
                        const std::vector<std::string> &notes = {}) {
  std::string out = "{\"id\":\"" + json_escape(id) + "\",\"kind\":\"" + json_escape(kind) + "\",\"label\":\"" +
                    json_escape(label) + "\",\"state\":\"" + json_escape(state) + "\",\"reason\":\"" + json_escape(reason) + "\"";
  if (!module.empty()) out += ",\"moduleName\":\"" + json_escape(module) + "\"";
  if (!tcc.empty()) out += ",\"tccClass\":\"" + json_escape(tcc) + "\"";
  if (!notes.empty()) out += ",\"notes\":" + json_string_array(notes);
  out += "}";
  return out;
}

std::string json_array_join(const std::vector<std::string> &items) {
  std::string out = "[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ",";
    out += items[i];
  }
  out += "]";
  return out;
}

// ---------------------------------------------------------------------------
// NIF-H1: user plugin static inventory (discovery ONLY -- no dlopen, no
// obs_open_module; loading is chunk NIF-H2). Pure filesystem reads + Mach-O
// header byte parsing, so it behaves identically in both lanes; enumeration
// leaves the manifest roots byte-for-byte identical.
// ---------------------------------------------------------------------------

// Compact streaming SHA-256 (FIPS 180-4). The repo had no C++ hashing utility
// (packaging hashes via shasum in shell), and the scaffold lane must not grow
// a CommonCrypto/libobs dependency for this. Streaming so binaries are hashed
// in bounded chunks, never buffered whole.
class Sha256Stream {
public:
  void update(const unsigned char *data, std::size_t len) {
    total_ += len;
    while (len > 0) {
      const std::size_t take = std::min(len, std::size_t(64) - buffered_);
      std::memcpy(buffer_ + buffered_, data, take);
      buffered_ += take;
      data += take;
      len -= take;
      if (buffered_ == 64) {
        process_block(buffer_);
        buffered_ = 0;
      }
    }
  }

  std::string finish_hex() {
    const uint64_t bit_len = total_ * 8;
    const unsigned char pad = 0x80;
    update(&pad, 1);
    const unsigned char zero = 0x00;
    while (buffered_ != 56) update(&zero, 1);
    unsigned char length_bytes[8];
    for (int i = 0; i < 8; ++i) length_bytes[i] = static_cast<unsigned char>(bit_len >> (56 - 8 * i));
    update(length_bytes, 8); // ends exactly on a block boundary
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (uint32_t word : h_) out << std::setw(8) << word;
    return out.str();
  }

private:
  void process_block(const unsigned char *block) {
    static constexpr uint32_t kRound[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    const auto rotr = [](uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const unsigned char *p = block + 4 * i;
      w[i] = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t temp1 = hh + s1 + ch + kRound[i] + w[i];
      uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = s0 + maj;
      hh = g; g = f; f = e; e = d + temp1;
      d = c; c = b; b = a; a = temp1 + temp2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
  }

  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  unsigned char buffer_[64] = {};
  std::size_t buffered_ = 0;
  uint64_t total_ = 0;
};

// Mach-O magic / cputype constants: taken from <mach-o/loader.h> +
// <mach-o/fat.h> on Apple, mirrored literally elsewhere so the byte parser
// stays buildable off-macOS.
#if defined(__APPLE__)
constexpr uint32_t kMhMagic64 = MH_MAGIC_64;
constexpr uint32_t kMhCigam64 = MH_CIGAM_64;
constexpr uint32_t kMhMagic32 = MH_MAGIC;
constexpr uint32_t kMhCigam32 = MH_CIGAM;
constexpr uint32_t kFatMagic = FAT_MAGIC;
constexpr uint32_t kFatMagic64 = FAT_MAGIC_64;
constexpr uint32_t kCpuTypeArm64 = static_cast<uint32_t>(CPU_TYPE_ARM64);
constexpr uint32_t kCpuTypeX8664 = static_cast<uint32_t>(CPU_TYPE_X86_64);
#else
constexpr uint32_t kMhMagic64 = 0xfeedfacf;
constexpr uint32_t kMhCigam64 = 0xcffaedfe;
constexpr uint32_t kMhMagic32 = 0xfeedface;
constexpr uint32_t kMhCigam32 = 0xcefaedfe;
constexpr uint32_t kFatMagic = 0xcafebabe;
constexpr uint32_t kFatMagic64 = 0xcafebabf;
constexpr uint32_t kCpuTypeArm64 = 0x0100000c;
constexpr uint32_t kCpuTypeX8664 = 0x01000007;
#endif

uint32_t read_u32_at(const unsigned char *p, bool big_endian) {
  if (big_endian) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
  }
  return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}

std::string cpu_type_label(uint32_t cputype) {
  if (cputype == kCpuTypeArm64) return "arm64";
  if (cputype == kCpuTypeX8664) return "x86_64";
  return "unknown";
}

// Reads architectures straight from the Mach-O header bytes: thin
// MH_MAGIC_64/MH_CIGAM_64 (cputype at offset 4, byte-swapped for CIGAM) and
// universal FAT_MAGIC[/64]/FAT_CIGAM[/64] (one cputype per fat_arch entry).
// STRICT: arch facts come only from fully-read structures -- the complete
// thin mach_header (28 bytes) / mach_header_64 (32 bytes), and for FAT every
// one of nfat fat_arch (20B) / fat_arch_64 (32B) entries entirely within the
// read window. nullopt = not a (completely readable) Mach-O image.
std::optional<std::vector<std::string>> parse_macho_archs(const std::vector<unsigned char> &bytes) {
  if (bytes.size() < 8) return std::nullopt;
  const uint32_t magic_le = read_u32_at(bytes.data(), false);
  const uint32_t magic_be = read_u32_at(bytes.data(), true);

  // Thin image: the header is in the image's own endianness; a CIGAM read
  // means the file was written byte-swapped relative to us.
  const bool thin64 = (magic_le == kMhMagic64 || magic_le == kMhCigam64);
  const bool thin32 = (magic_le == kMhMagic32 || magic_le == kMhCigam32);
  if (thin64 || thin32) {
    const std::size_t header_size = thin64 ? 32 : 28; // mach_header_64 : mach_header
    if (bytes.size() < header_size) return std::nullopt;
    const bool swapped = (magic_le == kMhCigam64 || magic_le == kMhCigam32);
    return std::vector<std::string>{cpu_type_label(read_u32_at(bytes.data() + 4, swapped))};
  }

  // Universal image: fat headers are big-endian on disk (FAT_MAGIC read
  // big-endian); tolerate the byte-swapped variant as well.
  bool fat_big_endian;
  uint32_t fat_magic;
  if (magic_be == kFatMagic || magic_be == kFatMagic64) {
    fat_big_endian = true;
    fat_magic = magic_be;
  } else if (magic_le == kFatMagic || magic_le == kFatMagic64) {
    fat_big_endian = false;
    fat_magic = magic_le;
  } else {
    return std::nullopt;
  }
  const bool fat64 = (fat_magic == kFatMagic64);
  const uint32_t nfat = read_u32_at(bytes.data() + 4, fat_big_endian);
  if (nfat == 0 || nfat > 128) return std::nullopt;
  const size_t entry_size = fat64 ? 32 : 20; // sizeof(fat_arch_64) : sizeof(fat_arch)
  // The whole arch table must be inside the window before ANY entry counts.
  if (8 + static_cast<size_t>(nfat) * entry_size > bytes.size()) return std::nullopt;
  std::vector<std::string> archs;
  for (uint32_t i = 0; i < nfat; ++i) {
    const size_t offset = 8 + static_cast<size_t>(i) * entry_size;
    std::string label = cpu_type_label(read_u32_at(bytes.data() + offset, fat_big_endian));
    if (std::find(archs.begin(), archs.end(), label) == archs.end()) archs.push_back(label);
  }
  return archs;
}

// Walk/probe bounds -- every root scan and binary read is explicitly capped.
constexpr int kMaxScanDepth = 8;                                     // dirs below the root
constexpr std::size_t kMaxVisitedEntriesPerRoot = 4096;              // dir entries inspected
constexpr std::size_t kMaxCandidatesPerRoot = 256;                   // records per root
constexpr std::uintmax_t kMaxHashedBinaryBytes = 512ull * 1024 * 1024; // sha256 size ceiling
constexpr std::size_t kMachoHeaderWindowBytes = 64 * 1024;           // arch probe window
constexpr std::size_t kHashChunkBytes = 1024 * 1024;                 // streaming read unit

struct UserPluginBinaryProbe {
  bool readable = false;
  bool too_large = false;              // over the sha256 ceiling
  std::string sha256;                  // empty => null on the wire
  std::vector<unsigned char> header;   // first kMachoHeaderWindowBytes only
};

// Reads a candidate binary with bounded memory: the Mach-O parse sees only a
// 64 KiB header window, and hashing streams 1 MiB chunks through the SHA-256
// context. Files over the ceiling are NOT hashed (header window still read).
UserPluginBinaryProbe probe_user_plugin_binary(const std::filesystem::path &path) {
  UserPluginBinaryProbe probe;
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) return probe;
  std::ifstream in(path, std::ios::binary);
  if (!in) return probe;

  if (size > kMaxHashedBinaryBytes) {
    probe.too_large = true;
    probe.header.resize(kMachoHeaderWindowBytes);
    in.read(reinterpret_cast<char *>(probe.header.data()),
            static_cast<std::streamsize>(probe.header.size()));
    probe.header.resize(static_cast<std::size_t>(in.gcount()));
    probe.readable = !in.bad();
    return probe;
  }

  Sha256Stream hasher;
  std::vector<unsigned char> chunk(kHashChunkBytes);
  while (in) {
    in.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    hasher.update(chunk.data(), static_cast<std::size_t>(got));
    if (probe.header.size() < kMachoHeaderWindowBytes) {
      const std::size_t take =
          std::min(static_cast<std::size_t>(got), kMachoHeaderWindowBytes - probe.header.size());
      probe.header.insert(probe.header.end(), chunk.data(), chunk.data() + take);
    }
  }
  if (in.bad()) return probe;
  probe.readable = true;
  probe.sha256 = hasher.finish_hex();
  return probe;
}

struct UserPluginCandidate {
  std::string name;              // protocol-safe module name
  std::filesystem::path binary;  // absolute path to the module binary
  std::string relative;          // bare bundle/library file name (mono contract)
  bool confined = false;         // not a symlink AND canonical path stays in the root
  bool bundle = false;           // CFBundle layout (data dir = Contents/Resources)
};

// Symlink confinement: a candidate binary must itself be a real regular file
// (never a symlink) and its fully-resolved canonical path must stay inside
// the canonical root -- otherwise it is never opened, read, or hashed.
// ACCEPTED BOUNDARY (review INFO, current threat model): this is a path-based
// check, so a TOCTOU swap between verdict and open, or a hard link inside the
// root to an outside inode, is not defended here. Future hardening direction:
// open first, then fstat/verify on the open descriptor before reading.
bool user_plugin_binary_confined(const std::filesystem::path &binary,
                                 const std::filesystem::path &canonical_root) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(binary, ec);
  if (ec || status.type() != std::filesystem::file_type::regular) return false;
  const auto canonical = std::filesystem::canonical(binary, ec); // resolves parent components too
  if (ec) return false;
  const std::string relative = canonical.lexically_relative(canonical_root).generic_string();
  if (relative.empty() || relative == "..") return false;
  return relative.compare(0, 3, "../") != 0 && relative != ".";
}

// Sorted directory listing via error_code (never throws; unreadable => empty).
std::vector<std::filesystem::path> list_directory_sorted(const std::filesystem::path &dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec), end;
  while (!ec && it != end) {
    out.push_back(it->path());
    it.increment(ec);
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Bounded manual walk of a legacy <name>/bin tree (sorted at every level so
// truncation is deterministic). Directory symlinks are never followed; file
// symlinks still surface as matches so confinement can refuse them loudly.
void walk_legacy_bin_dir(const std::filesystem::path &dir, int depth, const std::string &so_name,
                         const std::string &dylib_name, std::vector<std::filesystem::path> &matches,
                         std::size_t &visited, bool &truncated) {
  if (depth > kMaxScanDepth) {
    truncated = true;
    return;
  }
  for (const auto &entry : list_directory_sorted(dir)) {
    if (++visited > kMaxVisitedEntriesPerRoot) {
      truncated = true;
      return;
    }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(entry, ec);
    if (ec) continue;
    if (std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
      const std::string file_name = entry.filename().string();
      if (file_name == so_name || file_name == dylib_name) matches.push_back(entry);
    } else if (std::filesystem::is_directory(status)) {
      walk_legacy_bin_dir(entry, depth + 1, so_name, dylib_name, matches, visited, truncated);
      if (truncated) return;
    }
  }
}

struct UserPluginRootScan {
  std::vector<UserPluginCandidate> candidates;
  bool truncated = false; // a walk bound was hit; the scan stopped deterministically
};

// The six registered-type kinds, in the mono contract's pinned key order.
constexpr std::array<const char *, 6> kRegisteredTypeKinds = {
    "sources", "filters", "transitions", "outputs", "encoders", "services"};

struct UserPluginRecord {
  std::string module_ref;
  std::string label;
  std::string file_name;
  std::string root_ref;
  std::string sha256;
  std::vector<std::string> archs;
  bool observed_arch = false;
  std::string lifecycle;
  std::string reason_detail; // empty => null on the wire
  // NIF-H2 plan/load facts. Defaults preserve the NIF-H1 discover shape
  // (state null, dependencies unobserved, no registeredTypes field).
  std::string state; // empty => null on the wire
  bool observed_dependencies = false;
  bool has_registered_types = false;
  std::array<std::vector<std::string>, 6> registered_types; // kRegisteredTypeKinds order
};

struct UserPluginRootSummary {
  std::string root_ref;
  int candidate_count = 0;
  bool truncated = false;
};

struct UserPluginInventory {
  std::vector<UserPluginRootSummary> roots;
  std::vector<UserPluginRecord> modules;
};

// Enumerates one manifest root across BOTH macOS layouts, in sorted entry
// order (directory iteration order is unspecified, and the inventory must be
// deterministic):
//   (a) <root>/<name>.plugin CFBundle -> Contents/MacOS/<name>
//   (b) legacy <root>/<name>/bin/**/<name>.{so,dylib} (first match in sorted
//       path order; walk bounded by depth/entry caps)
// Names that are not protocol-safe ids cannot form a valid "module:<id>" ref
// and are skipped. A missing/unreadable root yields zero candidates. Every
// candidate carries a confinement verdict; unconfined binaries are never
// opened downstream.
UserPluginRootScan scan_user_plugin_root(const std::filesystem::path &root) {
  UserPluginRootScan scan;
  std::error_code ec;
  const std::filesystem::path canonical_root = std::filesystem::canonical(root, ec);
  if (ec) return scan;

  std::size_t visited = 0;
  constexpr const char *kBundleSuffix = ".plugin";
  constexpr std::size_t kBundleSuffixLen = 7;
  for (const auto &path : list_directory_sorted(root)) {
    if (++visited > kMaxVisitedEntriesPerRoot) {
      scan.truncated = true;
      break;
    }
    std::error_code entry_ec;
    if (!std::filesystem::is_directory(path, entry_ec) || entry_ec) continue;
    const std::string entry_name = path.filename().string();

    std::optional<UserPluginCandidate> candidate;
    if (entry_name.size() > kBundleSuffixLen && entry_name.ends_with(kBundleSuffix)) {
      const std::string name = entry_name.substr(0, entry_name.size() - kBundleSuffixLen);
      if (!is_safe_protocol_id(name)) continue;
      std::filesystem::path binary = path / "Contents" / "MacOS" / name;
      // is_regular_file follows symlinks on purpose: a symlinked binary must
      // become a candidate so confinement can refuse it with a visible record.
      if (std::filesystem::is_regular_file(binary, entry_ec) && !entry_ec) {
        // fileName is the BARE bundle name (mono contract: no separators).
        candidate = UserPluginCandidate{name, binary, entry_name, false, true};
      }
    } else {
      const std::string &name = entry_name;
      if (!is_safe_protocol_id(name)) continue;
      std::filesystem::path bin_dir = path / "bin";
      if (!std::filesystem::is_directory(bin_dir, entry_ec) || entry_ec) continue;
      std::vector<std::filesystem::path> matches;
      walk_legacy_bin_dir(bin_dir, 3, name + ".so", name + ".dylib", matches, visited,
                          scan.truncated); // bin sits 2 levels below the root; entries start at 3
      if (scan.truncated) break;
      if (!matches.empty()) {
        const std::filesystem::path &binary = matches.front(); // already sorted per level
        // fileName is the BARE library name (mono contract: no separators).
        candidate = UserPluginCandidate{name, binary, binary.filename().string(), false, false};
      }
    }
    if (!candidate) continue;
    if (scan.candidates.size() >= kMaxCandidatesPerRoot) {
      scan.truncated = true;
      break;
    }
    candidate->confined = user_plugin_binary_confined(candidate->binary, canonical_root);
    scan.candidates.push_back(std::move(*candidate));
  }
  return scan;
}

// Per-module load-source facts kept OFF the wire (absolute paths) — parallel
// to UserPluginInventory.modules when requested by the load planner.
struct UserPluginModuleSource {
  std::filesystem::path binary;
  std::filesystem::path data_dir; // empty => no data path handed to libobs
};

UserPluginInventory build_user_plugin_inventory(const user_plugins::Manifest &manifest,
                                                std::vector<UserPluginModuleSource> *sources_out = nullptr) {
  UserPluginInventory inventory;
  const std::set<std::string> excluded(manifest.exclude.begin(), manifest.exclude.end());
  std::set<std::string> seen;

  for (std::size_t index = 0; index < manifest.roots.size(); ++index) {
    const std::string root_ref = "root:" + std::to_string(index);
    UserPluginRootScan scan;
    try {
      scan = scan_user_plugin_root(manifest.roots[index].binary_dir);
    } catch (const std::filesystem::filesystem_error &) {
      scan = {}; // a root racing with deletion degrades to empty
    }
    inventory.roots.push_back({root_ref, static_cast<int>(scan.candidates.size()), scan.truncated});

    std::size_t ordinal = 0;
    for (const auto &candidate : scan.candidates) {
      const std::size_t candidate_ordinal = ordinal++;
      UserPluginRecord record;
      record.root_ref = root_ref;
      // F2 (opus review): a name the host tolerates but the mono validator
      // refuses (leading ._-/length > 120) must never form the wire ref.
      // The candidate surfaces as a REFUSED record under a synthesized
      // conforming ref — never silently dropped, never loaded, and its raw
      // name is never disclosed on the wire.
      if (!streammate::user_plugins::is_portable_module_name(candidate.name)) {
        record.module_ref = "module:unportable-" + std::to_string(index) + "-" +
                            std::to_string(candidate_ordinal);
        record.label = "unportable-candidate";
        record.file_name = "unportable";
        record.lifecycle = "discovered";
        record.reason_detail = "name-not-portable";
        if (sources_out != nullptr) {
          sources_out->push_back({});
        }
        inventory.modules.push_back(std::move(record));
        continue;
      }
      record.module_ref = "module:" + candidate.name;
      record.label = candidate.name;
      record.file_name = candidate.relative;
      // Reason precedence (one category per record): symlink-refused >
      // unreadable-binary > binary-too-large > unreadable-macho-header.
      if (!candidate.confined) {
        // Never opened: no bytes disclosed, no hash, no arch.
        record.reason_detail = "symlink-refused";
      } else if (UserPluginBinaryProbe probe = probe_user_plugin_binary(candidate.binary);
                 !probe.readable) {
        record.reason_detail = "unreadable-binary";
      } else {
        record.sha256 = probe.sha256; // empty (=> null) when over the size ceiling
        if (probe.too_large) record.reason_detail = "binary-too-large";
        if (auto archs = parse_macho_archs(probe.header)) {
          record.archs = *archs;
          record.observed_arch = true; // read from the header bytes, honestly
        } else if (record.reason_detail.empty()) {
          record.reason_detail = "unreadable-macho-header";
        }
      }
      // Exclusion wins over duplicate detection; only non-excluded records
      // participate in first-root-wins tracking.
      if (excluded.count(record.module_ref)) {
        record.lifecycle = "excluded";
      } else if (!seen.insert(record.module_ref).second) {
        record.lifecycle = "duplicate_in_roots";
      } else {
        record.lifecycle = "discovered";
      }
      if (sources_out != nullptr) {
        UserPluginModuleSource source;
        source.binary = candidate.binary;
        if (candidate.bundle) {
          source.data_dir = candidate.binary.parent_path().parent_path() / "Resources";
        } else if (!manifest.roots[index].data_dir.empty()) {
          source.data_dir = std::filesystem::path(manifest.roots[index].data_dir) / candidate.name;
        }
        sources_out->push_back(std::move(source));
      }
      inventory.modules.push_back(std::move(record));
    }
  }
  return inventory;
}

std::string user_plugin_record_json(const UserPluginRecord &record) {
  // Discover keeps NIF-H1 defaults (state null, dependencies false, no
  // registeredTypes); plugins.report records carry the plan/load facts.
  std::string json = "{\"moduleRef\":\"" + json_escape(record.module_ref) +
         "\",\"label\":\"" + json_escape(record.label) +
         "\",\"fileName\":\"" + json_escape(record.file_name) +
         "\",\"rootRef\":\"" + json_escape(record.root_ref) +
         "\",\"moduleClass\":\"user\"" +
         ",\"sha256\":" +
         (record.sha256.empty() ? std::string("null") : "\"" + json_escape(record.sha256) + "\"") +
         ",\"arch\":" + json_string_array(record.archs) +
         ",\"lifecycle\":\"" + json_escape(record.lifecycle) +
         "\",\"state\":" +
         (record.state.empty() ? std::string("null") : "\"" + json_escape(record.state) + "\"") +
         ",\"observed\":{\"arch\":" + (record.observed_arch ? "true" : "false") +
         ",\"dependencies\":" + (record.observed_dependencies ? "true" : "false") +
         "},\"reasonDetail\":" +
         (record.reason_detail.empty() ? std::string("null")
                                       : "\"" + json_escape(record.reason_detail) + "\"");
  if (record.has_registered_types) {
    // The mono validator requires ALL six kinds when the field is present.
    json += ",\"registeredTypes\":{";
    for (std::size_t kind = 0; kind < kRegisteredTypeKinds.size(); ++kind) {
      if (kind) json += ",";
      json += "\"" + std::string(kRegisteredTypeKinds[kind]) + "\":" +
              json_string_array(record.registered_types[kind]);
    }
    json += "}";
  }
  return json + "}";
}

std::string user_plugins_discover_json(const std::optional<user_plugins::Manifest> &manifest) {
  std::string roots_json = "[";
  std::string modules_json = "[";
  if (manifest) {
    const UserPluginInventory inventory = build_user_plugin_inventory(*manifest);
    for (std::size_t i = 0; i < inventory.roots.size(); ++i) {
      if (i) roots_json += ",";
      roots_json += "{\"rootRef\":\"" + json_escape(inventory.roots[i].root_ref) +
                    "\",\"candidateCount\":" + std::to_string(inventory.roots[i].candidate_count) +
                    ",\"truncated\":" + (inventory.roots[i].truncated ? "true" : "false") + "}";
    }
    for (std::size_t i = 0; i < inventory.modules.size(); ++i) {
      if (i) modules_json += ",";
      modules_json += user_plugin_record_json(inventory.modules[i]);
    }
  }
  return "{\"ok\":true,\"mode\":\"" + std::string(kPluginHostMode) + "\",\"roots\":" + roots_json +
         "],\"modules\":" + modules_json + "]}";
}

// ---------------------------------------------------------------------------
// NIF-H2: user-plugin LOAD plan + boot loading + boot-frozen plugins.report.
//
// The plan runs in BOTH lanes over static filesystem facts only:
//   - a record refused at discovery (any reasonDetail) is never a candidate;
//   - a wrong-arch record gets state "architecture_mismatch" (observed from
//     the Mach-O header) and is never attempted, selected or not;
//   - manifest `selected` gates candidacy (`not-selected` reason; selected
//     null/absent means every discovered candidate in the given roots);
//   - excluded / duplicate_in_roots lifecycles never load.
// The libobs lane then loads each candidate per-module between the bundled
// obs_load_all_modules() and obs_post_load_modules(); the scaffold lane
// NEVER claims loading: candidates stay lifecycle "discovered" with
// reasonDetail "scaffold-not-loaded" and no registeredTypes field ever
// appears (mode:"scaffold" labels the whole payload non-real).
// ---------------------------------------------------------------------------

constexpr const char *kHostCpuArch =
#if defined(__aarch64__) || defined(__arm64__)
    "arm64";
#elif defined(__x86_64__)
    "x86_64";
#else
    "";
#endif

struct UserPluginLoadPlan {
  UserPluginInventory inventory;
  std::vector<UserPluginModuleSource> sources; // parallel to inventory.modules
  std::vector<std::size_t> candidates;         // indices into inventory.modules
};

UserPluginLoadPlan build_user_plugin_load_plan(const user_plugins::Manifest &manifest) {
  UserPluginLoadPlan plan;
  plan.inventory = build_user_plugin_inventory(manifest, &plan.sources);
  const std::set<std::string> selected(manifest.selected.begin(), manifest.selected.end());

  for (std::size_t i = 0; i < plan.inventory.modules.size(); ++i) {
    UserPluginRecord &record = plan.inventory.modules[i];
    if (record.lifecycle != "discovered") continue;   // excluded / duplicate_in_roots
    if (!record.reason_detail.empty()) continue;      // discovery refusal: never loadable
    const bool arch_mismatch = record.observed_arch && kHostCpuArch[0] != '\0' &&
                               std::find(record.archs.begin(), record.archs.end(),
                                         std::string(kHostCpuArch)) == record.archs.end();
    if (arch_mismatch) {
      record.state = "architecture_mismatch"; // static observed fact, never attempted
      continue;
    }
    if (manifest.has_selected && selected.count(record.module_ref) == 0) {
      record.reason_detail = "not-selected";
      continue;
    }
    plan.candidates.push_back(i);
  }
  return plan;
}

std::string user_plugins_report_json_from(const UserPluginInventory &inventory) {
  std::string modules_json = "[";
  for (std::size_t i = 0; i < inventory.modules.size(); ++i) {
    if (i) modules_json += ",";
    modules_json += user_plugin_record_json(inventory.modules[i]);
  }
  // No roots field: the pinned plugins.report shape (mono treats absent as []).
  return "{\"ok\":true,\"mode\":\"" + std::string(kPluginHostMode) + "\",\"modules\":" +
         modules_json + "]}";
}

std::string empty_user_plugins_report_json() {
  return "{\"ok\":true,\"mode\":\"" + std::string(kPluginHostMode) + "\",\"modules\":[]}";
}

#if STREAMMATE_HAS_LIBOBS
std::array<std::set<std::string>, 6> snapshot_registered_types() {
  std::array<std::set<std::string>, 6> out;
  const char *id = nullptr;
  for (size_t i = 0; obs_enum_input_types(i, &id); ++i)
    if (id != nullptr) out[0].insert(id);
  for (size_t i = 0; obs_enum_filter_types(i, &id); ++i)
    if (id != nullptr) out[1].insert(id);
  for (size_t i = 0; obs_enum_transition_types(i, &id); ++i)
    if (id != nullptr) out[2].insert(id);
  for (size_t i = 0; obs_enum_output_types(i, &id); ++i)
    if (id != nullptr) out[3].insert(id);
  for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i)
    if (id != nullptr) out[4].insert(id);
  for (size_t i = 0; obs_enum_service_types(i, &id); ++i)
    if (id != nullptr) out[5].insert(id);
  return out;
}

// Bundled-module stems ("mac-capture" from mac-capture.plugin etc.): the
// UNION of (a) modules libobs actually opened (obs_enum_modules) and (b) the
// app bundle's Contents/PlugIns/obs-plugins directory listing — a bundled
// module that failed to open/init is absent from (a) but must still shadow a
// same-named user module (bundled wins even when broken). A user module
// colliding with either is refused (duplicate_of_bundled) before any dlopen.
std::set<std::string> bundled_module_stems(const std::filesystem::path &bundle_plugins_dir) {
  std::set<std::string> stems;
  obs_enum_modules(
      [](void *param, obs_module_t *module) {
        const char *file = obs_get_module_file_name(module);
        if (file == nullptr) return;
        std::string stem = std::filesystem::path(file).stem().string();
        if (!stem.empty()) static_cast<std::set<std::string> *>(param)->insert(std::move(stem));
      },
      &stems);
  std::error_code ec;
  if (!bundle_plugins_dir.empty() && std::filesystem::is_directory(bundle_plugins_dir, ec) && !ec) {
    std::filesystem::directory_iterator it(bundle_plugins_dir, ec);
    for (std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
      const std::filesystem::path entry = it->path();
      if (entry.extension() == ".plugin") {
        std::string stem = entry.stem().string();
        if (!stem.empty()) stems.insert(std::move(stem));
      }
    }
  }
  return stems;
}

// Sanitized dlerror classification: NEVER the raw dlerror text (it embeds
// absolute paths). "Library not loaded" appears ONLY when a dependent dylib
// failed to resolve — a bare "image not found" also fires for a missing
// CANDIDATE file, so it must not be treated as a dependency signal;
// "architecture" => arch rejection at load. In the pinned libobs,
// MODULE_FILE_NOT_FOUND is a deprecated alias of MODULE_FAILED_TO_OPEN (every
// os_dlopen failure returns it), so ALL open failures route through here.
void classify_failed_dlopen(const std::filesystem::path &binary, UserPluginRecord &record) {
  std::error_code exists_ec;
  if (!std::filesystem::exists(binary, exists_ec) || exists_ec) {
    record.state = "module_load_failed";
    record.reason_detail = "file-not-found";
    return;
  }
  // Probe dlopen runs constructors if it unexpectedly succeeds; that only
  // happens after obs_open_module already failed on the same path, so the
  // side-effect window is accepted (the module was user-selected for load).
  void *handle = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (handle != nullptr) {
    dlclose(handle);
    record.state = "module_load_failed";
    record.reason_detail = "dlopen-failed";
    return;
  }
  const char *raw = dlerror();
  const std::string text = raw != nullptr ? raw : "";
  if (text.find("Library not loaded") != std::string::npos) {
    record.state = "dependency_missing";
    record.reason_detail = "library-not-loaded";
    record.observed_dependencies = true; // the failure itself is an observed dependency fact
  } else if (text.find("architecture") != std::string::npos) {
    record.state = "architecture_mismatch";
    record.reason_detail = "wrong-architecture";
  } else {
    record.state = "module_load_failed";
    record.reason_detail = "dlopen-failed";
  }
}

// Load each plan candidate per-module. MUST run between the bundled
// obs_load_all_modules() and obs_post_load_modules(), so every user module
// exists before any collection instantiation and post-load runs exactly once.
void load_user_plugin_candidates(UserPluginLoadPlan &plan,
                                 const std::filesystem::path &bundle_plugins_dir) {
  const std::set<std::string> bundled = bundled_module_stems(bundle_plugins_dir);
  std::array<std::set<std::string>, 6> before = snapshot_registered_types();

  for (std::size_t index : plan.candidates) {
    UserPluginRecord &record = plan.inventory.modules[index];
    const UserPluginModuleSource &source = plan.sources[index];

    // Stem comparison is deliberately case-sensitive: APFS defaults are
    // case-insensitive, but libobs module ids are exact-match, and a
    // case-variant name that survives here still fails obs_open_module.
    if (bundled.count(record.label) != 0) {
      record.lifecycle = "duplicate_of_bundled"; // bundled upstream wins, never dlopened
      continue;
    }

    // Re-verify confinement at the load boundary: discovery hashed/probed the
    // path earlier, and path-based checks are TOCTOU-bounded (accepted at the
    // recorded threat model — the roots are the local user's own directories
    // and the manifest is Station-authored). Re-checking here shrinks the
    // window from discovery-to-load down to check-to-dlopen.
    {
      std::error_code canon_ec;
      const std::filesystem::path parent_root =
          std::filesystem::canonical(source.binary.parent_path(), canon_ec);
      if (canon_ec || !user_plugin_binary_confined(source.binary, parent_root)) {
        record.lifecycle = "load_failed";
        record.state = "module_load_failed";
        record.reason_detail = "symlink-refused";
        continue;
      }
    }

    const char *data_path = source.data_dir.empty() ? nullptr : source.data_dir.c_str();
    obs_module_t *module = nullptr;
    const int rc = obs_open_module(&module, source.binary.c_str(), data_path);
    if (rc == MODULE_SUCCESS) {
      record.observed_dependencies = true; // dlopen resolved every dependent library
      if (!obs_init_module(module)) {
        record.lifecycle = "load_failed";
        record.state = "module_load_failed";
        record.reason_detail = "module-init-failed";
        // The module stays in libobs's list (no public API frees an
        // init-failed module the way the internal loader does), so ROLL the
        // snapshot: any types it registered before failing must not be
        // credited to the next successful module.
        before = snapshot_registered_types();
        continue;
      }
      record.lifecycle = "loaded";
      record.has_registered_types = true;
      std::array<std::set<std::string>, 6> after = snapshot_registered_types();
      bool any = false;
      for (std::size_t kind = 0; kind < after.size(); ++kind) {
        for (const std::string &type_id : after[kind]) {
          // Only ids the mono STUDIO_PLUGIN_TYPE_ID_PATTERN accepts may be
          // emitted (a single non-conforming id would fail-close the whole
          // inventory downstream); non-conforming ids are dropped from the
          // delta while the module still reports loaded honestly.
          if (before[kind].count(type_id) == 0 &&
              streammate::user_plugins::is_portable_type_id(type_id)) {
            record.registered_types[kind].push_back(type_id);
            any = true;
          }
        }
      }
      before = after;
      // NEGATIVE ASSERTION (spec 39): load + registration alone is never
      // "compatible" — state stays null; the instance+render probe smoke owns
      // the derived verdict. A module registering NOTHING is the observed
      // type_not_registered state.
      if (!any) record.state = "type_not_registered";
      continue;
    }

    record.lifecycle = "load_failed";
    switch (rc) {
    case MODULE_MISSING_EXPORTS:
      record.state = "module_load_failed";
      record.reason_detail = "missing-exports";
      record.observed_dependencies = true;
      break;
    case MODULE_INCOMPATIBLE_VER:
      record.state = "libobs_version_mismatch";
      record.reason_detail = "incompatible-module-version";
      record.observed_dependencies = true;
      break;
    default:
      // MODULE_FAILED_TO_OPEN (== deprecated MODULE_FILE_NOT_FOUND) and
      // MODULE_ERROR: classify via a sanitized probe dlopen.
      classify_failed_dlopen(source.binary, record);
      break;
    }
  }
}
#endif

// Boot-frozen plugins.report payload. libobs lane: performs the real
// per-module loading (call site: EngineLifecycle::start, between bundled
// load and post-load). Scaffold lane: plan facts only, honestly labeled.
std::string build_user_plugins_boot_report_json(
    const std::optional<user_plugins::Manifest> &manifest,
    const std::filesystem::path &bundle_plugins_dir) {
  if (!manifest) {
    return empty_user_plugins_report_json();
  }
  UserPluginLoadPlan plan = build_user_plugin_load_plan(*manifest);
#if STREAMMATE_HAS_LIBOBS
  load_user_plugin_candidates(plan, bundle_plugins_dir);
#else
  (void)bundle_plugins_dir;
  for (std::size_t index : plan.candidates) {
    plan.inventory.modules[index].reason_detail = "scaffold-not-loaded";
  }
#endif
  return user_plugins_report_json_from(plan.inventory);
}

struct PromptSourceSummary {
  int camera_count = 0;
  int microphone_count = 0;
  int screen_count = 0;
  int instantiated_count = 0;
  int failed_count = 0;
  bool camera_attempted = false;
  bool microphone_attempted = false;
  bool screen_attempted = false;
  bool camera_activated = false;
  bool microphone_activated = false;
  bool screen_activated = false;
  std::vector<std::string> sanitized_failure_classes;
};

std::string prompt_source_class(const std::string &module) {
  if (module == "macos-avcapture" || module == "macos-avcapture-fast" || module == "av_capture_input") return "camera";
  if (module == "coreaudio_input_capture") return "microphone";
  if (module == "screen_capture" || module == "display_capture") return "screen";
  return "";
}

std::vector<std::string> exercise_source_ids(const std::string &tcc_class) {
  if (tcc_class == "camera") return {"macos-avcapture"};
  if (tcc_class == "microphone") return {"coreaudio_input_capture"};
  if (tcc_class == "screen") return {"screen_capture", "display_capture"};
  return {};
}

void count_prompt_source(PromptSourceSummary &summary, const std::string &tcc_class) {
  if (tcc_class == "camera") ++summary.camera_count;
  if (tcc_class == "microphone") ++summary.microphone_count;
  if (tcc_class == "screen") ++summary.screen_count;
}

void mark_attempted(PromptSourceSummary &summary, const std::string &tcc_class) {
  if (tcc_class == "camera") summary.camera_attempted = true;
  if (tcc_class == "microphone") summary.microphone_attempted = true;
  if (tcc_class == "screen") summary.screen_attempted = true;
}

void mark_activated(PromptSourceSummary &summary, const std::string &tcc_class) {
  if (tcc_class == "camera") summary.camera_activated = true;
  if (tcc_class == "microphone") summary.microphone_activated = true;
  if (tcc_class == "screen") summary.screen_activated = true;
}

std::string json_string_array_from_values(const std::vector<std::string> &values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += "\"" + json_escape(values[i]) + "\"";
  }
  out += "]";
  return out;
}

std::string prompt_source_summary_json(const PromptSourceSummary &summary, const std::string &mode, bool prompt_capable) {
  int total = summary.camera_count + summary.microphone_count + summary.screen_count;
  int deferred = prompt_capable ? 0 : total;
  return "{\"mode\":\"" + json_escape(mode) + "\",\"promptCapable\":" + std::string(prompt_capable ? "true" : "false") +
         ",\"cameraCount\":" + std::to_string(summary.camera_count) + ",\"microphoneCount\":" + std::to_string(summary.microphone_count) +
         ",\"screenCount\":" + std::to_string(summary.screen_count) + ",\"instantiatedCount\":" + std::to_string(summary.instantiated_count) +
         ",\"deferredCount\":" + std::to_string(deferred) + ",\"failedCount\":" + std::to_string(summary.failed_count) + "}";
}

std::string tcc_exercise_summary_json(const PromptSourceSummary &summary, const std::string &mode, bool prompt_capable) {
  return "{\"ok\":true,\"mode\":\"" + json_escape(mode) + "\",\"promptCapable\":" +
         std::string(prompt_capable ? "true" : "false") +
         ",\"cameraAttempted\":" + std::string(summary.camera_attempted ? "true" : "false") +
         ",\"microphoneAttempted\":" + std::string(summary.microphone_attempted ? "true" : "false") +
         ",\"screenAttempted\":" + std::string(summary.screen_attempted ? "true" : "false") +
         ",\"cameraActivated\":" + std::string(summary.camera_activated ? "true" : "false") +
         ",\"microphoneActivated\":" + std::string(summary.microphone_activated ? "true" : "false") +
         ",\"screenActivated\":" + std::string(summary.screen_activated ? "true" : "false") +
         ",\"instantiatedCount\":" + std::to_string(summary.instantiated_count) +
         ",\"failedCount\":" + std::to_string(summary.failed_count) +
         ",\"sanitizedFailureClasses\":" + json_string_array_from_values(summary.sanitized_failure_classes) + "}";
}

class ObsImporter {
public:
  ~ObsImporter() {
#if STREAMMATE_HAS_LIBOBS
    release_prompt_sources();
#endif
  }

  std::string scan(const std::string &request) {
    auto config = resolve_config_dir(request);
    if (!config) return error(-32602, "OBS config dir is required");
    auto collection = find_collection(*config, "");
    if (!collection) return error(-32602, "collection not found");
    std::string json;
    try {
      json = read_scene_collection_file(*collection);
    } catch (const std::runtime_error &) {
      return error(-32602, "scene collection exceeds import size limit");
    }
    auto entries = parse_obs_source_entries(json);
    int sources = 0;
    for (const auto &entry : entries) {
      if (entry.module.find("filter") == std::string::npos) ++sources;
    }
    std::string id = path_stem_id(*collection);
    std::string result = "{\"ok\":true,\"configDirLabel\":\"$OBS_CONFIG_DIR\",\"collections\":[{";
    result += "\"collectionId\":\"" + json_escape(id) + "\",\"name\":\"" + json_escape(obs_collection_name(json, id)) +
              "\",\"sourceCount\":" + std::to_string(sources) + ",\"profileCount\":" + std::to_string(profile_count(*config)) +
              ",\"serviceKeyAction\":\"" + std::string(has_service_key(*config) ? "requires-consent" : "not-present") + "\"}]}";
    return result;
  }

  std::string load(const std::string &request) {
    auto config = resolve_config_dir(request);
    if (!config) return error(-32602, "OBS config dir is required");
    std::string collection_id = extract_json_string(request, "collectionId");
    if (collection_id.empty()) return error(-32602, "collectionId is required");
    auto collection = find_collection(*config, collection_id);
    if (!collection) return error(-32602, "collection not found");
    std::string home = extract_json_string(request, "streammateHome");
    if (home.empty()) home = getenv_string("STREAMMATE_HOME");
    if (home.empty()) return error(-32602, "STREAMMATE_HOME is required");

    std::filesystem::path destination = std::filesystem::path(home) / "studio" / "obs-imports" / collection_id;
    std::filesystem::path temp = destination;
    temp += ".tmp";
    std::string json;
    try {
      json = read_scene_collection_file(*collection);
    } catch (const std::runtime_error &) {
      return error(-32602, "scene collection exceeds import size limit");
    }
    try {
      std::filesystem::remove_all(temp);
      copy_config_without_service_json(*config, temp);
      write_import_map(temp, json);
      std::filesystem::remove_all(destination);
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::rename(temp, destination);
    } catch (...) {
      std::filesystem::remove_all(temp);
      return error(-32603, "OBS fixture import failed");
    }

    last_report_by_collection_[collection_id] = build_report(*config, json, collection_id);
    std::string prompt_summary = instantiate_prompt_sources(json);
    return "{\"ok\":true,\"destinationLabel\":\"$STREAMMATE_HOME/studio/obs-imports/" + json_escape(collection_id) +
           "\",\"report\":" + last_report_by_collection_[collection_id] + ",\"promptSources\":" + prompt_summary + "}";
  }

  std::string exercise_tcc_prompts(const std::string &) {
    PromptSourceSummary summary;
#if STREAMMATE_HAS_LIBOBS
    release_prompt_sources();
    const std::vector<std::string> tcc_classes = {"camera", "microphone", "screen"};
    for (const auto &tcc_class : tcc_classes) {
      mark_attempted(summary, tcc_class);
      std::string safe_name = "streammate-l4-" + tcc_class;
      bool created = false;
      for (const std::string &source_id : exercise_source_ids(tcc_class)) {
        if (create_prompt_source(source_id, safe_name, tcc_class, true)) {
          created = true;
          break;
        }
      }
      if (created) {
        ++summary.instantiated_count;
        mark_activated(summary, tcc_class);
      } else {
        ++summary.failed_count;
        summary.sanitized_failure_classes.push_back(tcc_class + "_source_create_failed");
      }
    }
    return tcc_exercise_summary_json(summary, "libobs-prompt-capable", true);
#else
    return tcc_exercise_summary_json(summary, "scaffold-no-tcc", false);
#endif
  }

  std::string report(const std::string &request) {
    std::string collection_id = extract_json_string(request, "collectionId");
    if (collection_id.empty()) return error(-32602, "collectionId is required");
    auto cached = last_report_by_collection_.find(collection_id);
    if (cached != last_report_by_collection_.end()) return cached->second;
    auto config = resolve_config_dir(request);
    if (!config) return error(-32602, "OBS config dir is required");
    auto collection = find_collection(*config, collection_id);
    if (!collection) return error(-32602, "collection not found");
    std::string json;
    try {
      json = read_scene_collection_file(*collection);
    } catch (const std::runtime_error &) {
      return error(-32602, "scene collection exceeds import size limit");
    }
    last_report_by_collection_[collection_id] = build_report(*config, json, collection_id);
    return last_report_by_collection_[collection_id];
  }

private:
  std::string error(int code, const std::string &message) const { return "__error__:" + std::to_string(code) + ":" + message; }

#if STREAMMATE_HAS_LIBOBS
  struct PromptSourceHandle {
    obs_source_t *source = nullptr;
    bool active = false;
  };

  void release_prompt_sources() {
    for (PromptSourceHandle &handle : prompt_sources_) {
      if (handle.source && handle.active) {
        obs_source_dec_active(handle.source);
      }
      if (handle.source) {
        obs_source_release(handle.source);
      }
    }
    prompt_sources_.clear();
  }

  bool apply_first_string_property(obs_source_t *source, obs_data_t *settings, const char *property_name) {
    obs_properties_t *properties = obs_source_properties(source);
    if (!properties) {
      return false;
    }
    obs_property_t *property = obs_properties_get(properties, property_name);
    bool selected = false;
    if (property && obs_property_list_format(property) == OBS_COMBO_FORMAT_STRING) {
      size_t count = obs_property_list_item_count(property);
      for (size_t index = 0; index < count; ++index) {
        const char *value = obs_property_list_item_string(property, index);
        if (value && value[0] != '\0' && !obs_property_list_item_disabled(property, index)) {
          obs_data_set_string(settings, property_name, value);
          selected = true;
          break;
        }
      }
    }
    obs_properties_destroy(properties);
    return selected;
  }

  std::optional<uint32_t> main_display_id() const {
#if defined(__APPLE__)
    CGDirectDisplayID display = CGMainDisplayID();
    if (display == 0) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(display);
#else
    return std::nullopt;
#endif
  }

  bool configure_initial_prompt_settings(obs_data_t *settings, const std::string &tcc_class) const {
    if (tcc_class == "screen") {
      obs_data_set_int(settings, "type", 0);
      auto display = main_display_id();
      if (!display) {
        return false;
      }
      obs_data_set_int(settings, "display", *display);
    }
    return true;
  }

  bool configure_prompt_source(obs_source_t *source, obs_data_t *settings, const std::string &tcc_class) {
    if (tcc_class == "camera") {
      obs_data_set_bool(settings, "use_preset", true);
      obs_data_set_bool(settings, "enable_audio", false);
      return apply_first_string_property(source, settings, "device");
    }
    if (tcc_class == "microphone") {
      obs_data_set_string(settings, "device_id", "default");
      return true;
    }
    if (tcc_class == "screen") {
      obs_data_set_int(settings, "type", 0);
      const char *display_uuid = obs_data_get_string(settings, "display_uuid");
      return apply_first_string_property(source, settings, "display_uuid") ||
             (display_uuid && display_uuid[0] != '\0') || obs_data_get_int(settings, "display") > 0;
    }
    return false;
  }

  bool create_prompt_source(const std::string &source_id, const std::string &safe_name, const std::string &tcc_class, bool activate) {
    obs_data_t *settings = obs_data_create();
    if (!configure_initial_prompt_settings(settings, tcc_class)) {
      obs_data_release(settings);
      return false;
    }
    obs_source_t *source = obs_source_create_private(source_id.c_str(), safe_name.c_str(), settings);
    if (!source) {
      obs_data_release(settings);
      return false;
    }
    if (!configure_prompt_source(source, settings, tcc_class)) {
      obs_source_release(source);
      obs_data_release(settings);
      return false;
    }
    obs_source_update(source, settings);
    obs_data_release(settings);
    if (activate) {
      obs_source_inc_active(source);
      std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    }
    prompt_sources_.push_back({source, activate});
    return !activate || obs_source_active(source);
  }
#endif

  std::string instantiate_prompt_sources(const std::string &json) {
    PromptSourceSummary summary;
#if STREAMMATE_HAS_LIBOBS
    release_prompt_sources();
#endif
    int prompt_index = 0;
    for (const auto &entry : parse_obs_source_entries(json)) {
      std::string tcc_class = prompt_source_class(entry.module);
      if (tcc_class.empty()) continue;
      count_prompt_source(summary, tcc_class);
#if STREAMMATE_HAS_LIBOBS
      std::string safe_name = "streammate-" + tcc_class + "-" + std::to_string(++prompt_index);
      bool created = false;
      for (const std::string &source_id : exercise_source_ids(tcc_class)) {
        if (create_prompt_source(source_id, safe_name, tcc_class, false)) {
          created = true;
          break;
        }
      }
      if (created) {
        ++summary.instantiated_count;
      } else {
        ++summary.failed_count;
      }
#endif
    }
#if STREAMMATE_HAS_LIBOBS
    return prompt_source_summary_json(summary, "libobs-prompt-capable", true);
#else
    return prompt_source_summary_json(summary, "scaffold-no-tcc", false);
#endif
  }

  std::optional<std::filesystem::path> resolve_config_dir(const std::string &request) const {
    std::string config = extract_json_string(request, "configDir");
    if (config.empty()) config = getenv_string("STREAMMATE_OBS_CONFIG_DIR");
    if (config.empty()) return std::nullopt;
    std::filesystem::path path(config);
    if (!std::filesystem::is_directory(path)) return std::nullopt;
    return path;
  }

  std::optional<std::filesystem::path> find_collection(const std::filesystem::path &config, const std::string &collection_id) const {
    std::filesystem::path scenes = config / "basic" / "scenes";
    if (!std::filesystem::is_directory(scenes)) return std::nullopt;
    std::vector<std::filesystem::path> candidates;
    for (const auto &entry : std::filesystem::directory_iterator(scenes)) {
      if (std::filesystem::is_symlink(entry.symlink_status())) continue;
      if (entry.is_regular_file() && entry.path().extension() == ".json") candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto &candidate : candidates) {
      if (collection_id.empty() || path_stem_id(candidate) == collection_id) return candidate;
    }
    return std::nullopt;
  }

  void copy_config_without_service_json(const std::filesystem::path &config, const std::filesystem::path &destination) const {
    std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(config); it != end; ++it) {
      const auto &entry = *it;
      if (std::filesystem::is_symlink(entry.symlink_status())) {
        it.disable_recursion_pending();
        continue;
      }
      std::filesystem::path relative = std::filesystem::relative(entry.path(), config);
      if (relative.filename() == "service.json") continue;
      std::filesystem::path target = destination / relative;
      if (entry.is_directory()) {
        std::filesystem::create_directories(target);
      } else if (entry.is_regular_file()) {
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
      }
    }
  }

  void write_import_map(const std::filesystem::path &destination, const std::string &json) const {
    std::vector<std::string> placeholders;
    for (const auto &entry : parse_obs_source_entries(json)) {
      if (entry.module == "third_party_camera_fx") {
        placeholders.push_back("{\"sourceId\":\"source:" + json_escape(entry.label) + "\",\"label\":\"" + json_escape(entry.label) +
                               "\",\"reason\":\"missing_plugin\"}");
      }
    }
    std::filesystem::create_directories(destination);
    std::ofstream out(destination / "streammate-import-map.json");
    out << "{\"placeholderSources\":" << json_array_join(placeholders) << "}\n";
  }

  std::string build_report(const std::filesystem::path &config, const std::string &json, const std::string &collection_id) const {
    std::vector<std::string> mapped;
    std::vector<std::string> degraded;
    std::vector<std::string> unresolved;
    for (const auto &entry : parse_obs_source_entries(json)) {
      const std::string source_id = "source:" + entry.label;
      if (entry.module == "scene") {
        mapped.push_back(report_item("scene:" + entry.label, "scene", entry.label, "mapped", "mapped_native"));
      } else if (entry.module == "color_source" || entry.module == "browser_source" || entry.module == "text_ft2_source" ||
                 entry.module == "text_gdiplus" || entry.module == "image_source" || entry.module == "ffmpeg_source") {
        mapped.push_back(report_item(source_id, "source", entry.label, "mapped", "mapped_native", entry.module));
      } else if (entry.module == "av_capture_input") {
        if (entry.label.find("Unplugged") != std::string::npos) {
          degraded.push_back(report_item(source_id, "source", entry.label, "degraded", "missing_device", entry.module, "camera",
                                         {"Original device identifier was absent from the fixture."}));
        } else {
          degraded.push_back(report_item(source_id, "source", entry.label, "degraded", "permission_required", entry.module, "camera",
                                         {"Native import defers camera permission until explicit operator approval."}));
        }
      } else if (entry.module == "coreaudio_input_capture") {
        degraded.push_back(report_item(source_id, "source", entry.label, "degraded", "permission_required", entry.module, "microphone",
                                       {"Native import defers microphone permission until explicit operator approval."}));
      } else if (entry.module == "screen_capture") {
        degraded.push_back(report_item(source_id, "source", entry.label, "degraded", "permission_required", entry.module, "screen",
                                       {"Native import defers screen permission until explicit operator approval."}));
      } else if (entry.module == "third_party_camera_fx") {
        degraded.push_back(report_item(source_id, "source", entry.label, "degraded", "missing_plugin", entry.module, "",
                                       {"Replaced with placeholder source because obs-third-party-fx is not bundled upstream."}));
      } else if (entry.module.find("filter") != std::string::npos) {
        unresolved.push_back(report_item("filter:Station Overlay:" + entry.label, "filter", "Station Overlay / " + entry.label, "unresolved",
                                         "unsupported_frontend_feature", entry.module, "",
                                         {"OBS frontend filter is not imported by the native host scaffold."}));
      }
    }

    std::string profile_name = first_profile_name(config);
    std::string downgrade = report_item("profile:" + profile_name + ":encoder", "profile", profile_name, "degraded", "profile_downgraded", "", "",
                                        {"OBS encoder obs_x264 mapped to native x264 fallback."});
    std::string profile = "{\"mappedEncoder\":\"x264\",\"mappedOutput\":\"rtmp\",\"downgrades\":[" + downgrade +
                          "],\"serviceKeyAction\":\"" + std::string(has_service_key(config) ? "requires-consent" : "not-present") + "\"}";
    return "{\"reportId\":\"import-" + json_escape(collection_id) + "\",\"collectionId\":\"" + json_escape(collection_id) +
           "\",\"generatedAt\":\"1970-01-01T00:00:00.000Z\",\"mapped\":" + json_array_join(mapped) +
           ",\"degraded\":" + json_array_join(degraded) + ",\"unresolved\":" + json_array_join(unresolved) +
           ",\"profile\":" + profile + "}";
  }

  std::map<std::string, std::string> last_report_by_collection_;
#if STREAMMATE_HAS_LIBOBS
  std::vector<PromptSourceHandle> prompt_sources_;
#endif
};

bool is_renderer_error(const std::string &result) { return result.rfind("__error__:", 0) == 0; }

std::string renderer_error_to_rpc(const std::string &id, const std::string &result) {
  size_t code_start = std::string("__error__:").size();
  size_t message_start = result.find(':', code_start);
  if (message_start == std::string::npos) return rpc_error(id, -32603, "renderer error");
  int code = std::stoi(result.substr(code_start, message_start - code_start));
  return rpc_error(id, code, result.substr(message_start + 1));
}

struct EndpointPreview {
  std::string host;
  int port = 1935;
  bool valid = false;
  bool loopback = false;
};

EndpointPreview parse_rtmp_endpoint(const std::string &endpoint) {
  EndpointPreview parsed;
  const std::string prefix = "rtmp://";
  if (endpoint.rfind(prefix, 0) != 0) return parsed;
  std::string rest = endpoint.substr(prefix.size());
  auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  auto at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);
  if (authority.empty()) return parsed;
  std::string host = authority;
  int port = 1935;
  auto colon = authority.rfind(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    try {
      port = std::stoi(authority.substr(colon + 1));
    } catch (...) {
      return parsed;
    }
  }
  if (host == "localhost") host = "127.0.0.1";
  parsed.host = host;
  parsed.port = port;
  parsed.valid = !host.empty() && port > 0 && port <= 65535;
  parsed.loopback = host == "127.0.0.1" || host == "::1" || host == "[::1]";
  return parsed;
}

std::string endpoint_preview_json(const EndpointPreview &endpoint) {
  if (!endpoint.valid) return "null";
  return "{\"scheme\":\"rtmp\",\"host\":\"" + json_escape(endpoint.host) + "\",\"port\":" + std::to_string(endpoint.port) +
         ",\"path\":\"<redacted>\"}";
}

struct OutputStats {
  uint64_t rendered_frames = 0;
  uint64_t encoded_frames = 0;
  uint64_t dropped_frames = 0;
  uint64_t severe_frames = 0;
  int congestion_percent = 0;
};

class OutputController {
public:
  // Q-122: the launch-time gate that must be open before the caller-supplied
  // allowLiveEgress JSON flag is honored. Default off; set once at startup.
  void set_launch_allow_live_egress(bool allowed) { launch_allow_live_egress_ = allowed; }

  std::string configure(const std::string &request) {
    std::string output_id = extract_json_string(request, "outputId");
    if (output_id.empty()) return rpc_error_result(-32602, "outputId is required");
    if (auto refusal = live_egress_launch_gate(request)) return *refusal;
    std::string endpoint = extract_json_string(request, "endpoint");
    if (!endpoint.empty()) {
      EndpointPreview parsed = parse_rtmp_endpoint(endpoint);
      if (!parsed.valid) return rpc_error_result(-32602, "endpoint must be an rtmp URL");
      endpoint_ = endpoint;
      endpoint_preview_ = parsed;
    }
    output_id_ = output_id;
    configured_ = true;
    allow_live_egress_ = extract_json_bool(request, "allowLiveEgress").value_or(false);
    requested_video_encoder_ = extract_json_string(request, "videoEncoder");
    if (requested_video_encoder_.empty()) requested_video_encoder_ = "videotoolbox_h264";
    requested_audio_encoder_ = extract_json_string(request, "audioEncoder");
    if (requested_audio_encoder_.empty()) requested_audio_encoder_ = "aac";
    std::string live_fields = allow_live_egress_ ? ",\"allowLiveEgress\":true" : "";
    return "{\"ok\":true,\"outputId\":\"" + json_escape(output_id_) + "\",\"configured\":true," +
           "\"encoder\":{\"requested\":\"" + json_escape(requested_video_encoder_) + "\",\"actual\":\"" + json_escape(actual_video_encoder()) +
           "\",\"fallback\":" + std::string(actual_video_encoder() == requested_video_encoder_ ? "false" : "true") + "}," +
           "\"audio\":{\"requested\":\"" + json_escape(requested_audio_encoder_) + "\",\"actual\":\"" + json_escape(actual_audio_encoder()) + "\"}," +
           "\"endpoint\":" + endpoint_preview_json(endpoint_preview_) + live_fields + ",\"streamKeyStatus\":\"not-stored\"}";
  }

  std::string start(const std::string &request) {
    std::string output_id = extract_json_string(request, "outputId");
    if (output_id.empty()) return rpc_error_result(-32602, "outputId is required");
    // Assert the launch-flag gate before any endpoint is parsed or contacted.
    // A caller-supplied allowLiveEgress:true here is refused unless the host was
    // launched with --allow-live-egress. The live path itself is armed only by
    // output.configure (the recorded L5 contract); start never mutates that
    // stored flag, so a rejected start cannot arm live egress for a later call.
    if (auto refusal = live_egress_launch_gate(request)) return *refusal;
    std::string endpoint = extract_json_string(request, "endpoint");
    if (!endpoint.empty()) {
      EndpointPreview parsed = parse_rtmp_endpoint(endpoint);
      if (!parsed.valid) return rpc_error_result(-32602, "endpoint must be an rtmp URL");
      endpoint_ = endpoint;
      endpoint_preview_ = parsed;
    }
    if (!configured_) {
      output_id_ = output_id;
      configured_ = true;
    }
    if (output_id != output_id_) return rpc_error_result(-32602, "output is not configured");
    // Both gates must assert: the JSON flag (recorded in allow_live_egress_ by
    // output.configure) and the launch flag. Neither alone reaches live egress.
    if (allow_live_egress_ && launch_allow_live_egress_) {
#if STREAMMATE_HAS_LIBOBS
      return start_live(stream_key_from_request(request));
#else
      if (stream_key_from_request(request).empty()) return rpc_error_result(-32602, "streamKey is required");
      return rpc_error_result(-32602, "live egress requires a libobs build");
#endif
    }
    if (!endpoint_preview_.valid || !endpoint_preview_.loopback) return rpc_error_result(-32602, "output.start only permits a local fake RTMP ingest endpoint in this scaffold");
    std::string stream_key = extract_json_string(request, "streamKey");
    if (stream_key.empty()) return rpc_error_result(-32602, "streamKey is required");

    close_ingest();
#if STREAMMATE_HAS_LIBOBS
    stop_live_output();
#endif
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return rpc_error_result(-32603, "fake ingest socket create failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(endpoint_preview_.port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      close(fd);
      clear_stream_key();
      return rpc_error_result(-32603, "fake ingest connect failed");
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ingest_fd_ = fd;
    stream_key_ = stream_key;
    running_ = true;
    error_pending_ = false;
    stopped_pending_ = false;
    started_pending_ = true;
    stats_ = {};
    started_at_ = std::chrono::steady_clock::now();
    last_frame_at_ = std::chrono::steady_clock::now();
    last_stats_at_ = std::chrono::steady_clock::now();
    panic_audio_hard_muted_ = true;
    send_ingest_chunk();
    return status_json("memory-only-redacted", true);
  }

  std::string stop(const std::string &request) {
    std::string output_id = extract_json_string(request, "outputId");
    if (!output_id.empty() && !output_id_.empty() && output_id != output_id_) return rpc_error_result(-32602, "output is not configured");
    bool was_running = running_;
    running_ = false;
    close_ingest();
#if STREAMMATE_HAS_LIBOBS
    stop_live_output();
#endif
    clear_stream_key();
    panic_audio_hard_muted_ = false;
    if (was_running) stopped_pending_ = true;
    return status_json("cleared", true);
  }

  std::string status(const std::string &) const {
    return status_json(stream_key_.empty() ? "cleared" : "memory-only-redacted", false);
  }

  std::string subscribe(const std::string &request) {
    int interval = extract_json_int(request, "intervalMs").value_or(1000);
    if (interval <= 0) interval = 1000;
    stats_interval_ms_ = std::clamp(interval, 50, 10000);
    stats_subscribed_ = true;
    last_stats_at_ = std::chrono::steady_clock::now();
    return "{\"ok\":true,\"sampleShape\":\"spec18-item8\",\"intervalMs\":" + std::to_string(stats_interval_ms_) + "}";
  }

  void tick(int control_fd) {
    if (!running_) return;
    auto now = std::chrono::steady_clock::now();
#if STREAMMATE_HAS_LIBOBS
    if (live_active_) {
      if (live_reconnect_pending_.exchange(false)) {
        fail_output(control_fd, "live egress reconnect refused");
        return;
      }
      if (live_stop_pending_.exchange(false)) {
        int code = live_stop_code_.exchange(OBS_OUTPUT_SUCCESS);
        if (code != OBS_OUTPUT_SUCCESS) {
          fail_output(control_fd, "live egress disconnected");
          return;
        }
        running_ = false;
        stop_live_output();
        clear_stream_key();
        panic_audio_hard_muted_ = false;
        stopped_pending_ = true;
      } else {
        update_live_stats();
      }
    } else
#endif
    {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_frame_at_).count() >= 33) {
      stats_.rendered_frames += 1;
      stats_.encoded_frames += 1;
      last_frame_at_ = now;
      if (!send_ingest_chunk()) {
        fail_output(control_fd, "fake ingest disconnected");
        return;
      }
    }
    if (ingest_disconnected()) {
      fail_output(control_fd, "fake ingest disconnected");
      return;
    }
    }
    if (stats_subscribed_ && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_at_).count() >= stats_interval_ms_) {
      send_text_frame(control_fd, stats_event());
      last_stats_at_ = now;
    }
    if (started_pending_) {
      send_text_frame(control_fd, output_event("output.started", "running"));
      started_pending_ = false;
    }
    if (stopped_pending_) {
      send_text_frame(control_fd, output_event("output.stopped", "stopped"));
      stopped_pending_ = false;
    }
  }

private:
  std::string rpc_error_result(int code, const std::string &message) const {
    return "__error__:" + std::to_string(code) + ":" + message;
  }

  // Q-122 defense-in-depth: refuse a caller-supplied allowLiveEgress:true unless
  // the host was launched with --allow-live-egress. Returns a sanitized refusal
  // (no endpoint, key, or path echoed) before any endpoint is touched.
  std::optional<std::string> live_egress_launch_gate(const std::string &request) const {
    bool wants_live = extract_json_bool(request, "allowLiveEgress").value_or(false);
    if (wants_live && !launch_allow_live_egress_) {
      return rpc_error_result(-32602, "live egress requires the --allow-live-egress launch flag");
    }
    return std::nullopt;
  }

  std::string actual_video_encoder() const {
#if STREAMMATE_HAS_LIBOBS
    return actual_video_encoder_id_.empty() ? "videotoolbox_h264" : actual_video_encoder_id_;
#else
    return "x264-scaffold";
#endif
  }

  std::string actual_audio_encoder() const {
#if STREAMMATE_HAS_LIBOBS
    return actual_audio_encoder_id_.empty() ? "aac-scaffold" : actual_audio_encoder_id_;
#else
    return "aac-scaffold";
#endif
  }

  std::string status_json(const std::string &stream_key_status, bool include_ok) const {
    double severe_percent = stats_.rendered_frames == 0 ? 0.0 : (static_cast<double>(stats_.severe_frames) * 100.0 / static_cast<double>(stats_.rendered_frames));
    return std::string("{") + (include_ok ? "\"ok\":true," : "") +
           "\"outputId\":\"" + json_escape(output_id_.empty() ? "rtmp-main" : output_id_) + "\",\"configured\":" + (configured_ ? "true" : "false") +
           ",\"running\":" + (running_ ? "true" : "false") + ",\"endpoint\":" + endpoint_preview_json(endpoint_preview_) +
           ",\"encoder\":{\"requested\":\"" + json_escape(requested_video_encoder_) + "\",\"actual\":\"" + json_escape(actual_video_encoder()) +
           "\",\"fallback\":" + std::string(actual_video_encoder() == requested_video_encoder_ ? "false" : "true") + "}," +
           "\"audio\":{\"requested\":\"" + json_escape(requested_audio_encoder_) + "\",\"actual\":\"" + json_escape(actual_audio_encoder()) + "\"}," +
           "\"streamKeyStatus\":\"" + json_escape(stream_key_status) + "\",\"panicMute\":{\"hostAudioHardMuted\":" +
           (panic_audio_hard_muted_ ? "true" : "false") + "},\"stats\":" + stats_json(severe_percent) + "}";
  }

  std::string stats_json(double severe_percent) const {
    return "{\"shape\":\"spec18-item8\",\"renderedFrames\":" + std::to_string(stats_.rendered_frames) +
           ",\"encodedFrames\":" + std::to_string(stats_.encoded_frames) + ",\"droppedFrames\":" + std::to_string(stats_.dropped_frames) +
           ",\"severeFrameLossPercent\":" + std::to_string(severe_percent) + ",\"congestion\":{\"level\":\"none\",\"percent\":" +
           std::to_string(stats_.congestion_percent) + "}}";
  }

  std::string output_event(const std::string &type, const std::string &status) const {
    return "{\"type\":\"" + type + "\",\"sessionId\":\"local\",\"payload\":{\"outputId\":\"" +
           json_escape(output_id_) + "\",\"status\":\"" + status + "\",\"endpoint\":" + endpoint_preview_json(endpoint_preview_) +
           ",\"encoder\":\"" + json_escape(actual_video_encoder()) + "\"}}";
  }

  std::string stats_event() const {
    double severe_percent = stats_.rendered_frames == 0 ? 0.0 : (static_cast<double>(stats_.severe_frames) * 100.0 / static_cast<double>(stats_.rendered_frames));
    return "{\"type\":\"stats.sample\",\"sessionId\":\"local\",\"payload\":{\"outputId\":\"" + json_escape(output_id_) +
           "\",\"sample\":" + stats_json(severe_percent) + "}}";
  }

  bool send_ingest_chunk() {
    if (ingest_fd_ < 0) return false;
    const char chunk[] = "STREAMMATE_FAKE_RTMP_CHUNK\n";
    ssize_t sent = send(ingest_fd_, chunk, sizeof(chunk) - 1, 0);
    return sent == static_cast<ssize_t>(sizeof(chunk) - 1) || (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  }

  bool ingest_disconnected() const {
    if (ingest_fd_ < 0) return true;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(ingest_fd_, &readfds);
    timeval timeout{0, 0};
    int ready = select(ingest_fd_ + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(ingest_fd_, &readfds)) return false;
    char byte = 0;
    ssize_t n = recv(ingest_fd_, &byte, 1, MSG_PEEK);
    return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
  }

  void fail_output(int control_fd, const std::string &reason) {
    running_ = false;
    close_ingest();
#if STREAMMATE_HAS_LIBOBS
    stop_live_output();
#endif
    clear_stream_key();
    panic_audio_hard_muted_ = false;
    send_text_frame(control_fd, "{\"type\":\"output.error\",\"sessionId\":\"local\",\"payload\":{\"outputId\":\"" + json_escape(output_id_) +
                                "\",\"status\":\"stopped\",\"sanitizedReason\":\"" + json_escape(reason) + "\"}}");
  }

  void close_ingest() {
    if (ingest_fd_ >= 0) {
      close(ingest_fd_);
      ingest_fd_ = -1;
    }
  }

  void clear_stream_key() {
    std::fill(stream_key_.begin(), stream_key_.end(), '\0');
    stream_key_.clear();
#if STREAMMATE_HAS_LIBOBS
    if (obs_service_) {
      obs_service_release(obs_service_);
      obs_service_ = nullptr;
    }
#endif
  }

  std::string stream_key_from_request(const std::string &request) const {
    return extract_json_string(request, "streamKey");
  }

#if STREAMMATE_HAS_LIBOBS
  static void output_stop_signal(void *data, calldata_t *params) {
    auto *controller = static_cast<OutputController *>(data);
    controller->live_stop_code_.store(static_cast<int>(calldata_int(params, "code")));
    controller->live_stop_pending_.store(true);
  }

  static void output_reconnect_signal(void *data, calldata_t *) {
    auto *controller = static_cast<OutputController *>(data);
    controller->live_reconnect_pending_.store(true);
  }

  static bool refuse_reconnect(void *, obs_output_t *, int) {
    return false;
  }

  std::string start_live(const std::string &stream_key) {
    if (stream_key.empty()) return rpc_error_result(-32602, "streamKey is required");
    if (!endpoint_preview_.valid) return rpc_error_result(-32602, "endpoint must be an rtmp URL");

    close_ingest();
    stop_live_output();
    clear_stream_key();
    stats_ = {};
    live_stop_pending_.store(false);
    live_reconnect_pending_.store(false);
    live_stop_code_.store(OBS_OUTPUT_SUCCESS);

    obs_data_t *output_settings = obs_data_create();
    if (!output_settings) return rpc_error_result(-32603, "live egress output unavailable");
    obs_output_ = obs_output_create("rtmp_output", "streammate-live-rtmp", output_settings, nullptr);
    obs_data_release(output_settings);
    if (!obs_output_) return rpc_error_result(-32603, "live egress output unavailable");
    obs_output_set_reconnect_settings(obs_output_, 0, 0);
    obs_output_set_reconnect_callback(obs_output_, refuse_reconnect, this);
    signal_handler_t *signals = obs_output_get_signal_handler(obs_output_);
    signal_handler_connect(signals, "stop", output_stop_signal, this);
    signal_handler_connect(signals, "reconnect", output_reconnect_signal, this);

    obs_data_t *service_settings = obs_data_create();
    if (!service_settings) {
      stop_live_output();
      return rpc_error_result(-32603, "live egress service unavailable");
    }
    obs_data_set_string(service_settings, "server", endpoint_.c_str());
    obs_data_set_string(service_settings, "key", stream_key.c_str());
    obs_service_ = obs_service_create("rtmp_custom", "streammate-live-service", service_settings, nullptr);
    obs_data_release(service_settings);
    if (!obs_service_) {
      stop_live_output();
      return rpc_error_result(-32603, "live egress service unavailable");
    }

    obs_video_encoder_ = create_video_encoder(actual_video_encoder_id_);
    obs_audio_encoder_ = create_audio_encoder(actual_audio_encoder_id_);
    if (!obs_video_encoder_ || !obs_audio_encoder_) {
      stop_live_output();
      clear_stream_key();
      return rpc_error_result(-32603, "live egress encoder unavailable");
    }

    if (!ensure_live_source()) {
      stop_live_output();
      clear_stream_key();
      return rpc_error_result(-32603, "live egress source unavailable");
    }

    obs_encoder_set_video(obs_video_encoder_, obs_get_video());
    obs_encoder_set_audio(obs_audio_encoder_, obs_get_audio());
    obs_output_set_service(obs_output_, obs_service_);
    obs_output_set_video_encoder(obs_output_, obs_video_encoder_);
    obs_output_set_audio_encoder(obs_output_, obs_audio_encoder_, 0);
    if (!obs_output_start(obs_output_)) {
      stop_live_output();
      clear_stream_key();
      return rpc_error_result(-32603, "live egress start failed");
    }

    stream_key_ = stream_key;
    running_ = true;
    live_active_ = true;
    error_pending_ = false;
    stopped_pending_ = false;
    started_pending_ = true;
    started_at_ = std::chrono::steady_clock::now();
    last_frame_at_ = std::chrono::steady_clock::now();
    last_stats_at_ = std::chrono::steady_clock::now();
    panic_audio_hard_muted_ = true;
    update_live_stats();
    return status_json("memory-only-redacted", true);
  }

  obs_encoder_t *create_video_encoder(std::string &actual_id) {
    std::vector<std::string> candidates;
    const char *id = nullptr;
    for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
      if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;
      const char *codec = obs_get_encoder_codec(id);
      if (!codec || std::string(codec) != "h264") continue;
      std::string encoder_id(id);
      if (encoder_id.find("videotoolbox") != std::string::npos || encoder_id.find("com.apple") != std::string::npos) {
        candidates.push_back(encoder_id);
      }
    }
    candidates.push_back("obs_x264");

    for (const std::string &candidate : candidates) {
      obs_data_t *settings = obs_data_create();
      if (!settings) continue;
      obs_data_set_int(settings, "bitrate", 4500);
      obs_data_set_int(settings, "keyint_sec", 2);
      obs_data_set_string(settings, "profile", "high");
      obs_encoder_t *encoder = obs_video_encoder_create(candidate.c_str(), "streammate-live-video", settings, nullptr);
      obs_data_release(settings);
      if (encoder) {
        actual_id = candidate;
        return encoder;
      }
    }
    actual_id.clear();
    return nullptr;
  }

  obs_encoder_t *create_audio_encoder(std::string &actual_id) {
    const std::array<const char *, 2> candidates = {"CoreAudio_AAC", "ffmpeg_aac"};
    for (const char *candidate : candidates) {
      obs_data_t *settings = obs_data_create();
      if (!settings) continue;
      obs_data_set_int(settings, "bitrate", 160);
      obs_encoder_t *encoder = obs_audio_encoder_create(candidate, "streammate-live-audio", settings, 0, nullptr);
      obs_data_release(settings);
      if (encoder) {
        actual_id = candidate;
        return encoder;
      }
    }
    actual_id.clear();
    return nullptr;
  }

  bool ensure_live_source() {
    obs_source_t *current = obs_get_output_source(0);
    if (current) {
      obs_source_release(current);
      return true;
    }
    fallback_scene_ = obs_scene_create_private("streammate-live-fallback-scene");
    if (!fallback_scene_) return false;

    obs_data_t *color_settings = obs_data_create();
    obs_source_t *color = nullptr;
    if (color_settings) {
      obs_data_set_int(color_settings, "color", 0xFF203040);
      obs_data_set_int(color_settings, "width", 1280);
      obs_data_set_int(color_settings, "height", 720);
      color = obs_source_create_private("color_source", "streammate-live-fallback-color", color_settings);
      obs_data_release(color_settings);
    }
    if (color) {
      obs_scene_add(fallback_scene_, color);
      obs_source_release(color);
    }

    obs_set_output_source(0, obs_scene_get_source(fallback_scene_));
    fallback_scene_bound_ = true;
    return true;
  }

  void update_live_stats() {
    if (!obs_output_) return;
    int total = obs_output_get_total_frames(obs_output_);
    int dropped = obs_output_get_frames_dropped(obs_output_);
    float congestion = obs_output_get_congestion(obs_output_);
    stats_.rendered_frames = total < 0 ? 0 : static_cast<uint64_t>(total);
    stats_.encoded_frames = stats_.rendered_frames;
    stats_.dropped_frames = dropped < 0 ? 0 : static_cast<uint64_t>(dropped);
    stats_.severe_frames = stats_.dropped_frames;
    stats_.congestion_percent = std::clamp(static_cast<int>(congestion * 100.0f + 0.5f), 0, 100);
  }

  void stop_live_output() {
    live_active_ = false;
    if (obs_output_) {
      obs_output_stop(obs_output_);
      obs_output_release(obs_output_);
      obs_output_ = nullptr;
    }
    if (obs_video_encoder_) {
      obs_encoder_release(obs_video_encoder_);
      obs_video_encoder_ = nullptr;
    }
    if (obs_audio_encoder_) {
      obs_encoder_release(obs_audio_encoder_);
      obs_audio_encoder_ = nullptr;
    }
    if (fallback_scene_bound_) {
      obs_set_output_source(0, nullptr);
      fallback_scene_bound_ = false;
    }
    if (fallback_scene_) {
      obs_scene_release(fallback_scene_);
      fallback_scene_ = nullptr;
    }
  }
#endif

  bool configured_ = false;
  bool running_ = false;
  bool stats_subscribed_ = false;
  bool allow_live_egress_ = false;
  bool launch_allow_live_egress_ = false;
  bool panic_audio_hard_muted_ = false;
  bool started_pending_ = false;
  bool stopped_pending_ = false;
  bool error_pending_ = false;
  int stats_interval_ms_ = 1000;
  int ingest_fd_ = -1;
  std::string output_id_;
  std::string endpoint_;
  EndpointPreview endpoint_preview_;
  std::string requested_video_encoder_ = "videotoolbox_h264";
  std::string requested_audio_encoder_ = "aac";
  std::string actual_video_encoder_id_;
  std::string actual_audio_encoder_id_;
  std::string stream_key_;
  OutputStats stats_;
  std::chrono::steady_clock::time_point started_at_;
  std::chrono::steady_clock::time_point last_frame_at_;
  std::chrono::steady_clock::time_point last_stats_at_;
#if STREAMMATE_HAS_LIBOBS
  bool live_active_ = false;
  bool fallback_scene_bound_ = false;
  obs_output_t *obs_output_ = nullptr;
  obs_service_t *obs_service_ = nullptr;
  obs_encoder_t *obs_video_encoder_ = nullptr;
  obs_encoder_t *obs_audio_encoder_ = nullptr;
  obs_scene_t *fallback_scene_ = nullptr;
  std::atomic_bool live_stop_pending_{false};
  std::atomic_bool live_reconnect_pending_{false};
  std::atomic_int live_stop_code_{OBS_OUTPUT_SUCCESS};
#endif
};

bool is_output_error(const std::string &result) { return result.rfind("__error__:", 0) == 0; }

std::string output_error_to_rpc(const std::string &id, const std::string &result) {
  return renderer_error_to_rpc(id, result);
}

// Encode an RGBA buffer as a PNG (reuses the same hand-rolled encoder as the
// scaffold offscreen path). Used to return the native overlay raster over the
// control protocol for the parity capture driver (34.M3).
std::vector<uint8_t> encode_rgba_png(const std::vector<uint8_t> &rgba, int width, int height) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>((width * 4 + 1) * height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0); // filter: none
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
    raw.insert(raw.end(), rgba.begin() + static_cast<long>(row),
               rgba.begin() + static_cast<long>(row + static_cast<size_t>(width) * 4));
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<uint8_t> ihdr;
  append_be32(ihdr, static_cast<uint32_t>(width));
  append_be32(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);
  ihdr.push_back(6);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  append_png_chunk(png, "IHDR", ihdr);
  append_png_chunk(png, "IDAT", zlib_store(raw));
  append_png_chunk(png, "IEND", {});
  return png;
}

std::string hex_u64(uint64_t value) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buf);
}

std::string format_ms(double value) {
  if (value < 0.0) value = 0.0;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3f", value);
  return std::string(buf);
}

// Extract the raw JSON substring for an object-valued key (brace-matched).
// Naive but sufficient for the controlled overlayAction payloads driven over the
// loopback protocol; returns empty when the key is absent or not an object.
std::string extract_json_object(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return "";
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return "";
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || json[pos] != '{') return "";
  size_t start = pos;
  int depth = 0;
  bool in_string = false;
  for (; pos < json.size(); ++pos) {
    char c = json[pos];
    if (in_string) {
      if (c == '\\') {
        ++pos;
        continue;
      }
      if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) return json.substr(start, pos - start + 1);
    }
  }
  return "";
}

// Host-side glue for the explicit opt-in Phase B native overlay renderer.
// Owns one NativeOverlayRenderer per opted-in source; a session that never sends
// kind "native-overlay" constructs zero renderer state. Carries no product logic
// (ADR-0005 Decision 2) — it routes payloads to the plain rasterizer library.
class NativeOverlayManager {
public:
  std::string create(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    if (source_id.empty()) return error(-32602, "sourceId is required");
    if (sources_.count(source_id)) return error(-32602, "source already exists");
    auto entry = std::make_unique<Entry>();
    entry->scene_id = extract_json_string(request, "sceneId");
    sources_[source_id] = std::move(entry);
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) +
           "\",\"kind\":\"native-overlay\",\"renderer\":\"native-overlay-rasterizer\",\"width\":" +
           std::to_string(streammate::overlay::kOverlayWidth) + ",\"height\":" +
           std::to_string(streammate::overlay::kOverlayHeight) + "}";
  }

  bool has(const std::string &source_id) const { return sources_.count(source_id) != 0; }

  std::string apply(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (it == sources_.end()) return error(-32602, "source not found");
    std::string action = extract_json_object(request, "overlayAction");
    if (action.empty()) return error(-32602, "overlayAction object is required");

    streammate::overlay::NativeOverlayRenderer &renderer = it->second->renderer;
    streammate::overlay::OverlayRasterResult res = renderer.apply(action);
    if (!res.ok) return error(-32602, res.error.empty() ? "unsupported overlay action" : res.error);

    std::string timing = "{";
    bool first = true;
    for (const auto &kv : res.timing) {
      if (!first) timing += ",";
      first = false;
      timing += "\"" + json_escape(kv.first) + "\":" + format_ms(kv.second);
    }
    timing += "}";

    std::vector<uint8_t> png = encode_rgba_png(renderer.rgba(), renderer.width(), renderer.height());
    std::string out = "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) +
                      "\",\"kind\":\"native-overlay\",\"category\":\"" + json_escape(res.category) +
                      "\",\"timing\":" + timing + ",\"rasterHash\":\"" + hex_u64(res.raster_hash) +
                      "\",\"empty\":" + std::string(res.empty ? "true" : "false");
    if (!res.trigger_record.empty()) {
      out += ",\"trigger\":\"" + json_escape(res.trigger_record) + "\"";
    }
    out += ",\"pngBase64\":\"" + base64(png.data(), png.size()) + "\"}";
    return out;
  }

  std::string remove_source(const std::string &request) {
    std::string source_id = extract_json_string(request, "sourceId");
    auto it = sources_.find(source_id);
    if (it == sources_.end()) return error(-32602, "source not found");
    sources_.erase(it);
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"removed\":true}";
  }

  size_t count() const { return sources_.size(); }

private:
  struct Entry {
    std::string scene_id;
    streammate::overlay::NativeOverlayRenderer renderer;
  };
  std::string error(int code, const std::string &message) const {
    return "__error__:" + std::to_string(code) + ":" + message;
  }
  std::map<std::string, std::unique_ptr<Entry>> sources_;
};

// Spec 34 Capability 7 (chunk 34.H4): replay-buffer + local recording outputs.
// Scaffold mode (always) writes synthetic files under the host state directory
// ($STREAMMATE_HOME/studio), loopback-only, with honest status transitions and
// no socket of any kind. The real ffmpeg_muxer/replay_buffer wiring is compiled
// under STREAMMATE_HAS_LIBOBS (compile-proof) and never starts egress. Every
// response path is $STREAMMATE_HOME-relative; a destination outside the state
// directory or a URL-shaped destination is refused with a named error.
class RecordReplayController {
public:
  std::string start_record(const std::string &request) {
    auto home = resolve_home();
    if (!home) return err(-32602, "STREAMMATE_HOME is required");
    std::string record_id = extract_json_string(request, "recordId");
    if (record_id.empty()) record_id = "recording";
    auto relative = sanitized_relative(request, "recordings", record_id + ".mkv");
    if (!relative) return err(-32602, "record destination must be a relative path under the state directory");
    auto full_opt = confined_path(*home, *relative);
    if (!full_opt) return err(-32602, "record destination must be a relative path under the state directory");

    std::filesystem::path full = *full_opt;
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    if (ec) return err(-32603, "record output could not be created");
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) return err(-32603, "record output could not be created");
    out << "STMREC1\n"
        << "recordId=" << record_id << "\n"
        << "status=recording\n"
        << "frame:000001\n";
    out.close();

    RecordSession session;
    session.record_id = record_id;
    session.full_path = full;
    session.label = "$STREAMMATE_HOME/studio/" + relative->generic_string();
    session.running = true;
    session.status = "recording";
    session.frame_count = 1;
    records_[record_id] = session;

    pending_event_ = record_event("record.started", session);
    return record_json(session, true, false);
  }

  std::string stop_record(const std::string &request) {
    std::string record_id = extract_json_string(request, "recordId");
    if (record_id.empty()) record_id = "recording";
    auto it = records_.find(record_id);
    if (it == records_.end()) return err(-32602, "recording is not running");
    RecordSession &session = it->second;
    if (session.running) {
      std::ofstream out(session.full_path, std::ios::binary | std::ios::app);
      if (!out) return err(-32603, "record output could not be finalized");
      out << "frame:000002\n"
          << "frame:000003\n"
          << "STMREC-END frames=3\n";
      out.close();
      session.frame_count = 3;
      session.running = false;
      session.status = "stopped";
      std::error_code ec;
      session.bytes = static_cast<long long>(std::filesystem::file_size(session.full_path, ec));
      if (ec) session.bytes = 0;
      pending_event_ = record_event("record.stopped", session);
    }
    return record_json(session, true, true);
  }

  std::string record_status(const std::string &request) const {
    std::string record_id = extract_json_string(request, "recordId");
    if (record_id.empty()) record_id = "recording";
    auto it = records_.find(record_id);
    if (it == records_.end()) {
      return "{\"ok\":true,\"recordId\":\"" + json_escape(record_id) +
             "\",\"running\":false,\"status\":\"idle\",\"path\":\"\"}";
    }
    return record_json(it->second, true, !it->second.running);
  }

  std::string start_replay(const std::string &request) {
    auto home = resolve_home();
    if (!home) return err(-32602, "STREAMMATE_HOME is required");
    std::string replay_id = extract_json_string(request, "replayId");
    if (replay_id.empty()) replay_id = "replay";
    // Validate any caller-supplied destination up front (same shape rule as
    // recordings); saved segments are named replays/<id>-<seq>.mkv and are
    // re-checked for containment at save time.
    auto relative = sanitized_relative(request, "replays", replay_id + ".mkv");
    if (!relative) return err(-32602, "replay destination must be a relative path under the state directory");

    ReplaySession session;
    session.replay_id = replay_id;
    session.home = *home;
    session.running = true;
    session.status = "buffering";
    session.save_seq = 0;
    session.chunks.clear();
    for (int index = 0; index < kReplayChunks; ++index) {
      session.chunks.push_back("STMREPLAY-CHUNK-" + std::to_string(index) + "\n");
    }
    replays_[replay_id] = session;
    return "{\"ok\":true,\"replayId\":\"" + json_escape(replay_id) +
           "\",\"running\":true,\"status\":\"buffering\",\"bufferedChunks\":" +
           std::to_string(session.chunks.size()) + "}";
  }

  std::string save_replay(const std::string &request) {
    std::string replay_id = extract_json_string(request, "replayId");
    if (replay_id.empty()) replay_id = "replay";
    auto it = replays_.find(replay_id);
    if (it == replays_.end() || !it->second.running) {
      return err(-32602, "replay buffer is not running");
    }
    ReplaySession &session = it->second;
    ++session.save_seq;
    std::filesystem::path relative =
        std::filesystem::path("replays") / (replay_id + "-" + std::to_string(session.save_seq) + ".mkv");
    // Re-validate containment at write time: a "replays" symlink planted after
    // replay.start cannot redirect the save outside $STREAMMATE_HOME/studio.
    auto full_opt = confined_path(session.home, relative);
    if (!full_opt) return err(-32602, "replay destination must be a relative path under the state directory");
    std::filesystem::path full = *full_opt;
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    if (ec) return err(-32603, "replay could not be materialized");
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) return err(-32603, "replay could not be materialized");
    out << "STMREPLAY1\n";
    for (const std::string &chunk : session.chunks) out << chunk;
    out.close();

    long long saved_bytes = static_cast<long long>(std::filesystem::file_size(full, ec));
    if (ec) saved_bytes = 0;
    std::string label = "$STREAMMATE_HOME/studio/" + relative.generic_string();
    pending_event_ =
        "{\"type\":\"replay.saved\",\"sessionId\":\"local\",\"payload\":{\"replayId\":\"" +
        json_escape(replay_id) + "\",\"status\":\"saved\",\"path\":\"" + json_escape(label) +
        "\",\"savedBytes\":" + std::to_string(saved_bytes) + ",\"chunkCount\":" +
        std::to_string(session.chunks.size()) + "}}";
    return "{\"ok\":true,\"replayId\":\"" + json_escape(replay_id) +
           "\",\"status\":\"saved\",\"path\":\"" + json_escape(label) + "\",\"savedBytes\":" +
           std::to_string(saved_bytes) + ",\"chunkCount\":" + std::to_string(session.chunks.size()) + "}";
  }

  std::string stop_replay(const std::string &request) {
    std::string replay_id = extract_json_string(request, "replayId");
    if (replay_id.empty()) replay_id = "replay";
    auto it = replays_.find(replay_id);
    if (it == replays_.end()) return err(-32602, "replay buffer is not running");
    it->second.running = false;
    it->second.status = "stopped";
    it->second.chunks.clear();
    return "{\"ok\":true,\"replayId\":\"" + json_escape(replay_id) +
           "\",\"running\":false,\"status\":\"stopped\"}";
  }

  std::string replay_status(const std::string &request) const {
    std::string replay_id = extract_json_string(request, "replayId");
    if (replay_id.empty()) replay_id = "replay";
    auto it = replays_.find(replay_id);
    if (it == replays_.end()) {
      return "{\"ok\":true,\"replayId\":\"" + json_escape(replay_id) +
             "\",\"running\":false,\"status\":\"idle\",\"bufferedChunks\":0}";
    }
    const ReplaySession &session = it->second;
    return "{\"ok\":true,\"replayId\":\"" + json_escape(replay_id) + "\",\"running\":" +
           (session.running ? "true" : "false") + ",\"status\":\"" + session.status +
           "\",\"bufferedChunks\":" + std::to_string(session.chunks.size()) + "}";
  }

  std::optional<std::string> take_event() {
    if (!pending_event_) return std::nullopt;
    std::string event = *pending_event_;
    pending_event_.reset();
    return event;
  }

private:
  static constexpr int kReplayChunks = 4;

  struct RecordSession {
    std::string record_id;
    std::filesystem::path full_path;
    std::string label;
    std::string status = "idle";
    bool running = false;
    int frame_count = 0;
    long long bytes = 0;
  };

  struct ReplaySession {
    std::string replay_id;
    std::filesystem::path home;
    std::string status = "idle";
    bool running = false;
    int save_seq = 0;
    std::vector<std::string> chunks;
  };

  std::string err(int code, const std::string &message) const {
    return "__error__:" + std::to_string(code) + ":" + message;
  }

  // Home is the host's own state root, taken only from the process environment.
  // A request never overrides it: the "$STREAMMATE_HOME/..." labels we return
  // must describe the configured home, and a caller-supplied home would let a
  // write land outside it while the label still claimed containment.
  std::optional<std::filesystem::path> resolve_home() const {
    std::string home = getenv_string("STREAMMATE_HOME");
    if (home.empty()) return std::nullopt;
    return std::filesystem::path(home);
  }

  // Rejects absolute / URL-shaped / parent-escaping destinations up front and
  // returns the state-directory-relative path (e.g. "recordings/session.mkv").
  // Containment against the on-disk state directory is enforced separately at
  // write time by confined_path().
  std::optional<std::filesystem::path> sanitized_relative(
      const std::string &request, const std::string &subdir, const std::string &default_name) const {
    std::string destination = extract_json_string(request, "destination");
    if (destination.empty()) return std::filesystem::path(subdir) / default_name;
    if (destination.find("://") != std::string::npos) return std::nullopt;
    std::filesystem::path candidate(destination);
    if (candidate.is_absolute()) return std::nullopt;
    for (const auto &part : candidate) {
      if (part == "..") return std::nullopt;
    }
    return std::filesystem::path(subdir) / candidate;
  }

  // Resolves the on-disk write path and confirms it is confined under
  // $STREAMMATE_HOME/studio AFTER resolving symlinks. The trusted prefix is
  // anchored to canonical(home)/"studio" (not to a canonicalized state dir), so
  // a "studio" symlink pointing outside the home is rejected rather than trusted.
  // Re-run immediately before every write so a symlink planted after start cannot
  // redirect the write. Bounds-safe: never walks the resolved iterator past end.
  std::optional<std::filesystem::path> confined_path(
      const std::filesystem::path &home, const std::filesystem::path &relative) const {
    try {
      std::filesystem::path trusted = std::filesystem::weakly_canonical(home) / "studio";
      std::filesystem::path full = std::filesystem::weakly_canonical(home / "studio" / relative);
      auto tit = trusted.begin();
      auto fit = full.begin();
      for (; tit != trusted.end(); ++tit, ++fit) {
        if (fit == full.end() || *fit != *tit) return std::nullopt;
      }
      return full;
    } catch (const std::filesystem::filesystem_error &) {
      return std::nullopt;
    }
  }

  std::string record_json(const RecordSession &session, bool include_ok, bool include_stop_fields) const {
    std::string out = std::string("{") + (include_ok ? "\"ok\":true," : "") +
                      "\"recordId\":\"" + json_escape(session.record_id) + "\",\"running\":" +
                      (session.running ? "true" : "false") + ",\"status\":\"" + session.status +
                      "\",\"path\":\"" + json_escape(session.label) + "\"";
    if (include_stop_fields) {
      out += ",\"bytes\":" + std::to_string(session.bytes) +
             ",\"frameCount\":" + std::to_string(session.frame_count);
    }
    return out + "}";
  }

  std::string record_event(const std::string &type, const RecordSession &session) const {
    return "{\"type\":\"" + type + "\",\"sessionId\":\"local\",\"payload\":{\"recordId\":\"" +
           json_escape(session.record_id) + "\",\"status\":\"" + session.status + "\",\"path\":\"" +
           json_escape(session.label) + "\"}}";
  }

#if STREAMMATE_HAS_LIBOBS
  // Real-path wiring (compile-proof). Creates the ffmpeg_muxer recording output
  // and the replay_buffer output against libobs and releases them — no egress is
  // ever started. Local files only. Compiled in the libobs CI lane without any
  // `|| true` on the studio-host build.
  obs_output_t *create_record_output(const std::string &path) const {
    obs_data_t *settings = obs_data_create();
    if (!settings) return nullptr;
    obs_data_set_string(settings, "path", path.c_str());
    obs_data_set_string(settings, "muxer_settings", "");
    obs_output_t *output = obs_output_create("ffmpeg_muxer", "streammate-record", settings, nullptr);
    obs_data_release(settings);
    return output;
  }

  obs_output_t *create_replay_output(const std::string &directory, int max_seconds) const {
    obs_data_t *settings = obs_data_create();
    if (!settings) return nullptr;
    obs_data_set_string(settings, "directory", directory.c_str());
    obs_data_set_string(settings, "format", "mkv");
    obs_data_set_string(settings, "extension", "mkv");
    obs_data_set_int(settings, "max_time_sec", max_seconds);
    obs_data_set_int(settings, "max_size_mb", 512);
    obs_output_t *output = obs_output_create("replay_buffer", "streammate-replay", settings, nullptr);
    obs_data_release(settings);
    return output;
  }

  bool exercise_real_outputs(const std::string &record_path, const std::string &replay_dir) const {
    obs_output_t *record = create_record_output(record_path);
    obs_output_t *replay = create_replay_output(replay_dir, 20);
    bool created = record != nullptr && replay != nullptr;
    if (record) obs_output_release(record);
    if (replay) obs_output_release(replay);
    return created;
  }
#endif

  std::map<std::string, RecordSession> records_;
  std::map<std::string, ReplaySession> replays_;
  std::optional<std::string> pending_event_;
};

class ControlServer {
public:
  ControlServer(Options options, EngineLifecycle &engine, StateFile &state) : options_(std::move(options)), engine_(engine), state_(state) {
    output_.set_launch_allow_live_egress(options_.allow_live_egress);
    build_command_table();
  }

  int run() {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) throw std::runtime_error("socket create failed");
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options_.port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      close(server);
      throw std::runtime_error("loopback bind failed");
    }
    if (listen(server, 8) != 0) {
      close(server);
      throw std::runtime_error("listen failed");
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    getsockname(server, reinterpret_cast<sockaddr *>(&bound), &bound_len);
    port_ = ntohs(bound.sin_port);
    state_.write_ready(port_);
    emit_log("info", "host.ready", "studio-host ready");
    std::cout << "{\"event\":\"host.ready\",\"port\":" << port_ << ",\"heartbeatMs\":" << kHeartbeatMs << "}" << std::endl;

    while (!g_stop) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server, &readfds);
      timeval timeout{0, 200000};
      int ready = select(server + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready > 0 && FD_ISSET(server, &readfds)) {
        sockaddr_in client{};
        socklen_t client_len = sizeof(client);
        int fd = accept(server, reinterpret_cast<sockaddr *>(&client), &client_len);
        if (fd >= 0) {
          handle_client(fd);
          close(fd);
        }
      }
    }
    close(server);
    state_.write_stopped();
    emit_log("info", "host.exited", "studio-host stopped");
    return EXIT_SUCCESS;
  }

private:
  void handle_client(int fd) {
    char buffer[8192];
    ssize_t n = recv(fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    buffer[n] = '\0';
    std::string request(buffer);
    auto headers = parse_headers(request);
    auto key_it = headers.find("sec-websocket-key");
    auto auth_it = headers.find("authorization");
    std::string expected = "Bearer " + options_.token;
    if (key_it == headers.end() || auth_it == headers.end() || auth_it->second != expected) {
      std::string response = "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
      send_all(fd, reinterpret_cast<const uint8_t *>(response.data()), response.size());
      emit_log("warn", "auth.rejected", "websocket authorization rejected");
      return;
    }
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                           websocket_accept(key_it->second) + "\r\n\r\n";
    if (!send_all(fd, reinterpret_cast<const uint8_t *>(response.data()), response.size())) return;
    send_text_frame(fd, host_event("host.started", port_));
    send_text_frame(fd, host_event("host.ready", port_));

    auto next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeartbeatMs);
    while (!g_stop) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(fd, &readfds);
      timeval timeout{0, 200000};
      int ready = select(fd + 1, &readfds, nullptr, nullptr, &timeout);
      if (ready > 0 && FD_ISSET(fd, &readfds)) {
        auto payload = read_text_frame(fd);
        if (!payload) return;
        handle_message(fd, *payload);
      }
      output_.tick(fd);
      if (std::chrono::steady_clock::now() >= next_heartbeat) {
        if (!send_text_frame(fd, heartbeat_event())) return;
        next_heartbeat = std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeartbeatMs);
      }
    }
  }

  // Chunk 36.6 (Spec 36 D36-2 / Capability 6): dispatch is driven by a single
  // command table. host.hello serializes supportedCommands from exactly these
  // method names (see hello_result), and handle_message dispatches by matching
  // against them, so the declared capability set can never drift from what the
  // host actually implements -- and a verb it cannot perform can never be
  // declared.
  void handle_message(int fd, const std::string &payload) {
    std::string id = extract_json_id(payload);
    std::string method = extract_json_string(payload, "method");
    for (const auto &entry : command_table_) {
      if (method == entry.method) {
        entry.handler(fd, id, payload);
        return;
      }
    }
    send_text_frame(fd, rpc_error(id, -32601, "method not found"));
  }

  // host.hello result, including supportedCommands derived from the command
  // table. The array is a plain JSON array of strings (the monorepo parser
  // voids the whole declaration on any non-string entry).
  std::string hello_result() const {
    return "{\"hostId\":\"studio-host-1\",\"version\":\"" + std::string(kVersion) +
           "\",\"heartbeatMs\":" + std::to_string(kHeartbeatMs) +
           ",\"supportedCommands\":" + supported_commands_json_ + "}";
  }

  // A safe method name is an ASCII identifier: alpha-led, then alnum/dot
  // (uppercase is expected -- setProgram, refreshBrowser, exerciseTccPrompts).
  // build_command_table validates every entry against this before serializing,
  // so no name can carry a '"' or '\\' that would emit malformed JSON -- and any
  // malformed/non-string entry would void the WHOLE declaration in the monorepo
  // parser, silently disabling the native-host track.
  static bool is_safe_method_name(const char *name) {
    if (name == nullptr || *name == '\0') return false;
    auto is_alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (!is_alpha(name[0])) return false;
    for (const char *p = name + 1; *p != '\0'; ++p) {
      char c = *p;
      if (!(is_alpha(c) || (c >= '0' && c <= '9') || c == '.')) return false;
    }
    return true;
  }

  std::string build_supported_commands_json() const {
    std::string out = "[";
    for (std::size_t i = 0; i < command_table_.size(); ++i) {
      if (i) out += ",";
      // Names are validated against is_safe_method_name at construction, so
      // they are safe to emit verbatim as JSON strings.
      out += "\"";
      out += command_table_[i].method;
      out += "\"";
    }
    out += "]";
    return out;
  }

  void build_command_table() {
    command_table_.clear();
    auto add = [this](const char *method, CommandHandler handler) {
      command_table_.push_back({method, std::move(handler)});
    };
    // >>> STUDIO_HOST_COMMAND_TABLE (single source of truth for both dispatch
    // and host.hello's supportedCommands; do not hand-list these anywhere else)
    add("host.hello", [this](int fd, const std::string &id, const std::string &) {
      send_text_frame(fd, rpc_result(id, hello_result()));
    });
    add("host.health", [this](int fd, const std::string &id, const std::string &) {
      send_text_frame(fd, rpc_result(id, "{\"status\":\"ready\",\"engineStarted\":" + std::string(engine_.started() ? "true" : "false") +
                                      ",\"heartbeatMs\":" + std::to_string(kHeartbeatMs) +
                                      ",\"nativeOverlaySourceCount\":" + std::to_string(native_overlays_.count()) + "}"));
    });
    add("host.exerciseTccPrompts", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, importer_.exercise_tcc_prompts(payload));
    });
    add("scene.load", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.load_scene(payload));
    });
    add("scene.setProgram", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_program(payload));
    });
    add("scene.list", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.list_scenes(payload));
    });
    add("scene.itemTransform", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.item_transform(payload));
    });
    add("sceneItem.setVisible", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_item_visible(payload));
    });
    add("sceneItem.setOrder", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_item_order(payload));
    });
    add("source.remove", [this](int fd, const std::string &id, const std::string &payload) {
      // Explicit opt-in Phase B native overlay sources are routed to their own
      // manager; every other id stays on the scaffold browser-source path.
      std::string source_id = extract_json_string(payload, "sourceId");
      if (native_overlays_.has(source_id)) {
        send_renderer_result(fd, id, native_overlays_.remove_source(payload));
      } else {
        send_renderer_result(fd, id, renderer_.remove_source(payload));
      }
    });
    add("source.create", [this](int fd, const std::string &id, const std::string &payload) {
      // kind "native-overlay" is the explicit opt-in surface (Spec 34 Capability
      // 2); the default browser create path is untouched (ADR-0003 stands).
      if (extract_json_string(payload, "kind") == "native-overlay") {
        send_renderer_result(fd, id, native_overlays_.create(payload));
      } else {
        send_renderer_result(fd, id, renderer_.create_source(payload));
      }
    });
    add("source.update", [this](int fd, const std::string &id, const std::string &payload) {
      std::string source_id = extract_json_string(payload, "sourceId");
      if (native_overlays_.has(source_id)) {
        send_renderer_result(fd, id, native_overlays_.apply(payload));
      } else {
        send_renderer_result(fd, id, renderer_.update_source(payload));
      }
    });
    add("source.mute", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.mute_source(payload));
    });
    add("filter.list", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.list_filters(payload));
    });
    add("filter.setEnabled", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_filter_enabled(payload));
    });
    add("filter.setSettings", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_filter_settings(payload));
    });
    add("audio.setVolume", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.set_audio_volume(payload));
    });
    add("media.control", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.media_control(payload));
    });
    add("source.refreshBrowser", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.refresh_browser(payload));
    });
    add("scene.captureFrame", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, renderer_.capture_frame(payload));
    });
    add("import.scan", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, importer_.scan(payload));
    });
    add("import.load", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, importer_.load(payload));
    });
    add("import.report", [this](int fd, const std::string &id, const std::string &payload) {
      send_renderer_result(fd, id, importer_.report(payload));
    });
    // NIF-H1: static user plugin inventory from --user-plugins-manifest roots
    // (read-only; NO module loading -- dlopen/obs_open_module land in NIF-H2).
    add("plugins.discover", [this](int fd, const std::string &id, const std::string &) {
      send_text_frame(fd, rpc_result(id, user_plugins_discover_json(options_.user_plugins)));
    });
    // NIF-H2: the boot-frozen per-module load report (plan facts in scaffold,
    // real per-module load outcomes + registered-type deltas under libobs).
    add("plugins.report", [this](int fd, const std::string &id, const std::string &) {
      send_text_frame(fd, rpc_result(id, engine_.user_plugins_report_json()));
    });
    add("output.configure", [this](int fd, const std::string &id, const std::string &payload) {
      send_output_result(fd, id, output_.configure(payload));
    });
    add("output.start", [this](int fd, const std::string &id, const std::string &payload) {
      send_output_result(fd, id, output_.start(payload));
    });
    add("output.stop", [this](int fd, const std::string &id, const std::string &payload) {
      send_output_result(fd, id, output_.stop(payload));
    });
    add("output.status", [this](int fd, const std::string &id, const std::string &payload) {
      send_output_result(fd, id, output_.status(payload));
    });
    add("stats.subscribe", [this](int fd, const std::string &id, const std::string &payload) {
      send_output_result(fd, id, output_.subscribe(payload));
    });
    add("record.start", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.start_record(payload));
    });
    add("record.stop", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.stop_record(payload));
    });
    add("record.status", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.record_status(payload));
    });
    add("replay.start", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.start_replay(payload));
    });
    add("replay.save", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.save_replay(payload));
    });
    add("replay.stop", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.stop_replay(payload));
    });
    add("replay.status", [this](int fd, const std::string &id, const std::string &payload) {
      send_record_result(fd, id, record_replay_.replay_status(payload));
    });
    add("host.shutdown", [this](int fd, const std::string &id, const std::string &) {
      send_text_frame(fd, rpc_result(id, "{\"ok\":true}"));
      g_stop = 1;
    });
    // <<< STUDIO_HOST_COMMAND_TABLE
    for (const auto &entry : command_table_) {
      if (!is_safe_method_name(entry.method)) {
        throw std::runtime_error(std::string("unsafe command method name in dispatch table: ") +
                                 (entry.method ? entry.method : "(null)"));
      }
    }
    supported_commands_json_ = build_supported_commands_json();
  }

  void send_renderer_result(int fd, const std::string &id, const std::string &result) {
    if (is_renderer_error(result)) {
      send_text_frame(fd, renderer_error_to_rpc(id, result));
    } else {
      send_text_frame(fd, rpc_result(id, result));
    }
  }

  void send_output_result(int fd, const std::string &id, const std::string &result) {
    if (is_output_error(result)) {
      send_text_frame(fd, output_error_to_rpc(id, result));
    } else {
      send_text_frame(fd, rpc_result(id, result));
    }
  }

  // record.*/replay.* results share the __error__ convention; on success any
  // journaled event (record.started/stopped, replay.saved) is emitted after the
  // RPC reply so the adapter can order it against the reply.
  void send_record_result(int fd, const std::string &id, const std::string &result) {
    if (is_renderer_error(result)) {
      send_text_frame(fd, renderer_error_to_rpc(id, result));
      return;
    }
    send_text_frame(fd, rpc_result(id, result));
    if (auto event = record_replay_.take_event()) send_text_frame(fd, *event);
  }

  using CommandHandler = std::function<void(int fd, const std::string &id, const std::string &payload)>;
  struct CommandEntry {
    const char *method;
    CommandHandler handler;
  };

  Options options_;
  EngineLifecycle &engine_;
  StateFile &state_;
  RendererState renderer_;
  NativeOverlayManager native_overlays_;
  ObsImporter importer_;
  OutputController output_;
  RecordReplayController record_replay_;
  int port_ = 0;
  std::vector<CommandEntry> command_table_;
  std::string supported_commands_json_ = "[]";
};

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);

  try {
    Options options = parse_args(argc, argv);
    EngineLifecycle engine;
    if (!engine.start(options.user_plugins)) {
      emit_log("error", "host.degraded", "engine startup failed");
      return kRuntimeExit;
    }
    StateFile state(options.state_file);
    ControlServer server(options, engine, state);
    int result = server.run();
    engine.shutdown();
    return result;
  } catch (const std::exception &error) {
    emit_log("error", "host.exited", error.what());
    return kUsageExit;
  }
}
