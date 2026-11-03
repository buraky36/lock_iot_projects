#!/usr/bin/env python3
"""
Access Control Terminal - Multi-Environment Auto-Build & Packager (V2.0.13)
Compiles all 3 hardware environments (PIN+QR, PIN+RFID, RFID_ONLY) via PlatformIO,
packages merged binaries (0x0000), partitioned binaries, and FLASH_ADDRESSES.txt
into the structured outputs/ directory:
  outputs/OE_0113_PIN_QR/V2.0.13/
  outputs/OE_0111_PIN_RFID/V2.0.13/
  outputs/OE_0112_RFID_ONLY/V2.0.13/

Archives all previous versions into outputs/archieve/.
"""

import os
import shutil
import subprocess
import sys
import zipfile

VERSION = "V2.0.13"

VARIANTS = [
    {
        'env': 'pin_qr',
        'folder': 'OE_0113_PIN_QR',
        'code': 'OE0113',
        'product_id': '0x0113',
        'title': 'PIN + QR Controller',
        'merged': f'full_firmware_OE0113_PIN_QR_{VERSION}.bin',
        'zip_name': f'full_firmware_OE0113_PIN_QR_{VERSION}.zip'
    },
    {
        'env': 'pin_rfid',
        'folder': 'OE_0111_PIN_RFID',
        'code': 'OE0111',
        'product_id': '0x0111',
        'title': 'PIN + RFID Controller',
        'merged': f'full_firmware_OE0111_PIN_RFID_{VERSION}.bin',
        'zip_name': f'full_firmware_OE0111_PIN_RFID_{VERSION}.zip'
    },
    {
        'env': 'rfid_only',
        'folder': 'OE_0112_RFID_ONLY',
        'code': 'OE0112',
        'product_id': '0x0112',
        'title': 'RFID ONLY Controller',
        'merged': f'full_firmware_OE0112_RFID_ONLY_{VERSION}.bin',
        'zip_name': f'full_firmware_OE0112_RFID_ONLY_{VERSION}.zip'
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

def archive_old_outputs(root_dir, current_version):
    """Move older versions and deprecated folders to outputs/archieve/."""
    outputs_base = os.path.join(root_dir, 'outputs')
    if not os.path.exists(outputs_base):
        return

    archieve_dir = os.path.join(outputs_base, 'archieve')
    os.makedirs(archieve_dir, exist_ok=True)

    print("\n[ARŞİV] Eski versiyonlar 'outputs/archieve/' klasörüne taşınıyor...")

    # 1. Deprecated OE_0110_PIN_QR folder
    old_pin_qr = os.path.join(outputs_base, 'OE_0110_PIN_QR')
    if os.path.exists(old_pin_qr):
        target = os.path.join(archieve_dir, 'OE_0110_PIN_QR')
        if os.path.exists(target):
            shutil.rmtree(target)
        shutil.move(old_pin_qr, target)
        print("  [ARŞİVLENDİ] OE_0110_PIN_QR -> outputs/archieve/OE_0110_PIN_QR")

    # 2. Subversions in active model folders
    for v in VARIANTS:
        folder_path = os.path.join(outputs_base, v['folder'])
        if not os.path.exists(folder_path):
            continue

        target_model_arch = os.path.join(archieve_dir, v['folder'])
        os.makedirs(target_model_arch, exist_ok=True)

        for item in os.listdir(folder_path):
            item_path = os.path.join(folder_path, item)
            # Only move folders that are versions and not current version
            if os.path.isdir(item_path) and item != current_version:
                target_dest = os.path.join(target_model_arch, item)
                if os.path.exists(target_dest):
                    shutil.rmtree(target_dest)
                shutil.move(item_path, target_dest)
                print(f"  [ARŞİVLENDİ] {v['folder']}/{item} -> outputs/archieve/{v['folder']}/{item}")

    # 3. Old OTA parameter txt files in outputs root
    for item in os.listdir(outputs_base):
        if item.startswith('OTA_') and item.endswith('.txt'):
            src = os.path.join(outputs_base, item)
            dst = os.path.join(archieve_dir, item)
            if not os.path.exists(dst):
                shutil.move(src, dst)
                print(f"  [ARŞİVLENDİ] {item} -> outputs/archieve/{item}")

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
        esptool_py = "esptool.py"

    print("=" * 80)
    print(f"ACCESS CONTROL TERMINAL - VARYASYON DERLEME VE DAĞITIM PAKETLEYİCİ ({VERSION})")
    print("=" * 80)

    # Step 1: Archive old versions to outputs/archieve/
    archive_old_outputs(root_dir, VERSION)

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
        zip_name = v['zip_name']
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

            # Generate FLASH_ADDRESSES.txt in the variant folder
            guide_content = FLASH_GUIDE_TEMPLATE.format(
                title=title,
                product_id=prod_id,
                version=VERSION,
                merged_name=merged_name
            )
            guide_path = os.path.join(dst_dir, 'FLASH_ADDRESSES.txt')
            with open(guide_path, 'w', encoding='utf-8') as f:
                f.write(guide_content)

            # Create ZIP containing the merged full firmware binary and the flash guide
            zip_path = os.path.join(dst_dir, zip_name)
            with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
                zf.write(merged_path, arcname=merged_name)
                zf.write(guide_path, arcname='FLASH_ADDRESSES.txt')

            print(f"  [OK] {folder}/{VERSION}/ -> {merged_name} ({merged_size:,} bytes | fw: {fw_size:,} bytes)")
            print(f"       -> ZIP Paketi: {zip_name} ({os.path.getsize(zip_path):,} bytes)")
            summary_rows.append((folder, VERSION, merged_name, zip_name, merged_size, fw_size))
        else:
            print(f"  [HATA] {merged_name} birleştirilemedi: {res.stderr}")

    print("\n[3/3] DERLEME VE PAKETLEME TAMAMLANDI!")
    print("=" * 95)
    print(f"{'Varyasyon':<20} {'Versiyon':<10} {'Merged Binary':<42} {'Flash Boyutu':<14} {'Firmware Boyutu'}")
    print("-" * 95)
    for folder, ver, m_name, z_name, m_size, f_size in summary_rows:
        print(f"{folder:<20} {ver:<10} {m_name:<42} {m_size:,} B       {f_size:,} B")
    print("=" * 95)
    print(f"Çıktı Dizini: {outputs_base}\n")

if __name__ == '__main__':
    main()
