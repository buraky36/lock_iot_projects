# Akıllı Geçiş Kontrol Terminali - Kullanım Kılavuzu Referans Belgesi (V2.0.6)

Bu doküman, cihazın özellikleri, donanım varyasyonları, çalışma/geçiş modları, programlama adımları, OSDP/RS485 protokolü ve ağ (Online/Offline) kuralları hakkında User Manual (Kullanıcı Kılavuzu) hazırlarken faydalanabileceğiniz güncel teknik ve operasyonel bilgileri içermektedir.

---

## 1. Donanım Varyasyonları ve Temel Özellikler

Cihaz mimarisi 3 farklı **Controller (Denetleyici)** varyasyonu olarak sunulmaktadır:
1. **OE0110 (PIN + QR):** Dokunmatik tuş takımı ve XR1300 QR/Barkod okuyucuya sahiptir.
2. **OE0111 (PIN + RFID):** Dokunmatik tuş takımı ve MFRC522 13.56MHz Mifare RFID okuyucuya sahiptir.
3. **OE0112 (RFID ONLY):** Sadece MFRC522 RFID okuyucuya sahiptir; tuş takımı bulunmaz.

### Ortak Donanım Özellikleri:
- **İşlemci:** ESP32 tabanlı çift çekirdekli mikrodenetleyici (240MHz, 4MB Flash).
- **Kullanıcı Arayüzü:** Çok renkli WS2812B RGB LED durum göstergesi, PWM kontrollü sesli uyarıcı (Buzzer) ve tuşlu modellerde TSM12 kapasitif dokunmatik tuş takımı.
- **Wiegand Girişi:** Tüm varyasyonlarda harici Wiegand okuyucuları (26-bit / 34-bit) bağlamak için **Wiegand Input** mevcuttur (D0=GPIO 22, D1=GPIO 23).
- **RS485 & OSDP Protokolü:** 
  - OSDP v2.2 uyumlu Peripheral Device (PD / Slave) stack'i entegredir.
  - Pinler: `TX=GPIO 26`, `RX=GPIO 34`, `RTS/DE=GPIO 18` (Hardware Half-Duplex).
  - Varsayılan PD Adresi: `0x00`, Varsayılan Baudrate: `9600`.
  - Desteklenen komutlar: `osdp_POLL` (kart, QR, tuş bildirimleri), `osdp_ID`, `osdp_CAP`, `osdp_OUT` (kapı açma), `osdp_LED`, `osdp_BUZ`, `osdp_COMSET`.
- **Bağlantı ve İletişim:**
  - Wi-Fi (2.4 GHz 802.11 b/g/n) ve BLE (Bluetooth Low Energy) mobil provisioning.
  - Tüm varyasyonlarda Wi-Fi, BLE ve MQTT istemcisi tam zamanlı aktiftir.
- **Dijital Girişler:**
  - **Digital Input 1 (GPIO 19) → Exit Button (Kapı Açma Butonu):** Kapıyı içeriden açmak için çıkış butonu. Aktif HIGH, dahili pull-down dirençli.
  - **Digital Input 2 (GPIO 21) → IN2 / Donanımsal Fabrika Reset Girişi:** Dahili pull-up dirençli. Cihaz açılışı esnasında 10 saniye boyunca GND'ye bağlandığında tüm varyasyonlarda **Donanımsal Fabrika Reset (Hardware Factory Reset)** tetiklenir.
- **Çıkışlar:**
  - Dahili Röle (Shift register üzerinden sürülür) — Kapı kilidi kontrolü için (NO/NC/COM).

---

## 2. Çalışma Modları (Working Modes - Menü 6)

Cihaz 2 farklı rolde çalışabilir:
1. **Mod 1 - Standalone (Bağımsız Mod):** Cihaz harici Wiegand girişini dinlemez, geçiş kararlarını kendi yerel belleğindeki kullanıcılara göre verir.
2. **Mod 2 - Controller (Denetleyici Modu):** Cihaz hem kendi üzerindeki okuyucuları hem de harici Wiegand okuyucusunu dinler. Geçiş kararlarını kendisi verir ve rölesini tetikler.

---

## 3. Geçiş İzin Modları (Access Modes - Menü 5)

Kullanıcıların hangi yöntemlerle geçiş yapabileceği belirlenebilir:
- Sadece Kart, Sadece PIN, Sadece QR, Kart veya PIN, Kart veya QR, Herhangi Biri.

---

## 4. Programlama Modu ve Cihaz Ayarları (PIN Modelleri - V2.0.9)

