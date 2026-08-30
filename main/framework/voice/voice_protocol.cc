#include "voice_protocol.h"

#include <cJSON.h>
#include <cstring>

namespace voice {

// ---------------------------------------------------------------------------
// 二进制帧
// ---------------------------------------------------------------------------

// 帧布局（所有多字节整数为大端）：
//   [0..1]  magic    'V','1'
//   [2]     type
//   [3]     flags
//   [4..7]  turn_id
//   [8..11] sequence
//   [12..15]payload_size
//   [16..]  payload
static constexpr uint8_t kMagic0 = 'V';
static constexpr uint8_t kMagic1 = '1';

FrameParseResult DecodeBinaryFrame(const uint8_t* data, size_t len,
                                   ParsedFrame& out) {
    if (data == nullptr || len < kFrameHeaderSize) {
        return FrameParseResult::kNotEnough;
    }
    if (data[0] != kMagic0 || data[1] != kMagic1) {
        return FrameParseResult::kBadMagic;
    }

    const uint8_t type = data[2];
    if (type != static_cast<uint8_t>(FrameType::kInputPcm) &&
        type != static_cast<uint8_t>(FrameType::kOutputPcm) &&
        type != static_cast<uint8_t>(FrameType::kOutputOpus)) {
        return FrameParseResult::kBadType;
    }

    // 大端读取：高位在前。
    const uint32_t turn_id = (uint32_t(data[4]) << 24) |
                             (uint32_t(data[5]) << 16) |
                             (uint32_t(data[6]) << 8) | uint32_t(data[7]);
    const uint32_t sequence = (uint32_t(data[8]) << 24) |
                              (uint32_t(data[9]) << 16) |
                              (uint32_t(data[10]) << 8) | uint32_t(data[11]);
    const uint32_t payload_size = (uint32_t(data[12]) << 24) |
                                  (uint32_t(data[13]) << 16) |
                                  (uint32_t(data[14]) << 8) |
                                  uint32_t(data[15]);

    if (payload_size > kMaxPayloadBytes) {
        return FrameParseResult::kPayloadTooLarge;
    }
    if (len < kFrameHeaderSize + payload_size) {
        return FrameParseResult::kNotEnough;
    }
    // 下行 PCM16 必须按 2 字节对齐。
    if (type == static_cast<uint8_t>(FrameType::kOutputPcm) &&
        (payload_size % 2) != 0) {
        return FrameParseResult::kBadAlignment;
    }

    out.type = static_cast<FrameType>(type);
    out.flags = data[3];
    out.turn_id = turn_id;
    out.sequence = sequence;
    out.payload_size = payload_size;
    out.payload = data + kFrameHeaderSize;
    return FrameParseResult::kOk;
}

bool EncodeBinaryFrame(FrameType type, uint8_t flags, uint32_t turn_id,
                       uint32_t sequence, const uint8_t* payload,
                       size_t payload_size, std::string& out) {
    if (payload_size > kMaxPayloadBytes) {
        return false;
    }
    if (payload_size > 0 && payload == nullptr) {
        return false;
    }
    if (type == FrameType::kOutputPcm && (payload_size % 2) != 0) {
        return false;
    }

    out.clear();
    out.reserve(kFrameHeaderSize + payload_size);

    // 头
    out.push_back(static_cast<char>(kMagic0));
    out.push_back(static_cast<char>(kMagic1));
    out.push_back(static_cast<char>(type));
    out.push_back(static_cast<char>(flags));
    // 大端写入。
    out.push_back(static_cast<char>((turn_id >> 24) & 0xFF));
    out.push_back(static_cast<char>((turn_id >> 16) & 0xFF));
    out.push_back(static_cast<char>((turn_id >> 8) & 0xFF));
    out.push_back(static_cast<char>(turn_id & 0xFF));
    out.push_back(static_cast<char>((sequence >> 24) & 0xFF));
    out.push_back(static_cast<char>((sequence >> 16) & 0xFF));
    out.push_back(static_cast<char>((sequence >> 8) & 0xFF));
    out.push_back(static_cast<char>(sequence & 0xFF));
    out.push_back(static_cast<char>((payload_size >> 24) & 0xFF));
    out.push_back(static_cast<char>((payload_size >> 16) & 0xFF));
    out.push_back(static_cast<char>((payload_size >> 8) & 0xFF));
    out.push_back(static_cast<char>(payload_size & 0xFF));
    // payload
    if (payload_size > 0) {
        out.append(reinterpret_cast<const char*>(payload), payload_size);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 控制消息构造
// ---------------------------------------------------------------------------

static void AddU32(cJSON* obj, const char* key, uint32_t v) {
    cJSON_AddNumberToObject(obj, key, static_cast<double>(v));
}

std::string BuildTurnStart(uint32_t turn_id, const std::string& conversation_id,
                           const std::string& voice, const std::string& language,
                           uint32_t max_record_seconds,
                           const std::string& enabled_caps,
                           uint32_t history_limit) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(root, "type", "turn.start");
    AddU32(root, "turnId", turn_id);
    if (conversation_id.empty()) {
        cJSON_AddNullToObject(root, "conversationId");
    } else {
        cJSON_AddStringToObject(root, "conversationId", conversation_id.c_str());
    }
    cJSON_AddStringToObject(root, "voice", voice.c_str());
    cJSON_AddStringToObject(root, "language", language.c_str());
    AddU32(root, "maxRecordSeconds", max_record_seconds);
    // 请求下行 opus（60ms/24kHz/单声道）。旧后端会忽略未知字段并回退 PCM 帧。
    cJSON_AddStringToObject(root, "audioCodec", "opus");
    // 本轮允许的能力类别（空串 = 全关）。旧后端忽略该字段 → 全量注册工具。
    cJSON_AddStringToObject(root, "enabledCaps", enabled_caps.c_str());
    AddU32(root, "historyLimit", history_limit);

    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == nullptr) {
        return {};
    }
    std::string out(s);
    cJSON_free(s);
    return out;
}

std::string BuildTurnStop(uint32_t turn_id) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(root, "type", "turn.stop");
    AddU32(root, "turnId", turn_id);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == nullptr) {
        return {};
    }
    std::string out(s);
    cJSON_free(s);
    return out;
}

