# -*- coding: utf-8 -*-
"""
Build Script for ONLOI DVT Test Asistanı (V1.2)
Generates standalone Release executable without source code.
"""
import os
import sys
import shutil
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
RELEASE_DIR = os.path.join(PROJECT_ROOT, "Release", "Onloi_DVT_Tester_V1.2")
ICON_PATH = os.path.join(SCRIPT_DIR, "app_icon.ico")
VERSION_FILE = os.path.join(SCRIPT_DIR, "file_version_info.txt")
MAIN_SCRIPT = os.path.join(SCRIPT_DIR, "onloi_dvt_assistant.py")

def main():
    print("==================================================")
    print("  ONLOI DVT TEST ASISTANI V1.2 — RELEASE DERLEME")
    print("==================================================")

    os.makedirs(RELEASE_DIR, exist_ok=True)

    cmd = [
        sys.executable, "-m", "PyInstaller",
        "--noconsole",
        "--onefile",
        f"--name=Onloi_DVT_Tester_V1.2",
        f"--icon={ICON_PATH}",
        f"--version-file={VERSION_FILE}",
        f"--add-data={ICON_PATH};.",
        f"--distpath={RELEASE_DIR}",
        f"--workpath={os.path.join(SCRIPT_DIR, 'build')}",
        f"--specpath={SCRIPT_DIR}",
        "--clean",
        MAIN_SCRIPT
    ]

    print("PyInstaller komutu calistiriliyor...")
    print(" ".join(cmd))
    res = subprocess.run(cmd, cwd=SCRIPT_DIR)

    if res.returncode == 0:
        print("\n[OK] DERLEME BASARILI!")
        exe_path = os.path.join(RELEASE_DIR, "Onloi_DVT_Tester_V1.2.exe")
        if os.path.exists(exe_path):
            size_mb = os.path.getsize(exe_path) / (1024 * 1024)
            print(f"Exe Konumu: {exe_path} ({size_mb:.2f} MB)")

        # Create Release helper directories and README
        os.makedirs(os.path.join(RELEASE_DIR, "Reports"), exist_ok=True)
        os.makedirs(os.path.join(RELEASE_DIR, "Sessions"), exist_ok=True)

        readme_path = os.path.join(RELEASE_DIR, "KULLANIM_KILAVUZU.txt")
        readme_content = """========================================================================
ONLOI GEÇİŞ KONTROL TERMİNALİ — DVT TEST ASİSTANI (V1.2)
Sürümden Bağımsız / Otomatik Firmware Algılama (v2.0.27+ Uyumlu)
========================================================================

Bu yazılım, Onloi Geçiş Kontrol Terminallerinin (OE0111, OE0112, OE0113) 
saha öncesi tüm donanım ve yazılım fonksiyonlarını adım adım test ve 
doğrulama amacıyla hazırlanmış bağımsız test yürütücüsüdür.

HERHANGİ BİR PYTHON VEYA EK SÜRÜCÜ KURULUMU GEREKTİRMEZ.
CİHAZIN ÜZERİNDEKİ FIRMWARE SÜRÜMÜNÜ SERİ PORTTAN OTOMATİK ALGILAYARAK
RAPORLAR; YENİ FIRMWARE SÜRÜMLERİNDE UYGULAMAYI GÜNCELLEMEYE GEREK YOKTUR.

HIZLI BAŞLANGIÇ:
------------------------------------------------------------------------
1. "Onloi_DVT_Tester_V1.2.exe" uygulamasını çift tıklayarak çalıştırın.
2. Üst menüden cihazınızın bağlı olduğu COM Port'u seçin ve "Bağlan"a tıklayın.
3. Test ettiğiniz modeli seçin:
   - OE0111: Tuş Takımı + RFID + BLE + OSDP + IN1/IN2 + Röle
   - OE0112: Sadece RFID + BLE + Röle
   - OE0113: Tuş Takımı + RFID + BLE + Röle (Wiegand/OSDP Yok)
4. Personel Adı ve Seri Numarasını girin.
5. Ortadaki talimat kartındaki yönlendirmeleri sırayla uygulayın:
   - Cihazdan gelen yanıtlar seri port üzerinden otomatik doğrulanır (PASS).
   - Kart okutma, tuşlama veya LED/Buzzer gözlemlerinde "DOĞRULANDI" butonuna basın.
6. Test bittiğinde sol alttaki "Test Raporu Oluştur" butonu ile 
   HTML ve CSV formatında resmi kalite kontrol raporunuzu alın.
   Raporlar doğrudan bu klasörün içindeki "Reports" dizinine kaydedilir.

NOTLAR:
- Test adımlarında süre sınırı / zaman aşımı yoktur; adımları istediğiniz sürede yapabilirsiniz.
- Uygulamayı kapatsanız dahi kaldığınız adım, girilen bilgiler ve sonuçlar otomatik saklanır.
- Yeni bir cihaz için testi sıfırlamak isterseniz "🔄 Bu Modelin Testini Sıfırla" butonuna basabilirsiniz.

========================================================================
ONLOI AR-GE & KALİTE GÜVENCE EKİBİ
========================================================================
"""
        with open(readme_path, "w", encoding="utf-8") as f:
            f.write(readme_content)

        print(f"Kullanım kılavuzu eklendi: {readme_path}")
        print(f"Release Paketi Hazır: {RELEASE_DIR}")
    else:
        print("\n❌ DERLEME BASARISIZ! Hata kodu:", res.returncode)

if __name__ == "__main__":
    main()
