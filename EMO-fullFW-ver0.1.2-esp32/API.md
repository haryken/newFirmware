# EMO-Bridge API

Everything the robot exposes on the network. Two interfaces:

1. **HTTP** on port 80 (`http://<robot-ip>/`): status, settings, speaker, mic capture, K210 controls.
2. **TCP audio stream** that the robot *opens* towards the voice server (`EMO_HOST_IP:EMO_HOST_PORT`, default `192.168.137.1:7007`), one connection per wake-word utterance.

The robot's IP is printed on the serial console at boot and shown in the web page header. If no Wi-Fi is configured the robot opens the SoftAP `emo-bridge` / `emobridge` and is reachable at `192.168.4.1`.

All JSON responses are `application/json`. Request bodies for POST endpoints are limited to 8 KB except `/api/play_pcm`, which streams.

---

## 1. HTTP endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | web page |
| GET | `/app.js` | web page script |
| GET | `/api/status` | link, mic, speaker, server-stream and battery state |
| GET | `/api/config` | current settings (password redacted) |
| POST | `/api/config` | save settings |
| POST | `/api/tone` | 0.5 s 440 Hz test tone on the speaker |
| POST | `/api/play_pcm` | play raw PCM on the speaker |
| GET | `/api/mic.wav` | 3 s mic capture as WAV |
| GET | `/api/controls` | list of every named K210 command |
| POST | `/api/controls` | send one K210 command |
| GET | `/api/motion` | leg servo pose, playback state, list of `.mot` motions |
| POST | `/api/motion` | play / stop a motion, go to a pose, release the servos |
| POST | `/api/power` | `{"action":"off"}`: switch the whole robot off |

### GET /api/status

```json
{
  "ip": "192.168.137.42",
  "linked": true,            "state": 0,                "version": "3.1.0",
  "boot_done": true,         "mic_open": false,         "mic_peak": 812,
  "mic_samples": 1310720,    "play_queued": 0,
  "host_connected": false,   "fed": 2621440,            "acked": 2621440,
  "backlog": 0,              "overflow": 0,             "reconnects": 0,
  "wake_events": 4,          "session": false,          "sessions": 4,
  "bat_valid": true,         "bat_mv": 3912,            "bat_pct": 68,
  "bat_tier": 4,             "bat_sense": false,        "bat_low": false
}
```

| Field | Meaning |
|---|---|
| `ip` | robot IPv4 |
| `linked` | a valid K210 UART frame was seen in the last 15 s |
| `state` | link state: `0` idle (wake word armed), `1` listening (mic streaming), `2` speaking |
| `version` | K210 firmware version string, `""` until the K210 answered |
| `boot_done` | K210 boot sequence finished |
| `mic_open` | `MODE_MIC` sent and no end-of-speech yet |
| `mic_peak` | absolute peak of the last mic chunk, 0..32767 |
| `mic_samples` | total 16 kHz mic samples received from the K210 |
| `play_queued` | 48 kHz samples waiting in the speaker buffer |
| `host_connected` | TCP connection to the voice server currently open |
| `fed` / `acked` | bytes handed to the stream / bytes the server acknowledged |
| `backlog` | bytes buffered in PSRAM waiting to be sent |
| `overflow` | bytes dropped because the backlog was full |
| `reconnects` | failed or retried server connections |
| `wake_events` | wake words heard since boot |
| `session` | an utterance is being streamed right now |
| `sessions` | utterances streamed since boot |
| `bat_valid` | battery measurement available |
| `bat_mv` | battery voltage in mV (stock formula, see README "Battery meter") |
| `bat_pct` | 0..100 %, stock tiers interpolated |
| `bat_tier` | stock level `battery_l1..l5` as 1..5 |
| `bat_sense` | level of the GPIO36 sense input (selects the calibration offset) |
| `bat_low` | below 3299 mV, the stock low-power threshold |

### GET /api/config

```json
{"wifi_ssid":"home","wifi_pass":"***set"}
```

`wifi_pass` is `"***set"` when a password is stored and `""` when none is.

### POST /api/config

