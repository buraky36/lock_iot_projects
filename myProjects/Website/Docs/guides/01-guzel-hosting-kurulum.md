# Güzel Hosting Kurulum Rehberi (1. Aşama)

Bu rehber, daha önce hiç hosting veya cPanel kullanmamış olsanız bile **burakyalim.com** ve **noddat.com** alan adlarınızı Güzel Hosting **İdeal** paketinizde güvenli, düzenli ve **birbirinden ayrı klasörlerde** yayına almanız için hazırlanmıştır.

---

## 1. Mevcut Paket ve Klasör Mimarisi

Güzel Hosting'deki **İdeal** paketinizin özellikleri bu yapı için fazlasıyla uygundur:
- **Web Sitesi Barındırma Limiti:** 11 adet alan adı (domain)
- **Disk Alanı:** 5 GB SSD
- **Aylık Trafik:** 100 GB
- **E-posta Hesabı:** Limitsiz

Mevcut yapılandırmanızda **burakyalim.com** ana alan adıdır. **noddat.com** alan adını da aynı pakete **ayrı bir klasörle** ekleyeceğiz.

### Neden Klasörleri Ayırıyoruz?
`burakyalim.com` uzun vadede paylaşımlı hosting üzerinde rahatça çalışabilecek bir kişisel blogdur. Ancak `noddat.com`, ileride MQTT broker, zaman serisi veritabanı, API sunucuları ve gerçek zamanlı paneller çalıştıracağınız büyük bir IoT platformuna dönüşecek; yani bir noktada kendi VPS veya bulut sunucusuna taşınması gerekecek.

İki siteyi en baştan ayrı klasörlere koyduğumuzda:
1. Dosyalar birbirine karışmaz.
2. Her sitenin `.htaccess`, robots, e-posta ve SSL yönetimi bağımsız kalır.
3. İleride sadece `noddat.com` DNS kaydını yeni sunucuya yönlendirerek `burakyalim.com`'u hiç bozmadan taşıyabilirsiniz.

### Hedef Klasör Diyagramı (cPanel Dosya Yöneticisi)

```text
/home/<cpanel_kullanici_adiniz>/
├── public_html/                  <── burakyalim.com (Ana Alan Adı)
│   ├── index.html
│   ├── robots.txt
│   └── .htaccess
│
└── noddat.com/                   <── noddat.com (Tercih edilen tam izole klasör)
    ├── index.html
    ├── robots.txt
    └── .htaccess
```

> **Not:** Bazı cPanel/WHM ayarları yeni alan adı klasörünü otomatik olarak `/public_html/noddat.com/` altına koymaya zorlayabilir. Eğer cPanel `/noddat.com` yazmanıza izin vermezse `/public_html/noddat.com/` kullanın; bu da çalışır, yalnızca ana sitenin `.htaccess` kuralları alt klasörü etkileyebileceği için dikkatli olmak gerekir. Aşağıdaki adımlarda ikisini de anlattık.

---

## Adım 1 — Nameserver (DNS) Kontrolü

İki alan adının da Güzel Hosting sunucunuza işaret ettiğinden emin olmalısınız. İki alan adını da Güzel Hosting'den aldığınız için genelde otomatik ayarlı gelir, yine de kontrol edelim:

1. **Güzel Hosting Müşteri Paneli**'ne giriş yapın (`guzel.net.tr`).
2. Üst menüden **Hizmetlerim** bölümüne girin ve **İdeal (burakyalim.com)** paketinizin yanındaki **Yönet** butonuna tıklayın.
3. Açılan sayfada veya hosting açılış e-postanızda size tanımlı **Nameserver (NS)** adreslerini not alın (örneğin `ns1.guzel.net.tr`, `ns2.guzel.net.tr` veya `rdns...` ile başlayan size özel NS adresleri).
4. Şimdi sol veya üst menüden **Alan Adlarım** bölümüne gidin:
   - **burakyalim.com** yanındaki yönetim butonuna tıklayıp **Nameserver (Alan Adı Sunucuları)** sekmesinin hostinginizdeki NS adresleriyle birebir aynı olduğunu doğrulayın.
   - **noddat.com** için de aynı kontrolü yapın; farklıysa hostinginizin NS adreslerini girip kaydedin.

> **Bekleme Süresi:** DNS değişikliği yaptıysanız internet genelinde aktif olması genellikle 15–60 dakika sürer (nadiren 24 saate kadar çıkabilir). Aynı firmada olduğu için çoğu zaman birkaç dakikada güncellenir.

---

## Adım 2 — cPanel'de `noddat.com` İçin Ayrı Klasör Oluşturma

