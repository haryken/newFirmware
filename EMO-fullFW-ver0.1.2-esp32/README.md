# EMO-Bridge (ESP32 firmware)

Replacement firmware for the **ESP32** inside the EMO desk robot (living.ai).
It keeps the **stock K210 firmware v3.1.0** untouched and drives it over
UART1 + SPI, turning the robot into a network device:

- **Microphone → server on wake word.** The K210 does wake word and voice
  activity detection; each utterance is streamed as 16 kHz mono PCM over one
  TCP connection to a voice server, with a PSRAM backlog so nothing is lost
  while the connection comes up. The server address is set on the web page.
- **Network → speaker.** `POST /api/play_pcm` with raw 48 kHz mono int16 PCM.
- **Legs.** Plays the stock `.mot` motion files (walk, turn, dances, reactions)
  from the ESP32's own SPIFFS partition on the four leg servos, with the
  paired face animation and song.
- **Screen and sounds.** Every K210 face animation, sound, volume and mode
  command is available by name on `/api/controls`.
- **Battery meter**, servo idle release, power-off, all from a small web page
  and a JSON REST API ([API.md](API.md)).

The voice server (Whisper, LLM, TTS, MCP) is a separate project; this
repository is the firmware only. Any program that implements the TCP format in
API.md section 3 can receive the audio.

## Protocol source of truth

[FIRMWARE_HANDOFF.md](FIRMWARE_HANDOFF.md) (byte-exact, from logic-analyzer
captures), [uart_commands.md](uart_commands.md) (every UART command, from the
stock firmware decompile) and
[emo-libre/newFirmware `Hardware/GPIO_MAP.md`](https://github.com/emo-libre/newFirmware/blob/main/Hardware/GPIO_MAP.md)
for the pins. The headers under `main/` each summarise the part of the protocol
they implement.

| Link | Pins (menuconfig → EMO-Bridge configuration) |
|---|---|
| UART1 1 Mbaud | TX GPIO12 → K210, RX GPIO4 ← K210 |
| SPI2 8 MHz mode 1 | SCLK 14, MOSI 15, MISO 2, CS 13 (manual) |
| Control | K210 RESETN GPIO22, power-enable GPIO23 (HIGH = on) |
| Battery | ADC1_CH3 = GPIO39 (11 dB, 64-sample average), sense input GPIO36 |
| Leg servos | 50 Hz PWM: left legs GPIO21, right legs GPIO25, left foot GPIO5, right foot GPIO26 |

## Build & flash

ESP-IDF **v6.0.x**, target `esp32`, WROVER-class module (PSRAM required; the
stock board has it). `sdkconfig.defaults` carries every non-default option.

```bash
idf.py set-target esp32
idf.py menuconfig          # optional: Wi-Fi, pins, SD-card safety, servo idle time
idf.py build flash monitor
```

`idf.py flash` also builds and flashes the SPIFFS image with the motion files
(`idf.py storage-flash` flashes only that image).

**First boot.** With no Wi-Fi configured (menuconfig `EMO_WIFI_SSID`, blank by
default) the robot opens the SoftAP `emo-bridge` / password `emobridge`; open
`http://192.168.4.1`, enter your network and the voice-server address, save,
and reboot. From then on the robot joins your network, prints its IP on the
serial console and shows it on its screen for 5 s at boot.

**Flashing the stock board.** The ESP32 is a standard module; flash it with the
usual UART bootloader (`idf.py -p <port> flash`). Back up the original
firmware first (`esptool.py read_flash 0 0x400000 stock.bin`) if you want to
go back.

## What happens at boot

1. Power-enable high, K210 reset pulse, `GET_VERSION` polled until the K210
   answers `3.1.0`, then the stock bring-up frames. The ESP32's IP is put on
   the text screen (waiting up to 20 s for Wi-Fi), stays 5 s, then the K210 is
   reset and brought up a second time, and only then the `boot_start` face plays.
2. `MODE_IDLE`: the K210's wake word is armed. On `WAKE` the link shows the
   `voice_wake_up` face, sends `MODE_MIC`, and opens a TCP connection to the
   server; every mic frame of the utterance is streamed. The K210's own VAD
   cuts the window ~0.7 s after any pause, so if voice was heard within the
   last 1.5 s the link re-opens a follow-up window and keeps streaming; the
   turn ends after 1 s of real silence (or 15 s total). Then the remaining
   bytes are flushed, the connection is closed (one connection per utterance)
   and `MODE_IDLE` re-arms the wake word.
3. Queued speaker audio switches the link to `MODE_SPK` (mic stops: the K210 is
   half-duplex), blocks are served on the K210's requests, the tail is padded,
   the K210 drains, and the wake word is re-armed. A wake word during playback
   cuts the playback and starts a new utterance (barge-in).

## Battery meter

`main/battery.c` reproduces the stock ESP32 measurement: ADC1 channel 3
(GPIO39) at 11 dB, 64 samples averaged, eFuse calibration, then
`vbat = 2·mV + 50` when GPIO36 reads low or `2·mV − 130` when it reads high.
The stock tiers become 20/40/60/80/100 % (interpolated for the meter); below
3299 mV the status reports `bat_low`. The tier table depends on the board
revision (`EMO_BAT_OLD_SERVO_THRESHOLDS` in menuconfig). `/api/status` adds
`bat_valid`, `bat_mv`, `bat_pct`, `bat_tier`, `bat_sense`, `bat_low`.

## Motion (.mot files)

`main/legs.c` plays the motion files on the four leg servos (`main/servo.c` is
the PWM). The files live on the ESP32's SPIFFS partition (`storage`, mounted at
`/spiffs`): `spiffs_image/mot/` holds the 582 stock files and `idf.py flash`
builds and flashes the image. File names up to 63 chars need
`CONFIG_SPIFFS_OBJ_NAME_LEN=64` (in `sdkconfig.defaults`).

