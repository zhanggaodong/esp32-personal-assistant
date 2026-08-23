#pragma once

#include <map>
#include <string>

// 极简 JSON 工具：仅支持本项目 Web 配置 API 需要的"扁平字符串对象"。
// 所有配置值都以字符串形式传输（颜色/枚举/布尔/数字均如此），故无需泛型 JSON 库。
namespace json_util {

// 解析形如 { "k1":"v1", "k2":"v2" } 的平面对象（仅字符串值）。返回是否成功。
inline bool ParseFlatObject(const char* json, std::map<std::string, std::string>& out) {
    out.clear();
    if (json == nullptr) {
        return false;
    }
    std::string s = json;
    // 去掉首尾空白与外围大括号
    size_t begin = s.find('{');
    size_t end = s.rfind('}');
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return false;
    }
    std::string body = s.substr(begin + 1, end - begin - 1);
    size_t pos = 0;
    while (pos < body.size()) {
        // 查找 key 的引号
        size_t k1 = body.find('"', pos);
        if (k1 == std::string::npos) {
            break;
        }
        size_t k2 = body.find('"', k1 + 1);
        if (k2 == std::string::npos) {
            return false;
        }
        std::string key = body.substr(k1 + 1, k2 - k1 - 1);
        size_t colon = body.find(':', k2 + 1);
        if (colon == std::string::npos) {
            return false;
        }
        size_t vstart = body.find_first_not_of(" \t\r\n", colon + 1);
        if (vstart == std::string::npos) {
            return false;
        }
        if (body[vstart] == '"') {
            size_t vend = body.find('"', vstart + 1);
            if (vend == std::string::npos) {
                return false;
            }
            out[key] = body.substr(vstart + 1, vend - vstart - 1);
            pos = vend + 1;
        } else {
            // 非字符串值（数字/裸 token）取到逗号或结束
            size_t vend = body.find_first_of(",}", vstart + 1);
            if (vend == std::string::npos) {
                vend = body.size();
            }
            out[key] = body.substr(vstart, vend - vstart);
            pos = vend;
        }
        // 跳过分隔符
        size_t comma = body.find(',', pos);
        pos = (comma == std::string::npos) ? body.size() : comma + 1;
    }
    return true;
}

// 转义 JSON 字符串中的特殊字符
inline std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

inline std::string TypeName(int type) {
    switch (type) {
        case 0: return "string";
        case 1: return "int";
        case 2: return "number";
        case 3: return "color";
        case 4: return "image";
        case 5: return "enum";
        case 6: return "bool";
        default: return "string";
    }
}

}  // namespace json_util