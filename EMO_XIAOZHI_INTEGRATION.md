# EMO firmware — hiểu source & hướng tích hợp Xiaozhi

Tài liệu ghi lại những gì đã nắm về repo `newFirmware`, luồng Wi‑Fi/hotspot, và phương án tích hợp client Xiaozhi (chỉ gọi server, mic/loa vẫn theo EMO).

---

## 1. Source này là gì?

Đây là **firmware thay thế cho ESP32** trong robot bàn **EMO** (living.ai), cộng đồng / fork quanh [emo-libre](https://github.com/emo-libre).

| Thành phần | Vai trò |
|---|---|
| **ESP32** | Flash được — bridge mạng, UART/SPI với K210, PWM 4 servo chân, HTTP API, SoftAP |
| **K210** | **Giữ stock v3.1.0** — mic, wake word, VAD, loa, màn hình face, đọc SD (avi/mp3) |
| **`local-brain/`** | Voice assistant chạy trên PC (STT → LLM → TTS), **không** nằm trong firmware |

ESP32 **không** thay mic/loa I2S trực tiếp. Audio luôn đi qua K210:

```
PC / voice server
      ↕  Wi‑Fi (TCP EMOA hiện tại, hoặc WebSocket Xiaozhi sau này)
   ESP32 (FW này)
      ↕  UART 1 Mbaud (điều khiển) + SPI 8 MHz (audio)
   K210 (stock)
      ↕
   mic / loa / face / SD
```

**Hệ quả:** muốn đổi sâu wake-word model, face engine, v.v. phải đụng K210 riêng. Repo này chỉ điều khiển những gì K210 stock đã expose qua UART/SPI.

Phiên bản FW trong tree: `EMO-fullFW-ver0.1.2-esp32`.

---

## 2. Cấu trúc repo (ngắn)

```
newFirmware/
├── README.md                      # backup / flash ESP32, SPIFFS
├── base-firmware.bin
├── EMO-fullFW-ver0.1.2-esp32/     # firmware ESP32 chính
│   ├── README.md, API.md, FIRMWARE_HANDOFF.md, uart_commands.md
│   ├── main/                      # k210_*, microphone, speaker, legs, host_stream, rest_api…
│   └── spiffs_image/mot/          # ~582 file .mot stock
├── local-brain/                   # server voice trên Windows (EMOA :7007)
├── Hardware/                      # GPIO_MAP, UART_commands
└── APP - template/                # template app khác (node-manager style)
```

Module FW quan trọng:

| File | Việc |
|---|---|
| `k210_uart` / `k210_spi` / `k210_link` | Transport + half-duplex idle/listen/speak |
| `microphone` | MODE_MIC, wake / end-of-speech, hangover follow-up |
| `speaker` | MODE_SPK, PCM 48 kHz → K210 |
| `legs` / `servo` | Chơi `.mot`, gait walk/turn |
| `host_stream` | TCP protocol **EMOA** tới voice server |
| `net` / `settings` | Wi‑Fi STA + SoftAP, NVS |
| `rest_api` / `web` | HTTP `/api/*` + trang cấu hình |

---

## 3. Chức năng đã có (mức độ hoàn chỉnh)

### Đi đứng / chân
- Play **~582** motion `.mot` từ SPIFFS.
- **Walk / turn** bằng oscillator kiểu stock (Otto-like) — dùng được.
- Face + nhạc đi kèm motion qua `mot_media.txt`.
- API: `POST /api/motion` (`play`, `gait`, `pose`, `stop`, `release`).
- **Chưa 100%:** một số frame “dance generator” chỉ giữ pose (`unsupported`), không sinh procedural dance đầy đủ như stock.

### Giao tiếp audio / face
- Wake word + VAD từ **K210**.
- Stream mic 16 kHz mono PCM lên server (EMOA).
- Phát PCM 48 kHz mono ra loa; barge-in khi đang nói.
- Face / volume / controls: `GET|POST /api/controls`.
- Pin, power-off, web UI.

### “Não” hội thoại
- **Không** nằm trong ESP32. FW chỉ bridge.
- Tham chiếu sẵn: `local-brain` (Whisper/ASR → LLM → TTS + tool walk/turn).

### Giới hạn cứng
- K210 **half-duplex**: không vừa nghe vừa nói cùng lúc.
- Không flash Xiaozhi stock nguyên bản lên ESP32 — sẽ **mất** link K210 (mặt, mic, loa, chân).

---

## 4. Bắt Wi‑Fi bằng hotspot (đã có sẵn)

FW **đã hỗ trợ** cấu hình mạng kiểu provisioning:

1. Chưa có SSID trong NVS / menuconfig → ESP32 mở **SoftAP**:
   - SSID: `emo-bridge`
   - Password: `emobridge`
   - Web: `http://192.168.4.1:8080`
2. Trên điện thoại/PC: kết nối hotspot của robot → mở trang web → nhập:
   - Wi‑Fi nhà **hoặc hotspot điện thoại** (SSID + pass)
   - Địa chỉ voice server (`host_ip`, `host_port`) — hiện tại cho EMOA
3. Save → reboot → robot join mạng đó (STA).

Cũng có thể set qua `POST /api/config` (`wifi_ssid`, `wifi_pass`, `host_ip`, `host_port`, …) rồi reboot.

**Lưu ý khi dùng hotspot điện thoại:**
- Máy chạy Xiaozhi server (hoặc PC) phải **cùng mạng** với robot, hoặc robot/server reach được nhau.
- Không dùng `127.0.0.1` / `localhost` làm địa chỉ server trên robot — phải là IP LAN thật (vd. `192.168.43.x` khi phone hotspot).

Phần bắt Wi‑Fi **không phụ thuộc Xiaozhi**. Chỉ cần thêm field cấu hình (vd. OTA URL / WebSocket URL) nếu tích hợp Xiaozhi.

---

## 5. Protocol voice hiện tại (EMOA) vs Xiaozhi

### EMO hiện tại (`host_stream`)
- Robot **mở TCP** tới `EMO_HOST_IP:EMO_HOST_PORT` (mặc định `192.168.137.1:7007`) khi có wake.
- Hello 19 byte: magic `"EMOA"`, 16 kHz, mono, 16-bit.
- Chunk PCM + CRC; server ACK `"ACK0"` + offset.
- Một connection / một utterance; chi tiết trong `API.md` §3.

### Xiaozhi (cần tích hợp)
- Device lấy config từ **OTA HTTP**, rồi nối **WebSocket** (hoặc MQTT+UDP).
- Audio: **Opus** (thường mic 16 kHz frame ~60 ms; TTS có thể 24 kHz).
- JSON: `hello`, `listen`, STT/TTS events, **MCP** tools.
- LLM/STT/TTS chạy trên **server Xiaozhi** (cloud hoặc self-host), không trên ESP32.

Hai protocol **không tương thích byte-level**. Phải thay hoặc song song lớp `host_stream`, không phải chỉ đổi port.

---

## 6. Hướng tích hợp Xiaozhi (đã thống nhất)

### Mục tiêu
- **Chỉ tích hợp cơ chế gọi server Xiaozhi** (client protocol).
- **Luồng micro / loa vẫn theo source EMO** (K210 SPI PCM).
- Không flash firmware `xiaozhi-esp32` nguyên bản.

### Kiến trúc mục tiêu

```
[Wake K210] → ESP32 mở WebSocket Xiaozhi
           → PCM mic (microphone.c) → Opus encode → server
           → Opus TTS ← server → decode → resample 48 kHz → speaker.c
           → (sau TTS) microphone_open() lại → nói tiếp không cần wake
           → MCP walk/turn/anim → legs.c / k210_controls
[Timeout / hết hội thoại] → MODE_IDLE → chờ wake lần sau
```

### Giữ từ EMO | Lấy từ Xiaozhi

| Giữ | Port / thêm |
|---|---|
| `k210_*`, `microphone`, `speaker` | WebSocket + hello / listen / tts |
| `legs`, `.mot`, SoftAP Wi‑Fi | Opus encode/decode + resample |
| HTTP status / motion API (tuỳ chọn) | OTA URL → `websocket.url` |
| Face qua UART | MCP → map sang motion / controls |

### Không làm
- Thay driver audio bằng I2S board Xiaozhi.
- Kỳ vọng full-duplex như điện thoại.
- Chạy LLM trên ESP32.

---

## 7. Hội thoại liên tục (không wake mỗi câu)

**Làm được.**

FW EMO đã có:
- Hangover trong một utterance: VAD cắt sớm → tự `LISTEN_FOLLOWUP` (`4B`) + `MODE_MIC` nếu vẫn còn tiếng.
- `microphone_open()`: mở mic **không cần** wake (dùng cho WAV capture / control).

Xiaozhi / `local-brain` đã có pattern:
- Sau khi TTS xong → mở listen lại (`listen_again` / auto listen).
- Giới hạn số follow-up hoặc timeout im lặng → về idle, lần sau mới cần wake.

**Map cần làm khi gắn Xiaozhi:**

| Sự kiện Xiaozhi | Hành động EMO |
|---|---|
| Bắt đầu session (sau wake hoặc server bảo listen) | Session WS + `MODE_MIC` (wake: theo luồng sẵn; follow-up: `microphone_open`) |
| TTS start | `MODE_SPK`, đẩy PCM đã decode |
| TTS stop / hết câu trả lời | `microphone_open()` — **không** đợi WAKE |
| Hết hội thoại / timeout | `MODE_IDLE`, đóng WS nếu cần |

Giới hạn: half-duplex K210; im lặng lâu vẫn phải wake lại.

---

## 8. Self-host Xiaozhi — checklist mạng

1. Robot vào hotspot / Wi‑Fi (mục 4).
2. Chạy `xiaozhi-esp32-server` (hoặc tương đương) trên máy trong cùng LAN.
3. OTA / WS URL phải là IP máy đó, ví dụ:
   - OTA: `http://192.168.x.x:8003/xiaozhi/ota/`
   - WS: `ws://192.168.x.x:8000/xiaozhi/v1/`
4. Trên FW (sau khi tích hợp): lưu OTA URL trong NVS / trang SoftAP (giống cách Xiaozhi provisioning).
5. Firewall máy host mở port OTA + WS.

---

## 9. Thứ tự làm việc đề xuất

1. **Wi‑Fi / hotspot** — dùng SoftAP sẵn, xác nhận robot có IP, ping được máy host.
2. **Chat-only Xiaozhi** — thay `host_stream` bằng WS + Opus; mic/spk vẫn EMO; chưa MCP.
3. **Follow-up liên tục** — sau TTS gọi `microphone_open()`; timeout → idle.
4. **MCP / self-control** — tool walk, turn, play motion, face, volume → `legs` / `controls`.
5. (Tuỳ chọn) Gỡ dần phụ thuộc `local-brain` nếu server Xiaozhi đủ dùng.

---

## 10. Trạng thái triển khai (FW)

| Mục | Status |
|---|---|
| SoftAP `emo-bridge` / web `:8080` cấu hình Wi‑Fi | **Đã có** (`net.c`) |
| `voice_mode` = `emoa` \| `xiaozhi` (web + NVS) | **Đã có** |
| OTA → `websocket.url` + mã kích hoạt 6 số trên face | **Đã có** (`xiaozhi_client.c`) |
| WS hello / listen / Opus mic↑ TTS↓ / follow-up mic | **Đã có** |
| MCP walk / turn / play_motion / volume | **Cơ bản** |
| Flash stock `xiaozhi-esp32` | **Không làm** (mất K210) |

Cấu hình nhanh: SoftAP → nhập Wi‑Fi → `voice_mode=xiaozhi` + `ota_url=http://<server>:8003/xiaozhi/ota/` → Save → reboot → nối STA → nhập mã trên console server nếu có → gọi wake.

---

## 11. Tóm tắt một dòng

> Source này là **ESP32 bridge cho EMO + K210 stock**: đi đứng và audio qua API/UART-SPI đã khá đủ; Wi‑Fi SoftAP đã có. Tích hợp Xiaozhi = **port client gọi server** vào FW, **giữ pipeline mic/loa K210**, và **tự mở mic lại sau TTS** để nói liên tục không cần wake mỗi lần — không flash Xiaozhi nguyên bản.
