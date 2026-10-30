# EMO ESP32 ⇄ K210 UART command reference

Source: `ghidraExtracted-elfPartition1.c` (original ESP32 firmware, build `ad1f03d99-dirty`), cross-checked against the captures in your `emo_k210.c` and the verified frames in `FIRMWARE_HANDOFF.md`. UART1, 1 Mbaud 8N1, ESP32 TX = GPIO12, RX = GPIO4 (read from the firmware).

Confidence: ✅ confirmed by firmware strings or your captures · 🟡 inferred from call-site context · ❓ mechanism known, purpose unknown.

Totals: **62** small commands + **17** extended selectors ESP32 → K210; **35** small commands + **5** extended selectors K210 → ESP32.

## 1. Frame formats

### 1.1 Small frame (7 bytes, both directions)

```
55 AA CMD D0 D1 D2 CS        CS = (0x55 + 0xAA + CMD + D0 + D1 + D2) & 0xFF
```

- Built by `FUN_401d91d4(cmd, d0, d1, d2, wait)`. It is normally queued to `k210_uart_trans_task` (queue `0x3ffc9fac`), and written straight to UART1 when that task is idle.
- The K210 does not ACK small frames.
- RX parser `k210_uart_recv_task` (`FUN_401d8768`): state machine `55` → `AA` → 7 bytes. A bad checksum is only logged; the frame is still dispatched.

### 1.2 Extended frame (header + payload)

```
55 AA FF LEN_LO LEN_HI SEL CS   <LEN payload bytes>   PCS
CS  = sum of the 6 header bytes (& 0xFF)
PCS = CRC-8/MAXIM (Dallas/1-Wire: poly 0x31 reflected = 0x8C, init 0x00) over the payload
```

- Built by `FUN_401d92c8(sel, ptr, len, wait)`. The send task writes the 7-byte header, then the payload, then PCS (`FUN_40263850`, lookup table at `0x3f44d4a0`).
- String payloads include the terminating NUL (the wrappers pass `strlen + 1`), and PCS covers that NUL.
- After each extended frame the ESP32 waits up to 500 ms for an ACK frame (RX 0xFE). The stock K210 sends `55 AA FE AA 00 00 A7` (d0 = 0xAA). d0 == 0x55 is a special case that only resets the ESP32 RX parser, and a timeout logs `resault == pdFALSE, ack = %d`. Skipped when state+0x3E61 == 1.
- **Fix for your firmware:** PCS is not a byte sum. CRC-8/MAXIM reproduces all 5 payload checksums captured in `emo_k210.c` (`0xC4`, `0x54`, `0x10`, `0x04`, `0x70`), so `emo_k210_play_anim` can compute PCS for any animation name instead of using the hard-coded table:

```c
static uint8_t crc8_maxim(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0x8C) : (uint8_t)(c >> 1);
    }
    return c;   /* call with len = strlen(payload) + 1 so the NUL is included */
}
```

### 1.3 Bulk data goes over SPI

Audio, mic frames and file contents do not travel over the UART. The K210 announces them with a small frame (RX 0x00, 0x01, 0x04, 0x13; length = d0 + (d1 << 8)), then the ESP32 moves the bytes over SPI.

- **Bus (from `spi_init`, `FUN_401d9c6c`):** SPI2, MOSI = GPIO15, MISO = GPIO2, SCLK = GPIO14, 8 MHz (`0x7A1200`), mode 1, DMA on, queue 7. CS = GPIO13 as a plain GPIO: the pre-transaction callback drives it low and the post-transaction callback drives it high, so the command and the data each get their own CS window.
- **UART (from `FUN_401d86c0`):** UART1, 1 000 000 baud, 8N1, TX = GPIO12, RX = GPIO4, RX buffer 1 KB.
- **8-byte command phase** (`spi_trans_data` = `FUN_401d9e1c` for writes, `spi_recv_data` = `FUN_401d9ec4` for reads):

```
[0] a   [1] b   [2] len_lo   [3] len_hi   [4] type   [5] (a + b + len_lo + len_hi + type) & 0xFF   [6] 00   [7] 00
a = b = 0 at every call site; type: 0x00 speaker write (RX 0x00), 0x01 mic read (RX 0x01),
0x02 file chunk read (RX 0x04), 0x05 stream read (RX 0x13)
```

