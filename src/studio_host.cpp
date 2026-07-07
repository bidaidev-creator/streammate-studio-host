
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
#include <fcntl.h>
#include <filesystem>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <regex>
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

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <mach-o/dyld.h>
#endif

#if STREAMMATE_HAS_LIBOBS
#include <obs.h>
#endif

namespace {
constexpr const char *kVersion = STREAMMATE_STUDIO_HOST_VERSION;
constexpr int kUsageExit = 64;
constexpr int kRuntimeExit = 70;
constexpr int kHeartbeatMs = 5000;
constexpr std::uintmax_t kMaxSceneCollectionBytes = 8 * 1024 * 1024;
constexpr const char *kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

struct Options {
  std::string host = "127.0.0.1";
  int port = 0;
  std::string token;
  std::string state_file;
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
        out << "\\u" << std::hex << std::uppercase << static_cast<int>(c);
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

class EngineLifecycle {
public:
  bool start() {
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
    obs_post_load_modules();
#endif
    started_ = true;
    return true;
  }

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

  static void add_bundle_module_path() {
    auto executable = executable_path();
    if (!executable) {
      return;
    }
    std::filesystem::path contents = executable->parent_path().parent_path();
    std::filesystem::path plugins = contents / "PlugIns" / "obs-plugins";
    if (!std::filesystem::is_directory(plugins)) {
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
    return false;
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

std::string extract_json_id(const std::string &json) {
  auto marker = json.find("\"id\"");
  if (marker == std::string::npos) return "null";
  auto colon = json.find(':', marker);
  if (colon == std::string::npos) return "null";
  auto start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (start == std::string::npos) return "null";
  if (json[start] == '"') {
    auto end = json.find('"', start + 1);
    if (end == std::string::npos) return "null";
    return json.substr(start, end - start + 1);
  }
  auto end = json.find_first_of(",}\r\n", start);
  return json.substr(start, end == std::string::npos ? std::string::npos : end - start);
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
};

class RendererState {
public:
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
    return "{\"ok\":true,\"sceneId\":\"" + json_escape(scene_id) + "\",\"width\":" + std::to_string(width) +
           ",\"height\":" + std::to_string(height) + ",\"renderer\":\"offscreen-scaffold\"}";
  }

  std::string set_program(const std::string &request) {
    std::string scene_id = extract_json_string(request, "sceneId");
    if (scene_id.empty() || scenes_.find(scene_id) == scenes_.end()) return rpc_error_result(-32602, "scene not loaded");
    program_scene_id_ = scene_id;
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
    sources_[source_id] = source;
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
    it->second.muted = extract_json_bool(request, "muted").value_or(true);
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source_id) + "\",\"muted\":" +
           std::string(it->second.muted ? "true" : "false") + "}";
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

private:
  std::string rpc_error_result(int code, const std::string &message) const {
    return "__error__:" + std::to_string(code) + ":" + message;
  }

  std::string source_result(const SourceModel &source, bool url_touched) const {
    return "{\"ok\":true,\"sourceId\":\"" + json_escape(source.id) + "\",\"sceneId\":\"" + json_escape(source.scene_id) +
           "\",\"kind\":\"" + json_escape(source.kind) + "\",\"muted\":" + std::string(source.muted ? "true" : "false") +
           ",\"opacity\":" + std::to_string(source.opacity) + ",\"urlStatus\":\"" +
           std::string(url_touched || !source.url.empty() ? "stored-redacted" : "empty") + "\"}";
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
          if (source.scene_id != scene.id || source.muted) continue;
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
  std::string program_scene_id_;
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
      default:
        value.push_back(escaped);
        break;
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
  std::string configure(const std::string &request) {
    std::string output_id = extract_json_string(request, "outputId");
    if (output_id.empty()) return rpc_error_result(-32602, "outputId is required");
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
    if (allow_live_egress_) {
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

class ControlServer {
public:
  ControlServer(Options options, EngineLifecycle &engine, StateFile &state) : options_(std::move(options)), engine_(engine), state_(state) {}

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

  void handle_message(int fd, const std::string &payload) {
    std::string id = extract_json_id(payload);
    std::string method = extract_json_string(payload, "method");
    if (method == "host.hello") {
      send_text_frame(fd, rpc_result(id, "{\"hostId\":\"studio-host-1\",\"version\":\"" + std::string(kVersion) +
                                      "\",\"heartbeatMs\":" + std::to_string(kHeartbeatMs) + "}"));
    } else if (method == "host.health") {
      send_text_frame(fd, rpc_result(id, "{\"status\":\"ready\",\"engineStarted\":" + std::string(engine_.started() ? "true" : "false") +
                                      ",\"heartbeatMs\":" + std::to_string(kHeartbeatMs) + "}"));
    } else if (method == "host.exerciseTccPrompts") {
      send_renderer_result(fd, id, importer_.exercise_tcc_prompts(payload));
    } else if (method == "scene.load") {
      send_renderer_result(fd, id, renderer_.load_scene(payload));
    } else if (method == "scene.setProgram") {
      send_renderer_result(fd, id, renderer_.set_program(payload));
    } else if (method == "source.create") {
      send_renderer_result(fd, id, renderer_.create_source(payload));
    } else if (method == "source.update") {
      send_renderer_result(fd, id, renderer_.update_source(payload));
    } else if (method == "source.mute") {
      send_renderer_result(fd, id, renderer_.mute_source(payload));
    } else if (method == "scene.captureFrame") {
      send_renderer_result(fd, id, renderer_.capture_frame(payload));
    } else if (method == "import.scan") {
      send_renderer_result(fd, id, importer_.scan(payload));
    } else if (method == "import.load") {
      send_renderer_result(fd, id, importer_.load(payload));
    } else if (method == "import.report") {
      send_renderer_result(fd, id, importer_.report(payload));
    } else if (method == "output.configure") {
      send_output_result(fd, id, output_.configure(payload));
    } else if (method == "output.start") {
      send_output_result(fd, id, output_.start(payload));
    } else if (method == "output.stop") {
      send_output_result(fd, id, output_.stop(payload));
    } else if (method == "output.status") {
      send_output_result(fd, id, output_.status(payload));
    } else if (method == "stats.subscribe") {
      send_output_result(fd, id, output_.subscribe(payload));
    } else if (method == "host.shutdown") {
      send_text_frame(fd, rpc_result(id, "{\"ok\":true}"));
      g_stop = 1;
    } else {
      send_text_frame(fd, rpc_error(id, -32601, "method not found"));
    }
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

  Options options_;
  EngineLifecycle &engine_;
  StateFile &state_;
  RendererState renderer_;
  ObsImporter importer_;
  OutputController output_;
  int port_ = 0;
};

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);

  try {
    Options options = parse_args(argc, argv);
    EngineLifecycle engine;
    if (!engine.start()) {
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
