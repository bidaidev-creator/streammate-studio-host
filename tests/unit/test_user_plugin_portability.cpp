// NIF-H2 (opus review F1/F2): the portability predicates must mirror the mono
// validator's pinned patterns exactly — alnum-led, [A-Za-z0-9._-] charset,
// type ids <= 64 chars, module names <= 120 chars.

#include "user_plugin_portability.h"

#include <cstdio>
#include <string>

using streammate::user_plugins::is_portable_module_name;
using streammate::user_plugins::is_portable_type_id;

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  // Type ids: mirrors STUDIO_PLUGIN_TYPE_ID_PATTERN.
  expect(is_portable_type_id("streammate_test_source"), "plain type id accepted");
  expect(is_portable_type_id("9abc"), "digit-led type id accepted");
  expect(is_portable_type_id("a.b-c_d"), "charset type id accepted");
  expect(is_portable_type_id(std::string(64, 'a')), "64-char type id accepted");
  expect(!is_portable_type_id(std::string(65, 'a')), "65-char type id refused");
  expect(!is_portable_type_id("_leading_underscore"), "underscore-led type id refused");
  expect(!is_portable_type_id(".leading_dot"), "dot-led type id refused");
  expect(!is_portable_type_id("-leading-dash"), "dash-led type id refused");
  expect(!is_portable_type_id(""), "empty type id refused");
  expect(!is_portable_type_id("has space"), "space type id refused");
  expect(!is_portable_type_id(std::string("uni") + "\xc3" + "\xa9" + "code"),
         "non-ascii type id refused");

  // Module names: binding constraint across moduleRef/label/fileName.
  expect(is_portable_module_name("alpha"), "plain module name accepted");
  expect(is_portable_module_name(std::string(120, 'a')), "120-char module name accepted");
  expect(!is_portable_module_name(std::string(121, 'a')), "121-char module name refused");
  expect(!is_portable_module_name("_hidden"), "underscore-led module name refused");
  expect(!is_portable_module_name(".hidden"), "dot-led module name refused");
  expect(!is_portable_module_name(""), "empty module name refused");

  if (failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::printf("user-plugin-portability ok\n");
  return 0;
}
