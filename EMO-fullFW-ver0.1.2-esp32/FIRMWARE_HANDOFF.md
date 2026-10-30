# EMO replacement ESP32 firmware — implementation handoff

**Audience:** an engineer or LLM writing new ESP32 firmware that replaces the stock ESP32
firmware in the EMO robot and talks to the **unmodified stock K210 firmware (v3.1.0)**.
This file is self-contained: it holds everything needed to implement the K210 link.
Background and evidence: `SPI_PROTOCOL.md`, `EMO_LINK_AND_STARTUP.md`. The PC-side protocol the
firmware must also serve is in `AUDIO_IO_SPEC.md` (EA10).

Confidence tags used below:
* **[verified]**: observed byte-exact in captures and/or confirmed by listening.
* **[inferred]**: consistent with every capture, not proven. Implement it, but log around it.
* **[unknown]**: not determined. Treat it as an experiment (§10).

---

## 0. What to build

```
 PC / LLM client  <--TCP :3456, EA10 (AUDIO_IO_SPEC.md)-->  ESP32 (NEW FW)
                                                            |  UART 1 Mbaud  (control)
                                                            |  SPI 8 MHz     (audio, ESP32 = master)
                                                            v
                                                       K210 (STOCK FW v3.1.0)
                                     mics + wake word + VAD, speaker, face display, /spiffs assets
```

The new firmware must:
1. Bring the K210 up and keep it in "wake word armed" idle (§6, §7).
2. Stream the mic to the PC: K210 says a frame is ready over UART, ESP32 reads it over SPI,
   then sends it as EA10 `MIC_PCM` (§5.1, §8).
3. Play PC audio: EA10 `SPK_PCM` goes to a buffer, K210 requests space over UART, ESP32 writes
   a block over SPI (§5.2, §8).
4. Optionally drive face animations (§4.3).

---

## 1. Hardware interface

| Link | Settings | Notes |
|---|---|---|
| UART | **1 000 000 baud, 8N1**, no flow control | ESP32 TX -> K210 RX, K210 TX -> ESP32 RX [verified] |
| SPI | ESP32 **master**, **8 MHz**, **mode 1 (CPOL=0, CPHA=1)**, MSB first, 8-bit bytes in wire order | [verified] on a 40 MSa/s capture |
| CS | active low, **driven per phase**: one CS window for the 8-byte command, a separate window for the data | [verified] |

* **GPIO numbers: [unknown].** `pinout.txt` only lists the logic-probe wire colours
  (SCLK white, ESP TX violet, MISO orange, CS blue, MOSI green, ESP RX yellow). Get the ESP32
  pins from the board or the person running this project, or trace them. Do not guess.
* Use DMA for SPI. Data phases are 1024 B (mic) and 12288 B (speaker) at 8 MHz:
  1.03 ms and 12.3 ms on the wire.
* Control CS manually (or use 2 separate transactions with `SPI_TRANS_CS_KEEP_ACTIVE` off) so
  the command and the data get **separate** CS low periods.

---

## 2. UART frame format [verified]

### 2.1 Short frame, 7 bytes
```
[0]=0x55 [1]=0xAA [2]=cmd [3]=p1 [4]=p2 [5]=p3 [6]=chk
chk = (cmd + p1 + p2 + p3 - 1) & 0xFF
```
A 16-bit length, when present, is `p1 | (p2 << 8)`.

### 2.2 Extended frame (cmd = 0xFF)
```
[0]=0x55 [1]=0xAA [2]=0xFF [3]=lenL [4]=lenH [5]=sub [6]=chk  [7 .. 7+len-1]=payload  [7+len]=crc
chk = (0xFF + lenL + lenH + sub - 1) & 0xFF
crc = CRC-8/MAXIM over payload only (reflected poly 0x8C, init 0x00, no final xor)
```
Payload is a NUL-terminated ASCII string; `len` includes the NUL.

