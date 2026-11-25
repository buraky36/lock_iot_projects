# Akıllı Geçiş Kontrol Terminali - Kullanım Kılavuzu Referans Belgesi (V2.1.1)

Bu doküman; ONLOI Akıllı Geçiş Kontrol Terminali serisinin **v2.1.1 Release** yazılım sürümü doğrultusunda donanım varyasyonları, programlama adımları (**Menü 0–9**), Master Kart/QR yönetimi, **ardışık 5 hatalı erişim koruması (30 sn geçici kilitlenme)**, Online mod **Wi-Fi kopukluğu görsel uyarısı**, BLE kurulum korumaları (10 deneme limiti ve 2.4 GHz/Hotspot kuralları), OSDP/RS-485 protokolü, IN2 donanımsal fabrika sıfırlama algoritması, okuyucu tekrar okuma (cooldown) filtreleri ve çift yönlü OTA güncelleme görsel göstergeleri hakkında resmi referans bilgilerini içermektedir.

---

## 1. Donanım Varyasyonları ve Temel Özellikler

Cihaz mimarisi 3 farklı donanım varyasyonu olarak sunulmaktadır:
1. **OE0113 (PIN + QR):** Kapasitif dokunmatik tuş takımı ve XR1300 optik QR/Barkod okuyucuya sahiptir (`Product ID: 0x0113`).
2. **OE0111 (PIN + RFID):** Kapasitif dokunmatik tuş takımı ve MFRC522 13.56 MHz Mifare RFID okuyucuya sahiptir (`Product ID: 0x0111`).
3. **OE0112 (RFID ONLY):** Sadece MFRC522 13.56 MHz Mifare RFID okuyucuya sahiptir; fiziksel tuş takımı bulunmaz (`Product ID: 0x0112`).

### Ortak Donanım Özellikleri:
- **İşlemci:** ESP32 tabanlı çift çekirdekli mikrodenetleyici (240 MHz, 4 MB Flash).
- **Kullanıcı Arayüzü:** Çok renkli WS2812B RGB LED durum göstergesi, PWM kontrollü sesli uyarıcı (Buzzer) ve tuşlu modellerde (`OE0111`, `OE0113`) TSM12 kapasitif dokunmatik tuş takımı (Shift-register arka aydınlatma animasyonlu).
- **Tam Entegre Çalışma Yapısı:** Ayrı bir *Standalone / Controller* mod seçimi bulunmaz; tüm modeller kendi dahili okuyucularını, harici **Wiegand Girişini** (26-bit / 34-bit otomatik algılama ve Wiegand QR, `D0=GPIO 22`, `D1=GPIO 23`) ve **RS-485 / OSDP v2.2** hattını eşzamanlı olarak çalıştırır.
- **RS-485 & OSDP Protokolü:**
  - OSDP v2.2 uyumlu Peripheral Device (PD / Slave) yığını entegredir.
  - Pinler: `TX=GPIO 26`, `RX=GPIO 34`, `RTS/DE=GPIO 18` (Hardware Half-Duplex).
  - Varsayılan PD Adresi: `0x00`, Varsayılan Baudrate: `9600` bps (`9600–115200` bps destekli).
  - Desteklenen komutlar: `osdp_POLL` (kart, QR, tuş bildirimleri), `osdp_ID`, `osdp_CAP`, `osdp_OUT` (kapı açma/kapatma), `osdp_LED`, `osdp_BUZ`, `osdp_COMSET`.
- **Bağlantı ve İletişim:**
  - Wi-Fi **2.4 GHz (`802.11 b/g/n`)** ve BLE (Bluetooth Low Energy) mobil kurulum (`OE011X_<MACSon4Hane>`).
  - Cihaz Seri Numarası Formatı: `<ModelKodu>_<MACSon6Hane>` (Örn: `OE0111_3A7F12`, `OE0112_9C4B10`, `OE0113_28DA90`).
  - Wi-Fi ağ bağlantısı kurulduğu anda BLE tamamen kapatılır ve sistem belleğinden (~75 KB RAM) serbest bırakılır.
  - Çift yönlü (Upgrade & Downgrade) kesintiye dayanıklı OTA uzaktan yazılım güncellemesi desteklenir.
