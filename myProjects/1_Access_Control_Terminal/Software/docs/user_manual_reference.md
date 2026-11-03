# Akıllı Geçiş Kontrol Terminali - Kullanım Kılavuzu Referans Belgesi (V2.0.13)

Bu doküman, cihazın özellikleri, donanım varyasyonları, programlama adımları (Menü 1-9), Master Kart/QR yönetimi, OSDP/RS485 protokolü, IN2 donanımsal sıfırlama algoritması, okuyucu tekrar okuma (cooldown) filtreleri ve ağ (Online/Offline & Çift Yönlü OTA) kuralları hakkında resmi User Manual (Kullanıcı Kılavuzu) referans bilgilerini içermektedir.

---

## 1. Donanım Varyasyonları ve Temel Özellikler

Cihaz mimarisi 3 farklı donanım varyasyonu olarak sunulmaktadır:
1. **OE0113 (PIN + QR):** Kapasitif dokunmatik tuş takımı ve XR1300 optik QR/Barkod okuyucuya sahiptir (`Product ID: 0x0113`).
2. **OE0111 (PIN + RFID):** Kapasitif dokunmatik tuş takımı ve MFRC522 13.56MHz Mifare RFID okuyucuya sahiptir (`Product ID: 0x0111`).
3. **OE0112 (RFID ONLY):** Sadece MFRC522 13.56MHz Mifare RFID okuyucuya sahiptir; tuş takımı bulunmaz (`Product ID: 0x0112`).

### Ortak Donanım Özellikleri:
- **İşlemci:** ESP32 tabanlı çift çekirdekli mikrodenetleyici (240MHz, 4MB Flash).
- **Kullanıcı Arayüzü:** Çok renkli WS2812B RGB LED durum göstergesi, PWM kontrollü sesli uyarıcı (Buzzer) ve tuşlu modellerde TSM12 kapasitif dokunmatik tuş takımı (Shift-register arka aydınlatma animasyonlu).
- **Tam Entegre Çalışma Yapısı:** Ayrı bir *Standalone / Controller* mod seçimi bulunmaz; tüm modeller kendi dahili okuyucularını, harici **Wiegand Girişini** (26-bit / 34-bit ve Wiegand QR, `D0=GPIO 22`, `D1=GPIO 23`) ve **RS485 / OSDP** hattını eşzamanlı olarak çalıştırır.
- **RS485 & OSDP Protokolü:** 
  - OSDP v2.2 uyumlu Peripheral Device (PD / Slave) stack'i entegredir.
  - Pinler: `TX=GPIO 26`, `RX=GPIO 34`, `RTS/DE=GPIO 18` (Hardware Half-Duplex).
  - Varsayılan PD Adresi: `0x00`, Varsayılan Baudrate: `9600`.
  - Desteklenen komutlar: `osdp_POLL` (kart, QR, tuş bildirimleri), `osdp_ID`, `osdp_CAP`, `osdp_OUT` (kapı açma), `osdp_LED`, `osdp_BUZ`, `osdp_COMSET`.
- **Bağlantı ve İletişim:**
  - Wi-Fi (2.4 GHz 802.11 b/g/n) ve BLE (Bluetooth Low Energy) mobil provisioning (`OE011X_XXXX`).
  - Cihaz Seri Numarası Formatı: `<ModelKodu>_<MACSon6Hane>` (Örn: `OE0111_3A7F12`, `OE0112_9C4B10`, `OE0113_28DA90`).
  - Tüm varyasyonlarda Wi-Fi, BLE ve MQTT istemcisi tam zamanlı aktiftir; çift yönlü (Upgrade & Downgrade) OTA uzaktan yazılım güncellemesi desteklenir.
- **Okuyucu Tekrar Okuma (Anti-Repeat Cooldown) Filtreleri:**
  - **MFRC522 (RFID):** Farklı kartlar arasında **2.0 saniye**, aynı kart UID'si için **3.0 saniye** bekleme filtresi.
  - **XR1300 (QR):** Farklı QR kodlar arasında **2.5 saniye**, aynı QR dizesi için **3.0 saniye** bekleme filtresi.