Şimdi hosting kontrol paneli olan **cPanel**'e girip `noddat.com` alan adını ayrı bir klasöre tanımlayacağız:

1. Güzel Hosting Müşteri Paneli → **Hizmetlerim** → **İdeal (burakyalim.com)** → **Yönet** sayfasına gidin.
2. Sayfadaki **cPanel'e Giriş Yap** / **cPanel'e Bağlan** butonuna tıklayın.
3. cPanel ana ekranında **Etki Alanları (Domains)** başlığı altındaki **Etki Alanları (Domains)** simgesine tıklayın.
   *(Eski bir tema görüyorsanız "Eklenti Etki Alanları / Addon Domains" simgesini görebilirsiniz; mantık aynıdır.)*
4. Sağ üstteki **Yeni Alan Adı Oluştur (Create A New Domain)** butonuna tıklayın.
5. **Etki Alanı (Domain)** kutucuğuna tam olarak şunu yazın:
   `noddat.com`
6. **EN KRİTİK ADIM:** Kutucuğun hemen altında bulunan:
   `[ ] Belge kök dizinini (/home/.../public_html) paylaş` *(Share document root)*
   seçeneğinin **İŞARETİNİ KALDIRIN**!
   - Bu kutu işaretli kalırsa `noddat.com` ile `burakyalim.com` aynı klasörü açar; yani iki ayrı siteniz olmaz. **Mutlaka boş olmalıdır.**
7. İşareti kaldırdığınızda aşağıda **Belge Kök Dizini (Document Root)** kutusu açılacaktır:
   - **Tercih 1 (En temiz izolasyon):** Kutuda `public_html/noddat.com` yazıyorsa bunu doğrudan `noddat.com` şeklinde değiştirin (yani `/home/<kullanici>/noddat.com` olur).
   - **Tercih 2 (Eğer cPanel `public_html/` ön ekini değiştirmeye izin vermezse):** `public_html/noddat.com` olarak bırakın. Sorunsuz çalışır, sadece ileride `public_html/.htaccess` dosyasına büyük yönlendirme kuralları yazarsanız alt klasörü hariç tutmanız gerekir.
8. Alttaki **Gönder (Submit)** butonuna tıklayın.

Tebrikler! Artık iki alan adınız iki ayrı klasöre ayrılmış durumda.

---

## Adım 3 — Hazırlanan Dosyaları Yükleme (Dosya Yöneticisi)

Şimdi bu proje klasöründeki dosyaları cPanel **Dosya Yöneticisi (File Manager)** ile sunucuya yükleyeceğiz.

### Önce Gizli Dosyaları Görünür Yapın (`.htaccess` için)
1. cPanel ana ekranında **Dosyalar (Files)** başlığı altından **Dosya Yöneticisi (File Manager)** aracını açın.
2. Sağ üst köşedeki **Ayarlar (Settings)** butonuna tıklayın.
3. **"Gizli Dosyaları Göster (dotfiles)"** (*Show Hidden Files*) kutucuğunu işaretleyip **Save** deyin.

### 3A. `burakyalim.com` Dosyalarını Yükleme (`public_html/`)
1. Sol menüden **`public_html`** klasörüne tıklayıp içine girin.
2. İçeride varsayılan bir `default.html`, `index.html` veya `index.php` varsa silin veya adını `yedek-index.html` olarak değiştirin.
   > **DİKKAT:** Klasör içinde görebileceğiniz `.well-known` ve `cgi-bin` klasörlerine **DOKUNMAYIN / SİLMEYİN**! `.well-known` klasörü ücretsiz SSL sertifikası doğrulaması için gereklidir.
3. Üst menüden **Yükle (Upload)** butonuna tıklayın.
4. Bilgisayarınızdaki `sites/burakyalim.com/` klasörünün içindeki şu dosyaları seçip yükleyin:
   - `index.html`
   - `robots.txt`
   - `.htaccess` *(Eğer klasörde zaten sunucu tarafından oluşturulmuş bir `.htaccess` varsa üzerine yazmadan önce kontrol edebilir veya şimdilik sadece `index.html` ve `robots.txt` yükleyip ilerleyebilirsiniz; ancak bizim hazırladığımız `.htaccess` güvenlidir ve PHP yönlendirmesi vb. içermediği için tertemiz çalışır.)*
5. Yükleme %100 yeşil olunca **Go Back to ...** bağlantısına tıklayın ve dosyaların `public_html` içinde göründüğünü doğrulayın.

