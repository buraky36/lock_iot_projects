#!/usr/bin/env python3
"""
Access Control Terminal - Multi-Environment Auto-Build & Packager (V2.0.9)
Compiles all 3 hardware environments (PIN+QR, PIN+RFID, RFID_ONLY) via PlatformIO,
packages merged binaries (0x0000), partitioned binaries, and FLASH_ADDRESSES.txt
into the structured outputs/ directory:
  outputs/OE_0110_PIN_QR/V2.0.9/
  outputs/OE_0111_PIN_RFID/V2.0.9/
  outputs/OE_0112_RFID_ONLY/V2.0.9/

Also handles migration of legacy releases/ (V2.0.3, V2.0.4, V2.0.5) to outputs/.
"""

import os
import shutil
import subprocess
import sys

VERSION = "V2.0.10"

VARIANTS = [
    {
        'env': 'pin_qr',
        'folder': 'OE_0110_PIN_QR',
        'code': 'OE0110',
        'product_id': '0x0110',
        'title': 'PIN + QR Controller',
        'merged': 'full_firmware_OE0110.bin'
    },
    {
        'env': 'pin_rfid',
        'folder': 'OE_0111_PIN_RFID',
        'code': 'OE0111',
        'product_id': '0x0111',
        'title': 'PIN + RFID Controller',
        'merged': 'full_firmware_OE0111.bin'
    },
    {
        'env': 'rfid_only',
        'folder': 'OE_0112_RFID_ONLY',
        'code': 'OE0112',
        'product_id': '0x0112',
        'title': 'RFID ONLY Controller',
        'merged': 'full_firmware_OE0112.bin'
    }
]

FLASH_GUIDE_TEMPLATE = """================================================================================
ACCESS CONTROL TERMINAL - FLASH BELLEK ADRES REHBERİ
MODEL: {title} ({product_id}) | YAZILIM VERSİYONU: {version}
================================================================================

1) TEK DOSYA İLE FLASH ETME (MERGED BINARY - FABRİKA YÜKLEMESİ İÇİN ÖNERİLEN)
--------------------------------------------------------------------------------
Dosya: {merged_name}
Adres: 0x0000 (Flash başlangıç adresi)

Esptool Komutu:
esptool.py --chip esp32 --port COMx --baud 921600 write_flash 0x0000 {merged_name}

ESP Flash Download Tool Ayarları:
- Dosya: {merged_name}  -> Adres: 0x0000 (Seçili / Checked)
- SPI SPEED: 40MHz
- SPI MODE: DIO
- FLASH SIZE: 32Mbit-512KB (4MB)

--------------------------------------------------------------------------------
2) PARÇALI DOSYALAR İLE FLASH ETME (PARTITIONED BINARIES)
--------------------------------------------------------------------------------
- 0x1000   : bootloader.bin
- 0x8000   : partitions.bin
- 0x109000 : ota_data_initial.bin
- 0x110000 : firmware.bin

Esptool Komutu:
esptool.py --chip esp32 --port COMx --baud 921600 write_flash \\
    0x1000   bootloader.bin \\
    0x8000   partitions.bin \\
    0x109000 ota_data_initial.bin \\
    0x110000 firmware.bin

ESP Flash Download Tool Ayarları:
- bootloader.bin       -> Adres: 0x1000
- partitions.bin       -> Adres: 0x8000
- ota_data_initial.bin -> Adres: 0x109000
- firmware.bin         -> Adres: 0x110000

--------------------------------------------------------------------------------
3) ONLOI CLOUD / APP OTA GÜNCELLEMESİ İÇİN
--------------------------------------------------------------------------------
Dosya: firmware.bin
(Sadece uygulama binary dosyası 'firmware.bin' Onloi Cloud veya HTTP OTA sunucusuna yüklenmelidir.)
================================================================================
"""

def migrate_legacy_releases(root_dir):
    """Migrate releases/ (V2.0.3, V2.0.4, V2.0.5) to outputs/ structure and clean up."""
    legacy_releases = os.path.join(root_dir, 'releases')
    outputs_dir = os.path.join(root_dir, 'outputs')
    legacy_output = os.path.join(root_dir, 'output')

    if os.path.exists(legacy_releases):
        print("\n[MİGRASYON] Eski 'releases/' klasörü 'outputs/' altına taşınıyor...")
        for ver in ['V2.0.3', 'V2.0.4', 'V2.0.5']:
            src_ver_dir = os.path.join(legacy_releases, ver)
            if not os.path.exists(src_ver_dir):
                continue
            
            # Map legacy subfolder names to new folder names
            folder_map = {
                'OE0110_PIN_QR': 'OE_0110_PIN_QR',
                'OE0111_PIN_RFID': 'OE_0111_PIN_RFID',
                'OE0112_RFID_ONLY': 'OE_0112_RFID_ONLY'
            }
            
            for old_name, new_name in folder_map.items():
                old_path = os.path.join(src_ver_dir, old_name)
                if os.path.exists(old_path):
                    target_dir = os.path.join(outputs_dir, new_name, ver)
                    os.makedirs(target_dir, exist_ok=True)
                    for item in os.listdir(old_path):
                        s = os.path.join(old_path, item)
                        d = os.path.join(target_dir, item)
                        if os.path.isfile(s) and not os.path.exists(d):
                            shutil.copy2(s, d)
                    print(f"  [TAŞINDI] {ver}/{old_name} -> outputs/{new_name}/{ver}/")

        # Remove old releases directory
        try:
            shutil.rmtree(legacy_releases)
            print("  [TEMİZLENDİ] Eski 'releases/' klasörü başarıyla silindi.")
        except Exception as e:
            print(f"  [UYARI] 'releases/' silinirken hata: {e}")

    # Remove old output directory
    if os.path.exists(legacy_output):
        try:
            shutil.rmtree(legacy_output)
            print("  [TEMİZLENDİ] Eski 'output/' klasörü başarıyla silindi.")
        except Exception as e:
            print(f"  [UYARI] 'output/' silinirken hata: {e}")