### 2.3 Reference code
```c
#include <stdint.h>
#include <string.h>

static uint8_t crc8_maxim(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0x8C) : (uint8_t)(c >> 1); }
    return c;
}
static size_t emo_short(uint8_t o[7], uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3) {
    o[0]=0x55; o[1]=0xAA; o[2]=cmd; o[3]=p1; o[4]=p2; o[5]=p3;
    o[6]=(uint8_t)(cmd + p1 + p2 + p3 - 1);
    return 7;
}
static size_t emo_ext(uint8_t *o, uint8_t sub, const char *str) {   /* sends str incl. NUL */
    uint16_t len = (uint16_t)(strlen(str) + 1);
    o[0]=0x55; o[1]=0xAA; o[2]=0xFF; o[3]=len & 0xFF; o[4]=len >> 8; o[5]=sub;
    o[6]=(uint8_t)(0xFF + o[3] + o[4] + sub - 1);
    memcpy(o + 7, str, len);
    o[7 + len] = crc8_maxim(o + 7, len);
    return 8 + len;
}
```
Check vectors (from the capture):
* `emo_short(o, 0x09,0,0,0)` -> `55 AA 09 00 00 00 08`
* `emo_short(o, 0x06,0x03,0x10,0)` -> `55 AA 06 03 10 00 18`
* `emo_ext(o, 0x02, "[0,[[\"/spiffs/avi/chatgpt_speak.avi\"]]]")` -> `55 AA FF 28 00 02 28 … 00 1C`
* `crc8_maxim("/spiffs/json/profile.json\0", 26)` = `0x10`

### 2.4 Receive parser
Byte-wise state machine:
1. Hunt for `55`, then `AA`; anything else returns to hunting.
2. Read 5 bytes (cmd, p1, p2, p3, chk) and verify chk. On a mismatch, resync by re-hunting
   from the byte after the `55`.
3. If cmd == `0xFF`, read `len` + 1 more bytes and verify the CRC.

Back-to-back frames with no gap are normal: `0D …` followed immediately by `01 …` was seen.
Run the parser in a high-priority task. Mic requests arrive every 32 ms and must not be missed.

---

## 3. Message catalogue

Byte-exact frames. "†" marks frames known only from the boot capture, where bit 7 of every byte
was lost; there the byte shown is the best guess (the bit-7-clear value).

### 3.1 ESP32 -> K210
| Name | Bytes | Meaning / when | Tag |
|---|---|---|---|
| `MODE_IDLE` | `55 AA 06 01 10 00 16` | wake word armed, no streaming. Normal idle | verified bytes / inferred meaning |
| `MODE_MIC` | `55 AA 06 03 10 00 18` | start mic streaming (K210 begins `MIC_READY` within 4–19 ms) | verified |
| `MODE_SPK` | `55 AA 06 09 10 00 1E` | start speaker streaming (K210 begins `SPK_REQ` 30–57 ms later) | verified |
| `LISTEN_FOLLOWUP` | `55 AA 4B 00 00 00 4A` | sent ~12 ms before `MODE_MIC` on follow-up turns (no wake word) | verified bytes / meaning unknown |
| `STOP_ANIM` | `55 AA 09 00 00 00 08` | stop current face animation; K210 replies `ANIM_DONE` (25–50 ms) | verified / inferred |
| `PLAY_ANIM` | `FF`, sub `02`, JSON (§4.3) | play `/spiffs/avi/*.avi` (+ optional mp3); K210 replies `ACK` | verified |
| `GET_VERSION` | `55 AA 69 01 00 00 69` † | K210 replies `VERSION` | inferred |
| `REQ_FILE` | `FF`, sub `03`, `"/spiffs/json/profile.json"` | K210 replies `ACK`, then `FILE_CHUNK`… `FILE_DONE` | inferred, **avoid** (§10) |
| boot misc | `34`, `2C`, `2D`, `70`, `38 01`, `17 68`, `6A 46`, `6C`, `74`, `65 6B` (+ zeros) † | sent once at boot, no reply | meaning unknown |
| other modes | `06 00 00 00`, `06 00 10 00`, `06 01 00 00` | seen around boot / news playback | meaning unknown |