- **Dijital Girişler:**
  - **IN1 (GPIO 19) → Exit Button (Kapı Açma Butonu):** Kapıyı içeriden açmak için çıkış butonu girişi. Aktif HIGH, dahili pull-down dirençli. (Toggle modda kapı açıkken 2.5 sn koruma penceresi sonrasında basıldığında kapıyı kilitler).
  - **IN2 (GPIO 21) → Donanımsal Fabrika Reset Girişi:** Yalnızca cihazı fabrika ayarlarına döndürmek için kullanılır (Kapı sensörü olarak kullanılmaz). Cihaz açılışındaki ilk 25 saniye içinde 10 saniye boyunca kesintisiz +12V DC uygulanıp, ardından gelen 3 saniyelik sesli/sarı LED uyarı penceresinde +12V bağlantısı kesildiğinde **Donanımsal Fabrika Reset** tetiklenir.
- **Çıkışlar:**
  - Dahili Kilit Rölesi (Kuru kontak: `NO / NC / COM`).

---

## 2. Programlama Modu ve Cihaz Ayarları (PIN Modelleri - V2.0.13)

Tuş takımı bulunan modellerde (`OE0113 PIN+QR` ve `OE0111 PIN+RFID`):
- **Menüye Giriş:** Bekleme modunda `*` tuşuna basılır, ardından `[Master Kod] -> #` tuşlanır (Varsayılan Master Kod: `123456`) veya `* -> [Master Kart / Master QR Okut]` yapılır.
- **Menüden Çıkış:** `*` tuşuna basıldığında (veya 25 saniye hareketsizlik sonunda) programlama modundan çıkılır.
- **Menü 1 — Master Kod Değiştirme:** `1 -> [Yeni 4-6 Haneli Kod] -> # -> [Tekrar Yeni Kod] -> #`
- **Menü 2 — Master Kart / Master QR Ekleme:**
  - Otomatik ID ile: `2 -> [Kart Okut / QR Göster]` veya `2 -> [8/10/17 Hane Kart No] -> #`
  - Spesifik Master ID (`10000-10010`) ile: `2 -> [Master ID] -> # -> [Kart Okut / QR Göster / Kart No + #]`
- **Menü 3 — Kullanıcı Ekleme (Otomatik ID - Offline):**
  - `3 -> [Kart Okut / QR Göster]` veya `3 -> [8/10/17 Hane Kart No] -> #` veya `3 -> [4-6 Hane PIN] -> # -> [PIN Tekrar] -> #`. *(Online modda kilitlidir).*
- **Menü 4 — Kullanıcı Silme (Offline):**
  - Tekil silme: `4 -> [Kart / QR Okut]` veya `4 -> [Kullanıcı ID (1-9988)] -> #` veya `4 -> [8/10/17 Hane Kart No] -> #`
  - Tüm kullanıcıları silme: `4 -> [Master Kod] -> #` veya `4 -> [Master Kart/QR Okut] -> #`. *(Online modda kilitlidir).*
- **Menü 5 — Spesifik ID ile Kayıt & Seri (Blok) Kart Ekleme (Offline):**
  - Spesifik ID'ye Kart/PIN: `5 -> [Kullanıcı ID (1-9988)] -> # -> [Kart Okut / Kart No + # / PIN + #]`
  - Seri (Blok) Kart Ekleme: `5 -> [Başlangıç ID] -> # -> [Adet] -> # -> [İlk Kart No] -> #`
  - *(Sadece OE0111 PIN+RFID modelinde ve Offline modda çalışır).*
- **Menü 6 — Röle Çekili Kalma Süresi ve Toggle Modu:**
  - Darbe (Pulse) Modu: `6 -> [1-99 saniye] -> #` (Varsayılan: `5` sn). Kapı açıldıktan 2.5 sn sonra geçerli bir Kart/QR/PIN+# girilirse açık kalma süresi yeniden başlatılarak uzatılır.
  - Tutmalı (Toggle / Aç-Kapat) Modu: `6 -> 0 -> #`. İlk geçerli geçişte röle süresiz çekili kalır; kapı açıldıktan **2.5 saniye koruma penceresi** geçtikten sonra geçerli bir **Kart**, **QR Kod**, **PIN + #**, **IN1 Çıkış Butonu** veya **Bulut Uzaktan Kapatma (`0x4C`)** komutu ile röle bırakılarak kapı kilitlenir.