def main():
    root_dir = os.path.dirname(os.path.abspath(__file__))
    pio_exe = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\pio.exe"
    python_exe = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe"
    esptool_py = r"C:\Users\gentek-ideapad\.platformio\packages\tool-esptoolpy\esptool.py"

    if not os.path.exists(pio_exe):
        pio_exe = "pio"
    if not os.path.exists(python_exe):
        python_exe = sys.executable
    if not os.path.exists(esptool_py):
        # Fallback search
        esptool_py = "esptool.py"

    print("=" * 80)
    print(f"ACCESS CONTROL TERMINAL - VARYASYON DERLEME VE DAĞITIM PAKETLEYİCİ ({VERSION})")
    print("=" * 80)

    # Step 1: Migrate legacy folders if they exist
    migrate_legacy_releases(root_dir)

    # Step 2: Build all variations via PlatformIO
    print("\n[1/3] PlatformIO ile tüm varyasyonlar derleniyor...")
    cmd_build = [pio_exe, "run"]
    ret = subprocess.call(cmd_build, cwd=root_dir)
    if ret != 0:
        print("\n[HATA] PlatformIO derleme işlemi başarısız oldu!")
        sys.exit(1)

    # Step 3: Package into outputs/
    print(f"\n[2/3] İkili dosyalar (Binaries) 'outputs/' altına paketleniyor ({VERSION})...")
    outputs_base = os.path.join(root_dir, 'outputs')
    os.makedirs(outputs_base, exist_ok=True)

    summary_rows = []

    for v in VARIANTS:
        env = v['env']
        folder = v['folder']
        merged_name = v['merged']
        title = v['title']
        prod_id = v['product_id']

        src_dir = os.path.join(root_dir, '.pio', 'build', env)
        dst_dir = os.path.join(outputs_base, folder, VERSION)
        os.makedirs(dst_dir, exist_ok=True)

        # Copy individual partition binaries
        bin_files = ['bootloader.bin', 'partitions.bin', 'ota_data_initial.bin', 'firmware.bin']
        for f in bin_files:
            s = os.path.join(src_dir, f)
            d = os.path.join(dst_dir, f)
            if os.path.exists(s):
                shutil.copy2(s, d)
            else:
                print(f"  [UYARI] {f} bulunamadı: {s}")

        # Create merged full firmware binary
        merged_path = os.path.join(dst_dir, merged_name)
        cmd_merge = [
            python_exe, esptool_py,
            '--chip', 'esp32',
            'merge_bin',
            '-o', merged_path,
            '--flash_mode', 'dio',
            '--flash_freq', '40m',
            '--flash_size', '4MB',
            '0x1000', os.path.join(dst_dir, 'bootloader.bin'),
            '0x8000', os.path.join(dst_dir, 'partitions.bin'),
            '0x109000', os.path.join(dst_dir, 'ota_data_initial.bin'),
            '0x110000', os.path.join(dst_dir, 'firmware.bin')
        ]
        res = subprocess.run(cmd_merge, capture_output=True, text=True)
        if res.returncode == 0:
            fw_size = os.path.getsize(os.path.join(dst_dir, 'firmware.bin'))
            merged_size = os.path.getsize(merged_path)
            print(f"  [OK] {folder}/{VERSION}/ -> {merged_name} ({merged_size:,} bytes | fw: {fw_size:,} bytes)")
            summary_rows.append((folder, VERSION, merged_name, merged_size, fw_size))
        else:
            print(f"  [HATA] {merged_name} birleştirilemedi: {res.stderr}")

        # Generate FLASH_ADDRESSES.txt in the variant folder
        guide_content = FLASH_GUIDE_TEMPLATE.format(
            title=title,
            product_id=prod_id,
            version=VERSION,
            merged_name=merged_name
        )
        with open(os.path.join(dst_dir, 'FLASH_ADDRESSES.txt'), 'w', encoding='utf-8') as f:
            f.write(guide_content)

    print("\n[3/3] DERLEME VE PAKETLEME TAMAMLANDI!")
    print("=" * 80)
    print(f"{'Varyasyon':<22} {'Versiyon':<10} {'Merged Binary':<28} {'Flash Boyutu':<14} {'Firmware Boyutu'}")
    print("-" * 80)
    for folder, ver, m_name, m_size, f_size in summary_rows:
        print(f"{folder:<22} {ver:<10} {m_name:<28} {m_size:,} B       {f_size:,} B")
    print("=" * 80)
    print(f"Çıktı Dizini: {outputs_base}\n")

if __name__ == '__main__':
    main()
