# Onloi Access Control Terminal Firmware

Bu depo, Onloi Akıllı Geçiş Kontrol Terminali ailesi için geliştirilmiş resmi gömülü yazılım (firmware) projesidir.

---

## 1. Proje Özeti ve Teknoloji Yığını

- **Mikrodenetleyici:** Espressif ESP32 (ESP32-D0WD-V3, 240 MHz, Dual Core, 4MB Flash)
- **Framework:** ESP-IDF v5.3.1
- **Geliştirme Ortamı:** Visual Studio Code + PlatformIO
- **Platform Paketi:** `espressif32@6.9.0`
- **Mevcut Kaynak Versiyonu:** `v2.0.28` (Test & Geliştirme Modu: ESP-IDF logları ve Seri Port CLI açık)

---

## 2. Donanım Modelleri ve Ortamlar (Environments)

Yazılım tek bir kod tabanından derlenir ve derleme bayrakları (`build_flags`) ile 3 farklı donanım varyantını destekler:

| Model Kodu | PlatformIO Env | Açıklama | Donanım Özellikleri & Build Flags |
| :--- | :--- | :--- | :--- |
| **OE0111** | `pin_rfid` | PIN + RFID Terminal | `COMPILE_HW_VERSION=2`, Tuş Takımı (TSM12), RFID (MFRC522) |
| **OE0112** | `rfid_only` | Sadece RFID Terminal | `COMPILE_HW_VERSION=3`, RFID (MFRC522) (Tuş Takımı & QR yok) |
| **OE0113** | `pin_qr` | PIN + QR Kod Terminal | `COMPILE_HW_VERSION=1`, Tuş Takımı (TSM12), 2D Barkod/QR (XR1300) |

---

## 3. Kurulum ve Geliştirme Ortamı

