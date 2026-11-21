# LS30 A2 maliyet ve devre seçimi

19 Kasım 2026 · Ekransız · 220 VAC · Bir izole AI · Bir ortak topraklı AO · Üç COM/NO/NC kuru kontak

[Düzenlenebilir 1.000 adet BOM](LS30-A2-BOM-1000.xlsx) · [A1 tasarım notları ve KiCad bağlantısı](../LS30-Izole-Analog-A1/Tasarim-Notlari.md)

| Malzeme maliyeti | A1 | A2 temel | Düşüş |
|---|---:|---:|---:|
| İzole AI ve ortak topraklı AO / cihaz | 41,47 USD | 12,79 USD | 28,67 USD · %69,1 |
| Ürün BOM’u / cihaz | 59,87 USD | 28,87 USD | 31,00 USD · %51,8 |
| Ürün BOM’u / 1.000 cihaz | 59.872 USD | 28.867 USD | 31.005 USD |

A2 analog maliyeti yaklaşık **6,25 USD AI** ve **6,54 USD AO**. MCU stoktan karşılanıyor. Toplama kullanıcının flyback’i için 4,50 USD A1 bütçe rezervi, şebeke koruması, PCB, klemens ve mevcut kutu dahil. Montaj, test, vergi, navlun ve fire ayrı. 3PEAK parçalarının ve 3.000 rölenin teslim süresi açık. XLSX’te liste fiyatı ve tahmin dayanakları satır bazında yer alıyor.

## Blok şema

```text
  220 VAC ── Koruma ve EMI ── Kullanıcının flyback'i
                              ├── Bağımsız AI sekonderi ── 5 V / 3,3 V · GND_AI
                              └── Ana sekonder ── 5 V röle · 3,3 V MCU · ±15 V AO · GND_CORE

  AI+/AI− ─┬─ TPS26611 → 82,5 Ω şönt → ADC AIN0 ─┐
           └─ Korunan bölücü → TLV333 → ADC AIN1 ─┴─ ADS1118 ║ TPT7741 ║ STM32 SPI1
                    TPS26611 EN / SGOOD ──────────────────── ║ TPT7721 ║ STM32 GPIO

  STM32 SPI2 → DAC7760 IOUT/VOUT → TPS26611 → AO+/AO−
                     DAC +VSENSE ← OPA197 ve sense koruması ←┘
  STM32 GPIO → TPL7407L → 3 × HONGFA HF3FA · COM/NO/NC
```

