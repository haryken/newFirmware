# Approve → build (EMO custom ESP32 + K210 stock)

Checklist để **duyệt rồi bảo làm**.  
Phạm vi: đủ cơ sở code / RE — **không** = đã có sẵn trong FW.

Chi tiết cliff/sensor: `[RE_BEHAVIOR_SENSORS.md](RE_BEHAVIOR_SENSORS.md)`  
Xiaozhi / Wi‑Fi: `[EMO_XIAOZHI_INTEGRATION.md](EMO_XIAOZHI_INTEGRATION.md)`

---

## A. Đủ cơ sở — trạng thái implement

### A1. Explore + tránh vực — **DONE (an toàn)**

- [x] Đọc 4 cliff I2C FOOT `0x40`/`0x41` (`foot_cliff.c`) — fail → tắt cliff, không brick
- [x] Bitmask thr `<50`
- [x] Khi **đang play motion** + mask → stop + `cliff_L` / `cliff_R` / `cliff_B` + media
- [x] **Không** auto-walk explore (tránh rơi bàn nếu cliff lỗi)

### A2. Mắt / face “có hồn” — **DONE**

- [x] Idle scheduler: `stay_wink_1` / `stay_a_b` / `stay_c_d` / `look_around_*` định kỳ
- [x] `legs` mặc định `media=true` (đã có)
- [x] Daily_* chỉ qua API motion (không auto)

### A3. YOLO → face — **DONE nhẹ (an toàn)**

- [x] Parse UART `0x3F` → `behavior_on_yolo` → `see_EMO` (cooldown 8s)
- [ ] face_id / register_face — **không làm** (CF chưa xong, không 100%)

### A4. Voice / Xiaozhi — **SKIP**

- [ ] Không implement trong đợt này (không “chắc 100%”, cần server/Opus riêng)

### A5. Petting — **DONE**

- [x] Touch pads 7/8/9 thr `0x96` (`touch_pet.c`)
- [x] get_in → loop enjoy → get_out (`touch_*` + media)

### A6 + Web — **DONE**

- [x] SoftAP + HTTP API (đã có) trên **`:8080`**
- [x] Boot face text: `IP:8080`
- [x] Web toggles: docked / idle / cliff / petting / yolo

### C8. Lên sạc đứng yên — **DONE (web docked)**

- [x] Checkbox **On charger / docked** → hold 90° + `charge_get_in_new`, chặn explore/walk
- [ ] Auto-detect GPIO charge 100% — **không** (sense GPIO36 = divider, không chắc = dock)

---

## B. Không làm (brick / danger)

| Mục | Lý do |
|-----|--------|
| Livestream cam / JPEG | Không pipeline; photo UART **danger** |
| Chụp ảnh Living-style | gated / brick risk |
| Thay K210 FW | ngoài scope |
| Xiaozhi full | chưa chắc 100% |
| Auto explore walk | nguy hiểm nếu cliff fail |
| Games RPS/TTT ship | optional, không chặn A1 |

---

## Module mới (custom FW)

| File | Vai trò |
|------|---------|
| `main/foot_cliff.c` | I2C FOOT cliff |
| `main/touch_pet.c` | Touch pads petting |
| `main/behavior.c` | Orchestrator task |
| `http_server.c` | port **8080** |
| `k210_boot.c` | banner `IP:8080` |
| `settings` + web | docked + behaviour flags |

Flash app + mở `http://<ip>:8080`. Trên đế: bật **On charger / docked**.
