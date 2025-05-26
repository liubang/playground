// Copyright (c) 2025 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)

#pragma once

#include <cstdint>
#include <string>

namespace pl {

class UTF8DisplayWidth {
public:
    // 获取UTF-8字符串的显示宽度
    static size_t getDisplayWidth(const std::string& utf8_str) {
        size_t width = 0;
        size_t i = 0;

        while (i < utf8_str.length()) {
            uint32_t codepoint = 0;
            size_t bytes = decodeUTF8Char(utf8_str, i, codepoint);

            if (bytes == 0) {
                // 无效的UTF-8序列，跳过
                i++;
                continue;
            }

            width += getCharDisplayWidth(codepoint);
            i += bytes;
        }

        return width;
    }

    // 获取单个Unicode码点的显示宽度
    static int getCharDisplayWidth(uint32_t codepoint) {
        // ASCII字符
        if (codepoint < 0x80) {
            // 控制字符宽度为0
            if (codepoint < 0x20 || codepoint == 0x7F) {
                return 0;
            }
            return 1;
        }

        // 零宽字符
        if (isZeroWidth(codepoint)) {
            return 0;
        }

        // 全角字符（主要是CJK字符）
        if (isFullWidth(codepoint)) {
            return 2;
        }

        // 其他字符默认宽度为1
        return 1;
    }

private:
    // 解码UTF-8字符，返回字节数
    static size_t decodeUTF8Char(const std::string& str, size_t pos, uint32_t& codepoint) {
        if (pos >= str.length()) {
            return 0;
        }

        auto first = static_cast<unsigned char>(str[pos]);

        // ASCII字符 (0xxxxxxx)
        if ((first & 0x80) == 0) {
            codepoint = first;
            return 1;
        }

        // 2字节UTF-8 (110xxxxx 10xxxxxx)
        if ((first & 0xE0) == 0xC0) {
            if (pos + 1 >= str.length())
                return 0;
            auto second = static_cast<unsigned char>(str[pos + 1]);
            if ((second & 0xC0) != 0x80)
                return 0;

            codepoint = ((first & 0x1F) << 6) | (second & 0x3F);
            return 2;
        }

        // 3字节UTF-8 (1110xxxx 10xxxxxx 10xxxxxx)
        if ((first & 0xF0) == 0xE0) {
            if (pos + 2 >= str.length())
                return 0;
            auto second = static_cast<unsigned char>(str[pos + 1]);
            auto third = static_cast<unsigned char>(str[pos + 2]);
            if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80)
                return 0;

            codepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
            return 3;
        }

        // 4字节UTF-8 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
        if ((first & 0xF8) == 0xF0) {
            if (pos + 3 >= str.length())
                return 0;
            auto second = static_cast<unsigned char>(str[pos + 1]);
            auto third = static_cast<unsigned char>(str[pos + 2]);
            auto fourth = static_cast<unsigned char>(str[pos + 3]);
            if ((second & 0xC0) != 0x80 || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80)
                return 0;

            codepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) |
                        (fourth & 0x3F);
            return 4;
        }

        return 0; // 无效序列
    }

    // 判断是否为零宽字符
    static bool isZeroWidth(uint32_t codepoint) {
        // 组合字符 (Combining Diacritical Marks)
        if (codepoint >= 0x0300 && codepoint <= 0x036F)
            return true;

        // 零宽字符
        if (codepoint == 0x200B || // Zero Width Space
            codepoint == 0x200C || // Zero Width Non-Joiner
            codepoint == 0x200D || // Zero Width Joiner
            codepoint == 0xFEFF) { // Zero Width No-Break Space
            return true;
        }

        // 更多零宽字符范围...
        return false;
    }

    // 判断是否为全角字符
    static bool isFullWidth(uint32_t codepoint) {
        // CJK统一汉字
        if (codepoint >= 0x4E00 && codepoint <= 0x9FFF)
            return true;

        // CJK统一汉字扩展A
        if (codepoint >= 0x3400 && codepoint <= 0x4DBF)
            return true;

        // 全角字母和数字
        if (codepoint >= 0xFF01 && codepoint <= 0xFF60)
            return true;

        // 平假名
        if (codepoint >= 0x3040 && codepoint <= 0x309F)
            return true;

        // 片假名
        if (codepoint >= 0x30A0 && codepoint <= 0x30FF)
            return true;

        // 韩文字母
        if (codepoint >= 0xAC00 && codepoint <= 0xD7AF)
            return true;

        // 更多全角字符范围...
        return false;
    }
};

} // namespace pl