AI klemens dönüşü, ADC ve AI sekonder dönüşü `GND_AI` üzerindedir. AO klemens dönüşü ve ana kontrol `GND_CORE` paylaşır. ±15 V, AO çalışma raylarıdır ve ana sekondere referanslıdır. AI bariyerinin çalışma gerilimi, yüzeysel kaçak yolu ve hava açıklığı; iki izolatör, trafo, klemens ve mekanik yerleşimle birlikte doğrulanır. WSOP paketlerin bileşen izolasyon gerilimi kartın çalışma gerilimi olarak kullanılmaz. [3PEAK TPT7741](https://www.3peak.com/digital-isolators/tpt7741?tp=1), [3PEAK TPT772x veri sayfası](https://static.3peak.com/res/doc/ds/Datasheet_TPT772x.pdf)

## AI · 4–20 mA ve 0–10 V

- **ADS1118IDGSR** 16 bit SPI ADC, dahili referans ve ±2,048 V giriş aralığı. 62,5 µV/LSB, 82,5 Ω şöntle yaklaşık 0,76 µA/LSB. 4 mA → 0,330 V, 20 mA → 1,650 V, 24 mA → 1,980 V nominal. Şönt adayı **Susumu RG2012N-82R5-W-T1**, ±%0,05 ve ±10 ppm/°C. 20 mA’de 33 mW şönt gücü ve TPS26611’in en yüksek 12,5 Ω açık direnciyle yaklaşık 1,90 V klemens yükü, sigorta/kablo gibi ek düşümler hariç. Şönt gerilimi Kelvin bağlantıyla alınır. [ADS1118 veri sayfası](https://www.ti.com/lit/ds/symlink/ads1118.pdf), [şönt adayı](https://www.digikey.com/en/products/detail/susumu/RG2012N-82R5-W-T1/602226)
- Klemensin gerilim kolu yaklaşık **80,6 kΩ / 20,0 kΩ**, giriş empedansı yaklaşık 100,6 kΩ ve 10 V’ta ADC girişi yaklaşık 1,988 V. Üst direnç kolu gerilim dayanımı için seri bölünebilir. Bölücüde başlangıç oran hatası ≤%0,05 ve düşük oran sürüklenmesi hedeflenir. **TLV333** ADC girişinin bölücüyü yüklemesini azaltır. Giriş klempi için besleme kapalıyken karta akım basılması, sıfıra yakın doğrusallık ve sıcak kaçaklar kontrol edilir. [TLV333 veri sayfası](https://www.ti.com/lit/ds/symlink/tlv333.pdf), [TI ortak V/I giriş koruması, §9.3](https://www.ti.com/lit/ds/symlink/tps2661.pdf)
- Gerilim modunda TPS26611 `EN=0`; akım modunda `EN=1`. Akım ölçümünde üretim katsayısı olarak `I_klemens ≈ V_şönt / 82,5 Ω + V_bölücü / 20,0 kΩ` kullanılır. İkinci terim, gerilim kolunun klemense paralel çektiği akımı telafi eder. İki ADC ölçümü aynı ölçüm çevrimine alınır; RC yerleşmesi, çoklayıcı geçişi ve 50/60 Hz gürültüsü değerlendirilir. Kaçak akımların kalan hatası ölçülür. [TPS26611 veri sayfası](https://www.ti.com/lit/ds/symlink/tps2661.pdf)
- **3PEAK TPT7741-SOBR** için üç ileri hat `CS`, `SCLK`, `DIN`, bir geri hat `DOUT/DRDY`. **TPT7721-SOAR** için bir ileri `AI_VSEL`, bir geri `SGOOD`. Varsayılan yüksek çıkışlı paketler kullanılır. Donanımda `CS` yüksek, `AI_VSEL` yüksek ve invertör sonrasında `EN` düşük başlar. `SGOOD` normalde düşük, arızada yüksek. Üreticinin garanti ettiği 2,0 V yüksek seviye için 3,3 V mantıkta uygun giriş eşikli tampon kullanılır. AI’nin durum hattı gerilim modunda girişin doğruluğunu belirlemez. [3PEAK TPT7741, LCSC C22460454](https://www.lcsc.com/product-detail/C22460454.html), [3PEAK TPT7721, LCSC C18548049](https://www.lcsc.com/product-detail/C18548049.html), [TPS26611 §7.5 ve §8.3.7](https://www.ti.com/lit/ds/symlink/tps2661.pdf)

## AO · 4–20 mA ve 0–10 V

- **DAC7760IPWPR**, 12 bit ve dahili 5 V referans. `REFOUT`, `REFIN` pinine bağlanır. Akım modunda dahili RSET kullanılır. Adım büyüklüğü 3,906 µA ya da 2,441 mV. `DUAL OUTEN=0` ile tek çıkış modu kullanılır. `IOUT` ve `VOUT` ortak hatta bağlandığında `+VSENSE` girişinin 60 kΩ iç yolu akım modunda saha yükünden akım çekebilir. Bu yüzden `+VSENSE` için **OPA197** tamponundan yararlanılır. [DAC7760 §9.1.1.3](https://www.ti.com/lit/ds/symlink/dac7760.pdf), [TI ortak çıkış referansı](https://www.ti.com/tool/TIPD119), [OPA197 veri sayfası](https://www.ti.com/lit/ds/symlink/opa197.pdf)
- **TPS26611DDFR**, DAC tarafı `IN`, saha tarafı `OUT`. Seri korumanın 7,5 Ω tipik / 12,5 Ω en yüksek direnci gerilim modunda klemens tarafından alınan Kelvin geri beslemeyle telafi edilir. 500 Ω yükte 20 mA için 10 V çıkış, en kötü seri düşüm 0,25 V ve DAC için 2 V çalışma payı ile +12,25 V ve ek kablo/koruma payı gerekir. +15 V rayın alt toleransı, röle geçişleri ve en yüksek sıcaklıkta test edilir. Gerilim modunda 1 kΩ ve üzeri yük ilk tasarım koşuludur. [TPS26611 Figure 9-14](https://www.ti.com/lit/ds/symlink/tps2661.pdf), [DAC7760 çalışma koşulları](https://www.ti.com/lit/ds/symlink/dac7760.pdf)
- `+VSENSE` sahaya bakan ayrı bir yoldur. Saha seri korumasına ek olarak bu yol için düşük kaçaklı klemp, seri akım sınırlama ve kesilmiş/yalıtılmış sense yolunda DAC’ın yükselmesini önleyecek geri besleme davranışı tasarlanır. Normal gerilim modunda klemens doğruluğu, akım modunda kaçak, koruma kapalıyken geri besleme ve beslemesiz ±24 V uygulanırken raylara akım basılması prototip kriterleridir. Saha TVS’si darbe testine göre seçilir. TPS26611’in veri sayfasındaki sınırlar tüm devrenin arıza dayanımı olarak kullanılamaz. [TI analog çıkış koruması](https://www.ti.com/lit/ds/symlink/tps2661.pdf)
- **Koruma alternatifi** olarak **TMUX7462FPWR**, çıkış ve sense yollarına ayrı kanallar ayırabilir. TI kataloğunda 50 mA sürekli kanal sınırı listeleniyor. Sıcaklığa ve pakete bağlı akım ve güç sınırları, her kanalın saha yönü, kapalı cihaz davranışı ve kısa devre akımının ayrıca sınırlandırılması şema üzerinde doğrulanır. DigiKey 1.000 adet fiyatı 5,45 USD, seri TPS26611’e göre tek entegre bedelinde yaklaşık 4,48 USD fark yaratır. Farklı koruma ağları gerektirdiği için temel BOM’a dahil edilmedi. [TI TMUX7462F veri sayfası](https://www.ti.com/lit/ds/symlink/tmux7462f.pdf), [TMUX7462FPWR fiyatı](https://www.digikey.com/en/products/detail/texas-instruments/TMUX7462FPWR/21283924)

## Referans ve doğruluk

**MCP1502T-25E/CHY** uygun fiyatlı 2,5 V referans. SOT-23-6, ±%0,10 başlangıç toleransı ve en fazla 7 ppm/°C sıcaklık katsayısı. DigiKey’de 3.000’lik makara için 0,930 USD/adet listeleniyor. A1’deki ADR4525’in ayak dizilimi farklı. A2 temel seçiminde ADS1118 referansı dahili, DAC7760 referans girişi 4,95–5,05 V olduğundan 2,5 V referans iki entegreye de eklenmez. [Microchip veri sayfası](https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP1502-Data-Sheet-DS20006593.pdf), [DigiKey fiyatı](https://www.digikey.com/en/products/detail/microchip-technology/MCP1502T-25E-CHY/14319029)

| Seçenek | Hata ve maliyet değerlendirmesi |
|---|---|
| **ADS1118 temel AI** | ±2,048 V aralığında 25 °C’de en yüksek kazanç hatası ±%0,15 ve en yüksek sürüklenme 40 ppm/°C. 25 °C kart sıcaklık farkıyla ön hesap ±%0,25 okuma hatası, 20 mA’da ±%0,31 ölçüm aralığı. Şönt toleransı ve sürüklenmesiyle akım modu toplamı yaklaşık ±%0,41 ölçüm aralığına ulaşır. Başlangıç ofseti, ADC yerleşmesi, kaçak ve diğer etkiler ayrıca ölçülür. |
| **ADS1120IPWR + REF3425IDBVR hassas AI** | Yayımlanan fiyatlarla ADC ve referans farkı yaklaşık **+1,00 USD/cihaz**. 2,5 V REF3425 için başlangıç ±%0,05, sürüklenme en fazla 6 ppm/°C. Harici referanslı ölçüm, kazanç modu ve 0 V ortak-mod çalışma koşulları yeniden bütçelenir. ADS1120’nin 20 SPS modunda 50/60 Hz bastırma seçeneği de değerlendirilebilir. MCP1502 aynı gerilimde bir diğer aday, footprint ve EN pin kullanımına göre incelenir. |
| **DAC7760 temel AO** | Veri sayfasında 4–20 mA ve dahili RSET için harici 5 V referansla ±%0,25 ölçüm aralığı TUE. Dahili referansın başlangıç ±5 mV ve azami 10 ppm/°C sürüklenmesi ile ek çıkış yolu hataları ayrıca değerlendirilir. 25 °C kart sıcaklık farkıyla ilk hesapta DAC+referans yaklaşık ±%0,41 akım ölçüm aralığına ulaşır. |

Kalibrasyonsuz **±%0,50 ölçüm aralığı** prototip hedefi olarak alınmıştır. Bu hedef 4–20 mA’da ±80 µA ve 0–10 V’ta ±50 mV demektir. İlk doğrulama ortamı 0, 25 ve 50 °C, ayrıca kapalı kutu içindeki gerçek kart sıcaklığını kaydetme. A1’in daha sıkı doğruluk hedefleriyle eşdeğerlik henüz gösterilmedi. Üretim testi eşik doğrulamasından oluşur, kart başına kazanç/ofset katsayısı yazılmaz. [ADS1118](https://www.ti.com/lit/ds/symlink/ads1118.pdf), [ADS1120](https://www.ti.com/lit/ds/symlink/ads1120.pdf), [REF3425](https://www.ti.com/lit/ds/symlink/ref34.pdf), [DAC7760](https://www.ti.com/lit/ds/symlink/dac7760.pdf), [ADS1120 fiyatı](https://www.lcsc.com/product-detail/C84284.html), [REF3425 fiyatı](https://www.lcsc.com/product-image/C187836.html)

## Röle ve genel pasifler

- **HONGFA HF3FA/005-ZSTF**, 5 V bobinli SPDT, LCSC **C194963**. Üçünde nominal bobin akımı toplam yaklaşık 216 mA. Ürün için eski LS30 kılavuzundaki **250 VAC / 3 A** sınırı başlangıç alındı. Kutu içine sığma, pad dizilimi, kontak yük türü, açma kapama akımı ve izolasyon mesafeleri kart yerleşiminde doğrulanır. LCSC’nin 100+ fiyatı 0,7129 USD/parça ve BOM’da üçü 2,14 USD. 3.000 parça teklifi açık. 5 V SPDT **HF3FF/005-1ZTF**, LCSC C2764967, fiyat ve mekanik uygunluk alternatifi. [HF3FA fiyatı](https://www.lcsc.com/es/product-detail/C194963.html), [Hongfa HF3FA veri sayfası](https://source.hongfa.com/Uploads/Product/PDF/HF3FA_HF3FA-T_en.pdf), [HF3FF alternatifi](https://www.lcsc.com/product-detail/power-relays_hf-xiamen-hongfa-electroacoustic-hf3ff-005-1ztf_C2764967.html)
- LCSC genel pasif seçimi **C25804** 10 kΩ ±%1, **C14663** 100 nF 50 V X7R, **C15849** 1 µF 50 V X5R ve **C19666** 4,7 µF 16 V X5R. 16 V MLCC yalnızca düşük gerilim raylarında. Regülatör çıkışı ve AO besleme kondansatörlerinde DC bias altında etkin kapasite kontrolü, şönt ve ölçüm bölücüsünde hassasiyet, şebekede ilgili güvenlik sınıfları korunur. [10 kΩ](https://www.lcsc.com/product-detail/C25804.html), [100 nF](https://www.lcsc.com/product-detail/C14663.html), [1 µF](https://www.lcsc.com/product-detail/C15849.html), [4,7 µF](https://www.lcsc.com/product-detail/C19666.html)

## Yazılım ve doğrulama

1. **Açılış.** Röle ve AO koruma enable pinleri donanımdan pasif başlar. AI gerilim konumunda başlar. MCU, ayar kaydını sürüm ve CRC ile doğrular, AI SPI ile ADC’yi, AO SPI ile DAC’ı başlatır. İki SPI, izolatörün dönüş sürücüsünü DAC `SDO` hattından ayırır. [STM32L073 SPI özellikleri](https://www.st.com/resource/en/datasheet/stm32l073vz.pdf)
2. **Ölçüm.** RS485 üzerinden yapılandırılmış modu uygula. Mod değişiminden sonra anahtar ve RC yerleşmesini bekle ve ilk örneği at. AI akım modunda şönt ve klemens gerilimini eşleştir, nominal katsayılarla µA veya mV hesapla, filtrele. Aşım, açık akım döngüsü, SPI zaman aşımı ve `SGOOD` olaylarını ayrı durum alanlarına yansıt.
3. **Sürme.** Aralık değişiminde saha yolunu ve DAC çıkışını kapat, `DUAL OUTEN=0` ile aralık ve ilk kodu yaz, kontrol alanını geri oku ve ölçümle doğrulanmış sırayla çıkışı aç. Enable kapalıyken veya koruma tetiklenmişken gerilim geri beslemesinde yükselme oluşmadığını test et. `SGOOD` ve DAC `ALARM` olaylarını ayrıştır. Hata koşulunda analog çıkış ve röle politikası cihaz ayarından uygulanır. TPS26611 negatif hatadan sonra yeniden devreye alma koşulu veri sayfasına göre işlenir. [DAC7760 kontrol register’ları](https://www.ti.com/lit/ds/symlink/dac7760.pdf), [TPS26611 arıza davranışı](https://www.ti.com/lit/ds/symlink/tps2661.pdf)
4. **Prototip kabulü.** Kalibrasyonsuz 0/5/10 V ve 4/12/20 mA noktalarını sıcaklık, röle anahtarlama ve AO yük köşelerinde ölç. AO akım modunda 0/250/500 Ω, gerilim modunda açık devre/1 kΩ ve temsilî kablo kapasitesiyle ölç. Sense açık/klemp durumunu, AI/AO ±24 V yanlış bağlantıyı cihaz açık ve kapalıyken, kısa devreyi, enerji kesintisini ve darbe testlerini dene. AI ile ana alan arasındaki izolasyon ve şebeke/kontak mesafeleri son kutulu ürün üzerinden doğrulanır.

Bu revizyon parça, maliyet, blok şema ve yazılım akışı çalışmasıdır. A2 için pin seviyesinde KiCad şeması, kart yerleşimi ve prototip ölçümleri henüz oluşturulmadı. Ürün doğruluğu ve saha arıza limitleri prototip sonuçlarıyla belirlenir.
