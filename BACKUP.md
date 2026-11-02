# Backup / restore ESP32 EMO

Tài liệu này **chỉ gom lại** các bước đã ghi trong repo:

- [`README.md`](README.md)
- [`How to backup.txt`](How to backup.txt)

Không thêm bước ngoài hai file đó. Giả định: **PowerShell trên Windows**, đổi `COMx` / `COM25` / `COM11` thành cổng serial ESP32 thật, đổi đường dẫn `.bin` nếu muốn lưu chỗ khác.

---

## Flash layout (từ `README.md`, theo `partitions.csv`)

| Mục | Offset / size |
|-----|----------------|
| Full flash | `0x1000000` (16 MB) |
| App slot `ota_0` | `0x20000` |
| SPIFFS `storage` | offset `0xA20000`, size `0x100000` |

SPIFFS geometry dùng trong project:

- Block size: `4096`
- Page size: `256`

---

## Full backup (bắt buộc trước khi thử custom)

Theo `README.md` và `How to backup.txt`:

- Đọc **toàn bộ** 16 MB flash.
- Nên dump **ít nhất 5 lần** và **so hash** để chắc backup ổn định (`README.md`: *dump it at least 5 times and compare hashes*; `How to backup.txt`: *download the file at least 5 times*).

Lệnh (giống trong cả hai file; ví dụ port `COM25`, file trên Desktop):

```powershell
esptool.py --chip esp32 -p COM25 -b 115200 read_flash 0x0 0x1000000 "$env:USERPROFILE\Desktop\esp32_full_16MB.bin"
```

Đổi `-p COM25` và đường dẫn output nếu cần (`How to backup.txt`).

> Ghi chú trong `EMO-fullFW-ver0.1.2-esp32/README.md` còn có ví dụ ngắn hơn:  
> `esptool.py read_flash 0 0x400000 stock.bin`  
> Bản **chuẩn theo root project** (`README.md` / `How to backup.txt`) là full **`0x0` + `0x1000000` (16 MB)**. Nên dùng bản 16 MB làm backup chính trước khi test custom (`README.md` Safety Notes: *Keep at least one untouched full 16 MB backup*).

---

## Full restore

Theo `README.md`: ghi lại toàn bộ 16 MB từ file backup. **Full restore ghi đè mọi thứ**, gồm cả hai app slot và SPIFFS.

```powershell
esptool.py --chip esp32 -p COM11 -b 115200 write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$env:USERPROFILE\Desktop\esp32_full_16MB.bin"
```

Cùng thao tác trong `How to backup.txt` (port ví dụ `COM11`), baud **`460800`**:

```powershell
esptool.py --chip esp32 -p COM11 -b 460800 write_flash --flash_mode keep --flash_freq keep --flash_size keep 0x0 "$env:USERPROFILE\Desktop\esp32_full_16MB.bin"
```

Đổi `-p` cho đúng cổng ESP32. `How to backup.txt`: *this will rewrite the entire ROM*.

---

## Backup chỉ SPIFFS (`storage`)

Theo `README.md` — chỉ đọc partition `storage`:

```powershell
esptool.py --chip esp32 -p COMx -b 115200 read_flash 0xA20000 0x100000 "$env:USERPROFILE\Desktop\storage.bin"
```

### Giải nén file từ `storage.bin`

`spiffsgen.py` chỉ **tạo** image SPIFFS, không unpack. Dùng `mkspiffs` với geometry khớp project:

```powershell
mkspiffs -u .\spiffs_extracted -b 4096 -p 256 -s 0x100000 "$env:USERPROFILE\Desktop\storage.bin"
```

Theo `README.md`:

- Output: thư mục `.\spiffs_extracted`
- Với firmware image này, file giải ra là motion dưới `mot\`
- Muốn dump từ máy rồi unpack: chạy backup SPIFFS ở trên trước, rồi lệnh `mkspiffs` này

### Flash SPIFFS trở lại máy

```powershell
esptool.py --chip esp32 -p COMx -b 115200 write_flash --flash_mode keep --flash_freq keep --flash_size keep 0xA20000 .\build\storage_idf55.bin
```

---

## Flash custom firmware (không phải backup; ghi trong cùng docs)

Chỉ thay app tại `ota_0` (`0x20000`). Chạy từ **root project** (`How to backup.txt`).

`README.md` / `How to backup.txt`:

```powershell
esptool.py --chip esp32 -p COMx -b 115200 write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB 0x20000 .\build\emo_firmware.bin
```

Theo `README.md`:

- Flash app-only tại `0x20000` **không** restore nội dung SPIFFS.
- Sau khi flash `base-firmware.bin`, kết nối hotspot ESP32, set Wi‑Fi; web qua `node-manager.local`.
- Cài app qua Apps Manager trên web UI; app test thì bỏ check auto-start trước khi install.

---

## Safety notes (nguyên văn ý từ `README.md`)

- Double-check COM port trước khi **write**.
- Full restore ghi đè mọi thứ (cả hai app slot và SPIFFS).
- App-only flash tại `0x20000` không restore SPIFFS.
- Giữ ít nhất **một** bản full 16 MB chưa đụng trước khi test custom builds.

---

## Nguồn trong repo

| Nội dung | File |
|----------|------|
| Layout, full backup/restore, SPIFFS, custom flash, safety | `README.md` |
| Backup ≥ 5 lần, restore (baud 460800), custom flash | `How to backup.txt` |
| Gợi ý `read_flash 0 0x400000 stock.bin` trước khi flash board stock | `EMO-fullFW-ver0.1.2-esp32/README.md` |