Body: JSON object with any of the keys below. Unknown keys are ignored. Omitted keys keep their value, so send `wifi_pass` only to change it.

| Key | Type | Notes |
|---|---|---|
| `wifi_ssid` | string | STA network name |
| `wifi_pass` | string | STA password |

Response `{"ok":true}` or `{"ok":false}` (NVS write failed). Settings are stored in NVS and applied at the **next reboot**.

```sh
curl -X POST http://<robot>/api/config -H "Content-Type: application/json" \
     -d '{"wifi_ssid":"home","wifi_pass":"secret"}'
```

### POST /api/tone

No body. Queues 0.5 s of 440 Hz at 48 kHz on the speaker. Response `ok`.

### POST /api/play_pcm

Body: raw **48 kHz, mono, signed 16-bit little-endian** PCM, `Content-Type: application/octet-stream`. Any length. The request blocks while the robot drains its 0.68 s speaker buffer, so a 10 s clip takes about 10 s to return. Response `ok`.

While audio plays the link is in `MODE_SPK` and the mic is off (the K210 is half-duplex); the wake word is re-armed about 0.9 s after the last block.

```python
import wave, urllib.request
pcm = wave.open("reply_48k_mono.wav").readframes(10**9)
urllib.request.urlopen(urllib.request.Request(
    "http://<robot>/api/play_pcm", data=pcm, method="POST",
    headers={"Content-Type": "application/octet-stream"}), timeout=120)
```

### GET /api/mic.wav

Opens the mic if it is closed, records 3 s, returns `audio/wav` (16 kHz mono 16-bit). Fails with 500 only on out-of-memory. Returns fewer than 3 s if the K210 closes the window early. Add a cache-buster (`?t=…`) in browsers.

### GET /api/controls

Array describing every named K210 command (see section 2):

```json
[{"name":"volume","label":"Volume","group":"Audio",
  "hint":"55 AA 17 v: boot sends 0x68 (104); BT volume handler",
  "args":[{"name":"value","type":"num","def":104}]}, …]
```

`args` lists the optional parameters a `POST` may pass and their defaults. `type` is `num` or `str`.

### POST /api/controls

Body: `{"action":"<name>", …optional args}`.

| Arg | Used by | Meaning |
|---|---|---|
| `value` | 16-bit and 24-bit commands | goes out as D0 = low byte, D1 = next, D2 = high (24-bit only) |
| `d0`, `d1`, `d2` | three-byte commands | raw data bytes |
| `text` | extended frames | payload string (max 254 chars, NUL appended, CRC-8/MAXIM) |
| `name`, `loop`, `mp3` | `anim` | asset name, loop 0/1, `mp3: 1` adds `/spiffs/mp3/NAME_000.mp3` |

Numbers may be sent as JSON numbers or as strings (`"0x68"` works). Omitted args use the defaults from `GET /api/controls`.

Responses:

| HTTP | Body |
|---|---|
| 200 | `{"ok":true,"sent":"55 AA 17 68 00 00 7E"}` for small frames |
| 200 | `{"ok":true,"sent":"55 AA FF 16 00 44 58 +22 bytes crc 3A","ack":true}` for extended frames; `ack` = the K210 answered `FE` within 500 ms |
| 200 | `{"ok":true,"sent":"STOP_ANIM, PLAY_ANIM stay_wink_1 loop=0","ack":true}` for animations |
| 400 | `{"ok":false,"error":"missing action"}`, `"bad argument …"`, `"payload too long …"` |
| 404 | `{"ok":false,"error":"unknown action"}` |
| 409 | `{"ok":false,"error":"K210 busy (booting or speaking)"}` |
| 403 | `{"ok":false,"error":"OTA / SD-card command: ..."}`: the action is flagged `danger` in `GET /api/controls` (OTA, file download, photo transfer, SD reads: the stock paths that write the K210 SD card or update its firmware). By default these are compiled out (`disabled: true` in the list, menuconfig `EMO_ALLOW_SD_COMMANDS` off); when enabled they still need `"confirm":true` |

