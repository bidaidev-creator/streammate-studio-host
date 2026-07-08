#include "native_overlay_renderer.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace streammate::overlay {
namespace {

constexpr int kBytesPerPixel = 4;
constexpr size_t kSurfaceBytes =
    static_cast<size_t>(kOverlayWidth) * static_cast<size_t>(kOverlayHeight) * kBytesPerPixel;

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

bool is_ws(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

size_t skip_ws(std::string_view s, size_t pos) {
  while (pos < s.size() && is_ws(s[pos])) ++pos;
  return pos;
}

std::optional<size_t> value_start_for_key(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  size_t pos = 0;
  while ((pos = json.find(needle, pos)) != std::string_view::npos) {
    size_t p = skip_ws(json, pos + needle.size());
    if (p < json.size() && json[p] == ':') return skip_ws(json, p + 1);
    pos += needle.size();
  }
  return std::nullopt;
}

std::optional<std::string> parse_json_string(std::string_view json, size_t pos) {
  if (pos >= json.size() || json[pos] != '"') return std::nullopt;
  ++pos;
  std::string out;
  while (pos < json.size()) {
    char c = json[pos++];
    if (c == '"') return out;
    if (c == '\\') {
      if (pos >= json.size()) return std::nullopt;
      char e = json[pos++];
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out.push_back(e);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      default:
        out.push_back(e);
        break;
      }
    } else {
      out.push_back(c);
    }
  }
  return std::nullopt;
}

std::optional<std::string> json_string(std::string_view json, std::string_view key) {
  auto pos = value_start_for_key(json, key);
  if (!pos) return std::nullopt;
  return parse_json_string(json, *pos);
}

std::optional<size_t> top_level_value_start_for_key(std::string_view json, std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  int object_depth = 0;
  int array_depth = 0;

  for (size_t i = 0; i < json.size();) {
    const char c = json[i];
    if (c == '"') {
      if (object_depth == 1 && array_depth == 0 && json.substr(i, needle.size()) == needle) {
        const size_t p = skip_ws(json, i + needle.size());
        if (p < json.size() && json[p] == ':') return skip_ws(json, p + 1);
      }
      ++i;
      bool escape = false;
      while (i < json.size()) {
        const char s = json[i++];
        if (escape) {
          escape = false;
        } else if (s == '\\') {
          escape = true;
        } else if (s == '"') {
          break;
        }
      }
      continue;
    }
    if (c == '{') {
      ++object_depth;
    } else if (c == '}') {
      --object_depth;
    } else if (c == '[') {
      ++array_depth;
    } else if (c == ']') {
      --array_depth;
    }
    ++i;
  }
  return std::nullopt;
}

std::optional<std::string> json_top_level_string(std::string_view json, std::string_view key) {
  auto pos = top_level_value_start_for_key(json, key);
  if (!pos) return std::nullopt;
  return parse_json_string(json, *pos);
}

std::optional<double> json_number(std::string_view json, std::string_view key) {
  auto pos = value_start_for_key(json, key);
  if (!pos) return std::nullopt;
  size_t end = *pos;
  while (end < json.size()) {
    const char c = json[end];
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.' ||
          c == 'e' || c == 'E')) {
      break;
    }
    ++end;
  }
  if (end == *pos) return std::nullopt;
  const std::string tmp(json.substr(*pos, end - *pos));
  char *parsed_end = nullptr;
  const double value = std::strtod(tmp.c_str(), &parsed_end);
  if (parsed_end == tmp.c_str()) return std::nullopt;
  return value;
}

std::optional<bool> json_bool(std::string_view json, std::string_view key) {
  auto pos = value_start_for_key(json, key);
  if (!pos) return std::nullopt;
  if (json.substr(*pos, 4) == "true") return true;
  if (json.substr(*pos, 5) == "false") return false;
  return std::nullopt;
}

std::optional<std::string_view> json_array(std::string_view json, std::string_view key) {
  auto pos = value_start_for_key(json, key);
  if (!pos || *pos >= json.size() || json[*pos] != '[') return std::nullopt;

  int depth = 0;
  bool in_string = false;
  bool escape = false;
  for (size_t i = *pos; i < json.size(); ++i) {
    const char c = json[i];
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
    } else if (c == '[') {
      ++depth;
    } else if (c == ']') {
      --depth;
      if (depth == 0) return json.substr(*pos + 1, i - *pos - 1);
    }
  }
  return std::nullopt;
}