- **Then:** `ets_delay_us(500)`, then the data phase of `(len + 63) & ~63` bytes (padded to a multiple of 64).
- **Byte order:** every 32-bit word is byte-reversed (`FUN_40274a98`, over `(len + 3) & ~3` bytes). Writes swap before sending (between the 500 µs delay and the data phase, likely why the write gap measures ~1 ms instead of ~0.57 ms), reads swap after receiving. That is why mic samples come out "two per word in reversed order" and speaker samples big-endian.
- **Check against the handoff vectors:**
  - Mic: len 0x400, type 1 → `00 00 00 04 01 05 00 00` ✓
  - Speaker: len 0x3000, type 0 → `00 00 00 30 00 30 00 00` ✓
  - Speaker tail: `00 00 F0 nn 00 mm 00 00` is simply len = nn·256 + 0xF0 with mm = checksum (0xF0 + nn = nn − 0x10 mod 256), and a data phase of (nn + 1)·256 bytes. Any length works with this rule.
  - File chunk (10240 bytes) would be `00 00 00 28 02 2A 00 00`.

## 2. ESP32 → K210, small frames (62 commands)

"Sites" = call sites in the firmware. "Constant args" = argument values that are constants at those call sites (16-bit args go out as D0 = low byte, D1 = high byte; 24-bit args fill D0..D2).

