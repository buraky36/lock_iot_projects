# V2.0.6 Task Execution Checklist

- `[x]` **Varyasyon ve Donanım Tanımları (Faz 1):**
  - Reader Modu (Mod 3) kaldırıldı.
  - `RFID_ONLY` Controller varyasyonu (Product ID `0x0112`) eklendi.
  - `#define` yapıları (`HW_HAS_KEYPAD`, `HW_HAS_QR`, `HW_HAS_RFID`, `HW_BOOT_MASTER_ENROLL`) güncellendi.
  - Tüm varyasyonlarda Wi-Fi, BLE ve MQTT tam zamanlı aktif hale getirildi.
  - Wiegand Output tamamen kaldırıldı; Wiegand Input tüm varyasyonlarda aktif bırakıldı.

- `[x]` **PIN+QR Offline Yetenekleri (Faz 2):**
  - Master QR, Kullanıcı QR, Master Add ve Master Delete modları eklendi.
  - 31 karakterden uzun QR kodlar için mbedtls SHA-256 (31 hex) hashleme eklendi.
  - Yerel NVS blob yapısı (`sys_config_t`) %100 geriye dönük uyumlu korundu (V2.0.4 saha cihazları için).

- `[x]` **RFID_ONLY Controller & Donanımsal Reset (Faz 3):**
  - Açılışta 10 saniyelik Master Kart kayıt penceresi (10 kart max) uygulandı.
  - IN2 pini dahili pull-up ile yapılandırıldı; boot anında 10 saniye GND'ye çekildiğinde Donanımsal Fabrika Reset tetiklendi.

- `[x]` **Debug Log Temizliği (Faz 4):**
  - BLE bildirim logları, MQTT per-packet logları, XR1300 ham okuma logu ve 25s periyodik heartbeat yoruma alındı (`// [V2.0.6 log cleanup]`).

- `[x]` **RS485 & OSDP Protokol Entegrasyonu (Faz 5):**
  - Half-duplex RS485 UART2 sürücüsü (TX=GPIO 26, RX=GPIO 34, DE/RTS=GPIO 18) kuruldu.
  - OSDP v2.2 PD stack'i (`components/osdp_pd`) geliştirildi.
  - `osdp_POLL`, `osdp_ID`, `osdp_CAP`, `osdp_OUT`, `osdp_LED`, `osdp_BUZ`, `osdp_COMSET` komutları işlendi.
  - Kart, QR, PIN ve tuş olayları OSDP ACU'ya iletildi.

- `[x]` **Build Çıktıları ve Dağıtım Yapısı (Faz 6):**
  - `build_all.py` modernize edildi.
  - Eski `releases/` (V2.0.3, V2.0.4, V2.0.5) `outputs/` altına taşındı, eski klasörler temizlendi.
  - `outputs/OE_0110_PIN_QR/V2.0.6/`, `outputs/OE_0111_PIN_RFID/V2.0.6/`, `outputs/OE_0112_RFID_ONLY/V2.0.6/` klasörleri oluşturuldu.
  - Her klasöre `bootloader.bin`, `firmware.bin`, `ota_data_initial.bin`, `partitions.bin`, `full_firmware_OE011x.bin` ve `FLASH_ADDRESSES.txt` yerleştirildi.

- `[x]` **Dokümantasyon (Faz 7):**
  - `docs/user_manual_reference.md` güncellendi.
