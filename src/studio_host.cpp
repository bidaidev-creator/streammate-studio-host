
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if STREAMMATE_HAS_LIBOBS
#include <obs.h>
#endif

namespace {
constexpr const char *kVersion = STREAMMATE_STUDIO_HOST_VERSION;
constexpr int kUsageExit = 64;
constexpr int kRuntimeExit = 70;
constexpr int kHeartbeatMs = 5000;
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
    } else if (method == "host.shutdown") {
      send_text_frame(fd, rpc_result(id, "{\"ok\":true}"));
      g_stop = 1;
    } else {
      send_text_frame(fd, rpc_error(id, -32601, "method not found"));
    }
  }

  Options options_;
  EngineLifecycle &engine_;
  StateFile &state_;
  int port_ = 0;
};

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGINT, handle_signal);

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
