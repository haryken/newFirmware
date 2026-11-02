# Leg servos (EMO, ESP32)

Status:
- **Old servo board: works** (verified on the robot, 2026-11-02).
- **New servo board: auto-detected and powered, but UNTESTED** (no new-servo
  robot available yet). Motion on it is not verified, see "Known limits".

## Board auto-detect (servo.c)

At boot `servo_init()` mimics the stock detect routine:

1. Config NEW: GPIO21 (signal) LOW, GPIO0 (supply) LOW 200 ms then HIGH, wait
   2.5 s for the servos to boot.
2. For each pin (21, 25, 5, 26) send the RMT "read version" packet
   (`cmd 0xDD`, data `0x6358`), listen for a 6-byte answer whose byte[1] is `0xDD`.
3. Any answer = **NEW** (supply GPIO0, left legs GPIO21). No answer = **OLD**
   (supply GPIO21, left legs GPIO0), then the normal power-up sequence.

Serial log: `servo: detect gpioNN: N symbols, reply .. ..` per pin, then
`board auto-detect: OLD|NEW servo`. If a new robot is detected as OLD, send that
log: the answer decoder was written from the stock disassembly only.

Override (saved in NVS, applied at the next boot):
`POST /api/motion {"action":"servo_board","mode":"auto|old|new"}`.
`GET /api/motion` reports `board`, `board_mode`, `pwr_gpio` and `gpio`.

## What is wired (old servo)

| Function | GPIO | Notes |
|---|---|---|
| Servo supply enable | **21** | `CONFIG_EMO_SERVO_PWR_GPIO` |
| Left legs signal | **0** | `CONFIG_EMO_SERVO_LL_GPIO` (also the foot-sensor I2C SDA) |
| Right legs signal | 25 | MCPWM0B |
| Left foot signal | 5 | MCPWM1A |
| Right foot signal | 26 | MCPWM1B |

Drive: plain 50 Hz PWM, pulse = `deg * 2000 / 180 + 500` us (500..2500 us).
MCPWM unit 0, timers 0 and 1, up counter, duty mode 0.

Power-up sequence (stock `FUN_401f9b54`), implemented in `servo.c`
(`arm_new_servos`, name kept for history):

1. GPIO21 LOW, wait 200 ms.
2. Route the MCPWM signals to 0 / 25 / 5 / 26.
3. GPIO21 HIGH, wait 200 ms.
4. Drive all four servos to 90 deg.

## What was wrong before (why the legs never moved)

1. **Wrong pin for the left legs.** We used GPIO21. In the stock firmware the
   first MCPWM pin is read from a RAM variable (`0x3ffcbd04`), not a constant,
   and on this board it is **0**. I assumed 21 from the initial value / pin map
   and never checked what the stock firmware actually stores at run time.
2. **Servo supply never switched on.** GPIO21 is the supply enable
   (stock `power_object` methods +0x24 / +0x28 = `gpio_set_level(pin, 1/0)`).
   We drove it as a signal pin, so the servos had no power.
3. **Unneeded RMT "arm" packets.** We sent `C5/0`, `C3/0x44`, `C4/0x44` on each
   signal pin. Those belong to the new-servo protocol (parameter / version
   read-write). They are useless on old servos, and `C3` / `C4` change servo
   parameters, so they could have written wrong settings.
4. **`ack_ok` was misleading.** It only counted successful RMT transmissions,
   never a reply from a servo, so the software reported OK while nothing
   moved. A read probe (`cmd 0xDE`, data `0x6358`) showed no servo ever replied,
   which is what finally pointed at power / pin mapping.
5. **Pin clash on GPIO0.** The foot-sensor I2C uses GPIO0 as SDA. The I2C driver
   takes the pin away from the PWM. `foot_cliff_start()` now reads the ID once,
   releases I2C, and calls `servo_rebind()`. The stock firmware also loses the
   foot MCUs after that point (`FOOT ID ... fail`).

## How the pins were found from the stock binary

- Stock log at boot prints `AA_this is old servo` (serial, 115200).
- The board-detect routine near `0x4012dc20` writes two globals:
  - old servo: supply pin = 21, left-legs pin = 0, flag `0x3ffc21c4` = 0
  - new servo: supply pin = 0, left-legs pin = 21, flag = 1
  Detection: power the supply pin, query servo version (`0xDD`) over RMT, and if
  a servo answers `0xDD` it is a new servo, otherwise old.
- `FUN_40262f7c` (MCPWM init) routes MCPWM0A to the left-legs global, and
  0B / 1A / 1B to the constants 25 / 5 / 26.
- Procedure to repeat it: decompile the init, follow every write to the pin
  globals, and cross-check with the stock serial log.

## Known limits

- **Cliff / foot sensor is disabled** while the left-legs signal is on GPIO0
  (same pin as I2C SDA). Do not leave the robot walking near a table edge.
- **New-servo board: pins and supply only.** Supply on GPIO0, left legs on
  GPIO21. We then drive the same 50 Hz MCPWM as on the old board, but the stock
  gait skips MCPWM when its new-servo flag is set, so the real new-servo motion
  path is not understood. Still to do:
  - confirm the detect decoder on a real new-servo robot,
  - reverse how the stock firmware moves new servos, and
  - test on a robot with new servos.
- On the new board the foot-sensor I2C is skipped (GPIO0 is the supply).
- Wi-Fi settings are lost when flashing the stock image over this firmware
  (different partition table). Re-enter them in the `emo-bridge` portal.

## Auto-dock and explore speed

Charger detection uses GPIO36 (attery_get().sense, True while charging; a USB
debug cable also counts as a charger). Behaviour (`main/behavior.c`, `dock_tick`):

- Charger on (stable, 2.5 s debounce): stop motion, pose 90/90/90/90, mode `docked`.
- `CONFIG_EMO_DOCK_LIMP_S` s later (default 10): release the servos (limp).
- Charger removed: back to `idle`, explore starts about 1.5 s later.
- Disable with `CONFIG_EMO_AUTO_DOCK=n` (the manual `docked` setting still works).

Explore gait is slowed with `CONFIG_EMO_EXPLORE_PERIOD_PCT` (default 220, period
x2.2) and `CONFIG_EMO_EXPLORE_AMP_PCT` (default 150, amplitude x1.5, clamped to +/-24 deg).
Note: `sdkconfig` is not regenerated when building here, so add new `CONFIG_*`
lines to it by hand.
