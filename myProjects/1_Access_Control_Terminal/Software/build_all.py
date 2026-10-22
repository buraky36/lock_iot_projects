#!/usr/bin/env python3
"""
Access Control Terminal - Multi-Environment Auto-Build & Packager
Compiles all 3 hardware environments (PIN+QR, PIN+RFID, RFID-Only) via PlatformIO
and packages both single-file merged binaries (0x0) and individual partition binaries.
"""

import os
import shutil
import subprocess
import sys

def main():
    root_dir = os.path.dirname(os.path.abspath(__file__))
    pio_exe = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\pio.exe"
    python_exe = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe"
    esptool_py = r"C:\Users\gentek-ideapad\.platformio\packages\tool-esptoolpy\esptool.py"

    if not os.path.exists(pio_exe):
        pio_exe = "pio"
    if not os.path.exists(python_exe):
        python_exe = sys.executable

    print("=" * 70)
    print("ACCESS CONTROL TERMINAL - TÜM VARYASYONLAR DERLENİYOR...")
    print("=" * 70)

    # 1. Run PlatformIO build for all environments
    cmd_build = [pio_exe, "run"]
    print("\n[1/3] PlatformIO ile tüm varyasyonlar derleniyor...")
    ret = subprocess.call(cmd_build, cwd=root_dir)
    if ret != 0:
        print("\n[HATA] PlatformIO derleme işlemi başarısız oldu!")
        sys.exit(1)

    print("\n[2/3] İkili dosyalar (Binaries) kopyalanıyor ve paketleniyor...")
    variants = [
        ('pin_qr', 'OE0110_PIN_QR', 'full_firmware_OE0110.bin'),
        ('pin_rfid', 'OE0111_PIN_RFID', 'full_firmware_OE0111.bin'),
        ('rfid_only', 'OE0112_RFID_ONLY', 'full_firmware_OE0112.bin')
    ]

    for env, folder, merged_name in variants:
        src_dir = os.path.join(root_dir, '.pio', 'build', env)
        dst_dir = os.path.join(root_dir, 'output', folder)
        os.makedirs(dst_dir, exist_ok=True)

        files = ['bootloader.bin', 'partitions.bin', 'ota_data_initial.bin', 'firmware.bin']
        for f in files:
            s = os.path.join(src_dir, f)
            d = os.path.join(dst_dir, f)
            shutil.copy2(s, d)

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
            print(f"  [OK] {folder} -> {merged_name} ({os.path.getsize(merged_path)} bytes)")
        else:
            print(f"  [HATA] {merged_name} birleştirilemedi: {res.stderr}")

    # Backward compatibility for root output/
    shutil.copy2(os.path.join(root_dir, 'output', 'OE0110_PIN_QR', 'full_firmware_OE0110.bin'), os.path.join(root_dir, 'output', 'full_firmware_OE0110.bin'))
    shutil.copy2(os.path.join(root_dir, 'output', 'OE0110_PIN_QR', 'bootloader.bin'), os.path.join(root_dir, 'output', 'bootloader.bin'))
    shutil.copy2(os.path.join(root_dir, 'output', 'OE0110_PIN_QR', 'partitions.bin'), os.path.join(root_dir, 'output', 'partitions.bin'))
    shutil.copy2(os.path.join(root_dir, 'output', 'OE0110_PIN_QR', 'firmware.bin'), os.path.join(root_dir, 'output', 'firmware.bin'))

    print("\n[3/3] TÜM ÇIKTILAR BAŞARIYLA OLUŞTURULDU!")
    print(f"Konum: {os.path.join(root_dir, 'output')}")

if __name__ == '__main__':
    main()
