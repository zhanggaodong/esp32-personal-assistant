#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// 设备语音 WebSocket 协议 v1（对应计划第 4 节）。
//
// 上行 PCM 与下行 PCM 共用固定二进制头，所有整数用网络字节序（大端）；
// 控制消息为 JSON 文本帧。链路两端均遵守本协议，后端实现见
// ai-agent-assistant backend/src/modules/voice/voice-protocol.ts。
//
// 本文件只含"不依赖硬件"的纯函数：二进制帧编解码、控制消息 JSON 的
// 构造与解析，便于脱离 ESP32 用固定输入向量做单元验证（大小端、
// turnId、sequence、payload size）。
namespace voice {

// 协议版本（与后端 Device-Protocol-Version 头及 hello.version 一致）。
constexpr uint32_t kProtocolVersion = 1;

// 上行默认一帧 = 100ms / 16kHz / 16bit / 单声道 = 3200 字节。
constexpr size_t kInputFrameBytes = 3200;

// 单帧 payload 绝对上限：32 KB，超过立即关闭连接。
constexpr size_t kMaxPayloadBytes = 32 * 1024;

// 二进制帧头长度：magic(2) + type(1) + flags(1) + turn_id(4) + sequence(4) + payload_size(4)。
constexpr size_t kFrameHeaderSize = 16;

// 二进制帧类型。
enum class FrameType : uint8_t {
    kInputPcm = 1,   // 上行：设备 -> 后端
    kOutputPcm = 2,  // 下行：后端 -> 设备（PCM16，2 字节对齐）
};

// flags 位定义（bit0=first, bit1=last）。
constexpr uint8_t kFrameFlagFirst = 0x01;
constexpr uint8_t kFrameFlagLast = 0x02;

// 帧解析结果。
enum class FrameParseResult {
    kOk,
    kNotEnough,       // 数据不足"头 + payload"长度
    kBadMagic,        // magic 不是 'V','1'
    kBadType,         // type 不是 input/output
    kPayloadTooLarge, // payload_size 超过 32KB
    kBadAlignment,    // 下行 PCM16 未按 2 字节对齐
};

// 解析后的一帧（payload 指针指向调用方传入的数据缓冲内部，不拷贝）。
struct ParsedFrame {
    FrameType type = FrameType::kInputPcm;
    uint8_t flags = 0;
    uint32_t turn_id = 0;
    uint32_t sequence = 0;
    uint32_t payload_size = 0;
    const uint8_t* payload = nullptr;

    bool first() const { return (flags & kFrameFlagFirst) != 0; }
    bool last() const { return (flags & kFrameFlagLast) != 0; }
};

// 从网络收到的字节流中解析一帧。len 必须 >= 头 + payload_size，否则返回
// kNotEnough（调用方需继续缓存等后续数据）。成功时 out.payload 指向 data+16。
FrameParseResult DecodeBinaryFrame(const uint8_t* data, size_t len, ParsedFrame& out);

// 构造一帧二进制（上行/下行通用）。payload_size 必须 <= kMaxPayloadBytes；
// payload_size > 0 时 payload 不得为空。结果写入 out（含 16 字节头 + payload）。
bool EncodeBinaryFrame(FrameType type, uint8_t flags, uint32_t turn_id,
                       uint32_t sequence, const uint8_t* payload,
                       size_t payload_size, std::string& out);

// ---------------------------------------------------------------------------
// 控制消息 JSON 构造（设备 -> 后端）
// ---------------------------------------------------------------------------

// {type:"turn.start", turnId, conversationId|null, voice, language, maxRecordSeconds}
// conversation_id 为空时序列化为 null。
std::string BuildTurnStart(uint32_t turn_id, const std::string& conversation_id,
                           const std::string& voice, const std::string& language,
                           uint32_t max_record_seconds);

// {type:"turn.stop", turnId}
std::string BuildTurnStop(uint32_t turn_id);

// {type:"turn.cancel", turnId, reason}
std::string BuildTurnCancel(uint32_t turn_id, const std::string& reason);

// ---------------------------------------------------------------------------
// 控制消息 JSON 解析（后端 -> 设备）
// ---------------------------------------------------------------------------

// 服务端控制事件类型。
enum class MessageType {
    kUnknown,
    kHello,         // 连接握手
    kTurnReady,     // 可接收上行音频
    kAsrPartial,    // 录音期间滚动预热文本（仅日志/灯效）
    kAsrFinal,      // 松手后最终识别文本
    kChatDelta,     // Chat 流式增量（仅日志，不累加完整答案）
    kTtsSentence,   // 当前句子开始合成/播放
    kTurnDone,      // 本轮结束，携带 conversationId 与统计
    kTurnCancelled, // 后端已停止旧轮
    kTurnError,     // 阶段、错误码、可展示信息
};

// 根据 JSON 顶层 "type" 归类。解析失败返回 kUnknown。
MessageType ClassifyMessage(const char* json, size_t len);

// 解析一条完整 JSON 控制消息到 ServerMessage。type 必填；其余字段按事件类型填充。
// 返回是否成功解析出至少一个字段。
struct ServerMessage {
    MessageType type = MessageType::kUnknown;
    uint32_t turn_id = 0;
    bool has_turn_id = false;
    int version = 0;             // hello.version

    std::string text;            // asr.partial / asr.final / chat.delta 文本
    uint32_t sequence = 0;       // tts.sentence 序号
    std::string conversation_id; // turn.done

    std::string stage;           // turn.error
    std::string code;
    std::string message;
};

bool ParseServerMessage(const char* json, size_t len, ServerMessage& out);

}  // namespace voice