1. **Gereksinimler:**
   - [Visual Studio Code](https://code.visualstudio.com/)
   - VS Code içine **PlatformIO IDE** eklentisi (proje açıldığında otomatik önerilir)
   - Python 3.10+ (sistem yolunda veya PlatformIO sanal ortamında)
2. **Projeyi Açma:**
   - VS Code ile `Onloi_Access_Control_Terminal_FW` klasörünü açın.
   - PlatformIO ilk açılışta `platformio.ini` dosyasını okuyarak ESP-IDF 5.3.1 araç zincirini (`xtensa-esp32-elf`, `tool-ninja`, `tool-cmake` vb.) ve `dependencies.lock` içindeki Espressif kütüphanelerini (`espressif/led_strip` v3.0.3) otomatik olarak indirip hazırlar.

---

## 4. Temel Komutlar

PlatformIO terminalinde veya sistem terminalinde çalıştırabileceğiniz komutlar:

```bash
# Belirli bir donanım ortamını derleme
pio run -e pin_rfid    # OE0111 derlemesi
pio run -e rfid_only   # OE0112 derlemesi
pio run -e pin_qr      # OE0113 derlemesi

# Cihaza firmware yükleme (PlatformIO bağlı COM portu otomatik algılar)
pio run -e pin_rfid -t upload

# Seri Port Monitörünü Başlatma (115200 baud)
pio device monitor -b 115200

# Tüm varyasyonları tek seferde derleme, birleştirme (0x0000 merged) ve paketleme:
python build_all.py
```

---

## 5. Proje ve Bileşen (Component) Yapısı

```text
Onloi_Access_Control_Terminal_FW/
├── .vscode/               # VS Code PlatformIO eklenti önerisi
├── components/            # Modüler ESP-IDF bileşenleri (16 adet)
│   ├── api_client         # Backend REST API ve MQTT haberleşmesi, olay bildirimi
│   ├── app_state_machine  # Durum makinesi, geçiş izinleri ve röle mantığı
│   ├── ble_prov           # BLE GATT üzerinden Wi-Fi provizyon servisi
│   ├── hal_io             # Giriş/çıkış, LED ve sesli buzzer donanım sürücüsü
│   ├── hal_qr             # QR okuyucu UART donanım soyutlama katmanı
│   ├── hal_rfid           # RFID kart okuma donanım soyutlama katmanı
│   ├── hal_shift_reg      # 74HC595 shift register sürücüsü (LED/röle çoğullama)
│   ├── hal_touch          # Tuş takımı donanım soyutlama katmanı
│   ├── hal_wiegand        # Wiegand 26/34 protokol giriş/çıkış sürücüsü
│   ├── mfrc522            # NXP MFRC522 13.56 MHz RFID/NFC çip sürücüsü
│   ├── nv_storage         # NVS üzerinde kullanıcı veri tabanı, ayar ve çevrimdışı log saklama
│   ├── osdp_pd            # SIA OSDP v2.2 Peripheral Device (RS-485) protokol sürücüsü
│   ├── ota_manager        # HTTP/HTTPS üzerinden firmware OTA yöneticisi ve rollback kontrolü
│   ├── tsm12              # ADS TSM12 12-kanal I2C dokunmatik tuş takımı sürücüsü
│   ├── wifi_manager       # Wi-Fi istasyon bağlantı yönetimi ve otomatik tekrar bağlanma
│   └── xr1300             # Newland XR1300 2D barkod/QR tarayıcı sürücüsü
├── src/
│   ├── CMakeLists.txt     # Uygulama kaynak kayıt dosyası
│   └── main.c             # Sistem başlatma, donanım kontrolü ve süper-döngü
├── build_all.py           # Otomatik 3 varyasyonlu derleme ve paketleme aracı
├── CMakeLists.txt         # ESP-IDF üst düzey proje CMake tanımı
├── dependencies.lock      # Espressif IDF component manager kilit dosyası
├── partitions.csv         # 4 MB Flash bölümleme tablosu
├── platformio.ini         # PlatformIO ortam ve derleme konfigürasyonu
├── sdkconfig.defaults     # Genel ESP-IDF çekirdek ayarları
├── sdkconfig.pin_qr       # PIN+QR modeli ESP-IDF ayarları
├── sdkconfig.pin_rfid     # PIN+RFID modeli ESP-IDF ayarları
└── sdkconfig.rfid_only    # RFID-Only modeli ESP-IDF ayarları
```

---

## 6. Flash Haritası ve Yükleme Adresleri

Proje, 4 MB Flash bellek üzerinde çift OTA bölümü (`ota_0`, `ota_1`) kullanır:

| Bölüm Adı | Tür | Alt Tür | Flash Ofset | Boyut | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **bootloader** | boot | - | `0x1000` | 28 KB | İkinci aşama ESP-IDF Bootloader (`bootloader.bin`) |
| **partitions** | data | partitions | `0x8000` | 3 KB | Bölüm Tablosu (`partitions.bin`) |
| **nvs** | data | nvs | `0x9000` | 1024 KB | Kalıcı ayarlar, çevrimdışı loglar ve kullanıcı veritabanı |
| **otadata** | data | ota | `0x109000` | 8 KB | Aktif OTA slotu göstergesi (`ota_data_initial.bin`) |
| **phy_init** | data | phy | `0x10B000` | 4 KB | RF kalibrasyon verileri |
| **ota_0** | app | ota_0 | `0x110000` | 1400 KB | Varsayılan birincil uygulama yuvası (`firmware.bin`) |
| **ota_1** | app | ota_1 | `0x26E000` | 1400 KB | İkincil OTA güncelleme yuvası |

### Yükleme Adresleri (Ayrı Dosyalar):
```text
0x1000   -> bootloader.bin
0x8000   -> partitions.bin
0x109000 -> ota_data_initial.bin
0x110000 -> firmware.bin
```

### Tek Parça Fabrika İmajı (Merged):
`build_all.py` çalıştırıldığında yukarıdaki parçaları birleştirip `0x0000` adresinden doğrudan tek bir dosya olarak yazılabilecek `full_firmware_*.bin` dosyasını `outputs/` klasörüne üretir.

---

## 7. Sürüm Yönetimi ve Saha / Üretim Sürümü Üretimi

- **Geliştirme / Test Sürümü (`V2.0.28`):**
  - Kaynak kodda varsayılan olarak tanımlıdır.
  - Açılışta UART0 (115200) üzerinden detaylı ESP-IDF logları ve donanım test durumları basılır.
  - Seri port üzerinden CLI komutları (`onloi_fab_reset`, `onloi_user_list_view` vb.) aktiftir.

- **Saha / Müşteri Sürümü Üretimi:**
  Sahaya gidecek cihazlar için seri port çıktılarını tamamen kapatmak amacıyla:
  1. `src/main.c` dosyasında:
     ```c
     #define FW_VERSION_PATCH   29
     #define FW_VERSION_STR     "v2.0.29"
     #define PRODUCTION_RELEASE 1
     ```
  2. `sdkconfig.pin_qr`, `sdkconfig.pin_rfid` ve `sdkconfig.rfid_only` dosyalarında:
     ```ini
     CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y
     CONFIG_BOOTLOADER_LOG_LEVEL=0
     CONFIG_LOG_DEFAULT_LEVEL_NONE=y
     CONFIG_LOG_DEFAULT_LEVEL=0
     ```
  3. `python build_all.py` komutuyla paketleri üretin.
  4. **OTA Dağıtımı:** OTA sunucusuna veya panel sistemine yalnızca `firmware.bin` dosyası yüklenir.

---

## 8. Kritik Mimari ve Güvenlik Uyarıları

> [!IMPORTANT]
> **1. Bölümleme Tablosu Bütünlüğü (`partitions.csv`):**
> `partitions.csv` üzerindeki adres ve boyutlar kesinlikle değiştirilmemelidir. Bu tablonun değiştirilmesi durumunda sahadaki mevcut cihazların (v2.0.13 dahil) OTA güncelleme uyumluluğu geri dönüşsüz biçimde bozulur.

> [!CAUTION]
> **2. `fclose(stdout/stdin/stderr)` Kullanılmamalıdır:**
> ESP-IDF 5.3.1 Newlib C kütüphanesinde standart stream'ler kapatıldığında dahili kilit mutex nesnesi serbest bırakılır. Bu durum sonraki herhangi bir `printf` veya Newlib çağrısında `assert failed: check_lock_nonzero` hatası üreterek cihazın sürekli `SW_CPU_RESET` boot loop döngüsüne girmesine yol açar. Seri portu susturmak için `src/main.c` içindeki `silent_vprintf` ve `gpio_reset_pin(GPIO_NUM_1/3)` mekanizması kullanılmalıdır.

> [!WARNING]
> **3. Flash Encryption Durumu:**
> Bu projede donanımsal Flash Encryption **kullanılmamaktadır**. Flash Encryption ESP32 üzerindeki tek yönlü (irreversible) eFuse bitlerini yakar ve cihazı kalıcı olarak kilitler. Flash Encryption etkinleştirilmesi düşünülürse NVS anahtar yönetimi (`nvs_keys`) ve eFuse yakım prosedürleri için mutlaka Ar-Ge ekibiyle koordine olunmalıdır.