```sh
curl -X POST http://<robot>/api/controls -H "Content-Type: application/json" -d '{"action":"anim_stay_wink_1"}'
curl -X POST http://<robot>/api/controls -H "Content-Type: application/json" -d '{"action":"anim","name":"chatgpt_speak","loop":1}'
curl -X POST http://<robot>/api/controls -H "Content-Type: application/json" -d '{"action":"volume","value":80}'
curl -X POST http://<robot>/api/controls -H "Content-Type: application/json" -d '{"action":"screen_text","text":"hello"}'
curl -X POST http://<robot>/api/controls -H "Content-Type: application/json" -d '{"action":"clock_show","d0":14,"d1":30,"d2":100}'
```

### GET /api/motion

```json
{"fs_ok":true,"playing":true,"loop":false,"name":"look_around_1","frame":3,"frames":8,
 "unsupported":0,"pos":[90,90,80,70],"powered":true,"plays":2,"files":582,
 "fs_used":612352,"fs_total":1374476,
 "list":["BUG_shooting","Byebye","Camp_barbecue_end", …]}
```

| Field | Meaning |
|---|---|
| `fs_ok` | the SPIFFS `storage` partition is mounted (false = flash the image) |
| `playing`, `loop`, `name`, `frame`, `frames` | current or last motion and its progress |
| `media` | face/sound asset started with the motion (`""` = none) |
| `speed`, `expected_ms`, `elapsed_ms` | timing factor, the file's total duration at that speed, and the measured wall time of the current/last playback (compare the two if a motion looks slow) |
| `unsupported` | frames of the current motion that use a stock gait generator and were held instead of played |
| `pos` | commanded angles in degrees: left legs, right legs, left foot, right foot |
| `speed`, `speed_default` | factor of the current motion and the default used when a play gives none |
| `expected_ms`, `elapsed_ms`, `music_ms` | file time at that speed (gait frames = cycles x period), wall time so far / of the last playback, length of the paired song (0 = none) |
| `powered` | PWM active; false after `release` and, by default, 1.5 s after any move ends (menuconfig `EMO_SERVO_IDLE_MS`): the servos go limp to save battery and power up again on the next move |
| `plays` | motions started since boot |
| `files`, `list` | `.mot` files found, names without the extension, sorted |
| `fs_used`, `fs_total` | SPIFFS bytes |

### POST /api/motion

| Body | Effect |
|---|---|
| `{"action":"play","name":"look_around_1"}` | play `/spiffs/mot/look_around_1.mot` once (stops a running motion first); add `"loop":1` to repeat until stopped. `"media":false` plays the legs only (default: the paired face animation + music start on the K210 at the same moment, from `mot_media.txt` / profile.json, e.g. `d1_EmoDance` with its song). `"speed":2.0` scales the file timing (0.25..4, 1 = as authored; absent = the default speed, setting `motion_speed`); `"speed":"music"` scales the motion to end exactly with its song |
| `{"action":"stop"}` | stop the motion, servos keep holding the current pose |
| `{"action":"pose","pos":[90,90,90,90],"ms":500}` | move the four servos (left legs, right legs, left foot, right foot, degrees 0..180) over `ms` milliseconds (default 500, max 10000); blocks until done |
| `{"action":"release"}` | cut the PWM, servos go limp; any later move powers them again |
| `{"action":"gait","kind":"walk","reverse":false,"cycles":2}` | run the stock procedural gait (walk or turn) for `cycles` periods, `reverse` = backward / right. Optional `period_ms`, or raw oscillator arrays `A`, `O`, `phase0` (4 values each) and `dir` to override the preset |

Responses: `{"ok":true}`; `400 {"ok":false,"error":"no such motion"}` (also `bad name`, `file has no valid frames`, `SPIFFS not mounted`, `pos must be 4 numbers 0..180`); `404` for an unknown action.

```sh
curl -X POST http://<robot>/api/motion -H "Content-Type: application/json" -d '{"action":"play","name":"look_around_1"}'
curl -X POST http://<robot>/api/motion -H "Content-Type: application/json" -d '{"action":"pose","pos":[90,90,120,60],"ms":300}'
```

