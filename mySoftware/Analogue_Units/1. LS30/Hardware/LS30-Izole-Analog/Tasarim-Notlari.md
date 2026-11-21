# LS30 ekransız izole analog I/O

**A0 · 18 Kasım 2026 · Prototip şeması**

LS30’un ekransız varyantı için bir adet galvanik izole **4–20 mA / 0–10 V giriş** ve bir adet bağımsız galvanik izole **4–20 mA / 0–10 V çıkış**. STM32L073VZT6 üst kartta çalışır. Bağlantı, devreye alma ve tanılama üst kartın RS485/Modbus RTU arayüzünden yapılır. [KiCad projesi](LS30-Izole-Analog.kicad_pro), [şema ana sayfası](LS30-Izole-Analog.kicad_sch)

## Arayüz ve izolasyon

| Alan | Şemadaki donanım | Saha bağlantısı |
| --- | --- | --- |
| **Çekirdek** | J101 üzerinden korumalı/regüle 21,6–26,4 VDC, 3,3 V ve iki bağımsız SPI bağlantısı. Sinyaller ve açılış durumları şemanın ilk sayfasında. | RS485, röleler ve ana besleme üst kartta. AC model şebekeyi kendi izole AC/DC katında dönüştürür. |
| **Giriş** | THI 3-2412 ile ayrı galvanik besleme, yerel 5 V/3,3 V, ISO7762DW ile dijital bariyer. | J201/2 `AI_FIELD`, J201/3 `GND_AI`. J201/1 platform uyarlaması için ayrılmış. 4–20 mA girişi pasif alıcı. İki telli vericinin akım döngüsü harici beslenir. |
| **Çıkış** | THI 3-2423 ile ayrı ±15 V, yerel regülatörler ve ikinci ISO7762DW. | J301/1 `AO_FIELD`, J301/2 `GND_AO`. 4–20 mA çıkışı aktif akım kaynağı. Gerilim çıkışında aynı çift kullanılır. |

