# -*- coding: utf-8 -*-
"""
Build Script for ONLOI Firmware Studio (v1.0)
Generates standalone Release executable without source code.
"""
import os
import sys
import shutil
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
RELEASE_DIR = os.path.join(PROJECT_ROOT, "Release", "Onloi_Firmware_Studio_v1.0")
ICON_PATH = os.path.join(SCRIPT_DIR, "studio_icon.ico")
VERSION_FILE = os.path.join(SCRIPT_DIR, "file_version_info.txt")
MAIN_SCRIPT = os.path.join(SCRIPT_DIR, "onloi_firmware_studio.py")

def main():
    print("==================================================")
    print("  ONLOI FIRMWARE STUDIO v1.0 - RELEASE DERLEME")
    print("==================================================")

    os.makedirs(RELEASE_DIR, exist_ok=True)

    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconsole",
        "--onefile",
        "--name=Onloi_Firmware_Studio_v1.0",
        f"--icon={ICON_PATH}",
        f"--version-file={VERSION_FILE}",
        f"--add-data={ICON_PATH};.",
        f"--distpath={RELEASE_DIR}",
        f"--workpath={os.path.join(SCRIPT_DIR, 'build')}",
        f"--specpath={SCRIPT_DIR}",
        "--clean",
        MAIN_SCRIPT
    ]

    print("PyInstaller calistiriliyor...")
    res = subprocess.run(cmd, cwd=SCRIPT_DIR)

    if res.returncode == 0:
        print("\n[OK] DERLEME BASARILI!")
        exe_path = os.path.join(RELEASE_DIR, "Onloi_Firmware_Studio_v1.0.exe")
        if os.path.exists(exe_path):
            size_mb = os.path.getsize(exe_path) / (1024 * 1024)
            print(f"Exe Konumu: {exe_path} ({size_mb:.2f} MB)")

        readme_path = os.path.join(RELEASE_DIR, "KULLANIM_KILAVUZU.txt")
        readme_content = """========================================================================
ONLOI FIRMWARE STUDIO (v1.0)
Görsel Derleme, Yazılım Yükleme (Flash) ve Seri Port Monitörü
========================================================================

Bu yazılım, Onloi Geçiş Kontrol Terminallerinin (OE0111, OE0112, OE0113) 
yazılımlarını derlemek, birleştirmek (merged 0x0000), karta tek tıkla 
flash etmek ve anlık seri port loglarını izlemek için hazırlanmış 
kapsamlı mühendislik ve üretim asistanıdır.

ÖZELLİKLER:
------------------------------------------------------------------------
1. DERLEME (BUILD):
   - OE0111, OE0112, OE0113 veya tümünü tek tıkla derler.
   - Bootloader, partitions, ota_data ve firmware ikililerini 0x0000 
     başlangıç adresli tek bir 'full_firmware_OExxxx.bin' dosyasında birleştirir.
   - Çıktıları otomatik olarak 'Software/outputs/' altına arşivler.

2. YAZILIM YÜKLEME (FLASH):
   - Cihazın bağlı olduğu COM portu seçin.
   - 921600 baud yüksek hızda 15-20 saniyede tam yazılım yüklemesi yapar.
   - İster hazır derlenmiş model yazılımlarını, ister özel .bin dosyasını yükleyin.
   - Çip hafızasını temizlemek için 'Çipi Tamamen Sil (Erase Flash)' fonksiyonu mevcuttur.

3. CANLI SERİ MONİTÖR (UART LOG):
   - 115200 baud ile cihazın açılış ve çalışma loglarını anlık görüntüler.
   - Hızlı CLI butonları (OTA Info, Kullanıcılar, Masterlar, OSDP vb.).
   - Donanımsal ESP32 Reset (DTR/RTS) desteği.
   - Flash işlemi başladığında port çakışmasını önlemek için seri monitörü 
     otomatik durdurur; flash tamamlandığında cihaz loglarını anında 
     yakalamak için seri monitörü otomatik yeniden bağlar.

========================================================================
ONLOI AR-GE EKİBİ
========================================================================
"""
        with open(readme_path, "w", encoding="utf-8") as f:
            f.write(readme_content)

        # Clean build folder
        build_folder = os.path.join(SCRIPT_DIR, 'build')
        if os.path.exists(build_folder):
            shutil.rmtree(build_folder, ignore_errors=True)

        print(f"Release Paketi Hazir: {RELEASE_DIR}")
    else:
        print("\n[HATA] Derleme basarisiz oldu. Hata kodu:", res.returncode)

if __name__ == "__main__":
    main()