| CMD | | Meaning | Sites | Arg | Constant args seen | In your captures |
|---|---|---|---|---|---|---|
| `0x06` | ✅ | Audio mode word, D0..D2. Verified in the handoff: `06 01 10` = idle with wake word armed, `06 03 10` = mic streaming, `06 09 10` = speaker streaming (so D0 looks like a bit field: 0x01 wake word, 0x02 mic, 0x08 speaker). The firmware sends the value from system getter +0x16C on Wi-Fi connect, BT A2DP/AVRCP, weather/news playback and audio-status changes. | 17 | 24-bit | — | boot `06 01 00 00`; voice wake `06 03 10 00` |
| `0x09` | ✅ | Stop the current face animation (D0 = 0); the K210 replies RX 0x08 (ANIM_DONE). The firmware also sends D0 = 3 and 4 (campfire, Fit, test screen), meaning unknown. | 4 | 24-bit | `0x0`, `0x3`, `0x4` | voice wake `09 00 00 00` |
| `0x0A` | 🟡 | Request with a reply: ESP32 sends `0A 00 00 00`, then waits 200 ms for K210 extended frame SEL `0x0A` (payload to *(0x3ffc6d2c), queue 0x3ffc6b1c). Used by painting, charging and shutdown flows. | 1 | none | `00 00 00` |  |
| `0x0C` | ❓ | 24-bit parameter, from the main voice/behaviour task. | 1 | 24-bit | — |  |
| `0x15` | ❓ | d0 = 0/1/3. From the "show-off" behaviour (laser_eye, magic, etc.). | 3 | 16-bit | `0x0`, `0x1`, `0x3` |  |
| `0x17` | 🟡 | Volume / audio level. One caller is the BT AVRCP "set bt volume %d" handler; constants 0x00, 0x67, 0x6A, 0xFF are also sent. | 19 | 16-bit | `0x0`, `0x67`, `0x6a`, `0xff` | boot `17 68 00 00` |
| `0x1C` | ❓ | On/off (0/1). ChatGPT conversation flow and sleep/freeze behaviour. | 4 | 16-bit | `0x0`, `0x1` |  |
| `0x1F` | ❓ | On/off (0/1). Sleep and voice-interaction behaviour. | 4 | 16-bit | `0x0`, `0x1` |  |
| `0x23` | 🟡 | Start of a file download to the K210 (OTA "ota_init", debug TCP server "send ok to pc"). | 4 | none | `00 00 00` |  |
| `0x25` | 🟡 | File download step during OTA ("download_file", "k210 ota error, download this file again"). | 6 | none | `00 00 00` |  |
| `0x29` | 🟡 | OTA: start of a mot/resource download batch. | 1 | none | `00 00 00` |  |
| `0x2A` | ❓ | OTA error path (next to "errcode"). | 1 | none | `00 00 00` |  |
| `0x2B` | 🟡 | OTA mot/resource download, and the debug TCP server. | 2 | none | `00 00 00` |  |
| `0x2C` | ✅ | Begin file transfer session. Every K210 SD read (profile.json, .mot, photos) is wrapped in `2C` … `2D`. | 6 | none | `00 00 00` | boot `2C 00 00 00` |
| `0x2D` | ✅ | End file transfer session (pairs with 0x2C). | 8 | none | `00 00 00` | boot `2D 00 00 00` |
| `0x34` | 🟡 | Boot handshake; first command after the K210 reset pulse (app_main). | 1 | 16-bit | `0x0` | boot/update `34 00 00 00` |
| `0x35` | 🟡 | Colour-blob detection request: d0 = state+0x3AD4 (selected colour), then waits for K210 extended frame SEL `0x35`; the caller prints x / w / h / cy / pixels / color. | 1 | 8-bit | D0 = state+0x3AD4, D1 = D2 = 0 |  |
| `0x36` | ❓ | On/off (0/1), voice/behaviour task. | 4 | 16-bit | `0x0`, `0x1` |  |
| `0x38` | 🟡 | Scene/mode number (values 1-6). Seen with 1 = normal/charging, 4 = go-home, 5 = zombie/spray-paint games, 6 = dialogue. | 20 | 16-bit | `0x1`, `0x2`, `0x3`, `0x4`, `0x5`, `0x6` | boot `38 01 00 00` |
| `0x3A` | 🟡 | K210 firmware update (k210_update_receive path), d0 = variable. | 1 | 16-bit | — |  |
| `0x3B` | 🟡 | Sent once in app_main with d0 = 1. | 1 | 16-bit | `0x1` | update `3B 01 00 00` |
| `0x3C` | ✅ | Query the K210 firmware version *number*: sent up to 3 times, 200 ms apart, after a K210 update; the RX 0x3C reply byte is compared with the expected number ("Firmware update success/failed"). | 1 | 16-bit | `0x0` | update `3C 00 00 00` |
| `0x3E` | 🟡 | Show clock: d0, d1, d2 = time fields (callers build "it's %d o'clock" etc.). | 6 | D0 D1 D2 | `var 0x0 0x64`, `var var 0x65` |  |
| `0x41` | ❓ | d0 = 1/2/3, factory test screen and voice task. | 4 | 16-bit | `0x1`, `0x2`, `0x3` |  |
| `0x42` | ❓ | d0 = 1, factory test screen and voice task. | 2 | 16-bit | `0x1` |  |
| `0x43` | ❓ | On/off (0/1). Sent on BLE connect, screen-info screen, test theme, compass calibration. | 16 | 16-bit | `0x0`, `0x1` |  |
| `0x46` | 🟡 | Sent from app_main when OTA failed ("update_failed"). | 1 | none | `00 00 00` |  |
| `0x48` | 🟡 | Photo display (show_photo), d0 = 1 or variable. | 2 | 16-bit | `0x1` |  |
| `0x4A` | ❓ | d0 = 0, sleep/face behaviour. | 3 | 16-bit | `0x0` |  |
| `0x4B` | 🟡 | Sent before `MODE_MIC` on follow-up turns without a wake word (handoff LISTEN_FOLLOWUP); one call site, in the ChatGPT flow, D0 = 0. Same byte your firmware sends as "open_mic". | 1 | 16-bit | `0x0` |  |
| `0x4D` | ❓ | d0 = 0, sleep/face behaviour. | 1 | 16-bit | `0x0` |  |
| `0x4F` | ❓ | d0 = 0, sleep/face behaviour. | 1 | 16-bit | `0x0` |  |
| `0x50` | ❓ | Sleep/face behaviour. | 1 | 16-bit | — |  |
| `0x52` | ❓ | ChatGPT flow. | 1 | 16-bit | — |  |
| `0x53` | ❓ | d0 = 0/1/2/0xFF. Spray-paint and zombie gesture games (detection mode?). | 15 | 16-bit | `0x0`, `0x1`, `0x2`, `0xff` |  |
| `0x54` | 🟡 | Photo/painting transfer ("send_png_done"). | 2 | none | `00 00 00` |  |
| `0x56` | 🟡 | Photo/painting transfer done ("success", "it's done, hope you like it"). | 3 | none | `00 00 00` |  |
| `0x58` | 🟡 | Photo transfer (debug TCP photo path). | 1 | none | `00 00 00` |  |
| `0x59` | ❓ | d0 = 0x03/0x14/0x28/0x46/0x48/0x49/0x4B. Used by most games, eye stickers and ChatGPT (30 call sites). | 30 | 16-bit | `0x3`, `0x14`, `0x28`, `0x46`, `0x48`, `0x49`, `0x4b` |  |
| `0x5A` | ❓ | 3 bytes (d0, d1, d2), screen info and sleep behaviour; possibly a colour. | 3 | D0 D1 D2 | — |  |
| `0x5B` | ❓ | 2 bytes (0xFF, x / 0x09, 0x02), ChatGPT flow. | 7 | D0 D1 | `0x9 0x2`, `0xff 0x0`, `0xff 0x1`, `0xff var`, `var 0x1`, `var 0x2` |  |
| `0x5D` | ❓ | d0 = variable, app_main. | 1 | 16-bit | — |  |
| `0x5E` | ❓ | 2 bytes, fruit-machine game. | 3 | D0 D1 | `0x0 0x0`, `0x0 0x2` |  |
| `0x5F` | ❓ | 2 bytes (0/1, 0/3), show-off behaviour. | 6 | D0 D1 | `0x0 0x0`, `0x0 0x3`, `0x1 0x3`, `var 0x3` |  |
| `0x60` | ❓ | d0 = 0x6F, screen-info screen. | 1 | 16-bit | `0x6f` |  |
| `0x61` | ❓ | d0 = 0..3, Daily routines. | 7 | 16-bit | `0x0`, `0x1`, `0x2`, `0x3`, `0x4` | voice wake `61 00 00 00` |
| `0x62` | ❓ | On/off (0/1), spray-paint game. | 2 | 16-bit | `0x0`, `0x1` |  |
| `0x65` | 🟡 | D0 = 1/0xFF/0x6F/0xEB. Home-station search and charging-status flows. The handoff boot byte `65 6B` † is really `65 EB`: 0xEB is a constant at a call site and 0x6B is not. | 14 | 16-bit | `0x1`, `0x6f`, `0xeb`, `0xff` |  |
| `0x66` | 🟡 | OTA mot/resource download. | 3 | 16-bit | — |  |
| `0x67` | ❓ | d0 = 0/0x64/0x65/0x66, voice/behaviour task. | 5 | 16-bit | `0x0`, `0x64`, `0x65`, `0x66` |  |
| `0x68` | 🟡 | OTA / show_photo, d0 = 1 or 0x65 (the K210 answers RX 0x68 "SEND_RUN_OTA"). | 5 | 16-bit | `0x1`, `0x65` |  |
| `0x69` | ✅ | Get K210 version string (handoff GET_VERSION), D0 = 1, polled from app_main until the K210 answers with ext SEL 0x69 "3.1.0". | 2 | 16-bit | `0x1` | boot/update `69 01 00 00` x3 |
| `0x6A` | 🟡 | Sent at boot (screen-info), sleep behaviour, speaker/DJ mode. | 3 | 16-bit | — | boot `6A 46 00 00` |
| `0x6B` | ❓ | d0 = 1, app_main. | 1 | 16-bit | `0x1` |  |
| `0x6C` | 🟡 | Sent at boot (test theme) and in speaker/DJ mode. | 2 | 16-bit | — | boot `6C 00 00 00` |
| `0x6D` | ❓ | On/off, spray-paint game. | 3 | 16-bit | `0x0`, `0x1` |  |
| `0x6E` | ❓ | On/off, campfire scene. | 3 | 16-bit | `0x0`, `0x1` |  |
| `0x6F` | ❓ | 3 bytes, dice-pig game (d0 = step 1..0x0B, d1/d2 = dice values or 0x63). | 17 | D0 D1 D2 | `0x1 0x0 0x0`, `0x2 0x63 var`, `0x2 var 0x0`, `0x2 var var`, `0x3 var 0x63`, `0x3 var var`, `0x4 var 0x0`, `0x5 var 0x63` … |  |
| `0x70` | 🟡 | Sent once in app_main. | 1 | 16-bit | — | boot `70 00 00 00` |
| `0x72` | ❓ | On/off, campfire and Fit scenes. | 4 | 16-bit | `0x0`, `0x1` |  |
| `0x73` | ❓ | 3 bytes (0,0,0 / 1,0x1E,0), Fit scene (timer?). | 3 | D0 D1 D2 | `0x0 0x0 0x0`, `0x1 0x1e 0x0` |  |
| `0x74` | 🟡 | Sent at boot (screen-info), sleep behaviour, speaker/DJ mode. | 3 | 16-bit | — | boot `74 00 00 00` |