`GND_CORE`, `GND_AI` ve `GND_AO` arasında şemada doğrudan bağlantı bulunmuyor. Her saha alanının sinyali ve gücü bağımsız bariyerden geçiyor. J201/J301 sembolleri işlevleri gösterir. Arka kutudaki fiziksel sıra, klemens adımı, etiket ve yükseklik sınırı eski numune üzerinden dondurulur. Eski LS30/MLG30’un üç telli besleme ve ölçüm arayüzü bu varyant için ölçülüp eşleştirilecek ayrı bir uyumluluk çalışmasıdır. Servis sırasında izole prob veya izole beslemeli ölçüm cihazı kullanılır. [ISO7762DW](https://www.ti.com/lit/ds/symlink/iso7762.pdf), [THI 3](https://www.tracopower.com/thi3-datasheet), [eski LS30 kılavuzu](../../Docs/LS30-Kontrolor-Montaj-ve-Kullanma-Klavuzu_v1_04_310123.pdf)

THI 3 ailesinde üretici 4 kV RMS I/O izolasyon testi, ISO7762DW paketinde TI 5 kV RMS dayanım testi belirtir. Bunlar parça değerleridir. Ürün seviyesi çalışma gerilimi, temel/güçlendirilmiş izolasyon sınıfı, hava aralığı, yüzeysel kaçak yolu ve test gerilimi, kurulum kategorisi, kirlilik derecesi, rakım, malzeme ve kutu üzerinden belirlenir. AI–çekirdek, AO–çekirdek ve AI–AO ayrı kontrol edilir. ±15 V modülün asimetrik yükte iki rayı da ölçülür. Ancak bu ölçümden sonra ana kart güç ve izolasyon yerleşimi tamamlanır. [Traco veri sayfası](https://www.tracopower.com/thi3-datasheet), [TI izolatör veri sayfası](https://www.ti.com/lit/ds/symlink/iso7762.pdf), [IEC 61010-2-201](https://webstore.iec.ch/en/publication/64909)

## Analog giriş

U201 **TPS26611** akım modunda 100 Ω şönt kolunu bağlar. Gerilim modunda U201 açıkken direnç bölücüsü ölçüm yapar. R201 şöntü ±%0,025, ≤5 ppm/°C ve ≥0,25 W seçilir. Kelvin algılama, şöntün iki fiziksel pad’inden ayrı izlerle alınır. Tek bağlantı noktası R214 çevresinde oluşturulur. `MODE=0` ile TPS26611 için üreticinin 40 mA azami akım sınırı altında şöntte yaklaşık 160 mW oluşur. 20 mA nominalde şönt ve tipik 7,5 Ω koruma direnci birlikte yaklaşık **2,15 V** döngü gerilimi tüketir. TVS3301 ve harici seri dirençler saha paraziti ve hat arızaları için kullanılır. [TI TPS26611 ortak giriş devresi §9.3](https://www.ti.com/lit/ds/symlink/tps2661.pdf), [TVS3301](https://www.ti.com/product/TVS3301)

U202 **ADS124S06IPBS** 24 bit ADC’dir. Ortak girişin şönt yolu AIN0, gerilim bölücüsü AIN1, TPS26611 `SGOOD` izleme yolu AIN2’ye bağlıdır. **ADR4525BRZ** 2,5 V referansıyla kazanç 1 ve PGA bypass kullanılır. Gerilim kolunda üstte 2 × 40,2 kΩ, altta 20,0 kΩ bulunur. Nominal empedans 100,4 kΩ, ölçek 5,02:1 ve 10 V’ta ADC değeri yaklaşık 1,992 V. Dirençler ±%0,02 ve ≤5 ppm/°C ilk seçimidir. 0 V, kaçak, ADC enjeksiyonu ve enerjisiz karta sinyal uygulanması test listesinde. `SGOOD` pini normal durumda düşük, bölücü üzerinden yaklaşık 0 V okunur. Mod değişimi sonrasında yazılım oturma süresini bekler. [ADS124S06 veri sayfası](https://www.ti.com/lit/ds/symlink/ads124s06.pdf), [ADR4525BRZ](https://www.analog.com/en/products/adr4525.html), [BAV199](https://assets.nexperia.com/documents/data-sheet/BAV199.pdf)

```text
V_mid = AIN1'e ait dönüştürülmüş voltaj
V_shunt = AIN0'a ait dönüştürülmüş voltaj
V_terminal ≈ V_mid × 5,02
I_terminal ≈ V_shunt / 100 Ω + V_mid / 20 kΩ
```

Akım modunda ortak gerilim kolunun yaklaşık 21 µA tam ölçek yükü ikinci ADC okumasından hesaplanır. 20 mA ve 100 Ω şönt ile yalnız şöntü okumak yaklaşık 21 µA düşük sonuç verir. Bu düzeltme nominal devre topolojisinden gelir ve bütün kartlarda aynı katsayılarla çalışır. Bölücünün 0 V ve aşırı gerilim koşullarında kaçak doğrulaması üretim doğruluk bütçesine girer.

## Analog çıkış

U302 **AD5423BCPZ** ve 2,5 V harici referans, 16 bit akım ve gerilim çıkışını aynı uca verir. Akım modu harici 13,7 kΩ, ±%0,02, ≤5 ppm/°C RSET ile başlar. Ölçekler 4–20 mA ve 0–10 V. Yazılım uygun eşleme ile 20–4 mA ve 10–0 V yönlerini de sağlar. Akımın 20 mA’de 500 Ω yükte gerilim ihtiyacı 10 V’tur. AD5423 için en az 2,3 V pozitif paya anahtar, klemens ve kablo düşümleri eklenir. Gerilim modunda ≥1 kΩ, 0 V uç noktası ve kablo kapasitansı ölçülür. ±15 V raylar 0 V çevresinde gerilim payı sağlar. 220 pF C0G kompanzasyonuyla gerçek kablo ve ADG5401F birlikte yerleşme/aşım testine girer. [AD5423 elektriksel özellikleri](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf)

U301 **ADG5401F** yanlış kablolamaya karşı VIOUT ve pozitif geri besleme yollarını korur. S ile SFB klemens sinyal pad’inde birleşir. D, DAC akım/gerilim ucuna gider. DFB, 1 kΩ üzerinden +VSENSE ucuna döner. ADG5401F `FF_N` ve AD5423 `FAULT_N` birlikte izolatöre gider. Arızada yazılım önce DAC tanı kayıtlarından nedeni okur. DAC tarafında neden görünmüyorsa çıkış koruması da arıza adayı olarak raporlanır. Donanımda `AO_KILL=1` veya varsayılan izolatör seviyesi DAC’ı resette tutar ve çıkış anahtarını açar. [ADG5401F §Amplifier or DAC Output Protection](https://www.analog.com/media/en/technical-documentation/data-sheets/adg5401f.pdf)

## Ön doğruluk bütçesi

Önerilen geçiş hedefi girişte **±%0,25 FSR**, çıkışta **±%0,20 FSR**. Akımda FSR 16 mA, gerilimde 10 V. Sınırlar AI için ±40 µA / ±25 mV, AO için ±32 µA / ±20 mV. İlk bütçede kart üzerindeki referans sıcaklığının 25 °C’den sapması yaklaşık 25 °C kabul edildi. 0–50 °C ortamda kutu içi sıcaklık artışı ve üretim saçılımı testte ayrıca ölçülür. Kart başına gain/offset yazımı ve saha trim’i gerektirmeyen üretim hedeflenir.

| Yol | Ön bütçe | Ana katkılar |
| --- | --- | --- |
| **AI 4–20 mA** | ≈±%0,21 FSR | Şönt %0,038, referans %0,073, ADC kazancı %0,015. Çarpan 20/16. Offset/INL için %0,013 ve kaçak, gürültü, direnç bölücüsü telafisi ve yaşlanma için %0,04 tasarım payı. |
| **AI 0–10 V** | ≈±%0,19 FSR | Bölücü oranı için yaklaşık %0,05, referans %0,073, ADC kazancı %0,015, offset/INL %0,01, kart ve koruma %0,04. Gerilim kaynağı empedansı ve sıcak kaçakla doğrulanır. |
| **AO 4–20 mA** | ≈±%0,19 FSR | AD5423 TUE %0,05 (ideal harici RSET ve veri sayfası koşulları), RSET %0,033, referans %0,073, kart/koruma/yaşlanma %0,03. |
| **AO 0–10 V** | ≈±%0,15 FSR | AD5423 TUE %0,05, referans %0,073, kart/koruma/yaşlanma %0,03. Yük, 0 V, geri besleme ve yerleşme gözlemleriyle onaylanır. |

ADR4525B hesabında başlangıç, lehim kayması ve sıcaklık payı ayrı toplandı. Özellikle akım çıkışında marj dar olduğu için TUE laboratuvar koşulları, gerçek 500 Ω yük ve asimetrik besleme ile karşılaştırılır. Üretim testinde izlenebilir ölçü aleti kullanılır ve ölçüm belirsizliğine göre geçiş marjı tanımlanır. [ADC veri sayfası](https://www.ti.com/lit/ds/symlink/ads124s06.pdf), [DAC ve RSET hesabı](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf), [referans veri sayfası](https://www.analog.com/media/en/technical-documentation/data-sheets/adr4520_4525_4530_4533_4540_4550.pdf)

## Maliyet seçenekleri

| Blok | A0 seçimi | Değerlendirilecek maliyet seçeneği |
| --- | --- | --- |
| ADC | ADS124S06 | **ADS114S06**, aynı TQFP-32 pin düzeninde 16 bit aday. Veri uzunluğu sürücüde seçilir. 2,5 V referansla ±2,5 V kod aralığında 16 bit adım yaklaşık 76 µV; şönt yolunda yaklaşık 0,76 µA. Gürültü ve hatanın sıcaklık altında uygunluğu ölçülür. [ADS114S06](https://www.ti.com/lit/ds/symlink/ads114s06.pdf) |
| DAC | AD5423 + harici RSET | **DAC8760** ile üretim maliyeti kıyaslanabilir. Yeni besleme, ortak çıkış ve sense devresi gerekir. Üretici 4–20 mA modunda dahili RSET ve harici 5 V referansla ±%0,25 FSR azami TUE verir. ±%0,20 kart hedefi için A0 seçimi AD5423. **AD5423 dahili RSET** ayrı deneme adayı. Katalog TUE ±%0,12 ve harici referans katkısı eklendiğinde kart payı daralır. [DAC8760](https://www.ti.com/lit/ds/symlink/dac8760.pdf), [TI ortak terminal uygulaması](https://www.ti.com/tool/TIPD119), [AD5423](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf) |
| İzole güç | İki THI 3 modülü ve iki ISO7762DW | AO için **ADP1031 + uygun transformatör**, güç kaynakları ve SPI izolatörünü tek çözümde toplar. Üretici 2,5 kV RMS bileşen testini ve temel izolasyonu listeler. Hedef çalışma gerilimi ve ürün izolasyon sınıfı bu seçime izin verirse yerleşim, EMI ve maliyet açısından karşılaştırılır. AI ve AO için bağımsız sekonder alanlar korunur. [THI 3](https://www.tracopower.com/thi3-datasheet), [ADP1031](https://www.analog.com/en/products/adp1031.html) |
| Referans ve koruma | İki ADR4525BRZ, girişte TPS26611, çıkışta ADG5401F | Önce ADC değişimi ve güç modülü maliyeti ölçülür. Referans veya korumadaki tasarruf ancak ayarsız hata, yanlış bağlantı ve cihaz enerjisizken giriş/çıkış davranışı aynı testlerden geçerse değerlendirilir. |

Üretim karşılaştırması üç teklifle yapılır. Teklifte aynı adet ve teslim koşulu kullanılır. Tabloda özellikle iki güç modülü, iki izolatör, DAC, hassas direnç ve TVS’lerin parça bedeliyle birlikte PCB alanı, montaj ve otomatik test süresi izlenir.

## STM32 yazılım akışı

1. **Güvenli açılış.** `AI_VSEL=1`, `AO_KILL=1`, iki CS/SYNC hattı pasif seviyede. CRC’li ve sürümlü konfigürasyonu yükle. Geçersiz konfigürasyon halinde Modbus ile devreye alma durumuna geç ve AO kapalı kalsın.
2. **AI sürücüsü.** ADS124S06 için SPI mod 1, 1 MHz başlangıç. Harici `REF0`, PGA bypass, kazanç 1, CRC ve `DRDY` zaman aşımı. Pozitif referans tamponunu seç ve AVSS civarındaki negatif tamponu bypass et. Varsayılan hızlı profil 100 SPS. Akımda AIN0/AIN1 dönüşümlerini eşleştir ve AIN2 ile TPS durumunu kontrol et. AIN0/AIN1/AIN2 yaklaşık 30,5 ms nominal ilk dönüşüm toplamı, harici RC ve yazılım gecikmesi hariç. Şebeke parazitini bastıran 20 SPS profilinde dönüşümler daha uzundur ve veri yaşı Modbus’ta ayrı yayımlanır. Değişen modda oturma süresi ve ilk geçerli örneği yeniden belirle. [ADS124S06 Tablo 13 ve SPI](https://www.ti.com/lit/ds/symlink/ads124s06.pdf)
3. **Doğrula ve ölçekle.** İşaret uzatılmış 24 bit ADC kodunu `kod × VREF / 2^23` ile gerilime çevir. Kartlar için aynı nominal katsayıları kullan. Mikrovolt ve mikroamper hesabında ara çarpımları 64 bit tut. 0 V gerilim geçerli ölçümdür. Akımda düşük/yüksek sınır ve zaman aşımı, gerilimde negatif/üst aralık/arızalı örnek ayrı durum kodlarıyla yayımlanır. Örnek yaşı ve elektriksel arıza ayrı tutulur. Eşikler ve röle histerezisi ardından hesaplanır.
4. **AO’yu kur ve etkinleştir.** Geçerli giriş ve konfigürasyon sonrası `AO_KILL=0`. Reset sonrası veri sayfasındaki beklemeyi uygula. AD5423 için ayrı SPI hattında 1 MHz ile başla. CRC/readback kullan, fabrika kalibrasyon belleği yenileme komutunu ve referans durumunu kontrol et. Çıkış kapalıyken modu seç, veri sayfasındaki programlama sırasına göre DAC_INPUT yaz, yazılımsal LDAC yap ve `OUT_EN` etkinleştir. Mod değiştirilirken `OUT_EN` önce kapatılır. [AD5423 programlama sırası](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf)
5. **Sürekli çalışma ve arıza.** AI örnekle, filtrele, seviye/röle histerezisini hesapla, AO kodunu güncelle, ölçüm ve arıza kodlarını Modbus’a yaz. AI zaman aşımı veya çıkış hatasında önce nedeni kaydet ve `AO_KILL=1` yap. STM32 IWDG reseti de çekirdek pull-up üzerinden aynı durumu getirir. A0 varsayılan arıza çıkışı yüksek empedanstır. AD5423’ün SPI watchdog ve clear-code davranışı ek tanı/savunma olarak ayrı değerlendirilir. Arıza sonrası yeniden etkinleştirme geçerli ölçüm gerektirir. Eski register davranışı çalışan cihazla doğrulandıktan sonra yeni tanılar ayrı sürümlü register alanına eklenir. [AD5423 watchdog](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf), [Modbus RTU](https://www.modbus.org/modbus-specifications)

## Prototip geçiş kontrolleri

- **Mekanik:** Eski kutuya yerleşimi, klemens erişimini, iki DC/DC’nin yaklaşık 31,8 × 20,3 × 10,5 mm gövdesini ve iki yüzün parça yüksekliklerini kontrol et. Klemens, güç modülü ve özel IC ayak izlerinin son üretim onayı PCB aşamasında yapılır. [Traco mekanik çizim](https://www.tracopower.com/thi3-datasheet)
- **Analog:** 4/12/20 mA ve 0/5/10 V, besleme uçları, 0/25/50 °C, 0/250/500 Ω akım yükü, ≥1 kΩ gerilim yükü ve saha kablosu. Çıkışın 0 V, şönt akımı telafisi, ilk açılış, reset ve yeniden etkinleşme dalga biçimlerini kaydet.
- **Yanlış bağlantı:** ±24 V uygulaması, cihaz açık/kapalı, koruma kaçağı ve ADC pin enjeksiyonu, ileri/ters akım, AO açık devre ve kısa devre. Bu testler TVS/ADG ve saha kablosu seçimini doğrular. ESD/EFT/surge uygulamasında kullanılacak seviyeler hedef ürün standardı ve saha konfigürasyonuna göre belirlenir. [IEC 61326-1](https://webstore.iec.ch/en/publication/62793)
- **İzolasyon:** Klemensler ve kablo ekranı dahil üç alanın fiziksel bariyerini ölç. Ürün test gerilimi seçildikten sonra üç bariyer çifti için yalıtım direnci, dielektrik test ve kaçak akım planını tamamla.

Bu revizyon elektriksel şema ve yazılım akışını tanımlar. Üst kart şeması, PCB yerleşimi ve donanım ölçümleri sonraki adımlardır.
