# Access Control Terminal Firmware (v2.1.1)

ESP32 tabanlı Erişim Kontrol Terminali kaynak kodu (**v2.1.1**).

## Gereksinimler

- **VS Code + PlatformIO IDE** eklentisi (veya **PlatformIO CLI**)
- PlatformIO platformu: `espressif32@6.9.0` (ESP-IDF **5.3.1** otomatik olarak yönetilir)
- Flash boyutu: `4MB` (`partitions.csv` özel bölümleme tablosu)
- Bağımlılıklar: `managed_components/` ve `dependencies.lock` içerisinde sabitlenmiştir (`espressif/led_strip` v3.0.3).

> **Not:** `sdkconfig.pin_qr`, `sdkconfig.pin_rfid` ve `sdkconfig.rfid_only` dosyaları **v2.1.1** üretim (sessiz seri log / `LOG_DEFAULT_LEVEL_NONE` ve `BOOTLOADER_LOG_LEVEL_NONE`) yapılandırmasını içerir.

## Donanım Ortamları (Environments)

| Environment | Model Kodu | Açıklama | Yapılandırma Dosyası |
| :--- | :--- | :--- | :--- |
| `pin_qr` | OE0113 | Keypad (PIN) + QR Okuyucu | `sdkconfig.pin_qr` |
| `pin_rfid` | OE0111 | Keypad (PIN) + RFID Okuyucu | `sdkconfig.pin_rfid` |
| `rfid_only` | OE0112 | Sadece RFID Okuyucu | `sdkconfig.rfid_only` |

## Derleme ve Yükleme Komutları

### Tüm varyasyonları derleme (PlatformIO)
```bash
pio run
```

### Tek bir varyasyonu derleme
```bash
pio run -e pin_rfid
```

### Tüm varyasyonları derleyip `outputs/` altında paketleme
Merge binary (`0x0000`), parçalı binary dosyaları, `FLASH_ADDRESSES.txt` ve `.zip` paketlerini otomatik üretir:
```bash
python build_all.py
```

### Cihaza yükleme (Upload)
Seri port otomatik algılanır. Belirli bir port vermek isterseniz `--upload-port` parametresini veya `platformio.ini` içindeki `upload_port` satırını kullanabilirsiniz:
```bash
pio run -e pin_rfid -t upload
```
Port belirterek yükleme örneği:
```bash
pio run -e pin_rfid -t upload --upload-port COM3
```
