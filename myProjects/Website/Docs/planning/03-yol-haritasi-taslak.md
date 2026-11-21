# 3. Aşama Yol Haritası Taslağı — Asıl Siteler ve IoT Platformu

1. aşamada hosting/klasör ayrımını tamamlayıp 2. aşamada geçici oyunlu sayfalarımızı yayına aldıktan sonra, sırada iki projenin gerçek mimarisini planlamak var. Bu belge 3. aşama konuşmamız için bir başlangıç haritasıdır.

---

## 1. `burakyalim.com` — Kişisel Blog ve Mühendislik Notları

Kişisel blog tarafı Güzel Hosting İdeal paketinizde (5 GB SSD, 100 GB trafik) çok uzun yıllar ek masraf çıkarmadan rahatça yaşayabilir. İki ana yaklaşım arasından seçim yapacağız:

| Özellik | Statik Site Üreticisi (Astro / Hugo) | WordPress |
| :--- | :--- | :--- |
| **Hız ve Performans** | Çok yüksek (saf HTML/CSS, veritabanı yükü yok) | Orta (PHP + MySQL, önbellek eklentisi gerekir) |
| **Güvenlik ve Bakım** | Çok yüksek (saldırı yüzeyi yok, eklenti güncelleme derdi yok) | Düzenli çekirdek ve eklenti güncellemesi gerekir |
| **Yazı Yazma Akışı** | Markdown (`.md`) dosyaları + Git ile sürüm takibi | Tarayıcı üzerinden görsel yönetim paneli |
| **İdeal Hosting Uyumu** | Mükemmel — çok az CPU/RAM ve disk kullanır | İyi — küçük/orta trafikte sorunsuz çalışır |
| **Kime Uygun?** | Yazılım/donanım notları, kod blokları, teknik yazılar | Panelden hızlıca görsel ağırlıklı içerik girmek isteyenler |

> **Ön Öneri:** Mühendislik ve IoT projelerinizi paylaşacağınız, kod blokları ve teknik şemalar içerecek bir blog için **Astro** (modern, hızlı, Markdown tabanlı) çok temiz ve geleceğe dönük bir tercihtir.

---

## 2. `noddat.com` — Büyük IoT Platformu Yol Haritası

Güzel Hosting paylaşımlı bir web hosting olduğu için sürekli açık kalan MQTT broker'lar, WebSocket bağlantıları veya zaman serisi veritabanları çalıştırmak için tasarlanmamıştır. Bu nedenle **Noddat**'ı aşamalı büyütmek en akıllıca ve ekonomik yoldur:

### Faz 0: Tanıtım, Vizyon ve Bekleme Listesi (Şu Anki Güzel Hosting)
- Geçici oyunlu sayfa sonrası; platformun ne çözeceğini anlatan profesyonel bir tanıtım (landing) sayfası, dokümantasyon/mimari özeti ve erken erişim (waitlist) formu.
- Güzel Hosting üzerinde sıfır ek maliyetle barınır.

### Faz 1: MVP — Gerçek Çalışan Çekirdek (Ayrı VPS + Docker)
- **Altyapı:** Aylık düşük maliyetli tek bir Linux VPS üzerinde Docker Compose.
- **Cihaz Haberleşmesi:** EMQX veya Mosquitto (**MQTT Broker** — TLS şifreli) + HTTP/REST Ingest.
- **Zaman Serisi & Veri:** PostgreSQL + **TimescaleDB** (sensör ve cihaz telemetri verileri için) + **Redis** (anlık durum ve önbellek).
- **Backend & API:** Go, Node.js/TypeScript veya Python (FastAPI) tabanlı cihaz yönetimi, kural motoru ve kimlik doğrulama.
- **Arayüz (Dashboard):** Next.js / React veya Astro+React ile canlı grafikler, cihaz durumu, alarm kuralları.
- **Alan Adı Geçişi:** `noddat.com` DNS kaydı bu VPS'e (veya `app.noddat.com` / `mqtt.noddat.com` alt alan adlarına) yönlendirilir.

### Faz 2: Ölçeklenebilir Bulut Mimarisi (Büyüme Aşaması)
- Yatay ölçeklenebilen MQTT kümesi, yönetilen veritabanları, mesaj kuyrukları (Kafka / NATS), çok kiracılı (multi-tenant) organizasyon yapısı, faturalandırma ve OTA (havadan yazılım güncelleme) altyapısı.

---

## 3. Aşamaya Geçerken Cevaplayacağımız Sorular

3. aşamaya geçtiğimizde mimariyi netleştirmek için şu sorular üzerinden ilerleyeceğiz:

1. **Hedef Cihazlar ve Protokoller:** İlk etapta hangi donanımlar bağlanacak (ESP32, STM32, Raspberry Pi, endüstriyel PLC/Modbus, LoRaWAN)? Ağırlıklı olarak MQTT mi, HTTP mü yoksa CoAP/LoRa mı kullanılacak?
2. **Kullanım Modeli:** Platform ilk aşamada sizin kendi projeleriniz/müşterileriniz için mi, yoksa dışarıdan herkesin üye olup cihaz ekleyebileceği bir SaaS ürünü olarak mı kurgulanacak?
3. **Ölçek Hedefi:** İlk 6–12 ayda yaklaşık kaç cihazın saniyede/dakikada ne sıklıkla veri göndermesini öngörüyorsunuz?
4. **Temel Özellik Öncelikleri:** Canlı gösterge panelleri (dashboard), eşik değer alarmları (e-posta/Telegram/webhook), uzaktan komut gönderme (RPC) ve **OTA firmware güncelleme** özelliklerinden hangileri ilk sürümde (MVP) olmazsa olmaz?
5. **Blog (`burakyalim.com`) Tercihi:** Markdown + Git tabanlı süper hızlı bir statik blog mu (Astro), yoksa yönetim panelli klasik bir sistem mi (WordPress) tercih edersiniz?