Frames in mode `FE` move the servos; mode `FD` frames are skipped; modes `01`
(walk-type) and `02` (turn-type) run the stock oscillator gait with the frame's
parameters, so the explore/cliff/react files really move the robot; the few
remaining modes (dance generators) are held for their duration and counted in
`unsupported`.

---

## 2. Control actions

Every ESP32 → K210 command from `uart_commands.md`, with the bytes sent when no args are given. Commands whose purpose is unknown keep their command byte in the name. "✓" = verified in captures or firmware strings, "~" = inferred from call-site context, "?" = purpose unknown.

### Face animations

Each preset sends `STOP_ANIM` (`55 AA 09 00 00 00 08`), waits up to 200 ms for `ANIM_DONE`, then the extended frame `02` with the playlist JSON `[loop,[["/spiffs/avi/NAME.avi"(,[[0,"/spiffs/mp3/NAME_000.mp3"]])]]]`.

| Action | Asset | Loop | Sound | |
|---|---|---|---|---|
| `anim_stop` | — (STOP_ANIM only) | | | ✓ |
| `anim_boot_start` | `boot_start` | 0 | yes | ✓ |
| `anim_stay_wink_1` | `stay_wink_1` | 0 | | ✓ |
| `anim_stay_a_b` | `stay_a_b` | 0 | | ✓ |
| `anim_stay_c_d` | `stay_c_d` | 0 | | ✓ |
| `anim_voice_wake_up` | `voice_wake_up` | 0 | yes | ✓ |
| `anim_listen_and_think` | `listen_and_think` | 1 | | ✓ |
| `anim_chatgpt_start` | `chatgpt_start` | 0 | | ✓ |
| `anim_chatgpt_hear` | `chatgpt_hear` | 0 | yes | ✓ |
| `anim_chatgpt_stay` | `chatgpt_stay` | 1 | | ✓ |
| `anim_chatgpt_speak` | `chatgpt_speak` | 0 | | ✓ |
| `anim_speak4` / `anim_speak8` / `anim_speak10` | `speak4` / `speak8` / `speak10` | 0 | | ✓ |
| `anim_keep_vigilant1` | `keep_vigilant1` | 0 | | ✓ |
| `anim_emonews_loop1` | `EMOnews_loop1` | 1 | | ✓ |
| `anim_emonews_loop2` | `EMOnews_loop2` | 1 | | ✓ |
| `anim_emonews_loop3` | `EMOnews_loop3` | 1 | | ✓ |
| `anim_emonews_loop4` | `EMOnews_loop4` | 1 | | ✓ |
| `anim_daily_boxing_start` | `Daily_Boxing_start` | 0 | | ✓ |
| `anim_daily_boxing_loop1` | `Daily_Boxing_loop1` | 1 | | ✓ |
| `anim_daily_boxing_end` | `Daily_Boxing_end` | 0 | | ✓ |
| `anim` | `name` arg (letters, digits, `_`, `-`) | `loop` arg | `mp3` arg | ✓ |

### Screen & media

| Action | Frame | Args | |
|---|---|---|---|
| `play_video` | ext `18` "/spiffs/avi/….avi" | `text` | ✓ |
| `play_video_loop` | ext `1A` (flag = 1 variant) | `text` | ✓ |
| `play_audio` | ext `19` "/spiffs/mp3/….mp3" | `text` | ✓ |
| `play_audio_loop` | ext `1B` (flag = 1 variant) | `text` | ✓ |
| `screen_text` | ext `44`, factory-test text screen | `text` | ~ |
| `clock_show` | `3E d0 d1 d2` time fields, d2 = 0x64/0x65 | `d0 d1 d2` (12, 30, 100) | ~ |
| `alarm_text` | ext `3D` alarm list text | `text` | ~ |
| `photo_show` | `48 01` | `value` | ~ |
| `screen_color_5a` | `5A d0 d1 d2`, possibly a colour | `d0 d1 d2` (FF FF FF) | ? |
| `screen_info_60` | `60 6F` | `value` | ? |
| `screen_09` | `09 03` / `09 04` (campfire, Fit, test screen) | `value` (24-bit) | ? |
| `image_search` | ext `0E` | `text` | ? |
| `cmd_28_mp3` | ext `28` "mp3" | `text` | ? |