Tuş takımı bulunan modellerde (`PIN+QR` ve `PIN+RFID`):
- **Menüye Giriş:** Bekleme modunda `*` tuşuna basılır, ardından `[Master Kod]` ve `#` tuşlanır (Varsayılan Master Kod: `123456`).
- **Menüden Çıkış:** `*` tuşuna basıldığında programlama modundan çıkılır.
- **Menü 1:** Master Kod Değiştirme (`1 -> [Yeni 4-6 Haneli Kod] -> # -> [Tekrar Yeni Kod] -> #`).
- **Menü 2:** Master Kart / Master QR Ekleme (`2 -> [Kart Okut / QR Göster]` veya `2 -> [Kart No] -> #`).
- **Menü 3:** Kullanıcı Ekleme - Offline (`3 -> [Kart Okut / QR Göster]` veya `3 -> [4-6 Hane PIN] -> # -> [PIN Tekrar] -> #`). *Online modda kilitlidir.*
- **Menü 4:** Kullanıcı Silme - Offline (`4 -> [Kart / QR / ID] -> #` veya Master kart/QR ile tümünü silmek için `4 -> [Master Okut] -> #`). *Online modda kilitlidir.*
- **Menü 5:** Seri Kart Ekleme - Offline (`5 -> [Başlangıç ID] -> # -> [Adet] -> # -> [İlk Kart] -> #`). *Sadece PIN+RFID modelinde ve offline modda çalışır.*
- **Menü 6:** Röle Çekili Kalma Süresi (`6 -> [0-99 saniye] -> #`, `0` = Toggle modu).
- **Menü 7:** Ses ve LED Gösterge Ayarları (`7 -> [0: Ses Kapat, 1: Ses Aç, 2: LED Kapat, 3: LED Aç] -> #`).
- **Menü 9:** Fabrika Ayarlarına Dönüş (`9 -> 9#`).

### Menü Dışı Kullanıcı PIN Değiştirme (Standby):
- Bekleme modunda `* -> [Kullanıcı ID] -> # -> [Mevcut PIN] -> # -> [Yeni PIN] -> # -> [Yeni PIN Tekrar] -> #` tuşlanarak kullanıcı kendi PIN kodunu değiştirebilir. Cihaz Online moddayken bu işlem 3 bip ile reddedilir.

---

## 5. Master Kart / QR Mantığı ve Kart ile Yönetim

Tuş takımı olmadan da sistem yönetilebilir:
- **Master Ekleme Kartı/QR (Master Add):**
  - Bekleme modunda Master Kart / QR bir kez okutulduğunda **Mavi LED Yavaş Yanıp Söner**.
  - Bu moddayken okutulan yeni kullanıcı kartları / QR'ları otomatik olarak sisteme eklenir.
  - Çıkmak için Master Kart / QR tekrar okutulur.
- **Master Silme Kartı/QR (Master Delete):**
  - Bekleme modunda Master Kart / QR iki kez art arda okutulduğunda **Kırmızı LED Yavaş Yanıp Söner**.
  - Bu moddayken okutulan kullanıcı kartları / QR'ları sistemden silinir.
  - Çıkmak için Master Kart / QR tekrar okutulur.

### OE0112 (RFID ONLY) İlk Açılış Master Kart Tanımlama
`RFID_ONLY` varyasyonunda tuş takımı bulunmadığından:
1. Cihaz sıfırlandığında (Fabrika reset sonrası) ilk açılışta **10 saniyelik Master Kart Tanımlama Penceresi** açılır.
2. Bu 10 saniye içerisinde okutulan kartlar sırasıyla **Master Kart** olarak kaydedilir (En fazla 10 adet).
3. 10 saniye bittiğinde veya 10 kart dolduğunda Master kayıt penceresi kilitlenir. Artık bu kartlar Master Kart olarak çalışır.

---

## 6. Offline QR Kod Desteği ve SHA-256 Optimizasyonu

- `PIN+QR` varyasyonunda QR kodlar hem Online (MQTT/Bulut) hem de Offline (Yerel NVS) olarak desteklenir.
- 31 karakterden uzun QR kodlar, donanım kripto motoru (mbedtls SHA-256) ile güvenli bir şekilde hash'lenir ve ilk 31 hex karakteri yerel NVS hafızasında saklanır. Böylece NVS bellek yapısı bozulmadan 100% güvenli offline QR geçişi sağlanır.
- Master QR, Kullanıcı QR, Master Add ve Master Delete modları eksiksiz çalışır.

---

## 7. Fabrika Ayarlarına Dönüş (Factory Reset)

Cihazı fabrika ayarlarına döndürmek için:
1. **Tuş Takımı Üzerinden (PIN Modelleri):** `* -> [Master Kod] -> # -> 4 -> [Master Kod] -> #` tuşlanarak tüm kullanıcılar ve konfigürasyon silinir.
2. **Onloi Mobil Uygulaması / Cloud Üzerinden:** Uygulama üzerinden "Cihazı Sıfırla / Fabrika Ayarlarına Dön" komutu gönderildiğinde cihaz sıfırlanır.
3. **Seri Konsol (CLI) Üzerinden:** `buraky36_fab_reset` komutu girilerek cihaz doğrudan fabrika ayarlarına alınabilir.

---

## 8. OSDP ve RS485 Entegrasyonu

- Cihaz, bina yönetim ve geçiş kontrol panellerine (ACU) doğrudan RS485 üzerinden bağlanabilir.
- Baudrate ve adres bilgileri `osdp_COMSET` komutu ile panel üzerinden değiştirilebilir ve NVS'de saklanır.
- Panelden `osdp_OUT` komutu geldiğinde terminal kapı rölesini tetikler, `osdp_LED` ve `osdp_BUZ` komutları ile sesli ve görsel bildirimler panel tarafından uzaktan kontrol edilebilir.
- Terminale okutulan tüm kartlar (`osdp_RAW` / `osdp_FMT`), QR kodlar (`osdp_FMT`) ve tuş vuruşları (`osdp_KEYPAD`) anlık olarak panele bildirilir.
