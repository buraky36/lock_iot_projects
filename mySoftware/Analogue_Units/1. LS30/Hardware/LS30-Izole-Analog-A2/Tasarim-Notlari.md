# LS30 A2 pin seviyesinde şema

19 Kasım 2026 · KiCad 10 · 220–230 VAC · Ekransız · STM32L073VZT6

[Düzenlenebilir KiCad projesi](LS30-A2.kicad_pro) · [Yedi sayfalık şema](LS30-A2-Sema.pdf) · [A2 maliyet çalışması](../LS30-A2-Maliyet/Tasarim-Notlari.md)

## Güç bağlantıları

| Konnektör | Pin 1 | Pin 2 | Pin 3 |
|---|---|---|---|
| J101 · Ürünün şebeke girişi | L | N | PE |
| J102 · Kullanıcının flyback AC girişi | Sigortalı L | N | PE |
| J103 · Flyback DC dönüş girişi | +24 VDC | 24 V dönüşü | |

Flyback harici bir güç kartı olarak ele alındı. Şemada şebeke ve sekonder bağlantıları, kart besleme koruması ve yerel raylar var. Trafo, primer anahtarlama ve geri besleme devresi kullanıcının güç kartı üzerindedir. 45 W, flyback’in 24 V’ta **1,875 A** kaynak kapasitesidir. LS30 kartına ayrılan DC sigorta başlangıçta 1 A olarak çizildi. Şebeke sigortası, MOV, PE uygulaması ve mevcut güç kartında bulunan korumalar, flyback’in ilk akımı ve kutu/kablo düzeniyle birlikte doğrulanır. PE hattı çekirdek veya izole giriş dönüşüne bağlanmaz.