### Audio

| Action | Frame | Args | |
|---|---|---|---|
| `mode_idle` | `06 01 10 00` wake word armed | | ✓ |
| `mode_mic` | `4B 00 00 00` then `06 03 10 00`, through the link so its state machine stays in sync | | ✓ |
| `mode_spk` | `06 09 10 00`; the K210 then requests blocks until `mode_idle` | | ✓ |
| `audio_mode` | `06 …` raw bit field: 01 wake word, 02 mic, 08 speaker | `value` (24-bit, default 0x1001) | ✓ |
| `volume` | `17 v` (boot sends 0x68 = 104) | `value` | ~ |
| `volume_mute` | `17 00` | | ~ |
| `volume_max` | `17 FF` | | ~ |
| `followup_mic` | `4B 00`, sent before `MODE_MIC` on turns without a wake word | | ~ |

### Boot & link

| Action | Frame | Args | |
|---|---|---|---|
| `boot_gate` | `34 00` first command after the reset pulse | | ~ |
| `get_version` | `69 01`, K210 answers ext `69` "3.1.0" | | ✓ |
| `fw_version_number` | `3C 00`, K210 answers small `3C` | | ✓ |
| `request_0a` | `0A 00`, K210 answers ext `0A` | | ~ |
| `scene` | `38 n`: 1 normal, 4 go-home, 5 games, 6 dialogue | `value` (1) | ~ |
| `home_station_65` | `65 EB` (also 01, 6F, FF) | `value` (0xEB) | ~ |
| `boot_3b` / `boot_70` / `boot_6a` / `boot_6c` / `boot_74` | `3B 01` / `70 00` / `6A 46` / `6C 00` / `74 00` stock boot frames | `value` | ~ |
| `main_6b` / `main_5d` | `6B 01` / `5D 00` | `value` | ? |
| `update_failed` | `46 00` | | ~ |

### Files & OTA

| Action | Frame | Args | |
|---|---|---|---|
| `file_begin` / `file_end` | `2C 00` / `2D 00` wrap every K210 SD read | | ✓ |
| `read_file` | ext `03` path. **Avoid**: the K210 pushes file chunks over SPI that this firmware does not read | `text` | ✓ |
| `read_sd_buf` | ext `40` path, same SPI push | `text` | ✓ |
| `ota_begin` / `ota_file` / `ota_batch` / `ota_error` / `ota_mot` | `23` / `25` / `29` / `2A` / `2B` | | ~ |
| `ota_path` | ext `26` path/name buffer | `text` | ~ |
| `k210_update` | `3A v` | `value` | ~ |
| `ota_mot_66` | `66 v` | `value` | ~ |
| `ota_run_68` | `68 01` or `68 65`, K210 answers `SEND_RUN_OTA` | `value` (1) | ~ |
| `photo_png_done` / `photo_done` / `photo_58` | `54` / `56` / `58` | | ~ |
| `photo_57` | ext `57` photo/painting data or name | `text` | ~ |

### Behaviour & games