- **Menü 7 — Ses ve LED Gösterge Ayarları:** `7 -> [0: Ses Kapat, 1: Ses Aç, 2: LED Kapat, 3: LED Aç] -> #`.
- **Menü 8 — Master Kart / Master QR Silme:**
  - Tekil silme: `8 -> [Master Kart / Master QR Okut]` veya `8 -> [8/10/17 Hane Kart No] -> #` veya `8 -> [Master ID (10000-10010)] -> #`
  - Tüm Master Kart / QR'ları silme: `8 -> [Master Kod] -> #`
- **Menü 9 — Fabrika Ayarlarına Dönüş:** `9 -> 9 -> #`.

### Menü Dışı Kullanıcı PIN Değiştirme (Standby):
- Bekleme modunda `* -> [Kullanıcı ID] -> # -> [Mevcut PIN] -> # -> [Yeni PIN] -> # -> [Yeni PIN Tekrar] -> #` tuşlanarak kullanıcı kendi PIN kodunu değiştirebilir. Cihaz Online moddayken bu işlem 3 yavaş bip ile reddedilir.

---

## 3. Master Kart / QR Mantığı ve Kart ile Yönetim

Tuş takımı olmadan da sistem yönetilebilir:
- **Master Ekleme Modu (Master Add):**
  - Bekleme modunda Master Kart / QR 1 kez okutulduğunda **Mavi LED Yavaş Yanıp Söner** (2 kısa bip).
  - Bu moddayken okutulan yeni kullanıcı kartları / QR'ları otomatik olarak sisteme eklenir.
  - Çıkmak için Master Kart / QR tekrar okutulur (veya 10 sn beklenir).
- **Master Silme Modu (Master Delete):**
  - Bekleme modunda Master Kart / QR 5 saniye içinde 2 kez art arda okutulduğunda **Kırmızı LED Yavaş Yanıp Söner** (3 kısa bip).
  - Bu moddayken okutulan kullanıcı kartları / QR'ları sistemden silinir.
  - Eğer Master Silme moduna girdikten sonraki 5 saniye içinde Master Kart / QR **3. kez** okutulursa **tüm kullanıcılar silinir**.
  - Çıkmak için Master Kart / QR tekrar okutulur.

### OE0112 (RFID ONLY) İlk Açılış Master Kart Tanımlama
`RFID_ONLY` varyasyonunda tuş takımı bulunmadığından:
1. Cihaz sıfırlandığında (Fabrika reset sonrası) ilk açılışta **10 saniyelik Master Kart Tanımlama Penceresi** açılır.
2. Bu 10 saniye içerisinde okutulan kartlar sırasıyla **Master Kart** olarak kaydedilir (En fazla 10 adet).
3. 10 saniye bittiğinde (en az 1 kart kaydedilmişse) veya 10 kart dolduğunda Master kayıt penceresi kilitlenir.

---

## 4. Offline QR Kod Desteği ve SHA-256 Optimizasyonu

- `OE0113 (PIN+QR)` varyasyonunda QR kodlar hem Online (MQTT/Bulut) hem de Offline (Yerel NVS) olarak desteklenir.
- 31 karakterden uzun QR kodlar, donanım kripto motoru (`mbedtls SHA-256`) ile güvenli bir şekilde hash'lenir ve ilk 31 hex karakteri yerel NVS hafızasında saklanır.

---

## 5. Fabrika Ayarlarına Dönüş (Factory Reset)

Cihazı fabrika ayarlarına döndürmek için:
1. **IN2 Donanımsal Sıfırlama (Tüm Modeller):** Cihaz açılışındaki ilk 25 saniye içinde `IN2` girişine 10 saniye kesintisiz +12V uygulanır. Uzun bip ve hızlı sarı LED başladığında **3 saniye içinde** +12V kesilirse cihaz sıfırlanır.
2. **Tuş Takımı Üzerinden (PIN Modelleri):** `* -> [Master Kod] -> # -> 9 -> 9 -> #` tuşlanarak tüm kullanıcılar ve konfigürasyon silinir.
3. **Onloi Mobil Uygulaması / Cloud Üzerinden:** Uygulama üzerinden sıfırlama komutu gönderildiğinde cihaz sıfırlanır.
4. **Seri Konsol (CLI) Üzerinden:** `onloi_fab_reset` komutu girilerek cihaz doğrudan fabrika ayarlarına alınabilir.
