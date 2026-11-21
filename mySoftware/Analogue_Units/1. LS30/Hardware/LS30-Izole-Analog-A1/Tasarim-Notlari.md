# LS30 A1 güç mimarisi ve maliyet kapsamı

**220 VAC · Ekransız · 3 kuru kontak röle · İzole AI · Ortak referanslı AO**

[KiCad projesi](LS30-Izole-Analog.kicad_pro) · [1.000 adet BOM hesabı](outputs/01a1f020-e531-77a0-821e-a172b14e1b61/LS30-A1-BOM-1000.xlsx) · [A0 analog devre ve yazılım açıklaması](../LS30-Izole-Analog/Tasarim-Notlari.md)

## A1 değişiklikleri

- **Flyback.** Kullanıcının 220 VAC flyback tasarımı tek güç kaynağıdır. AI beslemesi J102’de kendi bağımsız sekonderi ve `GND_AI` dönüşü ile gelir. A1’de yaklaşık 12 V nominal varsayıldı. Mevcut 5 V ve 3,3 V yerel regülatörler ile **ISO7762DW** bariyeri girişte kullanılır. AI sekonderi ve ana sekonder arasında ortak dönüş, geri besleme veya servis kablosu üzerinden iletken bağlantı oluşmamalı. Trafo ve kart üzerinde primer–sekonder ve AI–ana sekonder çalışma gerilimi, yüzeysel kaçak yolu ve hava açıklığı ayrı değerlendirilir. [ISO7762 veri sayfası](https://www.ti.com/lit/ds/symlink/iso7762.pdf)
- **AO.** 4–20 mA / 0–10 V çıkışın dönüşü STM32 ile **`GND_CORE`** paylaşır. AO’nun dijital bağlantısı doğrudan STM32’ye gelir. Önceki ayrı izolasyon, AO’nun saha dönüşü ile cihazın çekirdek dönüşünü birbirinden ayırıyordu. AD5423 ve ADG5401F ölçüm/koruma devresi sürer. Şemadaki **+15 V ve −15 V** analog raylar, kullanıcının flyback tasarımına veya aynı sekondere bağlı yerel besleme çözümüne arayüz varsayımıdır. Bunlar ayrı galvanik alan oluşturmaz. 20 mA / 500 Ω yük, 0 V regülasyonu, TVS, ters bağlantı ve röle anahtarlama etkisi prototipte ölçülür. [AD5423](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5423.pdf), [ADG5401F](https://www.analog.com/media/en/technical-documentation/data-sheets/adg5401f.pdf)
- **Üç röle.** Fiyat hesabında COM/NO/NC değiştirici kuru kontaklı **G5LE-1 DC5** ×3 ve **TPL7407L** bobin sürücüsü kullanıldı. 5 V bobinlerin üçü birlikte yaklaşık 238 mA çeker. Flyback arayüzü için **5 V ana sekonderde en az 350 mA** ilk boyutlandırma varsayımıdır. Kontak ve klemenslerin yük tipi, izolasyon mesafeleri ve mevcut kutuya uyum üst kartın yerleşiminde değerlendirilir. A1 analog sayfaları güç arayüzünü gösterir. Röle, şebeke ve flyback parçaları ürün maliyetine dahildir ve üst kart şemasında yer alacaktır. [G5LE-1 DC5 katalog ve fiyat sayfası](https://www.digikey.com/en/products/detail/omron-electronics-inc-emc-div/G5LE-1-DC5/280368), [TPL7407L](https://www.ti.com/product/TPL7407L)

## Hesap varsayımları

1.000 cihaz için malzeme kalemleri ve kaynak fiyatlar çalışma kitabında. STM32 stok varsayımı düzenlenebilir. Flyback transformatörü, sekonder rayların üretimi, şebeke koruması, PCB, klemens ve kutu için bağımsız bütçe payları kullanıldı. Flyback BOM’u ve mekanik tedarikçi teklifi geldiğinde bu kalemler gerçek fiyatlarla güncellenir. Mevcut kutu bedeli ayrı satırda. Montaj, üretim testi, KDV, gümrük, navlun ve fire malzeme toplamının dışındadır.
