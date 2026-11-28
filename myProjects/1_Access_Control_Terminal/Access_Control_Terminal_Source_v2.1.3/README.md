# Access Control Terminal Firmware (v2.1.3)

ESP32 tabanlı Erişim Kontrol Terminali kaynak kodu (**v2.1.3 Release** / **v2.1.2 Test**).

## Gereksinimler

- **VS Code + PlatformIO IDE** eklentisi (veya **PlatformIO CLI**)
- PlatformIO platformu: `espressif32@6.9.0` (ESP-IDF **5.3.1** otomatik olarak yönetilir)
- Flash boyutu: `4MB` (`partitions.csv` özel bölümleme tablosu)
- Bağımlılıklar: `managed_components/` ve `dependencies.lock` içerisinde sabitlenmiştir (`espressif/led_strip` v3.0.3).

> **Not:** `sdkconfig.pin_qr`, `sdkconfig.pin_rfid` ve `sdkconfig.rfid_only` dosyaları **v2.1.3** üretim (sessiz seri log / `LOG_DEFAULT_LEVEL_NONE` ve `BOOTLOADER_LOG_LEVEL_NONE`) yapılandırmasını içerir. Test / Debug derlemesi almak için `src/main.c` içindeki `PRODUCTION_RELEASE` makrosunu `0` yapabilirsiniz.

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

---

## Yazılım Revizyon Notu — v2.1.2 (Test) / v2.1.3 (Release)

**Başlık:** `[QR Engine & UART] XR1300 Donanımsal CRC16_XMODEM Doğrulaması, Dahili PWM Gürültü Susturma ve Bozuk Çerçevede Anında Yeniden Tetikleme`

### 1. Sorunun Tanımı (Kök Neden)
- `OE0113 (PIN + QR)` modelinde dahili `XR1300` QR modülünün bağlı olduğu ESP32 `GPIO 39` (`SENSOR_VN`) hattında dahili pull-up direnci bulunmaması (High-Z) ve `XR1300` modülünün okuma anında kendi içinde 2.7 kHz PWM buzzer sürmesi nedeniyle, 9600 bps UART üzerinden iletilen verilerde zaman zaman tek veya çoklu bit bozulmaları (`1 <-> 0`) oluşabilmekteydi.
- Daha önceki sürümlerde (`v2.1.1`) bulunan şablon tabanlı/karakter tahminli düzelticiler Base64 veya serbest formatlı QR yüklerinde geçersiz kalmakta ve bozuk paketi sunucuya iletilmekten tamamen engelleyememekteydi.

### 2. Yapılan Yazılımsal İyileştirmeler (`xr1300.c` & `hal_wiegand.c`)
1. **Karakter Tahmin Etme (Heuristic Sanitizer) Tamamen Kaldırıldı:**
   - `xr1300.c` ve `hal_wiegand.c` içindeki tüm şablon/karakter tahmin kodları temizlendi; hiçbir koşulda bozuk karakter tahmini yapılarak veri değiştirilmemektedir.
2. **XR1300 Donanımsal `CRC16_XMODEM` Çıkışı Aktif Edildi (`$202301-9B71`):**
   - Modül açılışında `XR1300` üzerine donanımsal `CRC16_XMODEM` (`0x1021`) checksum ekleme özelliği (`$202301-9B71`) ve kalıcı NVM kaydı (`$010207-26A3`) gönderildi.
   - `xr1300_task()` içindeki UART alıcısı `512 byte` kapasiteye çıkarılıp `\r`/`\n` satır sonlarına göre izole edildi; böylece arka arkaya hızlı okutulan uzun QR paketlerinin birbiriyle birleşmesi önlendi ve gelen tam çerçeve 63 bayta kısaltılmadan önce `xr1300_crc16_xmodem()` ile doğrulanır. 4 karakter ASCII HEX veya 2 bayt binary CRC eşleşirse CRC eki kesilerek yalnızca %100 doğrulanmış temiz payload iletilir.
3. **Bozuk Çerçeve Reddi ve Anında Yeniden Okuma Tetiklemesi (`$108000-ADB0`):**
   - `CRC16` doğrulamasından geçemeyen (hatta basılabilir ASCII `32..126` dışına düşen tek bir biti bile bozulmuş) paketler kesinlikle düşürülür ve `2500 ms` okuma bekleme süresi (cooldown) tetiklenmez.
   - Aynı anda `XR1300`'e UART TX (`GPIO 25`) üzerinden `$108000-ADB0\r\n` (*Command Trigger 1*, min `150 ms` aralıkla) gönderilerek kameranın QR kodu anında temiz olarak tekrar okuması sağlanır; açılış doğrulamaları sonrası `$020B00-33A1` ile çalışma zamanı komut ACK yanıtı (`$000100-6148`) kapatılarak yeniden tetikleme yanıtlarının QR verisine karışması engellendi.
4. **XR1300 Dahili PWM Gürültü Susturma ve Hızlı Tekrar Okuma Ayarları:**
   - UART aktarımı sırasında modül içi 2.7 kHz PWM kaynaklı voltaj/saat dalgalanmasını kesmek için `$150000-EC9C` (Power-Up Beep kapalı) ve `$150100-9A28` (Read Beep kapalı) uygulandı.
   - Okuma aralıkları `$100300-3441` ve `$101304-52D4` ile minimize edildi ve standart çerçeve sonu için `$201003-881D` (`CRLF`) aktif edildi.
5. **`OE0112 (RFID_ONLY)` LED Senaryosu (`v2.1.2`):**
   - `OE0112 (RFID_ONLY)` modelinde bekleme modunda (IDLE) ve kart okutma uyandırmasında tuş takımı arka aydınlatma LED'lerinin sönük kalması (`ANIM_NONE`) sağlandı.
