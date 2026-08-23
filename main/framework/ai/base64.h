#pragma once

#include <string>
#include <vector>

// 极简 Base64 编解码（标准 alphabet，处理填充 '='）。
namespace base64 {

const char* kChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline int DecodeValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 解码 base64 字符串到 out。成功返回 true。
inline bool Decode(const std::string& in, std::vector<uint8_t>& out) {
    out.clear();
    int accumulator = 0;
    int bits = 0;  // accumulator 中有效位数
    for (char c : in) {
        if (c == '=') {
            break;
        }
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        int v = DecodeValue((unsigned char)c);
        if (v < 0) {
            // 非 base64 字符：容忍，直接跳过（严格模式可改为 return false）
            continue;
        }
        accumulator = (accumulator << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

inline void Encode(const uint8_t* data, size_t len, std::string& out) {
    out.clear();
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                     (uint32_t)data[i + 2];
        out += kChars[(v >> 18) & 0x3F];
        out += kChars[(v >> 12) & 0x3F];
        out += kChars[(v >> 6) & 0x3F];
        out += kChars[v & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)data[i] << 16;
        out += kChars[(v >> 18) & 0x3F];
        out += kChars[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out += kChars[(v >> 18) & 0x3F];
        out += kChars[(v >> 12) & 0x3F];
        out += kChars[(v >> 6) & 0x3F];
        out += "=";
    }
}

}  // namespace base64