Format and behaviour follow the stock firmware's `motion_parse_file` /
`animation_player` and were checked against every frame of the 582 files:

- 20-byte frames: `55 AA 55 AA`, type `02`, mode, 5 gait bytes, four servo
  angles (bytes 11–14, degrees 0–180, 90 = neutral), hold time in ms (bytes
  16–17), checksum over bytes 4–17, `ED`.
- mode `FE` (98 % of frames): all four servos ramp linearly to the new angles
  over the hold time, updated every 20 ms. Pulse = 500 + deg·2000/180 µs.
- mode `FD`: skipped.
- modes `01` and `02` (329 of the 360 gait frames): the stock oscillator gait,
  the open-source Otto biped oscillator (20 ms samples,
  `pos = A·sin(phase+phase0)+O` per servo, feet 90° out of phase, direction as
  a 180° phase flip, asymmetric hip amplitudes for turning). Walking, turning,
  edge reactions and startles work.
- the few remaining modes (dance generators, 31 frames): the pose is held for
  the frame's duration and the frame is counted in `unsupported`.

Every stock animation pairs a `.mot` with a face video and often a song.
`spiffs_image/mot_media.txt` (from the stock `profile.json`) maps each motion
to its media and the song's length, and `play` starts both unless
`"media":false`.

**Timing.** The file timing is what the robot plays by default: measured
against the songs, `d1_EmoDance` is 40.0 s of frames for a 40.1 s mp3 and the
median over the 13 dances is 0.97. Every 20 ms servo tick is placed on an
absolute schedule from the start of the motion, so servo writes never
accumulate drift, and the face/music request goes out after the legs are
already running. `/api/motion` reports `elapsed_ms` against `expected_ms` and
`music_ms`. Three ways to change the pace: a per-play `"speed"` factor,
`"speed":"music"` which scales the motion to end exactly with its song, and
the default for everything (`motion_speed` percent on the web page, menuconfig
`EMO_MOTION_SPEED_PCT`, 100 = as authored).

**Servo power.** LEDC at 50 Hz only while something moves: a holding servo
draws current the whole time and the robot stands fine with the legs limp, so
1.5 s after a motion, gait or pose ends the PWM is cut (`EMO_SERVO_IDLE_MS`,
0 = hold forever) and the next move powers them again. They stay unpowered
from boot unless `EMO_SERVO_HOLD_AT_BOOT` is on; "Servos off" on the web page
releases them at once.

## K210 SD card: what can touch it

The K210 keeps `profile.json`, every face video and every song on its SD card
and reads them itself whenever a face or a sound plays. This firmware never
writes there. The only stock commands that do are compiled out by default
(menuconfig `EMO_ALLOW_SD_COMMANDS`, off) and, when enabled, still need
`"confirm":true` on `/api/controls`:

| Group | Commands | Stock purpose |
|---|---|---|
| writes | `ota_begin` 23, `ota_file` 25, `ota_batch` 29, `ota_error` 2A, `ota_mot` 2B, `ota_path` ext 26, `ota_mot_66` 66, `ota_run_68` 68 | download .mot / resource files onto the SD |
| writes | `k210_update` 3A, `update_failed` 46 | K210 firmware update flow |
| writes | `photo_57` ext 57, `photo_png_done` 54, `photo_done` 56, `photo_58` 58 | photo / painting transfer |
| reads | `read_file` ext 03, `read_sd_buf` ext 40 | make the K210 push an SD file over SPI |
| reads (faces/sounds) | every ext `02` playlist, `play_video`, `play_audio` | the K210 reads the .avi/.mp3 from its SD; menuconfig `EMO_SD_FACES` (on by default) turns all of them off while the card is broken |
| reads, sent at boot | `2C` / `2D` | open/close the file session the stock uses around its profile.json read; menuconfig `EMO_BOOT_FILE_SESSION` (on by default) |

## HTTP API

Full reference, including every control action and the TCP stream format:
[API.md](API.md).

| Endpoint | Purpose |
|---|---|
| `GET /` | web page |
| `GET/POST /api/config` | settings JSON (`wifi_ssid`, `wifi_pass`, `host_ip`, `host_port`, `motion_speed`) |
| `POST /api/power` | `{"action":"off"}`: drops the power-enable pin, the whole robot switches off |
| `GET /api/status` | link/mic/speaker/host-stream/battery statistics |
| `POST /api/tone` | 0.5 s 440 Hz test tone on the speaker |
| `POST /api/play_pcm` | body = raw 48 kHz mono int16 LE → speaker |
| `GET /api/mic.wav` | records 3 s from the robot mic, returns a WAV |
| `GET /api/controls` | list of every named K210 command (`name`, `label`, `group`, `args`) |
| `POST /api/controls` | `{"action":"anim_stay_wink_1"}`, `{"action":"volume","value":80}`, `{"action":"screen_text","text":"hi"}` |
| `GET /api/motion` | servo pose, playback state and the list of `.mot` files |
| `POST /api/motion` | `{"action":"play","name":"look_around_1"}`, `{"action":"play","name":"d1_EmoDance","speed":"music"}`, `stop`, `release`, `{"action":"pose","pos":[90,90,90,90],"ms":500}` |

## Layout

One file per function. The K210 side is layered: transport (`k210_uart`,
`k210_spi`) → coordinator (`k210_link`: half-duplex state, RX dispatch, flags)
→ functions (`microphone`, `speaker`, `video`, `k210_boot`).

```
main/
  app_main.c        init order + callback wiring, nothing else
  k210_uart.c/h     UART1 transport, short/extended frame encoding, CRC-8
  k210_spi.c/h      SPI command/data phases, byte swap
  k210_link.c/h     link state (idle/listen/speak), RX task + dispatch, ACK/ANIM_DONE/VERSION flags,
                    power + reset pins, raw send API, stats
  k210_boot.c/h     the stock bring-up sequence, IP banner, double reset
  microphone.c/h    MODE_MIC, mic frames, utterance session (wake/end, hangover, watchdog)
  speaker.c/h       playback ring, MODE_SPK, SPK_REQ serving, test tone, barge-in
  video.c/h         face playlists, screen text
  servo.c/h         the four 50 Hz PWM outputs, idle release
  legs.c/h          .mot player, stock gait oscillator, motion media, speed
  battery.c/h       ADC battery meter (stock GPIO39 / GPIO36 path)
  k210_controls.c/h every named K210 command (uart_commands.md) for /api/controls
  net.c/h           Wi-Fi STA with SoftAP fallback
  settings.c/h      NVS-backed settings
  host_stream.c/h   mic → voice server TCP stream (one connection per utterance)
  http_server.c/h   HTTP server, serves web/
  rest_api.c/h      every /api/* endpoint
  web/              index.html, style.css, app.js (embedded at build time)
spiffs_image/
  mot/*.mot         stock motion files → SPIFFS "storage" partition
  mot_media.txt     motion → face/sound pairing (from the stock profile.json)
partitions.csv      nvs / phy / factory 2.5 MB / storage 1.4 MB
sdkconfig.defaults  PSRAM, 240 MHz, custom partition table, SPIFFS name length
```

## Credits

Protocol reverse engineering builds on the
[emo-libre](https://github.com/emo-libre) community work. The `.mot` motion
files and the K210 command set belong to the stock EMO firmware by living.ai
and are included for interoperability with an unmodified robot.