`24V_CORE`, DAC analog pozitif rayını ve LMR36510’u besler. LMR36510’un `5V_CORE` çıkışı, röleleri, yerel regülatörleri ve AI izolasyon modülünü besler. Geri besleme dirençleri R101/R102 **%0,1** seçildi. Gerilim ayarı nominal **5,016 V**. AIPU girişindeki 5 V, oturma sonrası **4,75–5,25 V** aralığında kalmalı. İlk çalıştırma, üç rölenin birlikte anahtarlanması ve 24 V giriş toleransları bu ölçüme dahil. Çekirdek 3,3 V’u TLV75533, AO koruması için +15 V’u L78L15 ve yaklaşık −5 V’u LM2776 sağlar. `24V_CORE` için ilk arayüz hedefi **21–28 V** ve açılışta aynı üst sınırdır. SMBJ28A darbe bastırıcıdır. DAC beslemesinin toplam ray farkı flyback’in tepe gerilimiyle birlikte doğrulanır. [LMR36510](https://www.ti.com/lit/ds/symlink/lmr36510.pdf), [LM2776](https://www.ti.com/lit/ds/symlink/lm2776.pdf), [DAC7760 çalışma koşulları](https://www.ti.com/lit/ds/symlink/dac7760.pdf)

## AI izolasyonu ve ölçüm

- **AIPU Power FW1-05S05B3** (`C427960`), regüle **5 V → 5 V / 1 W** ve **3 kVDC** modül izolasyon test değeri. U206 pin 1 `5V_CORE`, pin 2 `GND_CORE`, pin 4 `GND_AI`, pin 6 `AI5_RAW`. Yerel dört pedli SIP ayak izi üretici pin sırasına göre çizildi. Modül gövdesi **19,5 × 6 × 10 mm**. Eski KW1’in 22 × 9,5 mm gövde tabanına göre alanı yaklaşık **%44** daha küçük. Gövdenin pad sırasına göre konumu ve karttaki izolasyon mesafeleri numuneyle doğrulanır. [AIPU ürün listesi](https://aipupower.cn/sip-dip-fixed-regulated-075-2w/), [FW1-B3 veri sayfası](https://xonstorage.z8.web.core.windows.net/pdf/aipu_fw105s05b3_Lcs01_link.pdf), [önceki KW1 ölçüleri](https://www.aipupower.com/product/dc-dc-converter/sip-wide-input-regulated-1-3w/kw1-24s05e3r3.html)
- **R251 220 Ω**, modül ayaktayken yaklaşık 23 mA ön yük sağlar. C252 ve C254, üreticinin 0,5 W altındaki hafif yük için ek kapasite önerisi nedeniyle ilk dizgide takılmaz. Mevcut yerel bypass kondansatörleri ve FB251 filtresi kullanılır. Opsiyonel kondansatörler takılırsa 22 µF modül çıkış sınırı, kalkış süresi ve AI gürültüsü tekrar doğrulanır. Bu parça için üretici veri sayfası çıkış kısa devre koruması belirtmiyor. İzole 5 V ray kart içindedir. [FW1-B3 çıkış ve uygulama koşulları](https://xonstorage.z8.web.core.windows.net/pdf/aipu_fw105s05b3_Lcs01_link.pdf)
- LCSC’nin yayımlanmış **1.000 adet fiyat kademesi 1,1715 USD/adet**. Seri stok ve teslim süresi teyit bekliyor. Kapsam, DC/DC seçimi ve ilgili filtre parçalarıdır. [LCSC fiyat kademesi](https://www.lcsc.com/product-detail/Power-Modules_AIPULNION-Guangzhou-Aipu-Elec-Tech-FW1-05S05B3_C427960.html)
- **J201** pin 1 `AI+`, pin 2 `AI−`, pin 3 ayrılmış. 4–20 mA, TPS26611 ve 82,5 Ω hassas şönt üzerinden ADS1118’in AIN0 girişine gider. Gerilim kolu 44,2 kΩ + 44,2 kΩ / 20,0 kΩ bölücü ve TLV333 üzerinden AIN1’e gider. Nominal giriş empedansı 108,4 kΩ. 10 V, ADC’de yaklaşık 1,845 V verir. TLV333 için LM7705’in −0,232 V rayı, sıfıra yakın ölçümlerde tamponun çıkış sınırını iyileştirir. Gerilim modu pasif 0–10 V, akım modu haricen beslenen akım döngüsü içindir. Eski üç telli LS30/MLG30 sensörünün uyarma beslemesi ve pin uyumu ayrıca belirlenir. [ADS1118](https://www.ti.com/lit/ds/symlink/ads1118.pdf), [TPS26611 §9.3](https://www.ti.com/lit/ds/symlink/tps2661.pdf), [LM7705](https://www.ti.com/lit/ds/symlink/lm7705.pdf)
- **3PEAK TPT7741-SOBR** SPI’yi, **TPT7721F-SOAR** `AI_I_EN` ve `AI_OK` hatlarını taşır. F tipi düşük varsayılan çıkış sayesinde AI akım kolu açılışta devre dışı kalır. `AI_OK` düşük olduğunda akım kolunda arıza veya izole tarafın besleme kaybı araştırılır. TPS26611 `SGOOD` sinyali, düşük eşikle algılama ve seviyeyi yenileme için MMBT3904 üzerinden terslenir. [3PEAK TPT7741](https://www.3peak.com/digital-isolators/tpt7741), [3PEAK TPT772x](https://static.3peak.com/res/doc/ds/Datasheet_TPT772x.pdf)

ADC ±2,048 V kademesiyle ölçer. Yazılımda gerilim nominal `V_AIN1 × 5,42`, akım nominal `V_AIN0 / 82,5 Ω + V_AIN1 / 20 kΩ` olarak elde edilir. Kart başına kalibrasyon katsayısı yazılmaz. AI performansı 0/5/10 V ve 4/12/20 mA’da, 0/25/50 °C’de, flyback ve üç röle çalışırken ölçülür. **±%0,50 ölçüm aralığı** prototip kabul hedefidir.

## AO ve koruma

- **J301** pin 1 `AO+`, pin 2 `GND_CORE`. DAC7760, dahili REFOUT–REFIN bağlantısı ve dahili RSET ile kullanılır. `IOUT` ve `VOUT` ortak `DAC_DRV` düğümüne gider. Aynı anda tek mod etkinleştirilir. (−VSENSE) kart yerleşiminde J301 dönüşünden ayrı Kelvin iziyle alınır. DAC PowerPAD’i `NEG5V_AO` üzerinde, dijital ve analog GND pinleri 0 V’tadır. [DAC7760 §6, §9.1.1.3](https://www.ti.com/lit/ds/symlink/dac7760.pdf)
- **U302**, saha ana çıkış yoludur. **U303**, ayrı Kelvin yoludur. Her iki TPS26611’in `OUT` pini saha klemensine bakar. Kelvin yolu 1 kΩ, BAV199 klemp ve OPA197 tampon ile `+VSENSE` pinine gider. U303 kesime girdiğinde 100 kΩ ile `DAC_DRV`’ye yerel geri besleme sağlanır. `AO_EN` iki koruma kanalını açar ve donanım pull-down’ı ile düşük başlar. `AO_LINE_OK` ve `AO_SENSE_OK` ayrı izlenir. [TI TPS26611 §8.3.7 ve Şekil 9-14](https://www.ti.com/lit/ds/symlink/tps2661.pdf), [TI ortak V/I klemensi referansı](https://www.ti.com/tool/TIPD119)
- Gerilim modunda açık devre ve ≥1 kΩ yük, akım modunda 0/250/500 Ω yük ve kablo kapasitesiyle kararlılık ölçülür. Geçici yük, kısa devre, açık sense, kapalı/açık cihazda ±24 V uygulanması, geri besleme toparlanması ve DAC pad sıcaklığı prototip kapsamındadır. TI’nin örnek devresindeki darbe diyodu ve TVS yerleşimi ürünün darbe seviyesine göre ayrıca değerlendirilir. Geçme eşikleri ve EMC dayanımı ölçümle belirlenir.

## Röleler ve saha arayüzü

Üç **HONGFA HF3FA/005-ZSTF** bobini, 3,3 V kapı sürüşünde AO3400A ve SS14 ile anahtarlanır. Her J501–J503 klemensi pin 1 COM, pin 2 NO, pin 3 NC sırasındadır. Rölelerin saha kontakları birbirine ve bobin beslemesine bağlanmaz. Röle padleri G5LE tipi ortak mekanik desenle eşleştirildi. İlk numunede bobin, COM, NO, NC süreklilik eşlemesi ve mevcut kutu uyumu kontrol edilir. Ürün hedefi eski LS30’daki **250 VAC / 3 A** seviyesidir. Nihai yük tipi ve kart mesafeleri bu hedefi belirler. [Hongfa HF3FA teknik çizimi](https://www.hongfaamerica.com/hq/PDF/HF3FA_en.pdf), [LCSC C194963](https://www.lcsc.com/product-detail/C194963.html)

RS485, ana çekirdek referanslı THVD1410 ile sürülür. 120 Ω uçlandırma takılabilir olarak çizildi. DI1/DI2 için 3,3 V çekmeli gerilimsiz kontak kabul edildi. Eski ürünün kılavuzunda DI seviyeleri belirtilmediği için eski kablo düzeniyle uyumluluk doğrulanır.

## MCU ve çalıştırma sırası

| İşlev | STM32 ayakları |
|---|---|
| AI SPI1 | PA4 CS, PA5 SCK, PA6 MISO/DRDY, PA7 MOSI |
| AI kontrol | PC0 akım yolu enable, PC1 AI_OK |
| AO SPI2 | PB12 LATCH, PB13 SCK, PB14 SDO, PB15 DIN |
| AO kontrol | PC2 enable, PC3 saha durumu, PC4 sense durumu, PC5 DAC alarm |
| Röleler | PD0 / PD1 / PD2 |
| RS485 ve DI | PA8 DE, PA9 TX, PA10 RX, PC6 DI1, PC7 DI2 |
| SWD | PA13 SWDIO, PA14 SWCLK, NRST |

Açılışta röleler ve `AO_EN` pasif tutulur. ADC’nin mod değişiminden sonra yerleşme süresi beklenir. DAC başlatılır, çift çıkış biti kapatılır, çıkış aralığı ve ilk kod yüklenir. Saha yolu ölçümle doğrulanmış aralık değiştirme sırasıyla yeniden etkinleştirilir. Arıza tespitinde çıkış kapatılır ve Modbus’ta saha/sense/DAC durumları ayrı bildirilir. [STM32L073 veri sayfası](https://www.st.com/resource/en/datasheet/stm32l073vz.pdf), [DAC7760 veri sayfası](https://www.ti.com/lit/ds/symlink/dac7760.pdf)

## Doğrulama durumu

KiCad 10.0.8 elektriksel kural kontrolünde 0 hata ve 0 uyarı. Bu güncellemede dışa aktarılan netlistte U206’nın 5 V ve izolasyon bağlantıları, ayak izi padleri ve bileşen farkları kontrol edildi. Önceki netlistte DAC çıkış ve sense yolları, iki 3PEAK pin eşlemesi, flyback klemensleri ve üç bağımsız röle kontağı da kontrol edilmişti. PCB yerleşimi, mekanik numune ve elektriksel prototip ölçümleri açıktır. AIPU, ek AO koruyucusu ve yerel besleme parçaları önceki maliyet tablosunda yoktu.