| Action | Frame | Args | |
|---|---|---|---|
| `cmd_0c` | `0C …` 24-bit parameter | `value` | ? |
| `showoff_15` | `15 n` (0 / 1 / 3) | `value` (1) | ? |
| `chat_1c` | `1C n` on/off | `value` (1) | ? |
| `sleep_1f` | `1F n` on/off | `value` (1) | ? |
| `blob_detect` | `35 colour`, K210 answers ext `35` x/w/h | `value` (0) | ~ |
| `voice_36` | `36 n` on/off | `value` (1) | ? |
| `test_41` / `test_42` | `41 n` (1..3) / `42 01` | `value` | ? |
| `ble_43` | `43 n` on/off | `value` (1) | ? |
| `sleep_4a` / `sleep_4d` / `sleep_4f` / `sleep_50` | `4A 00` / `4D 00` / `4F 00` / `50 …` | `value` | ? |
| `sleep_4c` / `sleep_4e` | ext `4C` / `4E` | `text` | ? |
| `chat_52` | `52 …` | `value` | ? |
| `chat_5b` | `5B d0 d1` (pairs seen: 09 02, FF 00, FF 01) | `d0 d1 d2` (FF 01 00) | ? |
| `game_detect_53` | `53 n` (0 / 1 / 2 / FF) | `value` (0) | ? |
| `game_59` | `59 n` (03 14 28 46 48 49 4B) | `value` (3) | ? |
| `fruit_5e` | `5E d0 d1` (00 00 / 00 02) | `d0 d1 d2` | ? |
| `showoff_5f` | `5F d0 d1` (00 00 / 00 03 / 01 03) | `d0 d1 d2` (00 03 00) | ? |
| `daily_routine_61` | `61 n` (0..4; stock sends `61 00` on wake) | `value` (0) | ? |
| `daily_routine_49` | ext `49` descriptor, e.g. "Daily_Boxing(005+1)" | `text` | ~ |
| `spray_62` / `spray_6d` | `62 n` / `6D n` on/off | `value` (1) | ? |
| `voice_67` | `67 n` (00 / 64 / 65 / 66) | `value` (0) | ? |
| `campfire_6e` / `campfire_72` | `6E n` / `72 n` on/off | `value` (1) | ? |
| `dice_6f` | `6F step d1 d2` (step 1..0B, dice or 63) | `d0 d1 d2` (01 00 00) | ? |
| `fit_timer_73` | `73 d0 d1 d2` (00 00 00 / 01 1E 00) | `d0 d1 d2` (01 1E 00) | ? |
| `face_name_51` | ext `51` name for "hi %s." | `text` | ~ |

Wire formats, for reference: small frame `55 AA CMD D0 D1 D2 CS` with `CS = (CMD + D0 + D1 + D2 − 1) & 0xFF`; extended frame `55 AA FF LEN_LO LEN_HI SEL CS <payload + NUL> PCS` with `PCS` = CRC-8/MAXIM over the payload including the NUL.

---

## 3. Audio stream to the voice server (robot → server, TCP)

The robot connects to `EMO_HOST_IP:EMO_HOST_PORT` (menuconfig, default `192.168.137.1:7007`) when the K210 reports its wake word, streams the utterance, and closes the connection when the turn ends. One connection per utterance. All integers little-endian.

**Hello** (19 bytes, once per connection):

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `"EMOA"` |
| 4 | 1 | protocol version `1` |
| 5 | 4 | sample rate, `16000` |
| 9 | 1 | channels, `1` |
| 10 | 1 | bits per sample, `16` |
| 11 | 8 | stream offset of the first chunk (bytes since boot) |

**Chunk** (repeated):

| Offset | Size | Value |
|---|---|---|
| 0 | 8 | stream offset of this chunk's first byte |
| 8 | 2 | payload length |
| 10 | 4 | CRC-32 (IEEE, as `zlib.crc32`) of the payload |
| 14 | n | signed 16-bit mono PCM at 16 kHz |

**ACK** (server → robot, after every accepted chunk): `"ACK0"` + 8-byte offset = stream position acknowledged so far. The robot keeps unacknowledged bytes in PSRAM and resends them on the next connection if the server never ACKed them, so a chunk whose CRC fails should simply not be ACKed. At the end of the utterance the robot flushes, waits up to 300 ms for the last ACK, and closes. The utterance window is the K210's own voice-activity detection, extended by the link when speech continued within 1.5 s of a cut, with a 15 s cap.

Any TCP server that speaks this format can receive the audio; the reference voice server (Whisper + LLM + TTS, with an MCP server) is kept in a separate repository.

---

## 4. Serial console

Not an API, but useful alongside it: the UART monitor (`idf.py monitor`) logs every K210 frame other than mic/speaker traffic (`RX 55 AA …`), every control sent (`ctl: …`), the battery once a minute (`bat:`), and a heartbeat every 10 s (`link=1 state=0 rx=… mic=…`).
