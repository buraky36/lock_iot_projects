# burakyalim.com & noddat.com — Web Sitesi Kurulum ve Yol Haritası

Bu proje klasörü, Güzel Hosting üzerindeki **İdeal** paketinizde barındıracağınız iki ayrı alan adını (`burakyalim.com` ve `noddat.com`) düzenli bir şekilde yönetmeniz, birbirinden izole etmeniz ve ilk günden profesyonel geçici sayfalarla yayına almanız için hazırlanmıştır.

## 3 Aşamalı Yol Haritası

1. **1. Aşama — Hosting ve Klasör İzolasyonu (Şimdi):**
   - Güzel Hosting **İdeal** paketiniz en fazla **11 web sitesi** destekler; yani tek hosting içinde iki alan adını rahatlıkla barındırabilirsiniz.
   - `burakyalim.com` **Ana Alan Adı** olarak `public_html/` klasöründe tutulur.
   - `noddat.com` ise **Eklenti Alan Adı (Addon Domain)** olarak ayrı bir klasöre (`/noddat.com` veya `/public_html/noddat.com`) bağlanır. Böylece ileride IoT platformunuz büyüyüp ayrı bir VPS / bulut sunucuya geçmek istediğinde, `burakyalim.com` hiç etkilenmeden sadece `noddat.com` taşınabilir.
   - Adım adım kurulum için [guides/01-guzel-hosting-kurulum.md](guides/01-guzel-hosting-kurulum.md) rehberini izleyin.

2. **2. Aşama — Geçici "Hazırlanıyor" Sayfaları ve Mini Oyunlar (Hazır):**
   - Her iki alan adı için iki dilli (**TR / EN**), tek dosyalık (`index.html`), harici bağımlılık gerektirmeyen modern geçici sayfalar hazırlandı.
   - **`burakyalim.com`**: Açık/koyu tema destekli, sıcak editoryal blog temasında **Blog Runner** mini koşu oyunu.
   - **`noddat.com`**: Koyu mühendislik/telemetri temasında **Packet Runner** (IoT sinyal & veri paketi atlatma) mini koşu oyunu.

3. **3. Aşama — Asıl Siteler ve Noddat IoT Platform Planlaması (Sonraki Adım):**
   - Kişisel blog altyapısı (`burakyalim.com`) ve büyük IoT platformunun (`noddat.com`) teknik mimarisi için başlangıç taslağı [planning/03-yol-haritasi-taslak.md](planning/03-yol-haritasi-taslak.md) dosyasındadır.

---

## Klasör Yapısı ve cPanel Eşleşmesi

```text
Docs/
├── README.md                              # Bu genel özet dosyası
├── guides/
│   └── 01-guzel-hosting-kurulum.md        # Adım adım cPanel, DNS, klasör ayırma ve SSL rehberi
├── planning/
│   └── 03-yol-haritasi-taslak.md          # 3. Aşama: Blog & Noddat IoT platform planlama başlangıcı
└── sites/
    ├── burakyalim.com/                    ──▶ cPanel: /home/<kullanici>/public_html/
    │   ├── index.html
    │   ├── robots.txt
    │   └── .htaccess
    └── noddat.com/                        ──▶ cPanel: /home/<kullanici>/noddat.com/
        ├── index.html                         (veya cPanel izin vermezse /public_html/noddat.com/)
        ├── robots.txt
        └── .htaccess
```

---

## Bilgisayarınızda Önizleme ve Bilgileri Düzenleme

### 1. Sayfaları Bilgisayarda Test Etme
Her iki geçici sayfa da tamamen kendi içinde çalışan tek HTML dosyalarıdır:
- `sites/burakyalim.com/index.html` dosyasına çift tıklayarak tarayıcınızda açabilirsiniz.
- `sites/noddat.com/index.html` dosyasına çift tıklayarak tarayıcınızda açabilirsiniz.
- Sağ üstteki **TR / EN** (ve blog sayfasındaki **tema**) butonları ile dil ve görünüm değişimlerini test edebilir, **Boşluk (Space)** veya ekrana dokunarak mini oyunları oynayabilirsiniz.

### 2. İletişim ve Sosyal Medya Linklerini Düzenleme (`SITE_CONFIG`)
Her iki `index.html` dosyasının `<script>` bölümünün en üstünde kolayca düzenleyebileceğiniz `SITE_CONFIG` alanı bulunur:

```javascript
const SITE_CONFIG = {
  email: "merhaba@burakyalim.com",
  github: "https://github.com/kullaniciadiniz",
  linkedin: "https://linkedin.com/in/kullaniciadiniz",
  x: "", // Boş ("") bırakılan bağlantılar sayfada otomatik gizlenir!
  noddat: "https://noddat.com"
};
```

- Yer tutucu adresleri kendi hesaplarınızla değiştirebilirsiniz.
- Göstermek istemediğiniz herhangi bir satırı `""` (boş metin) yapmanız yeterlidir; sayfa o bağlantıyı otomatik olarak gizler.
