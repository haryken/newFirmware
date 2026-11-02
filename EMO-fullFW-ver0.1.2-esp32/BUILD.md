# Build firmware → bin

ESP-IDF **v5.5.x** (hoặc ≥5.5), target **esp32** (WROVER + PSRAM). Project: `EMO-fullFW-ver0.1.2-esp32/`.

Máy bạn đang dùng: `C:\Espressif\frameworks\esp-idf-v5.5.4` — ổn.

## 1. Môi trường

Windows (PowerShell) — mở **ESP-IDF 5.5 PowerShell** từ Start Menu, hoặc:

```powershell
# Espressif installer:
C:\Espressif\frameworks\esp-idf-v5.5.4\export.ps1
cd C:\Users\LEGION\Desktop\otto\newFirmware\EMO-fullFW-ver0.1.2-esp32
```

Linux / macOS:

```bash
. $HOME/esp/esp-idf/export.sh   # path theo máy bạn
cd .../EMO-fullFW-ver0.1.2-esp32
```

## 2. Build

Lần đầu (hoặc đổi IDF):

```bash
idf.py set-target esp32
idf.py build
```

Output chính:

| File | Ý nghĩa |
|---|---|
| `build/emo_bridge.bin` | App firmware (`project(emo_bridge)`) |
| `build/bootloader/bootloader.bin` | Bootloader |
| `build/partition_table/partition-table.bin` | Partition table |
| `build/storage.bin` (hoặc qua `storage-flash`) | SPIFFS `.mot` |

Xem tên app bin chính xác:

```bash
idf.py build
# hoặc
dir build\*.bin
```

Thường flash all-in-one:

```bash
idf.py -p COMx flash monitor
```

(`COMx` = cổng USB-UART của EMO.)

Chỉ SPIFFS motions:

```bash
idf.py -p COMx storage-flash
```

## 3. Backup stock trước khi flash

```bash
esptool.py -p COMx read_flash 0 0x400000 emo_stock_backup.bin
```

## 4. Sau khi lên FW

1. SoftAP `emo-bridge` / `emobridge` nếu chưa có Wi‑Fi → `http://192.168.4.1:8080`
2. Test tricks (không cần Xiaozhi):

```bash
curl -X POST http://<ip>:8080/api/tricks -H "Content-Type: application/json" -d "{\"action\":\"camping\"}"
curl -X POST http://<ip>:8080/api/tricks -d "{\"action\":\"timer\",\"seconds\":10}"
curl -X POST http://<ip>:8080/api/tricks -d "{\"action\":\"explore\",\"seconds\":15}"
curl -X POST http://<ip>:8080/api/tricks -d "{\"action\":\"game\",\"game\":\"rps\"}"
curl -X POST http://<ip>:8080/api/tricks -d "{\"action\":\"stop\"}"
```

3. Xiaozhi: `voice_mode=xiaozhi` + `ota_url` trên web, rồi wake EMO.

## 5. Lỗi thường gặp

- **Thiếu PSRAM / sai target:** phải `esp32` + module có PSRAM.
- **Component Opus/WS lần đầu:** `idf.py build` sẽ fetch từ component registry (cần mạng).
- **IDF không trong PATH:** chạy `export.ps1` / `export.sh` trước.