### 3B. `noddat.com` Dosyalarını Yükleme
1. Dosya Yöneticisi'nde Adım 2'de `noddat.com` için oluşturduğunuz klasöre girin:
   - Tercih 1'i yaptıysanız sol menüde ana dizin altında **`noddat.com`** klasörünü göreceksiniz.
   - Tercih 2 olduysa `public_html` içinde **`noddat.com`** klasörünü göreceksiniz.
2. Bu klasörün içine girin. (Varsa `.well-known` veya `cgi-bin` klasörlerine yine dokunmayın.)
3. Üst menüden **Yükle (Upload)** butonuna tıklayın ve bilgisayarınızdaki `sites/noddat.com/` klasöründeki dosyaları yükleyin:
   - `index.html`
   - `robots.txt`
   - `.htaccess`

---

## Adım 4 — Ücretsiz SSL (HTTPS) ve Otomatik Yönlendirme

Tarayıcıların "Güvenli Değil" uyarısı vermemesi için iki alan adına da ücretsiz SSL sertifikası (AutoSSL) aktif edilmelidir:

1. cPanel ana ekranında **Güvenlik (Security)** başlığı altındaki **SSL/TLS Durumu (SSL/TLS Status)** simgesine tıklayın.
2. Listede `burakyalim.com`, `www.burakyalim.com`, `noddat.com` ve `www.noddat.com` satırlarını göreceksiniz.
3. Sağ üstten veya üst kısımdan **AutoSSL'i Çalıştır (Run AutoSSL)** butonuna basın ve işlemin bitmesini bekleyin (1–3 dakika sürebilir).
4. Sayfa yenilendiğinde alan adlarınızın yanında **yeşil kilit (AutoSSL Domain Validated)** gördüğünüzde SSL hazır demektir.
   *(Not: Eğer `noddat.com` DNS'i henüz tam yayılmadıysa AutoSSL ilk seferde onu atlayabilir; 15–30 dakika sonra tekrar Run AutoSSL'e tıklayabilirsiniz.)*
5. **HTTPS'ye Zorlama (Force HTTPS):**
   - SSL'ler yeşil olduktan sonra cPanel ana sayfasından tekrar **Etki Alanları (Domains)** bölümüne gidin.
   - Her iki alan adının yanındaki **Force HTTPS Redirect** anahtarını **Açık (On)** konumuna getirin.
   - *(Önemli ipucu: Hazırladığımız `.htaccess` dosyalarında HTTPS yönlendirmesini bilerek yorum satırı `#` olarak kapalı bıraktık. Böylece SSL sertifikası oluşmadan önce siteniz döngüye girip hata vermez. cPanel'deki Force HTTPS Redirect anahtarı bu işi tek tıkla ve en temiz şekilde yapar.)*

---

## Adım 5 — Doğrulama, E-posta İpucu ve İleride Taşıma (Migration)

### Yayın Kontrol Listesi
- [ ] Tarayıcıda `https://burakyalim.com` açılıyor ve yeşil kilit simgesi görünüyor.
- [ ] Tarayıcıda `https://noddat.com` açılıyor ve yeşil kilit simgesi görünüyor.
- [ ] İki sitede de `TR` / `EN` dil değişimi ve mini oyunlar sorunsuz çalışıyor.
- [ ] Telefonunuzdan (mobil tarayıcıdan) her iki adrese girip taşma olmadan göründüğünü test ettiniz.

### Küçük İpucu: Kendi Alan Adınızla Ücretsiz E-posta
Güzel Hosting İdeal paketinizde **limitsiz e-posta** hakkınız var. cPanel → **E-posta Hesapları (Email Accounts)** kısmından:
- `merhaba@burakyalim.com`
- `hello@noddat.com`
gibi adresleri hemen oluşturup cPanel içindeki **Webmail** üzerinden veya Gmail / telefonunuzdaki posta uygulamasına bağlayarak kullanabilirsiniz.

### İleride `noddat.com` Büyüdüğünde Nasıl Taşınır?
`noddat.com`'u ayrı klasörde tuttuğumuz için gelecekte bir VPS / Bulut sunucuya (Hetzner, DigitalOcean, AWS vb.) geçerken yapacağınız tek şey şudur:
1. Yeni IoT sunucunuzu kurup ayağa kaldırırsınız.
2. Güzel Hosting alan adı panelinden **sadece `noddat.com` için DNS (A kaydı veya Nameserver)** bilgisini yeni sunucuya yönlendirirsiniz.
3. `burakyalim.com`, e-postalarınız ve mevcut blogunuz Güzel Hosting üzerinde **kesintisiz çalışmaya devam eder.**
