# RE: behavior + foot/cliff sensors (EMO / emo_ota0.elf)

Ghidra notes. Goal: explore + cliff avoid (không decompile full firmware).

## Kết luận

| Mục tiêu | Đủ? |
|----------|-----|
| Đọc 4 cliff + threshold + mask + phản ứng | **Có** (`cliff_avoid_task`) |
| Danh sách anim + **có/không âm thanh** khi lùi | **Có** (từ `mot_media.txt` + play path) |
| Map bitmask → `cliff_L`/`cliff_R` + sound khi lùi | **Có** (`cliff_edge_react` `@402024f8`) |

---

## Create Function — trạng thái

Cách làm: nếu `F` float → **Clear (`C`)** → **Disassemble (`D`)** → **Create Function**.

| Addr | Tên | Status |
|------|-----|--------|
| `4012eeb8` | `foot_cliff_read` | Done |
| `4012f104` | `foot_sensor_sample_vt` | Done |
| `4012cae4` | `cliff_avoid_task` | Done |
| `40201dc0` | `go_home_charging_flow` | Done (không phải cliff) |
| `402022c0` | look_around / turn_left loop | Done (không phải cliff) |
| `402023d8` | `play_status_event_anim` | Done (headset/env/… — không cliff) |
| `402024f8` | `cliff_edge_react` | Done |
| `401f282c` | `touch_petting_react` | Done |
| `401e5fd8` | `sleep_wake_anim` | Done |
| `401ea3a0` | `explore_timed_look` | Done (không phải gait FSM) |
| `401e6150` | `shake_react` | Done |
| `401ed998` | `sensor_debug_hud` | Done (debug overlay) |
| `401f5504` | `chatgpt_voice_session` | Done (không phải register_face) |
| `401f26a4` | `stay_agile_begin_wait` | Done |
| `401e6404` | `flip_a_coin_react` | Done |
| `401f56a4` | `show_painting_react` | Done |
| `401e64d8` | `stay_agile_step` | Done |
| `401f59a0` | `tictactoe_game` | Done (không phải register_face) |
| **`401f5f0c`** | register_face? (PARAM @`401f612e`) | **Còn CF** |
| **`401ecb94`** | RPS (`R_P_S_*`) | **Còn CF** |

Đã có từ trước: `foot_sensor_get`, `foot_sensor_sample`, `foot_sensor_check_id`, `foot_sensor_destroy`, `foot_cliff_calibrate`, `foot_cliff_update_mask`, `sensor_power_cliff_tick`, `play_named_animation`, `behavior_object_get`, `animation_player_get`, `imu_object_get`, `imu_probe_id`, `react_fall_sequence`.

---

## Foot / cliff sensor object

Singleton: **`foot_sensor_get`** `@4012ef48` → global **`g_foot_sensor`** `@401084b0`.

| Slot | Addr | Role |
|------|------|------|
| vtable +0 | `foot_sensor_sample_vt` `@4012f104` | gọi `foot_sensor_sample` |
| vtable +4 | `foot_sensor_destroy` `@4012eecc` | free singleton |
| vtable +8 | `foot_cliff_read` `@4012eeb8` | đọc 1 kênh |
| obj +0x10..+0x16 | 4× `uint16` | giá trị đã lọc |

### `foot_cliff_read(ch)` `@4012eeb8`

```c
return *(uint16_t *)(*g_foot_sensor + (ch + 8) * 2);
// ≡ obj + 0x10 + ch*2,  ch = 0..3
```

### I2C / sample

- **`foot_sensor_check_id`** `@4012eee4`: bus `0`, addr **`0x40`** expect `@`, addr **`0x41`** expect `A`.
- **`foot_sensor_sample`** `@4012ef80`: `read(0,0x40,buf,5)` + `read(0,0x41,…)`; EMA → 4 kênh (seed `0x07FF`); log `error raw = …`.
- Hai FOOT MCU I2C — **không** phải GPIO ESP32 thuần.

---

## Cliff policy — `cliff_avoid_task` `@4012cae4`

Loop cảm biến chính (gọi `foot_sensor_get`, `sensor_power_cliff_tick`, đọc 4 ch qua vtable+8):

1. Định kỳ `sensor_power_cliff_tick(200)`.
2. Nếu 4 kênh đều **trên** threshold “an toàn” → đếm / clear flag.
3. Ngược lại: đọc 4 ch vào `local_60[]`, dựng **bitmask**:
   - bit0..3 = `ch[i] < thr[i] / scale` (scale khác nhau tùy mode; có nhánh `thr >> 1`).
4. Mask hợp lệ → ghi hướng cliff (`robot+0x40c7`), gọi behavior:
   - `(behavior+0x17c)(mask)` hoặc `FUN_4012a418(2, 4, mask)`
   - go-home: log `go home cliff cnt` / `go home cliff find` rồi `FUN_4012a418(2, 4, mask, 0)` sau vài lần.
5. Nhánh `robot+0x41fa != 0` → chỉ **`foot_cliff_update_mask()`** (ghi mask `@robot+0x41fe`).

### Threshold helpers

