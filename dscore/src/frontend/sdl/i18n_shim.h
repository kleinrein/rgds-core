// SPDX-License-Identifier: GPL-3.0-or-later
// RG DS: upstream's frontend/i18n is a catalog for DSperate's own pause menu. RG DS Core draws its own menus and
// translates them in the launcher, so the strings the shared input code builds stay English here.
#pragma once
#include <initializer_list>
#include <string>

#ifndef N_
#define N_(s) s // marks a string upstream's catalog would translate
#endif

namespace ds::i18n {
inline const char* tr(const char* english) { return english; }
// "{1} AND {2}": the placeholders filled in order.
inline std::string trf(const char* english, std::initializer_list<std::string> args) {
  std::string out = english;
  int n = 1;
  for (const std::string& a : args) {
    const std::string mark = "{" + std::to_string(n++) + "}";
    for (size_t at = out.find(mark); at != std::string::npos; at = out.find(mark, at + a.size()))
      out.replace(at, mark.size(), a);
  }
  return out;
}
} // namespace ds::i18n