## 3. ESP32 → K210, extended frames (17 selectors)

| SEL | | Payload / meaning | Sites | In your captures |
|---|---|---|---|---|
| `0x02` | ✅ | Play animation playlist (JSON), e.g. `[0,[["/spiffs/avi/NAME.avi",[[0,"/spiffs/mp3/NAME_000.mp3"]]]]]`. Sent by the behaviour engine; the ESP32 then waits for RX 0x08 d0=0 (playback finished). | 2 | boot / voice-wake JSON |
| `0x03` | ✅ | Read a file from the K210 SD card; payload = path (`esp32_read_k210_sdcard_file`, `esp32_read_k210_json_file_to_buf`). Used for /spiffs/json/profile.json and /spiffs/mot/*.mot. Data comes back as RX 0x04 (SPI chunks) and RX 0x05 (done). | 2 | boot `/spiffs/json/profile.json` |
| `0x0E` | ❓ | Image-search payload (image_search behaviour). | 1 |  |
| `0x18` | ✅ | Play video; payload = "/spiffs/avi/NAME.avi" (`FUN_401d9454` with flag = 0). | 4 |  |
| `0x19` | ✅ | Play audio; payload = "/spiffs/mp3/NAME_000.mp3" or "/spiffs/mp3/NAME.mp3" (flag = 0). | 2 |  |
| `0x1A` | ✅ | Play video, flag = 1 variant (`FUN_401d9454` with flag != 0; probably loop or queue). 21 call sites. | 21 |  |
| `0x1B` | ✅ | Play audio, flag = 1 variant. | 2 |  |
| `0x26` | 🟡 | Path/name buffer during OTA mot download and in the debug TCP server. | 4 |  |
| `0x28` | ❓ | 3-byte payload ("mp3") from `FUN_401d1650` (face_id.avi test). | 1 |  |
| `0x3D` | 🟡 | Alarm list text ("%02d:%02d %s %s\n") for the alarm screen. | 2 |  |
| `0x40` | ✅ | Read a K210 SD file into a buffer (`esp32_read_k210_sd_file_to_buf`); used for photos. | 1 |  |
| `0x44` | 🟡 | Text to show on screen: factory test screens ("Wifi %d %s\nVer…", "Touch…", "IMU…"), screen info, servo-update progress. | 34 |  |
| `0x49` | 🟡 | Daily-routine descriptor (strings such as "Daily_Boxing(005+1)"). | 2 |  |
| `0x4C` | ❓ | From the sleep/face behaviour. | 1 |  |
| `0x4E` | ❓ | From the sleep/face behaviour. | 1 |  |
| `0x51` | 🟡 | Face/name data (next to "hi %s." and face_rsp); len = name length + 2. | 1 |  |
| `0x57` | 🟡 | Photo/painting data or name. | 2 |  |

Helpers that build these: `FUN_401d9454(name, flag, delay)` → SEL 0x18 (flag 0) / 0x1A (flag 1) with "/spiffs/avi/%s.avi"; `FUN_401d94ac` → SEL 0x19 / 0x1B with "/spiffs/mp3/%s_000.mp3"; `FUN_401d9504` → SEL 0x19 / 0x1B with "/spiffs/mp3/%s.mp3"; `FUN_401d955c` = video + audio together. Each helper then sleeps `delay` ms.

## 4. K210 → ESP32, small frames

"state" = the global system struct at *(0x3ffc9fe4); "system +0xNN" and "behaviour +0xNN" = function-pointer slots in that struct and in the object returned by `FUN_401e19b0`.

| CMD | | What the ESP32 does with it |
|---|---|---|
| `0x00` | ✅ | Speaker data request (handoff SPK_REQ, `00 00 30` = 12288 bytes). The ESP32 takes min(requested, available) bytes from its playback ring buffer 0x3ffc9ffc and writes them to the K210 with `spi_trans_data` (type byte 0x00). If the buffer is empty it ignores the request. A short write is the `F0` "tail" block. |
| `0x01` | ✅ | Mic frame ready (handoff MIC_READY, `00 04` = 1024 bytes). The ESP32 reads it with `spi_recv_data` (type byte 0x01) into ring buffer 0x3ffc9fec, which `record_read` consumes. |
| `0x04` | ✅ | File data chunk for a TX ext 0x03/0x40 read: len = d0 + (d1 << 8). SPI read with type byte 0x02, appended to the open file buffer (running sum and XOR kept). |
| `0x05` | ✅ | File transfer finished (`FUN_401e012c` closes the read). |
| `0x06` | ✅ | Ignored by the parser. |
| `0x08` | 🟡 | Playback status. d0 = 0: playlist finished (posted to queue 0x3ffc9fc4, which the TX 0x02 senders wait on). d0 = 1: sets flag state+0x3F97. d0 = 2: sets flag state+0x3F98 and calls FUN_4025a2a8. |
| `0x0B` | ✅ | Voice event. d0 = 1: wake word detected, d1 passed to behaviour +0x12C (handoff saw 03/04, direction?). d0 = 0: end of speech (VAD), the K210 has already stopped mic frames. |
| `0x0C` | ❓ | state+0x3E88 = d0. |
| `0x0D` | ❓ | Seen during mic streaming with d0 = 0..5 (handoff). The ESP32 only stores d0 in 0x3ffc9fa4 (and calls system +0x244(0x0E)) when behaviour state == 6 and flag 0x3ffc6a68 == 0; otherwise ignored. |
| `0x0F` | ❓ | Behaviour object +0x128(d0). |
| `0x10` | ✅ | Ignored by the parser. |
| `0x13` | 🟡 | Data ready, len = d0 + (d1 << 8); SPI read with type byte 0x05 into ring buffer 0x3ffca00c. |
| `0x14` | 🟡 | Reset ring buffer 0x3ffca00c and post to queue 0x3ffcbee4 (end of that stream). |
| `0x1C` | ❓ | Behaviour +0x80(); if 1: system +0x1DC and behaviour +0x160(0), otherwise sets flag state+0x3ADE. |
| `0x1D` | ❓ | Sub-commands on d0: 0x12 and 0x15 (behaviour-state checks against 0x35/0x36, then system +0x234(1) or a callback). |
| `0x1F` | ❓ | Behaviour-state checks (0x18, 0x19, 0x0A), then system +0x1E0. |
| `0x21` / `0x24` / `0x33` | 🟡 | 16-bit value (d0 + (d1 << 8)) posted to queue 0x3ffc93a0, read by the K210-update, download_file and debug-TCP tasks (transfer ack/progress). |
| `0x37` | ❓ | If not busy (state+0x278C == 0): system +0x1F8(d0). |
| `0x39` | ❓ | If d0 differs from system +0x118(): system +0x1E4(d0), then +0x22C. |
| `0x3C` | ✅ | K210 firmware version reply: d0 stored in 0x3ffc9fe0 (read by check_firmware_version). |
| `0x3F` | ✅ | YOLO object detection: class = d0, x = d1, y = d2 (logged "EMO_SEND_YOLO_CLASS %d, x=%d, y=%d"); stored at state+0x277C / +0x2784 / +0x2788. |
| `0x47` | 🟡 | Sent once at boot ~1 s after VERSION (handoff READY?). The ESP32 calls system +0x250(1) and stores d0 at state+0x335E; if +0xE0() == 1 and d0 == 2 it restarts the ESP32. |
| `0x48` | ❓ | System +0x1FC(2). |
| `0x55` | 🟡 | 16-bit value posted to queue 0x3ffc82e8 (photo/painting transfer status). |
| `0x59` | ❓ | state+0x2718 = 0 (clears a busy flag). |
| `0x5C` | ✅ | Restart the ESP32 (calls esp_restart). |
| `0x5D` | ❓ | state+0x3271 = d0. |
| `0x5F` | ❓ | state+0x2D4B = 1. |
| `0x62` | 🟡 | Tracked target position: x = int16(d0 + (d1 << 8)), y = int8(d2) → state+0x3382 / +0x3384 (home-station / object tracking). |
| `0x64` | 🟡 | 16-bit value posted to queue 0x3ffc9424 (download_file_post_task). |
| `0x68` | ✅ | "SEND_RUN_OTA %d": state+0x3C14 = d0. |
| `0x71` | ❓ | state+0x3F96 = d0. |
| `0xFE` | ✅ | ACK of an extended frame. The stock K210 sends `55 AA FE AA 00 00 A7` (d0 = 0xAA, handoff). The send task waits up to 500 ms for it: any d0 except 0x55 counts as received; d0 == 0x55 logs " ack == 0x55 " and resets the RX parser; a timeout logs "resault == pdFALSE" and bumps an error counter. |
| `0xFF` | ✅ | Extended frame from the K210 (section 5). |

Any other CMD byte falls through to the default branch (clears a local flag) and is otherwise ignored.

## 5. K210 → ESP32, extended frames (CMD 0xFF)

Header `55 AA FF LEN_LO LEN_HI SEL CS`, then LEN payload bytes read straight from the UART. LEN > 0x1000 → "small pack error".

| SEL | | Payload goes to | Notes |
|---|---|---|---|
| `0x0A` | ✅ | buffer *(0x3ffc6d2c), then queue 0x3ffc6b1c | Reply to TX small 0x0A |
| `0x35` | ✅ | buffer *(0x3ffc6d28), then queue 0x3ffc6b18 | Reply to TX small 0x35 (colour-blob result) |
| `0x4A` | ❓ | state+0x1CF0 | |
| `0x4F` | ❓ | state+0x2014 (1 s timeout); state+0x246C = 1 on a full read; UART RX flushed | |
| `0x69` | ✅ | state+0x3D26; UART RX flushed | K210 version string, e.g. "3.1.0" (reply to TX 0x69) |

## 6. Observed sequences (from your captures; every byte appears in the firmware tables above)

- **Boot:** RESETN pulse → `34` → `69 01` ×3 → `2C` → ext SEL 0x03 "/spiffs/json/profile.json" → `2D` → `70` → `38 01` → `06 01` → `17 68` → `6A 46` → `6C` → `74` → ext SEL 0x02 boot_start JSON.
- **Update:** `34` → RESETN → `3C` → `3B 01` → `69 01` ×3.
- **Voice wake:** `61` → `09` → ext SEL 0x02 voice_wake_up JSON → `06 03 10`.

## 7. Notes

- `profile.json` and the `.mot` files live on the K210 SD card. The mot loader (`FUN_40143a50`) first checks the ESP32 SPIFFS; on a miss it sends `2C`, reads the file with ext SEL 0x03, then sends `2D`. Your SPIFFS `/mot/` folder is that cache. This corrects my earlier answer that `profile.json` was simply missing from your dump: it is on the K210 side.
- Every small command has its own one-byte wrapper at `0x401d9578`–`0x401d9a1c` (plus the generic `FUN_401d9614(cmd)` and two direct calls for 0x0A/0x35); every extended selector has a wrapper at `0x401d9364`–`0x401d9440` or a direct call. The tables cover all of them, so there is no other UART command byte on the ESP32 side.
- The names in `k210_commands.csv` (e.g. "Motion Control 0x67") do not come from the firmware, and its frame layout `55 AA 00 CMD …` is off by one: the command is byte 2.
- All raw UART writes (`FUN_401d4b24`) were checked: four go to UART1 (the 7-byte header from the send task, the direct small-frame path, the ext payload, and the PCS byte). The other two go to UART2 (10 bytes from 0x3ffc2134 and 0x208 bytes from 0x3ffcb955), which is a different peripheral, not the K210.
