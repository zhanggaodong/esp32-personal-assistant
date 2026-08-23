# Implement Music Playback and Search Integration

This plan outlines the steps to integrate a music playback module into the existing project. This includes adding a new library dependency, wiring the [Esp32Music](file:///e:/test/xiaozhi-esp32/main/boards/common/esp32_music.h#28-110) module into the [Board](file:///e:/test/xiaozhi-esp32/main/boards/common/board.h#49-86) and [Application](file:///e:/test/xiaozhi-esp32/temp_new_main/application.h#39-41) layers, and exposing a music search tool via MCP.

## User Review Required

> [!IMPORTANT]
> This change introduces a new library dependency `chmorgan/esp-libhelix-mp3`. Ensure your network allows downloading from the IDF component registry.

> [!NOTE]
> The music search functionality depends on an external API (`http://110.42.59.54:2233/stream_pcm`). Please verify if this endpoint is intended for production use.

## Proposed Changes

### Component: Dependencies
- Update [main/idf_component.yml](file:///e:/test/xiaozhi-esp32/main/idf_component.yml) to include the `chmorgan/esp-libhelix-mp3` library.

#### [MODIFY] [idf_component.yml](file:///e:/test/xiaozhi-esp32/main/idf_component.yml)
```yaml
  chmorgan/esp-libhelix-mp3:
    version: "*"
```

---

### Component: Board Layer
- Integrate [Music](file:///e:/test/xiaozhi-esp32/main/boards/common/music.h#6-20) provider into the base [Board](file:///e:/test/xiaozhi-esp32/main/boards/common/board.h#49-86) class and its implementations (e.g., `WifiBoard`).

#### [MODIFY] [board.h](file:///e:/test/xiaozhi-esp32/main/boards/common/board.h)
- Add [Music](file:///e:/test/xiaozhi-esp32/main/boards/common/music.h#6-20) forward declaration and `GetMusic()` virtual method.
- Add `music_` protected member.

#### [MODIFY] [board.cc](file:///e:/test/xiaozhi-esp32/main/boards/common/board.cc)
- Include [esp32_music.h](file:///e:/test/xiaozhi-esp32/main/boards/common/esp32_music.h).
- Initialize `music_` in the constructor and cleanup in the destructor.

#### [MODIFY] [wifi_board.cc](file:///e:/test/xiaozhi-esp32/main/boards/common/wifi_board.cc) (and other board implementations)
- Ensure `GetMusic()` is properly handled or use default implementation from base class.

---

### Component: Application Layer
- Add audio data handling for music streaming and state-dependent music control.

#### [MODIFY] [application.h](file:///e:/test/xiaozhi-esp32/main/application.h)
- Add [AddAudioData(AudioStreamPacket&& packet)](file:///e:/test/xiaozhi-esp32/temp_new_main/application.cc#880-1002) method.

#### [MODIFY] [application.cc](file:///e:/test/xiaozhi-esp32/main/application.cc)
- Implement [AddAudioData](file:///e:/test/xiaozhi-esp32/temp_new_main/application.cc#880-1002) with resampling logic as seen in `new_main`.
- Update [SetDeviceState](file:///e:/test/xiaozhi-esp32/main/application.cc#57-60) to stop music when transitioning from [Idle](file:///e:/test/xiaozhi-esp32/main/audio/audio_service.cc#622-626).

---

### Component: MCP Server
- Register a `music.search` tool to allow AI to find and play music.

#### [MODIFY] [mcp_server.cc](file:///e:/test/xiaozhi-esp32/main/mcp_server.cc)
- Add `self.music.search` tool in [AddCommonTools](file:///e:/test/xiaozhi-esp32/main/mcp_server.cc#33-127).

---

## Verification Plan

### Automated Tests
- I will attempt a build using `idf.py build` to ensure the new dependency is resolved and the code compiles.

### Manual Verification
1. **Music Search**: Ask the AI "Play some music" or "Search for a song by [Artist]".
2. **Playback**: Verify that the AI calls the `music.search` tool and starts streaming audio.
3. **Control**: Verify that music stops when the wake word is detected or the device state changes.