### 3.2 K210 -> ESP32
| Name | Bytes | Meaning | Tag |
|---|---|---|---|
| `MIC_READY` | `55 AA 01 00 04 00 04` | 1024-byte mic frame ready, **read it now** (§5.1). Every 32.2 ms while `MODE_MIC` | verified |
| `SPK_REQ` | `55 AA 00 00 30 00 2F` | room for one 12288-byte block, write one if you have it (§5.2). Every 64.5 ms while `MODE_SPK` | verified |
| `WAKE` | `55 AA 0B 01 xx 00 chk` (`xx` = 03 or 04 seen) | wake word detected | verified / inferred |
| `END_OF_SPEECH` | `55 AA 0B 00 00 00 0A` | VAD end; the K210 has **already stopped** mic frames (last one 32 ms before) | verified / inferred |
| `MIC_LEVEL?` | `55 AA 0D xx 00 00 chk` (`xx` 0..5) | occasional during mic streaming. Level or direction? Safe to ignore | unknown |
| `ACK` | `55 AA FE AA 00 00 A7` | ACK of any extended frame, ~0.8 ms after it | verified |
| `ANIM_DONE` | `55 AA 08 00 00 00 07` | animation finished (loop = 0) or stopped by `STOP_ANIM` | verified / inferred |
| `VERSION` | `FF`, sub `69`, `"3.1.0"` | reply to `GET_VERSION` | inferred † |
| `READY?` | `55 AA 47 00 00 00 46` † | once, ~0.9 s after `VERSION` | unknown |
| `FILE_CHUNK` / `FILE_DONE` | `04 lenL lenH 00` / `05 00 00 00` † | after `REQ_FILE` (10240-byte chunks); SPI side **not captured** | unknown |

---

## 4. Shared rules

### 4.1 Requests the ESP32 can't serve
Ignoring a `SPK_REQ` is fine: the stock firmware ignores ~13 of them at the end of every
utterance, and the K210 just asks again 64.5 ms later. **Never** ignore a `MIC_READY`: all
490 were served in the reference capture. What the K210 does on overrun is [unknown].

