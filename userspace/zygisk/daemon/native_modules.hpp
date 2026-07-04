/* SPDX-License-Identifier: GPL-3.0 */
/*
 * Shared ZN native module manifest helpers.
 */
#pragma once

#include "uapi/yukizygisk.h"

#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>

namespace yukizygisk::native {

struct NativeModule {
  std::string module_id;
  uint8_t target_type = 0;
  std::string target;
  std::string lib_path;
  bool has_companion = false;
};

inline std::string trim_copy(const std::string &s) {
  size_t first = 0;
  while (first < s.size() &&
         std::isspace(static_cast<unsigned char>(s[first])) != 0)
    ++first;
  size_t last = s.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(s[last - 1])) != 0)
    --last;
  return s.substr(first, last - first);
}

inline void replace_all(std::string &s, const std::string &from,
                        const std::string &to) {
  if (from.empty())
    return;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

inline bool parse_native_module_line(const std::string &module_id,
                                     const std::string &base,
                                     const std::string &line,
                                     NativeModule *out) {
  std::string text = trim_copy(line);
  if (text.empty() || text[0] == '#')
    return false;

  std::istringstream iss(text);
  std::string head;
  if (!(iss >> head))
    return false;

  NativeModule m{};
  m.module_id = module_id;
  constexpr char kNamePrefix[] = "name=";
  constexpr char kPathPrefix[] = "path=";
  if (head.rfind(kNamePrefix, 0) == 0) {
    m.target_type = YZ_NATIVE_TARGET_NAME;
    m.target = head.substr(sizeof(kNamePrefix) - 1);
  } else if (head.rfind(kPathPrefix, 0) == 0) {
    m.target_type = YZ_NATIVE_TARGET_PATH;
    m.target = head.substr(sizeof(kPathPrefix) - 1);
  } else {
    return false;
  }
  if (m.target.empty() || m.target.size() >= YZ_NATIVE_TARGET_VALUE_MAX)
    return false;

  std::string token;
  std::string lib_rel;
  while (iss >> token) {
    if (token == "companion") {
      m.has_companion = true;
    } else if (lib_rel.empty()) {
      lib_rel = token;
    }
  }
  if (lib_rel.empty())
    return false;

  replace_all(lib_rel, "${moduleId}", module_id);
  replace_all(lib_rel, "$moduleId", module_id);
  if (!lib_rel.empty() && lib_rel[0] == '/')
    m.lib_path = lib_rel;
  else
    m.lib_path = base + "/" + lib_rel;

  if (access(m.lib_path.c_str(), F_OK) != 0)
    return false;
  *out = std::move(m);
  return true;
}

} // namespace yukizygisk::native