std::vector<std::string_view> objects_in_array(std::string_view array) {
  std::vector<std::string_view> out;
  int depth = 0;
  bool in_string = false;
  bool escape = false;
  size_t object_start = std::string_view::npos;

  for (size_t i = 0; i < array.size(); ++i) {
    const char c = array[i];
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
      if (depth == 0) object_start = i;
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0 && object_start != std::string_view::npos) {
        out.push_back(array.substr(object_start, i - object_start + 1));
        object_start = std::string_view::npos;
      }
    }
  }
  return out;
}

std::vector<std::string> label_values_from_array(std::string_view json, std::string_view key) {
  std::vector<std::string> labels;
  auto arr = json_array(json, key);
  if (!arr) return labels;
  for (std::string_view obj : objects_in_array(*arr)) {
    if (auto label = json_string(obj, "label")) labels.push_back(*label);
  }
  return labels;
}

struct BreakdownEntry {
  std::string label;
  double percentage = 0.0;
};

std::vector<BreakdownEntry> breakdown_values(std::string_view json) {
  std::vector<BreakdownEntry> entries;
  auto arr = json_array(json, "breakdown");
  if (!arr) return entries;
  for (std::string_view obj : objects_in_array(*arr)) {
    BreakdownEntry entry;
    if (auto label = json_string(obj, "label")) entry.label = *label;
    if (auto percentage = json_number(obj, "percentage")) entry.percentage = *percentage;
    entries.push_back(entry);
  }
  return entries;
}

void clear_surface(std::vector<uint8_t> &rgba) {
  std::fill(rgba.begin(), rgba.end(), 0);
}

void fill_rect(std::vector<uint8_t> &rgba, int x, int y, int w, int h, uint8_t r, uint8_t g,
               uint8_t b) {
  if (w <= 0 || h <= 0) return;
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(kOverlayWidth, x + w);
  const int y1 = std::min(kOverlayHeight, y + h);
  if (x0 >= x1 || y0 >= y1) return;

  for (int py = y0; py < y1; ++py) {
    size_t off = (static_cast<size_t>(py) * kOverlayWidth + x0) * kBytesPerPixel;
    for (int px = x0; px < x1; ++px) {
      rgba[off + 0] = r;
      rgba[off + 1] = g;
      rgba[off + 2] = b;
      rgba[off + 3] = 255;
      off += kBytesPerPixel;
    }
  }
}