std::string BuildTurnCancel(uint32_t turn_id, const std::string& reason) {
    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return {};
    }
    cJSON_AddStringToObject(root, "type", "turn.cancel");
    AddU32(root, "turnId", turn_id);
    cJSON_AddStringToObject(root, "reason", reason.c_str());
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == nullptr) {
        return {};
    }
    std::string out(s);
    cJSON_free(s);
    return out;
}

// ---------------------------------------------------------------------------
// 控制消息解析
// ---------------------------------------------------------------------------

MessageType ClassifyMessage(const char* json, size_t len) {
    if (json == nullptr || len == 0) {
        return MessageType::kUnknown;
    }
    const std::string type = [&]() -> std::string {
        cJSON* root = cJSON_ParseWithLength(json, len);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return {};
        }
        cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "type");
        const char* v = cJSON_IsString(t) ? t->valuestring : nullptr;
        std::string s = v ? v : "";
        cJSON_Delete(root);
        return s;
    }();

    if (type == "hello") return MessageType::kHello;
    if (type == "turn.ready") return MessageType::kTurnReady;
    if (type == "asr.partial") return MessageType::kAsrPartial;
    if (type == "asr.final") return MessageType::kAsrFinal;
    if (type == "chat.delta") return MessageType::kChatDelta;
    if (type == "tts.sentence") return MessageType::kTtsSentence;
    if (type == "turn.done") return MessageType::kTurnDone;
    if (type == "turn.cancelled") return MessageType::kTurnCancelled;
    if (type == "turn.error") return MessageType::kTurnError;
    if (type == "turn.progress") return MessageType::kTurnProgress;
    return MessageType::kUnknown;
}

// 取对象内的字符串字段（不存在或非字符串时返回空且 found=false）。
static bool GetStr(const cJSON* obj, const char* key, std::string& out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != nullptr) {
        out = v->valuestring;
        return true;
    }
    return false;
}

// 取对象内的 uint32 字段（存在且为数字才填充）。
static bool GetU32(const cJSON* obj, const char* key, uint32_t& out) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d >= 0) {
            out = static_cast<uint32_t>(d);
            return true;
        }
    }
    return false;
}

bool ParseServerMessage(const char* json, size_t len, ServerMessage& out) {
    if (json == nullptr || len == 0) {
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    out = ServerMessage{};
    out.type = ClassifyMessage(json, len);
    if (out.type == MessageType::kUnknown) {
        cJSON_Delete(root);
        return false;
    }

    uint32_t turn_id = 0;
    if (GetU32(root, "turnId", turn_id)) {
        out.turn_id = turn_id;
        out.has_turn_id = true;
    }
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNumber(v)) {
        out.version = static_cast<int>(v->valuedouble);
    }

    switch (out.type) {
        case MessageType::kAsrPartial:
        case MessageType::kAsrFinal:
        case MessageType::kChatDelta:
            GetStr(root, "text", out.text);
            break;
        case MessageType::kTtsSentence:
            GetStr(root, "text", out.text);
            {
                uint32_t seq = 0;
                if (GetU32(root, "sequence", seq)) {
                    out.sequence = seq;
                }
            }
            break;
        case MessageType::kTurnDone:
            GetStr(root, "conversationId", out.conversation_id);
            break;
        case MessageType::kTurnError:
            // 正式字段为 phase；缺失时回退读旧服务端的 stage（过渡兼容，最终移除）。
            if (!GetStr(root, "phase", out.phase)) {
                GetStr(root, "stage", out.phase);
            }
            GetStr(root, "code", out.code);
            GetStr(root, "message", out.message);
            break;
        default:
            break;  // hello / turn.ready / turn.cancelled 无额外字段
    }

    cJSON_Delete(root);
    return true;
}

}  // namespace voice