- **Okuyucu Tekrar Okuma (Anti-Repeat Cooldown) Filtreleri:**
  - **MFRC522 (RFID):** Farklı kartlar arasında **2.0 saniye**, aynı kart UID'si için **3.0 saniye** bekleme filtresi.
  - **XR1300 (QR):** Farklı QR kodlar arasında **2.5 saniye**, aynı QR dizesi için **3.0 saniye** bekleme filtresi.
  - **İlk Açılış Koruma Penceresi:** Kapı açıldıktan sonraki ilk **2.5 saniye** boyunca okuyucu önündeki kart/QR tekrarları yoksayılır.
- **Dijital Girişler:**
  - **IN1 (GPIO 19) → Exit Button (Kapı Açma / Çıkış Butonu):** Kapıyı içeriden açmak için çıkış butonu girişi. Aktif HIGH (+12V), dahili pull-down dirençli. (Toggle modda kapı açıkken basıldığında kapıyı kilitler; 30 saniyelik Brute-Force kilitlenmesi sırasında dahi aktif kalır ve kapıyı açtığında kilidi kaldırır).
  - **IN2 (GPIO 21) → Donanımsal Fabrika Reset Girişi:** Yalnızca cihazı fabrika ayarlarına döndürmek için kullanılır (Kapı sensörü olarak KULLANILMAZ; normal çalışmada boş bırakılmalıdır). Cihaz açılışındaki **ilk 12 saniye** penceresi içinde **5 saniye** kesintisiz `+12V DC` uygulanıp (`600 ms` bip + Hızlı Sarı LED uyarısı ardından) **6 saniye içinde** `+12V` bağlantısı kesilerek **3 saniye** boşta (LOW) tutulduğunda (`3 onay bipi` sonrası) **Donanımsal Fabrika Reset** tetiklenir.
- **Çıkışlar:**
  - Dahili Kilit Rölesi (Kuru kontak: `NO / NC / COM`, varsayılan darbe süresi `5 saniye`, `0` = Toggle Aç/Kapat modu).

---

## 2. Güvenlik Korumaları (v2.1.1)

### 2.1 Ardışık Hatalı Erişim (Brute-Force) Koruması — 5 Hatalı Deneme / 30 Saniye Kilitlenme
- **Tetiklenme Koşulu:** Cihaz Bekleme (`STATE_IDLE`) modundayken (hem **Offline** yerel doğrulamada hem de **Online** sunucu reddi yanıtında) art arda **5 kez geçersiz giriş denemesi** (yanlış PIN, tanımsız RFID kart, harici Wiegand veya yetkisiz QR kod) yapıldığında koruma devreye girer.
- **5. Hatalı Denemede Cihaz Tepkisi:**
  - **Ses:** `1000 Hz` frekansta **1 saniye uzun uyarı sesi**.
  - **RGB LED:** **30 saniye boyunca Hızlı Yanıp Sönen Kırmızı** (`LED_COLOR_RED`).
- **30 Saniye Kilitlenme Süresince Davranış:**
  - 30 saniye boyunca tüm yerel okuyucu girişleri (**Dokunmatik Tuş Takımı**, **Dahili MFRC522 RFID**, **Harici Wiegand D0/D1** ve **Optik XR1300 QR**) geçici olarak kilitlenir ve gelen denemeler işlenmez.
  - Kilitli süre içerisinde herhangi bir tuşa basılır veya kart/QR okutulursa cihaz **2 kısa uyarı bipi (`1000 Hz, 150 ms × 2`)** vererek kilitli durumda olduğunu bildirir.
- **Acil Çıkış ve Kilidin Açılması (İstisnalar):**
  - İçeriden **Çıkış Butonu (`IN1`)** ve Onloi Mobil Uygulamasından **Uzaktan Kapı Açma (`0x40`)** komutları 30 saniyelik kilitlenmeden etkilenmez; her zaman aktif kalır.
  - `IN1` veya `0x40` ile kapı açıldığında ya da cihaz Programlama / Master Ekleme / Fabrika Reset moduna geçirildiğinde 30 saniyelik kilitlenme ve hata sayacı derhal sıfırlanır.
  - 5 hatalı denemeye ulaşmadan önce gerçekleşen her geçerli geçişte hata sayacı otomatik olarak `0`'a sıfırlanır.

---

## 3. Sesli ve Görsel Uyarı Özeti (v2.1.1)