std::array<uint8_t, 7> glyph(char raw) {
  const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
  switch (c) {
  case 'A':
    return {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
  case 'B':
    return {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};
  case 'C':
    return {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111};
  case 'D':
    return {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
  case 'E':
    return {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
  case 'F':
    return {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000};
  case 'G':
    return {0b01111, 0b10000, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111};
  case 'H':
    return {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
  case 'I':
    return {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b11111};
  case 'J':
    return {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100};
  case 'K':
    return {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001};
  case 'L':
    return {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
  case 'M':
    return {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001};
  case 'N':
    return {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001};
  case 'O':
    return {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
  case 'P':
    return {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000};
  case 'Q':
    return {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101};
  case 'R':
    return {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001};
  case 'S':
    return {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
  case 'T':
    return {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
  case 'U':
    return {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110};
  case 'V':
    return {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100};
  case 'W':
    return {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010};
  case 'X':
    return {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001};
  case 'Y':
    return {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100};
  case 'Z':
    return {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111};
  case '0':
    return {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110};
  case '1':
    return {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110};
  case '2':
    return {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111};
  case '3':
    return {0b11110, 0b00001, 0b00001, 0b00110, 0b00001, 0b00001, 0b11110};
  case '4':
    return {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010};
  case '5':
    return {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110};
  case '6':
    return {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110};
  case '7':
    return {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000};
  case '8':
    return {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110};
  case '9':
    return {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110};
  case '.':
    return {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b01100};
  case ',':
    return {0b00000, 0b00000, 0b00000, 0b00000, 0b01100, 0b00100, 0b01000};
  case ':':
    return {0b00000, 0b01100, 0b01100, 0b00000, 0b01100, 0b01100, 0b00000};
  case '%':
    return {0b11001, 0b11010, 0b00100, 0b01000, 0b10000, 0b01011, 0b10011};
  case '!':
    return {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100};
  case '?':
    return {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b00000, 0b00100};
  case '-':
    return {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000};
  case '\'':
    return {0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000};
  case ' ':
  default:
    return {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000};
  }
}

void draw_text(std::vector<uint8_t> &rgba, int x, int y, int scale, std::string_view text,
               uint8_t r, uint8_t g, uint8_t b) {
  if (scale <= 0) return;
  int pen_x = x;
  int pen_y = y;
  for (char c : text) {
    if (c == '\n') {
      pen_x = x;
      pen_y += 8 * scale;
      continue;
    }
    const auto bits = glyph(c);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if ((bits[row] & (1u << (4 - col))) != 0) {
          fill_rect(rgba, pen_x + col * scale, pen_y + row * scale, scale, scale, r, g, b);
        }
      }
    }
    pen_x += 6 * scale;
  }
}

void draw_toast(std::vector<uint8_t> &rgba, std::string_view json) {
  uint8_t r = 43, g = 96, b = 145;
  if (auto tone = json_string(json, "tone")) {
    if (*tone == "success") {
      r = 34;
      g = 150;
      b = 93;
    } else if (*tone == "warning") {
      r = 210;
      g = 139;
      b = 28;
    }
  }

  fill_rect(rgba, 818, 46, 394, 114, 16, 22, 30);
  fill_rect(rgba, 826, 54, 378, 98, r, g, b);
  fill_rect(rgba, 826, 54, 10, 98, 245, 248, 250);
  const std::string message = json_string(json, "message").value_or("");
  draw_text(rgba, 858, 86, 4, message, 255, 255, 255);
}

void draw_lower_third(std::vector<uint8_t> &rgba, std::string_view json) {
  fill_rect(rgba, 0, 480, 1280, 180, 19, 34, 49);
  fill_rect(rgba, 0, 480, 1280, 8, 77, 179, 204);
  fill_rect(rgba, 68, 514, 12, 108, 238, 188, 64);
  draw_text(rgba, 104, 514, 6, json_string(json, "title").value_or(""), 255, 255, 255);
  draw_text(rgba, 108, 588, 3, json_string(json, "subtitle").value_or(""), 207, 222, 229);
}

void draw_vote_callout(std::vector<uint8_t> &rgba, std::string_view json) {
  fill_rect(rgba, 48, 88, 444, 544, 12, 45, 58);
  fill_rect(rgba, 64, 104, 412, 72, 24, 117, 131);
  draw_text(rgba, 86, 126, 3, json_string(json, "question").value_or(""), 255, 255, 255);

  const auto labels = label_values_from_array(json, "options");
  int y = 216;
  int index = 1;
  for (const auto &label : labels) {
    fill_rect(rgba, 82, y, 360, 52, 245, 248, 250);
    fill_rect(rgba, 96, y + 11, 30, 30, 24, 117, 131);
    draw_text(rgba, 106, y + 17, 2, std::to_string(index), 255, 255, 255);
    draw_text(rgba, 148, y + 16, 3, label, 12, 45, 58);
    y += 72;
    ++index;
  }
}

void draw_vote_result(std::vector<uint8_t> &rgba, std::string_view json) {
  fill_rect(rgba, 250, 108, 780, 504, 31, 31, 36);
  fill_rect(rgba, 270, 128, 740, 74, 66, 82, 105);
  draw_text(rgba, 294, 151, 3, json_string(json, "question").value_or(""), 255, 255, 255);
  fill_rect(rgba, 294, 226, 692, 60, 36, 137, 93);
  draw_text(rgba, 318, 244, 3, "WINNER: " + json_string(json, "winnerLabel").value_or(""), 255, 255,
            255);

  const auto entries = breakdown_values(json);
  int y = 330;
  for (const auto &entry : entries) {
    const double pct = std::clamp(entry.percentage, 0.0, 100.0);
    const int bar_w = static_cast<int>(pct * 5.4);
    fill_rect(rgba, 318, y, 560, 34, 61, 65, 75);
    fill_rect(rgba, 318, y, bar_w, 34, 238, 188, 64);
    draw_text(rgba, 318, y - 28, 2, entry.label, 255, 255, 255);
    draw_text(rgba, 892, y + 8, 2, std::to_string(static_cast<int>(pct)) + "%", 255, 255, 255);
    y += 76;
  }
}

void draw_celebration_burst(std::vector<uint8_t> &rgba, std::string_view json) {
  int rows = 5;
  int cols = 14;
  if (auto intensity = json_string(json, "intensity")) {
    if (*intensity == "medium") {
      rows = 8;
      cols = 18;
    } else if (*intensity == "high") {
      rows = 12;
      cols = 24;
    }
  }
  constexpr std::array<std::array<uint8_t, 3>, 5> colors = {{
      {238, 76, 89},
      {238, 188, 64},
      {77, 179, 204},
      {34, 150, 93},
      {245, 248, 250},
  }};
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < cols; ++col) {
      const auto color = colors[static_cast<size_t>((row * 7 + col * 3) % colors.size())];
      const int x = 78 + col * 49 + ((row % 2) * 18);
      const int y = 72 + row * 43;
      const int size = 12 + ((row + col) % 3) * 4;
      fill_rect(rgba, x, y, size, size, color[0], color[1], color[2]);
    }
  }
  fill_rect(rgba, 392, 310, 496, 104, 31, 31, 36);
  fill_rect(rgba, 404, 322, 472, 80, 238, 76, 89);
  draw_text(rgba, 444, 346, 5, json_string(json, "label").value_or(""), 255, 255, 255);
}

void draw_generated_image(std::vector<uint8_t> &rgba, std::string_view json) {
  fill_rect(rgba, 352, 82, 576, 452, 24, 32, 40);
  fill_rect(rgba, 368, 98, 544, 380, 245, 248, 250);
  for (int y = 0; y < 380; y += 32) {
    for (int x = 0; x < 544; x += 32) {
      const bool dark = ((x / 32) + (y / 32)) % 2 == 0;
      fill_rect(rgba, 368 + x, 98 + y, 32, 32, dark ? 82 : 185, dark ? 102 : 202,
                dark ? 122 : 215);
    }
  }
  fill_rect(rgba, 388, 118, 504, 340, 0, 0, 0);
  for (int y = 126; y < 450; y += 32) {
    for (int x = 396; x < 884; x += 32) {
      const bool light = ((x + y) / 32) % 2 == 0;
      fill_rect(rgba, x, y, 24, 24, light ? 77 : 238, light ? 179 : 188, light ? 204 : 64);
    }
  }
  std::string caption = json_string(json, "caption").value_or("");
  if (caption.empty()) caption = json_string(json, "alt").value_or("");
  fill_rect(rgba, 352, 548, 576, 64, 24, 32, 40);
  draw_text(rgba, 380, 569, 3, caption, 255, 255, 255);
}

void draw_sound_cue(std::vector<uint8_t> &rgba, std::string_view json) {
  const double volume = json_number(json, "volume").value_or(100.0);
  const bool muted = volume <= 0.0;
  fill_rect(rgba, 458, 252, 364, 184, 25, 29, 35);
  fill_rect(rgba, 482, 276, 316, 136, muted ? 77 : 34, muted ? 77 : 150, muted ? 82 : 93);
  fill_rect(rgba, 520, 324, 38, 42, 255, 255, 255);
  fill_rect(rgba, 558, 306, 46, 78, 255, 255, 255);
  if (muted) {
    fill_rect(rgba, 632, 306, 18, 88, 238, 76, 89);
    fill_rect(rgba, 602, 340, 78, 18, 238, 76, 89);
  } else {
    fill_rect(rgba, 632, 324, 14, 42, 255, 255, 255);
    fill_rect(rgba, 666, 312, 14, 66, 255, 255, 255);
    fill_rect(rgba, 700, 300, 14, 90, 255, 255, 255);
  }
  draw_text(rgba, 520, 394, 2, json_string(json, "label").value_or(""), 255, 255, 255);
}

} // namespace

const std::vector<std::string> &native_overlay_categories() {
  static const std::vector<std::string> kCategories = {
      "toast",       "lower-third",     "vote-callout", "vote-result",
      "celebration-burst", "generated-image", "sound-cue",    "clear"};
  return kCategories;
}

const std::vector<std::string> &native_overlay_budget_keys(const std::string &category) {
  static const std::vector<std::string> kToast = {"enterMs", "exitMs"};
  static const std::vector<std::string> kLowerThird = {"enterMs", "exitMs"};
  static const std::vector<std::string> kVoteCallout = {"enterMs", "exitMs"};
  static const std::vector<std::string> kVoteResult = {"enterMs", "exitMs"};
  static const std::vector<std::string> kCelebrationBurst = {"enterMs", "sustainedFrameSkipPercent",
                                                            "exitMs"};
  static const std::vector<std::string> kGeneratedImage = {"preloadMs", "enterMs", "exitMs"};
  static const std::vector<std::string> kSoundCue = {"latencyMs"};
  static const std::vector<std::string> kClear = {"appliesMs"};
  static const std::vector<std::string> kNone;

  if (category == "toast") return kToast;
  if (category == "lower-third") return kLowerThird;
  if (category == "vote-callout") return kVoteCallout;
  if (category == "vote-result") return kVoteResult;
  if (category == "celebration-burst") return kCelebrationBurst;
  if (category == "generated-image") return kGeneratedImage;
  if (category == "sound-cue") return kSoundCue;
  if (category == "clear") return kClear;
  return kNone;
}

uint64_t fnv1a64(const uint8_t *data, size_t len) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

struct NativeOverlayRenderer::Impl {
  Impl()
      : rgba(kSurfaceBytes, 0),
        color_space(CGColorSpaceCreateDeviceRGB()),
        context(CGBitmapContextCreate(rgba.data(), kOverlayWidth, kOverlayHeight, 8,
                                      kOverlayWidth * kBytesPerPixel, color_space,
                                      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
                                          kCGBitmapByteOrder32Big)) {}

  ~Impl() {
    if (context) CGContextRelease(context);
    if (color_space) CGColorSpaceRelease(color_space);
  }

  std::vector<uint8_t> rgba;
  CGColorSpaceRef color_space = nullptr;
  CGContextRef context = nullptr;
};

NativeOverlayRenderer::NativeOverlayRenderer() : impl_(std::make_unique<Impl>()) {}
NativeOverlayRenderer::~NativeOverlayRenderer() = default;

OverlayRasterResult NativeOverlayRenderer::apply(const std::string &action_json) {
  OverlayRasterResult result;
  const auto type = json_top_level_string(action_json, "type");
  if (!type || native_overlay_budget_keys(*type).empty()) {
    result.ok = false;
    result.error = "unsupported overlay action type";
    return result;
  }

  result.ok = true;
  result.category = *type;

  if (*type == "generated-image") {
    const auto preload_start = Clock::now();
    const auto preload_end = Clock::now();
    result.timing["preloadMs"] = elapsed_ms(preload_start, preload_end);
  }

  const auto start = Clock::now();

  if (*type == "clear") {
    if (!json_bool(action_json, "freeze").value_or(false)) clear_surface(impl_->rgba);
  } else {
    clear_surface(impl_->rgba);
    if (*type == "toast") {
      draw_toast(impl_->rgba, action_json);
    } else if (*type == "lower-third") {
      draw_lower_third(impl_->rgba, action_json);
    } else if (*type == "vote-callout") {
      draw_vote_callout(impl_->rgba, action_json);
    } else if (*type == "vote-result") {
      draw_vote_result(impl_->rgba, action_json);
    } else if (*type == "celebration-burst") {
      draw_celebration_burst(impl_->rgba, action_json);
    } else if (*type == "generated-image") {
      draw_generated_image(impl_->rgba, action_json);
    } else if (*type == "sound-cue") {
      draw_sound_cue(impl_->rgba, action_json);
      const std::string cue_id = json_string(action_json, "cueId").value_or("unknown");
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          Clock::now().time_since_epoch())
                          .count();
      result.trigger_record = "sound-cue:" + cue_id + "@" + std::to_string(ns);
    }
  }

  const auto end = Clock::now();
  const double ms = elapsed_ms(start, end);

  if (*type == "toast" || *type == "lower-third" || *type == "vote-callout" ||
      *type == "vote-result") {
    result.timing["enterMs"] = ms;
    result.timing["exitMs"] = ms;
  } else if (*type == "celebration-burst") {
    result.timing["enterMs"] = ms;
    result.timing["sustainedFrameSkipPercent"] = 0.0;
    result.timing["exitMs"] = ms;
  } else if (*type == "generated-image") {
    result.timing["enterMs"] = ms;
    result.timing["exitMs"] = ms;
  } else if (*type == "sound-cue") {
    result.timing["latencyMs"] = ms;
  } else if (*type == "clear") {
    result.timing["appliesMs"] = ms;
  }

  result.empty = is_empty();
  result.raster_hash = hash();
  return result;
}

int NativeOverlayRenderer::width() const { return kOverlayWidth; }

int NativeOverlayRenderer::height() const { return kOverlayHeight; }

const std::vector<uint8_t> &NativeOverlayRenderer::rgba() const { return impl_->rgba; }

std::vector<uint8_t> NativeOverlayRenderer::bgra() const {
  std::vector<uint8_t> out = impl_->rgba;
  for (size_t i = 0; i + 3 < out.size(); i += kBytesPerPixel) {
    std::swap(out[i + 0], out[i + 2]);
  }
  return out;
}

bool NativeOverlayRenderer::is_empty() const {
  return std::all_of(impl_->rgba.begin(), impl_->rgba.end(), [](uint8_t b) { return b == 0; });
}

uint64_t NativeOverlayRenderer::hash() const {
  return fnv1a64(impl_->rgba.data(), impl_->rgba.size());
}

} // namespace streammate::overlay
