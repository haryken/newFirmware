# Behaviors chưa làm được (blocked / cần RE)

Custom ESP32 + K210 stock 3.1.0. Các mục dưới **chưa implement** vì thiếu subsystem hoặc rủi ro brick.

Xem nhóm chắc / đã làm: [BEHAVIOR_FEASIBILITY.md](BEHAVIOR_FEASIBILITY.md).

---

## Danh sách blocked

| Mục | Vì sao chưa | Cần làm để mở |
|---|---|---|
| **Go home** tự động | Không RX `0x62`, không BLE home-station FSM | Log stock go-home; parse `0x62`; FSM walk→dock; có station trong lab |
| **Take photo full** | UART/SD photo danger-gate; chưa RX SPI file | Backup SD; RE capture; SPI reassembly; HTTP `/api/photo` |
| **Games CV thật** (RPS tay, paint shot, …) | Chưa RE detect `0x53` / blob `0x35` / hand | CF/RE + FSM; trước mắt chỉ dùng **game demo** (anim) |
| **Explore stock 100%** | Walker liên tục + IMU tường + auto go-home | Sau khi go-home OK; explore gated (đã có path chắc) là đủ tạm |
| **Alarm / timer Living.AI cloud** | Sync app Living.AI | Dùng **timer local** trên FW; bỏ cloud trừ khi reverse API |
| **BT speaker A2DP** | Chưa Bluedroid trong project | IDF A2DP sink → `speaker_play`; test Wi‑Fi coexist |
| **Smart light Living.AI** | Không phải HW robot | Hue/`local-brain` hoặc HA trên PC; MCP gọi host |

---

## Không nhầm với demo chắc

Các cái sau **không** thuộc file này — làm được bằng `.mot` / timer ESP (xem code `tricks.c`):

- Camping scripted  
- Timer / alarm **local**  
- Explore **có giới hạn** (cliff + timeout)  
- Game **demo** (RPS/TTT/zombie/fix-bugs anim)

---

## Checklist mở khóa (bạn làm)

1. **Go home:** station + UART log + `0x62` handler.  
2. **Photo:** bench + danger flag có kiểm soát + 1 ảnh HTTP.  
3. **CV games:** log `0x53` khi chơi stock.  
4. **BT:** branch riêng A2DP.  
5. **Light:** không làm trên ESP trừ khi muốn HTTP→Hue.
