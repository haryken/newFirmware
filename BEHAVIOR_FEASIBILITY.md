# EMO behavior — nhóm chắc vs chưa chắc

Phạm vi: custom ESP32 FW (`EMO-fullFW-ver0.1.2-esp32`) + **K210 stock 3.1.0**.  
Mục tiêu: voice / Xiaozhi MCP / local-brain tool → robot hành động. Không flash `xiaozhi-esp32` nguyên bản.

**Nhóm chưa làm được (blocked):** xem riêng [BEHAVIOR_BLOCKED.md](BEHAVIOR_BLOCKED.md).  
**Build bin:** [EMO-fullFW-ver0.1.2-esp32/BUILD.md](EMO-fullFW-ver0.1.2-esp32/BUILD.md).

Nguồn đối chiếu: [Living.AI voice commands EN](https://living.ai/voice-commands-english/), `Documentation-emo/Behaviors`, `Documentation-emo/Intents`, `uart_commands.md`, `RE_BEHAVIOR_SENSORS.md`, SPIFFS `spiffs_image/mot/` (~582 `.mot`).

---

## 1. Nhóm chắc (làm được với API FW hiện tại)

Không cần RE bin sâu. Chủ yếu: `legs_play` + media, `legs_gait`, `volume`, face UART, cliff đã có.

| Hành vi / lệnh kiểu stock | Cơ chế | Trạng thái code |
|---|---|---|
| Sing a song | `.mot` dance/sings + media | MCP `self.robot.sing_a_song` |
| Dance / Let’s dance | `d1`…`d11`, `Christmas_song` | MCP `self.robot.dance` |
| Move forward/back, turn | gait walk/turn | MCP `walk` / `turn` |
| Look around | `look_around_1`…`8` | MCP `look_around` |
| Greeting (hi / bye / good morning…) | `Hi`, `Byebye`, `Good_*` | MCP `greet` |
| Act like a {animal} | `Pig`, `Dog`, … | MCP `act_like` |
| Laser eyes / pose / show | `laser_eye*`, `show_*` | MCP `laser_eyes` / `pose` |
| Volume up/down / mute / set | UART `0x17` | MCP volume tools |
| Play bất kỳ `.mot` / face | `play_motion` / `play_face` | MCP có |
| Stop motion | `legs_stop` | MCP `stop` |
| Idle faces / cliff interrupt / petting / docked hold | `behavior.c` | Đã ship (APPROVE A*) |
| **Camping (scripted)** | Chuỗi `Camp_*` / `campfire_*` | **`tricks.c`** + MCP/`POST /api/tricks` |
| **Timer / alarm local** | `esp_timer` + `alarm_shock_*` | **`tricks_timer_start`** (không sync Living.AI) |
| **Explore có giới hạn** | `explore_*` + cliff bắt buộc + timeout | **`tricks_explore_start`** |
| **Games demo (anim)** | rps / ttt / zombie / fix_bugs | **`tricks_game_start`** (không CV) |

API: `GET|POST /api/tricks` — `camping`, `timer`, `explore`, `game`, `stop`.

---

## 2. Nhóm chưa chắc / blocked (chưa làm đúng stock)

| Hành vi | Verdict | Thiếu gì |
|---|---|---|
| **Go home** (tự tìm dock) | Partial → **blocked full** | Track home-station RX **`0x62`**, BLE mesh station, FSM tiến/dock; detect sạc thật. Hiện chỉ có cờ web **`docked`** (đứng im trên đế). |
| **Take photo đầy đủ** | **Blocked** (an toàn) | Pipeline SPI/SD (`0x40`/`0x03`/`0x04`/`0x55`…), trigger capture, đưa file ra app/HTTP. UART photo đang **danger-gate** (`EMO_ALLOW_SD_COMMANDS`). Pose `Group_photo_*` thì chắc. |
| **Games đầy đủ (CV)** | Hard | RPS cần tay; TTT stock có logic ESP32 nhưng ASR số / board UX; zombie/paint cần detect `0x53` / blob `0x35` / YOLO policy. |
| **Explore stock parity** | Hard | Walker liên tục + IMU tường + low-bat → go home. |
| **Alarm/timer Living.AI** | Hard (cloud) | Sync app/cloud Living.AI; local timer thì thuộc nhóm chắc. |
| **BT speaker** | Hard / chưa có stack | Bluedroid **A2DP sink** + AVRCP volume → `0x17`; Wi‑Fi + BT coexist; không có trong `CMakeLists` hiện tại. |
| **Smart light** | Blocked on-robot | Không phải HW EMO; Living.AI utilities / Hue. Hue đã có phía **`local-brain`**, không phải FW robot. |

### Function mới (chỉ khi mở khóa RE / hạ tầng)

| Function | Điều kiện mở |
|---|---|
| `behavior_go_home()` | Parse RX `0x62`, có home-station, map scene `0x38=4` / `0x65` |
| `photo_capture_pipeline()` | Bật an toàn lệnh SD/photo, RX file SPI, endpoint HTTP/BLE |
| `game_*_start()` bản CV | RE tay/ảnh + UART `0x53`/`0x35` ổn định |
| A2DP sink → `speaker_play` | Thêm component BT, test coexist Wi‑Fi |
| `light_set_hsl()` trên ESP | Bridge Hue/HA + credential (hoặc giữ tool trên PC) |

---

## 3. Cần làm gì để mở nhóm chưa chắc

### 3.1 Go home
1. RE / log UART khi stock go-home: sequence `0x38=4`, `0x65`, anim `gohome_connect`.
2. Implement RX **`0x62`** trong `k210_link.c` (hiện bỏ qua).
3. Có **home-station** BLE mesh trong lab; đo tọa độ track.
4. FSM: tìm hướng → walk có cliff → tiếp cận → `docked=true` + charge anim.
5. (Tuỳ) cảm biến “đang sạc” đáng tin hơn GPIO36.

### 3.2 Take photo đầy đủ
1. RE chuỗi capture stock (lệnh nào kích K210 chụp).
2. Chỉ bật danger SD/photo sau khi có backup SD + test trên bench.
3. RX SPI chunks (`0x04`/`0x05`/`0x55`), ghép file.
4. Đưa ra: SPIFFS/HTTP `/api/photo` hoặc BLE như app stock.
5. MCP chỉ gọi khi pipeline ổn — trước mắt giữ **pose only**.

### 3.3 Games CV / explore parity
1. CF/RE tiếp: RPS helper, detect mode `0x53`, colour blob `0x35`.
2. TTT: port logic board (đã có manh mối decompile) + nhập số qua voice/Xiaozhi tool.
3. Explore: FSM + timeout + web kill; không bật mặc định.
4. Demo anim ship trước; CV là phase 2.

### 3.4 Alarm Living.AI / smart light
1. Alarm cloud: reverse API Living.AI hoặc bỏ — dùng **alarm local** (SNTP + NVS).
2. Light: giữ Hue/`local-brain`; hoặc MCP Xiaozhi gọi PC/HA. Không giả lập trên servo EMO.

### 3.5 BT speaker
1. Thêm Bluedroid A2DP sink (ESP-IDF).
2. PCM → cùng ring `speaker.c` (48 kHz).
3. AVRCP → UART volume `0x17`.
4. Đo Wi‑Fi drop khi BT bật; có thể cần mode “chỉ loa” / tắt STA tạm.

---

## 4. Thứ tự đề xuất

```
[Done]   Dance / sing / greet / animal / volume / walk MCP
[Next]   camping_run + timer local + explore gated + game demos + MCP
[Later]  TTT logic thật, zombie mode tốt hơn
[RE]     0x62 go-home, photo SPI pipeline
[Big]    A2DP BT speaker, Hue-on-ESP (optional)
```

---

## 5. Một dòng

> **Chắc** = play `.mot` / gait / volume / script ngắn (kể cả camping & timer local & game demo).  
> **Chưa chắc** = cần track dock, camera/SD pipeline, BT stack, hoặc cloud Living.AI — phải RE / thêm subsystem trước khi hứa “như stock”.