| Fn | Addr | What |
|----|------|------|
| `foot_cliff_calibrate` | `4012ad00` | ch0 > `0x32` (50) → seed thr |
| `foot_cliff_update_mask` | `4012bb50` | 4 ch → bitmask `@robot+0x41fe` |
| `sensor_power_cliff_tick` | `4012c4b0` | `< 0x32`, `> 200`, `> 500` (+ IMU) |

---

## Anim + âm thanh khi lùi / tránh vực

Nguồn: `EMO-fullFW-ver0.1.2-esp32/spiffs_image/mot_media.txt`  
Format: `mot_name media_name mp3flag duration_ms`

Play path stock (K210): tên anim → `/spiffs/avi/%s.avi` + nếu `mp3flag=1` → **`/spiffs/mp3/%s.mp3`** (`FUN_401db998`). Custom FW: `legs.c` `media_for()` cùng bảng.

### Có tiếng (mp3flag=1)

| Motion | Media | MP3 | ~ms |
|--------|-------|-----|-----|
| `cliff_B` | `cliff_B` | **có** | 3048 |
| `cliff_L` | `cliff_L` | **có** | 2064 |
| `cliff_R` | `cliff_R` | **có** | 2064 |
| `cliff_LB` | `cliff_LB` | **có** | 3048 |
| `cliff_RB` | `cliff_RB` | **có** | 3048 |
| `cliff_LF1` | `cliff_LF1` | **có** | 3048 |
| `cliff_LF2` | `cliff_LF2` | **có** | 3048 |
| `cliff_RF1` | `cliff_RF1` | **có** | 3048 |
| `cliff_RF2` | `cliff_RF2` | **có** | 3048 |
| `cliff_react_left_1..3` | cùng tên | **có** | 4056 |
| `cliff_react_right_1..3` | cùng tên | **có** | 4056 |
| `react_back_1..5,7,8` | cùng tên | **có** | 4056 |
| `react_back_6` | cùng tên | **có** | 5064 |

→ **Khi play `cliff_*` / `cliff_react_*` / `react_back_*` thì có face avi + file mp3 cùng tên** (không phải gait im lặng).

### Không media/mp3 trong bảng

| Motion | mp3flag |
|--------|---------|
| `explore_back_away1..3` | **0** (chỉ chân / không pair avi+mp3 trong `mot_media.txt`) |

### Có string ELF nhưng không có dòng `mot_media` (FW image này)

`cliff_F`, `cliff_F2`, `cliff_LF3`, `cliff_RF3` — có trong `emo_ota0.elf` rodata; **không** liệt kê trong `mot_media.txt` hiện tại. Có thể chỉ trên stock SPIFFS đầy đủ / path anim khác.

### Dispatch từ sensor

`cliff_avoid_task` khi mask ≠ 0:

- `behavior+0x17c(mask)` và/hoặc **`FUN_4012a418(2, 4, mask)`** → handler **`cliff_edge_react`** `@402024f8` (anim player vtable slot `@401d22ac`).
- Go-home cliff: `FUN_4012a418(2, 4, mask, 0)` sau vài lần đếm.

### `cliff_edge_react` — mask → hành vi + âm thanh

`param_2+8` = cliff bitmask (cùng nghĩa `robot+0x40c7` / bits từ 4 kênh). Log `on_edge3`, `edge %d`.

| mask | Hành vi chính |
|------|----------------|
| 1 | Lùi/pose (`FUN_40264af4` dir~1) + **face/mp3 random** từ bảng `@401d1d50` |
| 2 | Lùi dir~2 + random face/mp3; có nhánh “edge again” |
| **3** | Play named **`cliff_L`** (`FUN_402019a0` → anim+media) |
| 4 | Lùi + random face/mp3 |
| 6 | Lùi + random face/mp3 |
| 8 | Lùi + random face/mp3 |
| 9 | Lùi + random face/mp3 |
| **0xC (12)** | Play named **`cliff_R`** |
| (state đặc biệt mask 2/4) | Thêm **`mood_wronged`** |

Âm thanh trong react:

1. **`FUN_401db9f0(name,…)`** = avi `/spiffs/avi/%s.avi` + mp3 **`/spiffs/mp3/%s_000.mp3`** (có tiếng).
2. Play **`cliff_L` / `cliff_R`** qua `FUN_402019a0` / `play_named_animation` → pair theo `mot_media` (`mp3flag=1`, file `%s.mp3`, ~2s).
3. Kết thúc: `foot_cliff_calibrate()`, clear mask `@0x40c7`.

→ **Khi lùi tránh vực stock có âm thanh** (face clip `_000.mp3` và/hoặc `cliff_L`/`cliff_R.mp3`), không chỉ servo im lặng.

---

## Explore skeleton (custom FW)

1. Lấy 4 cliff: telemetry `cliff[]` hoặc I2C `0x40`/`0x41` + filter.
2. Bitmask: `ch[i] < thr` (thr calibrate / thô `< 50`).
3. Bit set → `play_motion("cliff_L"|… , media=true)` để **có âm thanh** như stock; hoặc `explore_back_away*` nếu chỉ muốn lùi **không** mp3.
4. `behavior_task` = queue; policy = `cliff_avoid_task`, không GPIO ISR.
