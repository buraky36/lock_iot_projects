import sys
import os
import time
import subprocess
import serial

PORT = "COM11"
BAUD = 115200
ESPTOOL_PATH = r"C:\Users\gentek-ideapad\.platformio\packages\tool-esptoolpy\esptool.py"
PYTHON_PATH = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe"
BIN_PATH = os.path.join(os.path.dirname(__file__), "output", "OE0110_PIN_QR", "full_firmware_OE0110.bin")

def wait_for_bootloader(timeout_sec=30):
    print(f"\n[1/3] ESP32 Bootloader Bekleniyor (COM4)...")
    print("-" * 65)
    print(">> Kart otomatik bootloader moduna geçmezse:")
    print("   1. GPIO0 (ESP32 Pin 25 veya R5 direncinin bacağı) -> GND'ye dokundurun")
    print("   2. EN (J2 Pin 3) pinini anlık olarak GND'ye dokundurup bırakın (Reset)")
    print("   3. GPIO0'ı GND'den ayırın")
    print("-" * 65)
    
    start = time.time()
    dot_count = 0
    while time.time() - start < timeout_sec:
        # Check chip_id with 1 attempt and short timeout
        cmd = [
            PYTHON_PATH, ESPTOOL_PATH,
            "--chip", "esp32",
            "--port", PORT,
            "--baud", "115200",
            "--connect-attempts", "1",
            "chip_id"
        ]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if "Chip is ESP32" in proc.stdout:
            print("\n\n[OK] ESP32 Bootloader modunda tespit edildi!")
            for l in proc.stdout.splitlines():
                if "Chip is" in l or "MAC:" in l or "Features:" in l:
                    print("   " + l.strip())
            return True
        sys.stdout.write(".")
        sys.stdout.flush()
        dot_count += 1
        if dot_count % 40 == 0:
            sys.stdout.write("\n")
        time.sleep(0.4)
    print("\n[Zaman Aşımı] Cihaz bootloader moduna geçmedi.")
    return False

def flash_firmware():
    print(f"\n[2/3] Firmware Yükleniyor ({BIN_PATH})...")
    cmd = [
        PYTHON_PATH, ESPTOOL_PATH,
        "--chip", "esp32",
        "--port", PORT,
        "--baud", "460800",
        "--before", "no_reset",
        "--after", "hard_reset",
        "write_flash", "-z",
        "--flash_mode", "dio",
        "--flash_freq", "40m",
        "--flash_size", "4MB",
        "0x0", BIN_PATH
    ]
    ret = subprocess.call(cmd)
    return ret == 0

def run_monitor():
    print("\n" + "=" * 65)
    print("[3/3] CANLI DONANIM TEST VE LOG MONİTÖRÜ (115200 Baud)")
    print("=" * 65)
    print("Tuş takımına (TSM12) dokunun veya QR barkod okutun.")
    print("Çıkmak için: Ctrl + C\n")
    print("-" * 65)
    
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
        # Kartı resetle
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setDTR(True)
        ser.setRTS(False)
        time.sleep(0.1)
        ser.setDTR(False)
        ser.setRTS(False)

        while True:
            line = ser.readline()
            if line:
                try:
                    text = line.decode('utf-8', errors='replace').rstrip('\r\n')
                    if not text:
                        continue
                    if "WARNING" in text or "error" in text.lower():
                        print(f"[*] {text}")
                    elif any(k in text for k in ["TSM12", "XR1300", "QR", "Key", "OE0110", "Relay", "Buzzer", "BLE Name"]):
                        print(f"[+] {text}")
                    else:
                        print(f"    {text}")
                except Exception:
                    print(line)
    except KeyboardInterrupt:
        print("\nMonitör kapatıldı.")
    except Exception as e:
        print(f"\nSeri port hatası: {e}")

def main():
    print("=" * 65)
    print("  ACCESS CONTROL TERMINAL - OE0110 (QR + TUŞ TAKIMI) TEST  ")
    print("=" * 65)
    
    if not os.path.exists(BIN_PATH):
        print(f"[HATA] Dosya bulunamadı: {BIN_PATH}")
        return

    # Önce hızlı bir yükleme dene
    print("\nOtomatik yükleme deneniyor...")
    cmd_quick = [
        PYTHON_PATH, ESPTOOL_PATH,
        "--chip", "esp32",
        "--port", PORT,
        "--baud", "460800",
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash", "-z",
        "--flash_mode", "dio",
        "--flash_freq", "40m",
        "--flash_size", "4MB",
        "0x0", BIN_PATH
    ]
    res = subprocess.call(cmd_quick)
    if res == 0:
        print("\n[OK] Yazılım başarıyla yüklendi!")
        run_monitor()
        return

    # Otomatik başarısız olduysa, bekleme döngüsüne gir
    if wait_for_bootloader(timeout_sec=45):
        if flash_firmware():
            print("\n[OK] Yazılım başarıyla yüklendi!")
            run_monitor()
        else:
            print("\n[HATA] Yazma işlemi sırasında hata oluştu.")
    else:
        print("\nYükleme yapılamadı. Bağlantıları kontrol edip tekrar deneyin.")

if __name__ == "__main__":
    main()
