//=====================================================================
//
// json.h -
//
// Created by liubang on 2023/05/28 21:00
// Last Modified: 2023/05/28 21:00
//
//=====================================================================

#pragma once

#include <charconv>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace playground::cpp::misc::json {

struct JSONObject {
  std::variant<std::nullptr_t,                              // null
               bool,                                        // true|false
               int,                                         // 123
               double,                                      // 3.14
               std::string,                                 // "hello world"
               std::vector<JSONObject>,                     // [42, "hello", true]
               std::unordered_map<std::string, JSONObject>  // {"name": "zhangsan", "age": 12}
               >
      inner;

  template <class T>
  [[nodiscard]] bool is() const {
    return std::holds_alternative<T>(inner);
  }

  template <class T>
  T const &get() const {
    return std::get<T>(inner);
  }

  template <class T>
  T &get() {
    return std::get<T>(inner);
  }
};

char unescaped_char(char c) {
  switch (c) {
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case '0':
      return '\0';
    case 't':
      return '\t';
    case 'v':
      return '\v';
    case 'f':
      return '\f';
    case 'b':
      return '\b';
    case 'a':
      return '\a';
    default:
      return c;
  }
}

template <class T>
std::optional<T> try_parse_num(std::string_view str) {
  T value;
  auto res = std::from_chars(str.data(), str.data() + str.size(), value);
  if (res.ec == std::errc() && res.ptr == str.data() + str.size()) {
    return value;
  }
  return std::nullopt;
}

std::pair<JSONObject, std::size_t> parse(std::string_view json) {
  if (json.empty()) {
    return {JSONObject{std::nullptr_t{}}, 0};
  }
  if (std::size_t off = json.find_first_not_of(" \n\r\t\v\f\0");
      off != 0 && off != std::string_view::npos) {
    auto [obj, eaten] = parse(json.substr(off));
    return {std::move(obj), eaten + off};
  }
  // parse number
  if (('0' <= json[0] && json[0] <= '9') || json[0] == '+' || json[0] == '-') {
    std::regex num_re{"[+-]?[0-9]+(\\.[0-9]*)?([eE][+-]?[0-9]+)?"};
    std::cmatch match;
    if (std::regex_search(json.data(), json.data() + json.size(), match, num_re)) {
      std::string str = match.str();
      // if (auto num = try_parse_num<int>(str); num.has_value()) {
      // equals to blow
      if (auto num = try_parse_num<int>(str)) {
        return {JSONObject{*num}, str.size()};
      }
      if (auto num = try_parse_num<double>(str)) {
        return {JSONObject{*num}, str.size()};
      }
    }
  }

  // parse string
  else if (json[0] == '"') {
    std::string str;
    enum {
      Raw,
      Escaped,
    } phase = Raw;
    std::size_t i;
    for (i = 1; i < json.size(); ++i) {
      char ch = json[i];
      if (phase == Raw) {
        if (ch == '\\') {
          phase = Escaped;
        } else if (ch == '"') {
          break;
        } else {
          str += ch;
        }
      } else if (phase == Escaped) {
        str += unescaped_char(ch);
        phase = Raw;
      }
    }
    return {JSONObject{std::move(str)}, i};
  }

  // parse list
  else if (json[0] == '[') {
    std::vector<JSONObject> res;
    std::size_t i;
    for (i = 1; i < json.size();) {
      if (json[i] == ']') {
        i += 1;
        break;
      }
      auto [obj, eaten] = parse(json.substr(i));
      if (eaten == 0) {
        i = 0;
        break;
      }
      res.push_back(std::move(obj));
      i += eaten;
      if (json[i] == ',') {
        i += 1;
      }
    }
    return {JSONObject{std::move(res)}, i};
  }

  // parse map
  else if (json[0] == '{') {
    std::unordered_map<std::string, JSONObject> res;
    size_t i;
    for (i = 1; i < json.size();) {
      if (json[i] == '}') {
        i += 1;
        break;
      }
      auto [keyobj, keyeaten] = parse(json.substr(i));
      if (keyeaten == 0) {
        i = 0;
        break;
      }
      i += keyeaten;
      if (!std::holds_alternative<std::string>(keyobj.inner)) {
        i = 0;
        break;
      }
      if (json[i] == ':') {
        i += 1;
      }
      std::string key = std::move(std::get<std::string>(keyobj.inner));
      auto [valobj, valeaten] = parse(json.substr(i));
      if (valeaten == 0) {
        i = 0;
        break;
      }
      i += valeaten;
      res.try_emplace(std::move(key), std::move(valobj));
      if (json[i] == ',') {
        i += 1;
      }
    }
    return {JSONObject{std::move(res)}, i};
  }

  return {JSONObject{std::nullptr_t{}}, 0};
}

}  // namespace playground::cpp::misc::json