- **Bekleme Modu (Idle):** RGB LED hem Offline hem de Online modda **Yavaş Yanıp Sönen Kırmızı** yanar.
- **Ekran Koruyucu Animasyonları (20 sn hareketsizlikte):**
  - `OE0113` (PIN+QR): Yılan (Snake) LED animasyonu.
  - `OE0111` (PIN+RFID): Dalga (Wave) LED animasyonu.
  - *İlk dokunuş yalnızca tuş takımını 20 saniyeliğine uyandırır; şifre hanesi sayılmaz.*
- **Online Modda Wi-Fi Kopukluğu Uyarısı (`ANIM_WIFI_DISCONNECTED`):**
  - Cihaz Online moddayken Wi-Fi bağlantısı koptuğunda, tuş takımlı modellerde (`OE0111`, `OE0113`) bekleme ekranında tuş LED'leri sürekli **Aşağı Kayan Ok (`↓`: `2 ➔ 1-5-3 ➔ 4-8-6 ➔ 7-0-9 ➔ *-#`)** animasyonu oynatır.
  - Kullanıcı tuşa dokunduğunda tuş takımı 20 saniyeliğine normal aydınlanarak işlem yapılmasına olanak tanır; Wi-Fi yeniden bağlandığında ok animasyonu otomatik olarak kapanır.
  - `OE0112` (tuş takımsız) modelde ise Wi-Fi kopukken arka planda otomatik yeniden bağlanma sürerken yapılan kart okumaları Strict Online Mode gereği reddedilir (`3 yavaş bip + 3 sn Sabit Kırmızı LED`).
- **Kapı Açıldı (`STATE_DOOR_OPEN`):** RGB LED **Yavaş Yanıp Sönen Yeşil** yanar ve tuş takımında çerçeve onay animasyonu (`ANIM_SUCCESS_TICK`) oynatılır.
- **OTA Güncelleme İlerleme Göstergesi (`ANIM_OTA_PROGRESS`):**
  - Tuş takımı `1..9, 0` rakamları sırasıyla `%10..%100` dilimlerini temsil eder; tamamlanan `%10` dilimleri sabit yanar, indirilmekte olan aktif dilim tuşu her `180 ms`'de bir yanıp söner.
  - RGB Durum LED'i her `%10` dilimde renk değiştirir: `0–9% Kırmızı` ➔ `10–19% Turuncu` ➔ `20–29% Sarı` ➔ `30–39% Lime` ➔ `40–49% Cyan` ➔ `50–59% Mavi` ➔ `60–69% Mor` ➔ `70–79% Magenta` ➔ `80–89% Beyaz` ➔ `90–100% Yeşil`.

---

## 4. Programlama Modu ve Cihaz Ayarları (OE0111 & OE0113 — Menü 0–9)

Tuş takımı bulunan modellerde (`OE0113 PIN+QR` ve `OE0111 PIN+RFID`):
- **Menüye Giriş:** Bekleme modunda `*` tuşuna basılır, ardından `[Master Kod] -> #` tuşlanır (Varsayılan Master Kod: `123456`) veya `* -> [Master Kart / Master QR Okut]` yapılır.
- **Menüden Çıkış:** `*` tuşuna basıldığında (veya 25 saniye hareketsizlik sonunda) programlama modundan çıkılır.
- **Menü 0 — Wiegand / RFID Bit Formatı Seçimi (`26-bit` / `34-bit`):**
  - `0 -> 26 -> #`: **26-bit** Wiegand-26 (24-bit maskeli kart ID) modu *(Fabrika ve sıfırlama varsayılanı: `26`)*.
  - `0 -> 34 -> #`: **34-bit** tam 32-bit kart UID modu.
  - **Otomatik Algılama (Auto-Detect):** Harici Wiegand (`D0/D1`) okuyucudan 26-bit veya 34-bit bir kart okutulduğunda cihaz okuyucunun bit uzunluğunu otomatik algılar ve kalıcı olarak NVS'e kaydeder.
- **Menü 1 — Master Kod Değiştirme:** `1 -> [Yeni 4-6 Haneli Kod] -> # -> [Tekrar Yeni Kod] -> #`
- **Menü 2 — Master Kart / Master QR Ekleme:**
  - Otomatik ID ile: `2 -> [Kart Okut / QR Göster]` veya `2 -> [8/10/17 Hane Kart No] -> #`
  - Spesifik Master ID (`10000–10010`) ile: `2 -> [Master ID] -> # -> [Kart Okut / QR Göster / Kart No + #]`
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