### 4.2 Clocking / drift
The K210 is the audio clock master. Measured cadences are 32.24 ms per 512 mic samples and
64.5 ms per 3072 speaker frames, ~0.8 % slower than nominal 16 kHz / 48 kHz (relative to the
logic analyser's crystal). Pace everything from the K210's requests, never from an ESP32
timer. Give the speaker path an elastic buffer: PC audio arriving at a nominal 48 kHz will
slowly overfill it. For long playback, drop or insert a few samples, or just let the buffer
grow; utterances are short.

### 4.3 Face animations (optional but expected by users)
```
STOP_ANIM  ->  wait ANIM_DONE (timeout 200 ms)  ->  PLAY_ANIM  ->  wait ACK (timeout 50 ms)
JSON:  [loop,[["/spiffs/avi/NAME.avi"]]]
       [loop,[["/spiffs/avi/NAME.avi",[[0,"/spiffs/mp3/NAME_000.mp3"]]]]]
loop: 0 = play once then K210 sends ANIM_DONE;  1 = loop until replaced   [inferred]
```
Known assets: `boot_start`, `stay_wink_1`, `stay_a_b`, `stay_c_d`, `voice_wake_up`,
`listen_and_think`, `chatgpt_start`, `chatgpt_hear`, `chatgpt_stay`, `chatgpt_speak`, `speak4`,
`speak8`, `speak10`, `keep_vigilant1`, `EMOnews_loop1..4`, `Daily_Boxing_start/loop1/end`.
Stock firmware: `voice_wake_up`+mp3 on wake, `[1,listen_and_think]` or `[1,chatgpt_stay]` while
thinking, `chatgpt_speak` (re-sent on each `ANIM_DONE`) while speaking, `chatgpt_hear`+mp3 when
ready for a follow-up.

---

## 5. SPI transactions [verified]

Each transfer = **command phase** (CS low, 8 bytes MOSI, 64 clocks) -> CS high -> **data
phase** (CS low, N bytes) -> CS high. Send the command bytes **verbatim**; the meaning of
most header bytes is unknown.

Stock timing to copy at first (shorter may work, untested):

| | UART request -> command CS | command CS -> data CS |
|---|---|---|
| mic read | ~0.64 ms | ~0.57 ms |
| speaker write | ~1.9 ms | ~1.0 ms |

The gap between the two CS windows probably lets the K210 slave parse the command and arm DMA.
Keep at least **0.5 ms** read / **1.0 ms** write until tested shorter.

### 5.1 Mic read (on `MIC_READY`)
```
cmd  (MOSI): 00 00 00 04 01 05 00 00
data (MISO): 1024 bytes      (MOSI during data: send anything, stock sends stale bytes; zeros are fine)
```
**Payload = 512 samples, 16 kHz, mono, signed 16-bit**, sent by the K210 as 256 32-bit SPI
words, MSB first. On the wire, each 4-byte group `b0 b1 b2 b3` holds **two samples in reversed
order** [verified: by smoothness on 97 % of loud frames, and by ear]:
```c
/* wire[] = 1024 bytes as received; out[] = 512 samples in time order (host int16) */
for (int k = 0; k < 256; k++) {
    const uint8_t *b = wire + 4*k;
    out[2*k]     = (int16_t)((b[2] << 8) | b[3]);   /* earlier sample */
    out[2*k + 1] = (int16_t)((b[0] << 8) | b[1]);   /* later sample   */
}
```
Frames are contiguous (no overlap, no gap). One K210 frame (32 ms) maps to exactly one EA10
`MIC_PCM` frame (1024 B, 512 samples, s16 **little-endian**). Expect a per-frame digital gain
on the K210 side (samples are multiples of a per-frame factor); leave it as is.

### 5.2 Speaker write (on `SPK_REQ`)
```
full block  cmd: 00 00 00 30 00 30 00 00     data (MOSI): 12288 bytes   (MISO: zeros)
last block  cmd: 00 00 F0 nn 00 mm 00 00     data: (nn+1)*256 bytes, mm = nn - 0x10
```
**Payload = 3072 frames, 48 kHz, 2 channels with L = R, signed 16-bit big-endian.** That is
64 ms per block [verified: by ear].
```c
/* in[] = 3072 mono samples (host int16); wire[] = 12288 bytes */
for (int i = 0; i < 3072; i++) {
    uint8_t hi = (uint16_t)in[i] >> 8, lo = in[i] & 0xFF;
    uint8_t *w = wire + 4*i;
    w[0] = hi; w[1] = lo; w[2] = hi; w[3] = lo;     /* L = R, so pair order doesn't matter */
}
```
**Recommendation: avoid the `F0` tail format.** Pad the final block with silence to a full
12288 bytes and send it with the full-block header. That path is the well-verified one, and
the `F0` field `mm = nn - 0x10` is undefined for short tails (`nn < 0x10`). Seen tails:
`nn` 0x1C, 0x22, 0x26, 0x2B.

---

## 6. Runtime state machine

```
            ┌───────────── WAKE (0B 01 ..) or PC "listen" ─────────────┐
            │                                                          v
 [IDLE] MODE_IDLE sent                                   [LISTEN] send (LISTEN_FOLLOWUP if no wake) + MODE_MIC
   ^  ^                                                    on MIC_READY: SPI read -> EA10 MIC_PCM
   │  │                                                    on END_OF_SPEECH: -> [WAIT_REPLY]
   │  │                                                          │
   │  └── after last block + drain (≈13 SPK_REQ / ~0.9 s): ──┐   v
   │      send MODE_IDLE, EA10 EVENT 0x01             [SPEAK] MODE_SPK sent
   │                                                   on SPK_REQ: write 1 block if buffered, else ignore
   └──────────── timeout / PC cancels ──────────── [WAIT_REPLY] (PC is thinking; optional anim)
                                                     on first EA10 SPK_PCM: -> [SPEAK]
```
Rules:
* **Half-duplex.** Mic and speaker streaming never overlap in the stock firmware. Enter
  `[SPEAK]` only from `[WAIT_REPLY]`/`[IDLE]`. EA10 already documents "no mic frames while
  playing".
* **Ending playback:** after the last block (EA10 `SPK_END` received and buffer empty), keep
  ignoring `SPK_REQ` for **~0.9 s (13 requests)** so the K210's own buffer drains. Then send
  `MODE_IDLE` and EA10 `EVENT 0x01`. Stock waited 0.88 s, 0.88 s, 0.88 s and 1.08 s. Sending
  `MODE_IDLE` earlier may cut the end of the sentence [inferred].
* **Ending the mic:** the K210 stops by itself and sends `END_OF_SPEECH`. To stop early (PC
  cancel), send `MODE_IDLE`. Not observed in stock, [inferred].
* **Mic without wake word** (PC push-to-talk / follow-up): `LISTEN_FOLLOWUP` then `MODE_MIC`,
  as stock does for follow-up turns.

---

## 7. Boot procedure

The K210 needs ~9 s from power-on before it answers. Stock ESP32 boot, in order (times from
power-on, ±0.5 s):

| t (s) | send | expect |
|---|---|---|
| 4.3 | `34 00 00 00` † | nothing |
| 8.4 | `GET_VERSION`, repeat every ~210 ms | `VERSION "3.1.0"` (arrived on the 3rd try) |
| 9.75 | — | `47 00 00 00` † |
| 13.95 | `2C 00 00 00` † | nothing |
| 14.15 | `REQ_FILE "/spiffs/json/profile.json"` | `ACK`, then 17 × `FILE_CHUNK`, `FILE_DONE` (needs SPI reads, header unknown) |
| 20.65 | `2D 00 00 00` † | |
| 20.75 | `70 00 00 00` † | |
| 22.30 | `38 01 00 00` † | |
| 25.59 | `06 01 00 00` | |
| 25.67 | `17 68 00 00` † | |
| 25.83 | `6A 46 00 00` † | |
| 25.94 | `6C 00 00 00` † | |
| 26.05 | `74 00 00 00` † | |
| 26.17 | `PLAY_ANIM [0,[["/spiffs/avi/boot_start.avi",[[0,"/spiffs/mp3/boot_start_000.mp3"]]]]]` | `ACK`, `ANIM_DONE` ~4 s later |
| 30.86 | `65 6B 00 00` † | |
| 30.93 | `PLAY_ANIM [0,[["/spiffs/avi/stay_wink_1.avi"]]]` | `ACK`, `ANIM_DONE` |
| 31.78 | `PLAY_ANIM [0,[["/spiffs/avi/stay_a_b.avi"]]]` | `ACK` |
| 32.74 | `MODE_IDLE` (`06 01 10 00`) | (idle; wake word armed) |

Recommended implementation order:
1. **Minimal boot (try first):** poll `GET_VERSION` every 210 ms until `VERSION` arrives (give
   up after 30 s and log). Wait 1 s, send `MODE_IDLE`, then test wake word -> `WAKE`, and
   `MODE_MIC` -> `MIC_READY`.
2. If the K210 doesn't respond as expected, **replay the full table** (skip `REQ_FILE`), with
   the same order and roughly the same spacing. Use the † bytes as listed and compute the
   checksum with `emo_short`.
3. Only if still failing: add `REQ_FILE` (then the `FILE_CHUNK` SPI reads must be worked out
   first, §10).

Log every received frame with a timestamp during bring-up.

---

## 8. Bridging to EA10 (`AUDIO_IO_SPEC.md`)

| EA10 | K210 side |
|---|---|
| `MIC_PCM` (1024 B, 16 kHz s16 LE) | one per `MIC_READY`: unpack (§5.1), write LE |
| `SPK_PCM` (48 kHz s16 LE mono, ≤8192 B) | append to speaker ring buffer (mono int16) |
| `SPK_END` | mark end. On the first `SPK_PCM` of an utterance send `MODE_SPK`. Serve `SPK_REQ` from the buffer in 3072-sample blocks; pad the last with zeros; then drain (§6) |
| `EVENT 0x01` | send after the drain, together with `MODE_IDLE` |
| `PING`/`PONG` | ESP32-local, unrelated to the K210 |
| (new, optional) | forward `WAKE` / `END_OF_SPEECH` to the PC as EA10 `EVENT` codes of your choice |

Mic gating: only send `MODE_MIC` when a client is connected (or always, if you prefer the
stock wake-word UX), and drop frames if the TCP queue is full (EA10 rule: drop oldest).
Speaker start-up latency is one request period (≤64.5 ms) plus 30–57 ms after `MODE_SPK`.
Buffer at least 1–2 blocks (≥128 ms) before sending `MODE_SPK` so the first requests can be
served.

---

## 9. Measured reference values

| Item | Value |
|---|---|
| UART | 1 Mbaud 8N1 (bit = 1.000 µs) |
| SPI clock / mode | 8 MHz, mode 1, MSB first, SCLK idle low |
| Command phase | 64 clocks = 8 µs |
| Mic cadence | 32.24 ms (512 samples) |
| Speaker cadence | 64.5 ms (3072 frames) |
| `MODE_MIC` -> first `MIC_READY` | 4–19 ms |
| `MODE_SPK` -> first `SPK_REQ` | 30–57 ms, then exactly every 64.5 ms (no pre-fill burst) |
| `END_OF_SPEECH` | 32 ms after the last `MIC_READY` |
| Last speaker block -> `MODE_IDLE` (stock) | 0.88–1.08 s (13–16 ignored `SPK_REQ`) |
| Extended frame -> `ACK` | ~0.8 ms |
| `STOP_ANIM` -> `ANIM_DONE` | ~25–50 ms |
| `WAKE` -> stock `STOP_ANIM` | ~60 ms |

---

## 10. Unknowns and experiments (in priority order)

1. **ESP32 GPIOs** for UART/SPI/CS: needed before anything works.
2. **Minimal boot:** does the K210 work with only `GET_VERSION` + `MODE_IDLE`? (§7.1)
3. **Short CS gaps:** can the command->data gap shrink below 0.5 / 1.0 ms?
4. **Missed `MIC_READY`:** what does the K210 do when a frame isn't read?
5. **`06` parameter `0x10`**, `4B`, `0D xx`, `0B 01 xx`: meanings.
6. **`REQ_FILE` / `FILE_CHUNK` SPI header:** capture a stock boot at **40 MSa/s with SPI and
   UART** to see the read command used for 10240-byte chunks.
7. **Bit 7 of † bytes:** the same 40 MSa/s boot capture resolves them all.
8. **`F0` tail block:** only needed if padding the last block is unacceptable.

## 11. How to verify the new firmware

* Capture the bus at **40 MSa/s** (8 channels: D0 ESP TX, D1 SCLK, D2 CS, D3 MISO, D4 K210 TX,
  D5 MOSI). Lower rates corrupt SPI.
* `python3 emo_extract_audio.py capture.sr --outdir check` decodes it to WAV. Compare the
  `spk_*.wav` with what the PC sent (bit-exact expected), and listen to `mic_*.wav`.
* Check against the stock reference: header bytes identical, cadences as in §9, every
  `MIC_READY` followed by a read.
