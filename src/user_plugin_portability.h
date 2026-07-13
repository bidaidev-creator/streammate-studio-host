// Spec 39 / NIF-H2 (opus review F1/F2) — wire-portability predicates for
// user-plugin identifiers, mirroring the MONO validator's pinned patterns
// exactly (packages/protocol/src/index.ts). The host's looser
// is_safe_protocol_id (any of [A-Za-z0-9._-], length <= 255) admits values
// the mono validator refuses; anything emitted into StudioPluginModuleRecord
// fields must pass THESE checks or the whole inventory fail-closes
// downstream.

#pragma once

#include <cctype>
#include <string>

namespace streammate::user_plugins {

inline bool portable_charset(const std::string &value) {
  for (unsigned char c : value) {
    if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

// Mirrors STUDIO_PLUGIN_TYPE_ID_PATTERN: ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$
// (alnum-led, <= 64 chars total).
inline bool is_portable_type_id(const std::string &value) {
  if (value.empty() || value.size() > 64) return false;
  if (!std::isalnum(static_cast<unsigned char>(value[0]))) return false;
  return portable_charset(value);
}

// A module NAME must survive every mono field it feeds:
//   moduleRef "module:<name>"  -> ^module:[A-Za-z0-9][A-Za-z0-9._-]{0,127}$
//   label     "<name>"         -> sanitized string <= 120
//   fileName  "<name>.plugin"  -> sanitized string <= 160
// Binding constraints: alnum-led, portable charset, length <= 120.
inline bool is_portable_module_name(const std::string &value) {
  if (value.empty() || value.size() > 120) return false;
  if (!std::isalnum(static_cast<unsigned char>(value[0]))) return false;
  return portable_charset(value);
}

} // namespace streammate::user_plugins
