# OK0110 V2_01 — Final şematik snapshot + PCB hızlı plan

**Tarih:** 2026-09-15  
**Durum:** PCB’ye geçmeden önceki **son şematik** (ekran görüntüleri + PDF)  
**Klasör:** `schematic_final_2026-09-15/`

| Dosya | Açıklama |
|--------|----------|
| `01_root.png` … `10_sensors.png` | Sayfa ekran görüntüleri |
| `OK0110_V2_01_Schematic_FINAL_2026-09-15.pdf` | Aynı görüntülerden PDF |

---

## Bu snapshot’ta kilit mimari (hafıza / değerlendirme)

- **MCU:** STM32G070 — SWD PA13/PA14, ESP ISP BOOT/NRST, wake PA12↔ESP IO2, BT_RST→ESP EN  
- **ESP32-C6:** 32.768 kHz, auto DTR/RTS, STM32_BOOT jumper, Type-C UART dongle planı ayrı  
- **Güç:** AP63300 3.3 V; CN3302 2S şarj; AP22802 RF/FP/Audio; +VSW motor (nSLEEP); **power mux yok** → USB’de motor sınırla  
- **Motor:** DRV8874 + IPROPI  
- **RF:** RFM300; **Audio:** PAM+AP23; **TSM12**; **LIS3DH**  
- **RGB:** adreslenebilir LED (IN-PI22 / benzeri) — TIM PWM+DMA (SPI1 CMT’ye ait)

---

## Bugün içinde 2→4 kat PCB — sıra (hızlı yol)

### 0) Yedek (2 dk)
1. Projeyi kopyala veya zip: `…_V2_01_backup_YYYYMMDD.zip`  
2. Özellikle: `*.kicad_pcb`, `*.kicad_pro`, `*.kicad_sch`

### 1) Şematik kilit (5–10 dk)
1. KiCad Schematic → **Electrical Rules Check (ERC)** — error kalmasın  
2. Footprint atanmamış / DNP netleri temizle  
3. İsteğe: **File → Plot → PDF** (KiCad native; bu klasördeki PNG PDF yedek)

### 2) PCB’yi şemadan güncelle (10 dk)
1. PCB Editor aç (`OK0110_…_V2_01.kicad_pcb`)  
2. **Tools → Update PCB from Schematic…** (`F8`)  
   - Match by reference  
   - Delete footprints with no symbols: **isteğe bağlı** (silinen parçalar için ON)  
3. Çıkan farkları Apply et  
4. Footprint’i olmayanları hemen ata (Properties)

### 3) Eski bakırları temizle (5 dk)
1. **Edit → Global Deletions…** veya seçerek sil:  
   - **Tracks**  
   - **Vias**  
   - (İsteğe) Zones — sonra yeniden çizilecek  
2. **Footprint’leri silme** — yerleşim (veya konnektör/mekanik) kalabilir  
3. Board outline / mounting holes varsa **tut**

> Not: 2 kat → 4 kat geçerken eski route’lar çoğu zaman çöp; sıfırdan net daha hızlı.

### 4) 4 kat stack-up (10 dk)
1. **File → Board Setup → Layers**  
   - Copper layers: **4**  
2. Önerilen isimler / kullanım (hızlı ürün):

| Katman | Rol |
|--------|-----|
| **F.Cu** | Sinyal + kısa güç |
| **In1.Cu** | **GND** düzlem (solid) |
| **In2.Cu** | **+3.3V** (ve/veya +BATT bölgeleri) |
| **B.Cu** | Sinyal + kalan güç |

3. **Board Setup → Constraints:** clearance, track min (üretici: genelde 0.15–0.2 mm yeterli)  
4. Via: örn. 0.45/0.25 veya üretici min  
5. **Net Classes:** Motor/VM kalın; USB; RF antenna keepout

### 5) Yerleşim (placement) — en kritik (1–3 saat)
Sıra (mekanik önce):

1. **Konnektörler / Type-C / batarya / motor / hoparlör / FP** — muhafaza delikleri  
2. **ESP32-C6** + anten keepout (metalden uzak)  
3. **RFM300** + anten  
4. **STM32** merkeze yakın  
5. **DRV8874 + motor güç** kısa, kalın, sense yakın  
6. **AP63300 + CN3302** şarj/buck — Cin/Cout yakın IC  
7. **TSM12 / LIS3DH**  
8. **PAM/AP23**  
9. **WS2812 / LED** dokunmatik yakını  

Her blokta: dekupaj VDD pinine **mümkün olduğunca yakın**.

### 6) Plane + kritik route (2–4 saat)
1. **In1 = GND zone** tüm board (Priority yüksek)  
2. **In2 = +3.3V zone** (veya split: 3V3 / +BATT)  
3. Önce güç: +BATT, VDD, +VSW, V_USB  
4. Sonra yüksek hızlı / hassas:  
   - ESP UART, SWD  
   - CMT SPI (kısa)  
   - I2C (pull-up yakını)  
   - Encoder PC6/7  
5. Motor OUT kalın + via dikiş  
6. RF: 50 Ω / üretici önerisi; GND stitching vias  
7. Crystal 32k: kısa, GND guard

### 7) DRC + üretim (30–60 dk)
1. **Inspect → Design Rules Checker** — 0 error  
2. Unconnected: ratsnest temiz  
3. Silkscreen okunaklı (Ref, Type-C yön oku)  
4. **File → Fabrication Outputs:** Gerber + drill + BOM + CPL  
5. Üreticiye 4-layer stack notu yaz

---

## Bugün yetiştirmek için “kes” listesi

Yap **şunları ertele** (zaman kalmazsa):

- Perfect length match / güzel zig-zag  
- Tüm silkscreen sanatı  
- İkinci RF matching iterasyonu  
- Power mux ekleme (şemada yok — FW ile motor USB’de kapalı)

**Mutlaka bitir:**

1. Update from schematic  
2. 4L + GND plane  
3. Tüm netler bağlı + DRC clean  
4. Mekanik/konnektör doğru  
5. Gerber export

---

## KiCad kısa komutlar

| İş | Nerede |
|----|--------|
| Şema → PCB | PCB: `F8` Update PCB from Schematic |
| Track sil | Global Deletions → Tracks + Vias |
| 4 kat | Board Setup → Physical Stackup / Board Stackup |
| Zone | `Ctrl+Shift+Z` veya Add Filled Zone |
| DRC | Inspect → DRC |
| Ratsnest | View → Show board ratsnest |

---

## Riskler (bugün)

- Eski 2L PCB’de **outline yanlış** kalırsa muhafaza uymaz → önce outline doğrula  
- Footprint değişen parçalar (DRV8874, C6, CN3302, LED) → Update sonrası **overlap** kontrol  
- ESP/RF anten kenarı — metal şasi  

Bu dosya + PNG/PDF = **şematik baseline**. PCB değişiklikleri bundan saparsa not düş.