## 5. Master Kart / QR Mantığı ve OE0112 İlk Kurulum

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

## 6. Offline QR Kod Desteği ve SHA-256 Optimizasyonu

- `OE0113 (PIN+QR)` varyasyonunda QR kodlar hem Online (MQTT/Bulut) hem de Offline (Yerel NVS) olarak desteklenir.
- Virgülle ayrılmış dinamik QR kodlarda (`<ID>,<Tarih>,QR`) kimlik öneki ayrıştırılır; 31 karakterden uzun QR kodlar ise donanım kripto motoru (`mbedtls SHA-256`) ile hash'lenerek ilk 31 hex karakteri yerel NVS hafızasında saklanır.

---

## 7. Online Mod, BLE Kurulum Korumaları ve Uzaktan Sıfırlama

- **BLE ile Wi-Fi Kurulumu ve 10 Deneme Koruma Limiti:**
  - Kurulum sırasında verilen Wi-Fi ağına **10 kez** üst üste bağlanılamazsa (`MAX_PROV_WIFI_RETRIES = 10`) cihaz sonsuz döngüde takılı kalmaz; geçersiz Wi-Fi bilgisini temizleyerek otomatik olarak temiz BLE kurulum bekleme durumuna döner.
  - Kullanıcı uygulamadan yeni bir Wi-Fi taraması (`0x03`) başlattığında devam eden bağlantı denemesi anında iptal edilir.
- **Wi-Fi / Telefon Hotspot Kurulum Kuralları:**
  - Cihaz yalnızca **2.4 GHz Wi-Fi (`802.11 b/g/n`)** destekler. Cep telefonu Hotspot kullanımında **2.4 GHz / "Uyumluluğu En Üst Düzeye Çıkar"** modu açık olmalı ve ağ adında (SSID) tipografik kesme işareti (`’`) gibi özel karakterler kullanılmamalıdır.
- **Bulut / Mobil Uygulama Sıfırlama Komutları:**
  - **`0x58` (Wi-Fi Reset):** Yalnızca Wi-Fi ağ bilgilerini siler; yerel veriler ve bulut eşleşme bilgisi korunarak cihaz yeni Wi-Fi kurulumu için BLE moduna alınır.
  - **`0x59` (Server Disconnect / Uygulamadan Silme):** Cihaz mobil uygulamadan kaldırıldığında Wi-Fi ve bulut eşleşme bilgilerini siler; yerel kullanıcı kayıtlarını koruyarak Offline/BLE kurulum moduna döner.
  - **`0x50` (Factory Reset):** Tüm kullanıcıları, Master kayıtlarını, Wi-Fi ve bulut bilgilerini uzaktan sıfırlar.
  - **`0x61` (Local Factory Reset Event):** Cihaz üzerinden (Menü 9 veya IN2 ile) fabrika sıfırlaması yapıldığında buluta `0x61` yaşam döngüsü bildirimi gönderilerek uygulama eşleşmesi otomatik düşürülür.

---

## 8. Fabrika Ayarlarına Dönüş (Factory Reset)

Cihazı fabrika ayarlarına döndürmek için:
1. **IN2 Donanımsal Sıfırlama (Tüm Modeller):** Cihaz açılışındaki **ilk 12 saniye** içinde `IN2` girişine **5 saniye** kesintisiz `+12V` uygulanır. 5. saniyede `600 ms` bip ve **Hızlı Sarı LED** başladığında **6 saniye içinde** `+12V` kesilir (`3 adet onay bipi` duyulur) ve kablo **3 saniye** boşta tutulduğunda fabrika sıfırlaması başlatılır.
2. **Tuş Takımı Üzerinden (PIN Modelleri):** `* -> [Master Kod] -> # -> 9 -> 9 -> #` tuşlanarak tüm kullanıcılar ve konfigürasyon silinir.
3. **Onloi Mobil Uygulaması / Cloud Üzerinden:** Uygulama üzerinden sıfırlama komutu (`0x50`) gönderildiğinde cihaz sıfırlanır.
4. **Seri Konsol (CLI — Test Derlemeleri V2.1.0):** `onloi_fab_reset` komutu girilerek cihaz doğrudan fabrika ayarlarına alınabilir *(Saha Release `V2.1.1` sürümünde güvenlik nedeniyle UART CLI/logları kapalıdır)*.
