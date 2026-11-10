# -*- coding: utf-8 -*-
"""
ONLOI GEÇİŞ KONTROL TERMİNALİ
Firmware Studio — Derleme, Paketleme, Yazılım Yükleme (Flash) & Seri Monitör (v1.0)
"""
import sys
import os
import re
import time
import datetime
import html
import shutil
import subprocess
from typing import Dict, List, Optional, Any

# Ensure local packages and current dir are in path
if getattr(sys, 'frozen', False):
    APP_DIR = os.path.dirname(sys.executable)
    RESOURCE_DIR = getattr(sys, '_MEIPASS', APP_DIR)
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))
    RESOURCE_DIR = APP_DIR

SCRIPT_DIR = APP_DIR
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
SOFTWARE_DIR = os.path.join(PROJECT_ROOT, "Software")
OUTPUTS_DIR = os.path.join(SOFTWARE_DIR, "outputs")

# Tools paths with fallbacks
PIO_EXE = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\pio.exe"
PYTHON_EXE = r"C:\Users\gentek-ideapad\.platformio\penv\Scripts\python.exe"
ESPTOOL_PY = r"C:\Users\gentek-ideapad\.platformio\packages\tool-esptoolpy\esptool.py"

if not os.path.exists(PIO_EXE):
    PIO_EXE = "pio"
if not os.path.exists(PYTHON_EXE):
    PYTHON_EXE = sys.executable
if not os.path.exists(ESPTOOL_PY):
    ESPTOOL_PY = "esptool.py"

import serial
import serial.tools.list_ports
from PyQt5 import QtCore, QtGui, QtWidgets
from PyQt5.QtCore import Qt, pyqtSignal, pyqtSlot, QThread, QTimer

STUDIO_VERSION = "v1.1"

def parse_version_tuple(ver_str: str):
    nums = re.findall(r'\d+', ver_str)
    return tuple(map(int, nums)) if nums else (0,)

def detect_default_fw_version() -> str:
    """Dynamically detects the latest firmware version from Software/src/main.c or outputs/."""
    main_c = os.path.join(SOFTWARE_DIR, "src", "main.c")
    if os.path.exists(main_c):
        try:
            with open(main_c, "r", encoding="utf-8", errors="ignore") as f:
                content = f.read()
            m = re.search(r'#define\s+FW_VERSION_STR\s+"v?(\d+\.\d+\.\d+)"', content, re.IGNORECASE)
            if m:
                return f"V{m.group(1)}"
        except Exception:
            pass
    # Fallback: check outputs directory for highest version folder
    if os.path.exists(OUTPUTS_DIR):
        best_ver = None
        for root, dirs, _ in os.walk(OUTPUTS_DIR):
            for d in dirs:
                if re.match(r'^[vV]?\d+\.\d+\.\d+$', d):
                    if best_ver is None or parse_version_tuple(d) > parse_version_tuple(best_ver):
                        best_ver = d
        if best_ver:
            return best_ver.upper() if best_ver.lower().startswith('v') else f"V{best_ver}"
    return "V2.0.27"

DEFAULT_FW_VERSION = detect_default_fw_version()

def find_merged_bin_in_dir(dir_path: str, model_code: str) -> Optional[str]:
    """Finds merged binary in given version directory regardless of exact naming convention."""
    if not os.path.isdir(dir_path):
        return None
    files = os.listdir(dir_path)
    # 1. Match full_firmware containing model code (e.g. full_firmware_OE0111_PIN_RFID_V2.0.12.bin or full_firmware_OE0111.bin)
    for f in files:
        if f.startswith("full_firmware") and f.endswith(".bin") and model_code in f:
            return os.path.join(dir_path, f)
    # 2. Match any full_firmware*.bin
    for f in files:
        if f.startswith("full_firmware") and f.endswith(".bin"):
            return os.path.join(dir_path, f)
    # 3. Fallback to firmware.bin
    if "firmware.bin" in files:
        return os.path.join(dir_path, "firmware.bin")
    return None

def get_outputs_search_roots() -> List[str]:
    candidates = [
        OUTPUTS_DIR,
        os.path.join(APP_DIR, "outputs"),
        os.path.join(APP_DIR, "..", "outputs"),
        os.path.join(APP_DIR, "Software", "outputs"),
    ]
    roots = []
    for c in candidates:
        c_abs = os.path.abspath(c)
        if os.path.exists(c_abs) and c_abs not in roots:
            roots.append(c_abs)
            for sub in ['archieve', 'archive']:
                sub_path = os.path.join(c_abs, sub)
                if os.path.exists(sub_path) and sub_path not in roots:
                    roots.append(sub_path)
    return roots

# Variants definition matching Software/build_all.py
VARIANTS = {
    'all': {
        'title': 'Tüm Varyasyonlar (Build All: OE0111 + OE0112 + OE0113)',
        'env': 'all',
        'code': 'ALL',
        'folder': 'ALL'
    },
    'OE0111': {
        'title': 'OE0111 — PIN + RFID Kontrolcü',
        'env': 'pin_rfid',
        'folder': 'OE_0111_PIN_RFID',
        'code': 'OE0111',
        'product_id': '0x0111',
        'merged': 'full_firmware_OE0111.bin'
    },
    'OE0112': {
        'title': 'OE0112 — RFID ONLY Kontrolcü',
        'env': 'rfid_only',
        'folder': 'OE_0112_RFID_ONLY',
        'code': 'OE0112',
        'product_id': '0x0112',
        'merged': 'full_firmware_OE0112.bin'
    },
    'OE0113': {
        'title': 'OE0113 — PIN + QR Kontrolcü',
        'env': 'pin_qr',
        'folder': 'OE_0113_PIN_QR',
        'code': 'OE0113',
        'product_id': '0x0113',
        'merged': 'full_firmware_OE0113.bin'
    }
}

FLASH_GUIDE_TEMPLATE = """================================================================================
ACCESS CONTROL TERMINAL - FLASH BELLEK ADRES REHBERİ
MODEL: {title} ({product_id}) | YAZILIM VERSİYONU: {version}
================================================================================

1) TEK DOSYA İLE FLASH ETME (MERGED BINARY - ÖNERİLEN)
--------------------------------------------------------------------------------
Dosya: {merged_name}
Adres: 0x0000 (Flash başlangıç adresi)

Esptool Komutu:
esptool.py --chip esp32 --port COMx --baud 921600 write_flash 0x0000 {merged_name}

--------------------------------------------------------------------------------
2) PARÇALI DOSYALAR İLE FLASH ETME
--------------------------------------------------------------------------------
- 0x1000   : bootloader.bin
- 0x8000   : partitions.bin
- 0x109000 : ota_data_initial.bin
- 0x110000 : firmware.bin
================================================================================
"""

# =============================================================================
# WORKER THREAD: SERIAL MONITOR
# =============================================================================
class SerialWorker(QThread):
    line_received = pyqtSignal(str)
    connected = pyqtSignal(str)
    disconnected = pyqtSignal()
    error_occurred = pyqtSignal(str)

    def __init__(self, port: str, baudrate: int = 115200):
        super().__init__()
        self.port = port
        self.baudrate = baudrate
        self._running = True
        self._ser: Optional[serial.Serial] = None

    def run(self):
        try:
            self._ser = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self.connected.emit(self.port)
        except Exception as e:
            self.error_occurred.emit(f"Port açılamadı ({self.port}): {e}")
            return

        buf = bytearray()
        while self._running:
            try:
                if self._ser and self._ser.is_open:
                    data = self._ser.read(256)
                    if data:
                        buf.extend(data)
                        while b'\n' in buf:
                            idx = buf.index(b'\n')
                            line_bytes = buf[:idx]
                            buf = buf[idx + 1:]
                            line = line_bytes.decode('utf-8', errors='replace').rstrip('\r')
                            if line.strip():
                                self.line_received.emit(line)
                    else:
                        time.sleep(0.01)
                else:
                    break
            except Exception as e:
                if self._running:
                    self.error_occurred.emit(f"Seri port okuma hatası: {e}")
                break

        if self._ser and self._ser.is_open:
            try:
                self._ser.close()
            except Exception:
                pass
        self.disconnected.emit()

    def send_line(self, cmd: str):
        if self._ser and self._ser.is_open:
            try:
                self._ser.write((cmd.strip() + "\r\n").encode('utf-8'))
                self._ser.flush()
            except Exception as e:
                self.error_occurred.emit(f"Komut gönderilemedi: {e}")

    def reset_esp32(self):
        """Hardware reset via DTR/RTS lines for ESP32 auto-reset circuit."""
        if self._ser and self._ser.is_open:
            try:
                self._ser.dtr = False
                self._ser.rts = True
                time.sleep(0.1)
                self._ser.dtr = True
                self._ser.rts = False
                time.sleep(0.1)
                self._ser.dtr = False
            except Exception as e:
                self.error_occurred.emit(f"ESP32 donanımsal reset hatası: {e}")

    def stop(self):
        self._running = False
        self.wait(1500)


# =============================================================================
# WORKER THREAD: BUILD & MERGE
# =============================================================================
class BuildWorker(QThread):
    log_line = pyqtSignal(str, str) # text, color
    build_finished = pyqtSignal(bool, str) # success, message

    def __init__(self, target: str, version: str, clean_first: bool = False):
        super().__init__()
        self.target = target
        self.version = version
        self.clean_first = clean_first

    def run(self):
        self.log_line.emit("=" * 60, "#38BDF8")
        self.log_line.emit(f"🔨 DERLEME VE PAKETLEME BAŞLADI: {self.target} ({self.version})", "#38BDF8")
        self.log_line.emit("=" * 60, "#38BDF8")

        if not os.path.exists(SOFTWARE_DIR):
            self.log_line.emit(f"HATA: 'Software' dizini bulunamadı: {SOFTWARE_DIR}", "#EF4444")
            self.build_finished.emit(False, "Software klasörü bulunamadı!")
            return

        # 0. Sync version in Software/src/main.c if it differs from self.version
        main_c_path = os.path.join(SOFTWARE_DIR, "src", "main.c")
        nums = re.findall(r'\d+', self.version)
        if len(nums) >= 3 and os.path.exists(main_c_path):
            maj, min_, pat = nums[0], nums[1], nums[2]
            ver_str_lower = f"v{maj}.{min_}.{pat}"
            try:
                with open(main_c_path, "r", encoding="utf-8") as mf:
                    mc = mf.read()
                new_mc = re.sub(r'(#define\s+FW_VERSION_MAJOR\s+)\d+', rf'\g<1>{maj}', mc)
                new_mc = re.sub(r'(#define\s+FW_VERSION_MINOR\s+)\d+', rf'\g<1>{min_}', new_mc)
                new_mc = re.sub(r'(#define\s+FW_VERSION_PATCH\s+)\d+', rf'\g<1>{pat}', new_mc)
                new_mc = re.sub(r'(#define\s+FW_VERSION_STR\s+)"[^"]+"', rf'\g<1>"{ver_str_lower}"', new_mc)
                if new_mc != mc:
                    with open(main_c_path, "w", encoding="utf-8") as mf:
                        mf.write(new_mc)
                    self.log_line.emit(f"📝 main.c versiyon tanımı güncellendi: {ver_str_lower}", "#10B981")
            except Exception as e:
                self.log_line.emit(f"⚠️ main.c versiyonu güncellenemedi: {e}", "#F59E0B")

        # 1. Clean if requested
        if self.clean_first:
            self.log_line.emit("🧹 .pio derleme önbelleği temizleniyor...", "#F59E0B")
            cmd_clean = [PIO_EXE, "run", "--target", "clean"]
            self._run_cmd(cmd_clean, SOFTWARE_DIR)

        # 2. Determine target environments
        if self.target == 'all':
            targets = ['pin_rfid', 'rfid_only', 'pin_qr']
        elif self.target in VARIANTS:
            targets = [VARIANTS[self.target]['env']]
        else:
            targets = ['pin_rfid', 'rfid_only', 'pin_qr']

        # 3. Compile each target
        for env in targets:
            self.log_line.emit(f"\n🚀 PlatformIO Derleniyor: env:{env}...", "#38BDF8")
            cmd_build = [PIO_EXE, "run", "-e", env]
            success = self._run_cmd(cmd_build, SOFTWARE_DIR)
            if not success:
                self.log_line.emit(f"❌ Derleme başarısız oldu: {env}", "#EF4444")
                self.build_finished.emit(False, f"Derleme hatası: {env}")
                return

        # 4. Package & Merge Binaries
        self.log_line.emit(f"\n📦 İkili dosyalar 'outputs/' altına paketleniyor ve birleştiriliyor ({self.version})...", "#10B981")
        outputs_base = os.path.join(SOFTWARE_DIR, 'outputs')
        os.makedirs(outputs_base, exist_ok=True)

        variants_to_package = [v for k, v in VARIANTS.items() if k != 'all' and (self.target == 'all' or k == self.target)]

        for v in variants_to_package:
            env = v['env']
            folder = v['folder']
            merged_name = v['merged']
            title = v['title']
            prod_id = v['product_id']

            src_dir = os.path.join(SOFTWARE_DIR, '.pio', 'build', env)
            dst_dir = os.path.join(outputs_base, folder, self.version)
            os.makedirs(dst_dir, exist_ok=True)

            # Copy partition binaries
            bin_files = ['bootloader.bin', 'partitions.bin', 'ota_data_initial.bin', 'firmware.bin']
            for f in bin_files:
                s = os.path.join(src_dir, f)
                d = os.path.join(dst_dir, f)
                if os.path.exists(s):
                    shutil.copy2(s, d)
                else:
                    self.log_line.emit(f"  [UYARI] {f} bulunamadı: {s}", "#F59E0B")

            # Create merged binary at 0x0000
            merged_path = os.path.join(dst_dir, merged_name)
            cmd_merge = [
                PYTHON_EXE, ESPTOOL_PY,
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

            self.log_line.emit(f"  🔗 Birleştiriliyor: {merged_name} (0x0000)...", "#38BDF8")
            res = subprocess.run(cmd_merge, capture_output=True, text=True)
            if res.returncode == 0:
                fw_size = os.path.getsize(os.path.join(dst_dir, 'firmware.bin'))
                m_size = os.path.getsize(merged_path)
                self.log_line.emit(f"  ✔ [BAŞARILI] {folder}/{self.version}/ -> {merged_name} ({m_size:,} bytes | fw: {fw_size:,} bytes)", "#10B981")
            else:
                self.log_line.emit(f"  ❌ [HATA] {merged_name} birleştirilemedi: {res.stderr}", "#EF4444")
                self.build_finished.emit(False, f"Merge hatası: {merged_name}")
                return

            # Write FLASH_ADDRESSES.txt
            guide_content = FLASH_GUIDE_TEMPLATE.format(
                title=title,
                product_id=prod_id,
                version=self.version,
                merged_name=merged_name
            )
            with open(os.path.join(dst_dir, 'FLASH_ADDRESSES.txt'), 'w', encoding='utf-8') as gf:
                gf.write(guide_content)

        self.log_line.emit("\n" + "=" * 60, "#10B981")
        self.log_line.emit(f"🎉 TÜM İŞLEMLER BAŞARIYLA TAMAMLANDI! Çıktılar: outputs/", "#10B981")
        self.log_line.emit("=" * 60, "#10B981")
        self.build_finished.emit(True, "Derleme ve paketleme başarıyla tamamlandı!")

    def _run_cmd(self, cmd: List[str], cwd: str) -> bool:
        try:
            p = subprocess.Popen(
                cmd,
                cwd=cwd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True
            )
            for line in p.stdout:
                line_str = line.rstrip()
                if not line_str:
                    continue
                color = "#CBD5E1"
                if "FAILED" in line_str or "Error" in line_str or "error:" in line_str:
                    color = "#EF4444"
                elif "WARNING" in line_str or "warning:" in line_str:
                    color = "#F59E0B"
                elif "SUCCESS" in line_str or "Compiling" in line_str or "Linking" in line_str:
                    color = "#38BDF8"
                self.log_line.emit(line_str, color)

            p.wait()
            return p.returncode == 0
        except Exception as e:
            self.log_line.emit(f"Komut çalıştırma hatası: {e}", "#EF4444")
            return False


# =============================================================================
# WORKER THREAD: FLASH UPLOADER & ERASER
# =============================================================================
class FlashWorker(QThread):
    log_line = pyqtSignal(str, str) # text, color
    progress = pyqtSignal(int)      # percentage 0-100
    flash_finished = pyqtSignal(bool, str) # success, message

    def __init__(self, port: str, baud: int, bin_path: str, address: str = "0x0000", is_erase: bool = False):
        super().__init__()
        self.port = port
        self.baud = baud
        self.bin_path = bin_path
        self.address = address
        self.is_erase = is_erase

    def run(self):
        if self.is_erase:
            self.log_line.emit(f"🧹 ESP32 Çip Hafızası Siliniyor... Port: {self.port} Baud: {self.baud}", "#F59E0B")
            cmd = [
                PYTHON_EXE, ESPTOOL_PY,
                '--chip', 'esp32',
                '--port', self.port,
                '--baud', str(self.baud),
                'erase_flash'
            ]
        else:
            if not os.path.exists(self.bin_path):
                self.log_line.emit(f"HATA: Binary dosyası bulunamadı: {self.bin_path}", "#EF4444")
                self.flash_finished.emit(False, "Seçilen .bin dosyası bulunamadı!")
                return

            size_bytes = os.path.getsize(self.bin_path)
            self.log_line.emit(f"⚡ FLASH YAZMA BAŞLATILDI: {os.path.basename(self.bin_path)} ({size_bytes:,} B) -> {self.address}", "#38BDF8")
            self.log_line.emit(f"Bağlantı: {self.port} | Hız: {self.baud} bps", "#38BDF8")
            
            cmd = [
                PYTHON_EXE, ESPTOOL_PY,
                '--chip', 'esp32',
                '--port', self.port,
                '--baud', str(self.baud),
                '--before', 'default_reset',
                '--after', 'hard_reset',
                'write_flash',
                '-z',
                '--flash_mode', 'dio',
                '--flash_freq', '40m',
                '--flash_size', '4MB',
                self.address, self.bin_path
            ]

        self.progress.emit(5)
        try:
            p = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                universal_newlines=True
            )

            for line in p.stdout:
                line_str = line.rstrip()
                if not line_str:
                    continue

                color = "#CBD5E1"
                # Parse progress percentage e.g. "Writing at 0x00010000... (25 %)"
                match = re.search(r'\((\d+)\s*%\)', line_str)
                if match:
                    pct = int(match.group(1))
                    self.progress.emit(pct)

                if "Connecting" in line_str:
                    color = "#F59E0B"
                elif "Writing" in line_str or "Compressed" in line_str:
                    color = "#38BDF8"
                elif "Hash of data verified" in line_str or "Leaving" in line_str:
                    color = "#10B981"
                elif "A fatal error" in line_str or "Error" in line_str:
                    color = "#EF4444"

                self.log_line.emit(line_str, color)

            p.wait()
            if p.returncode == 0:
                self.progress.emit(100)
                msg = "Çip başarıyla silindi!" if self.is_erase else "Yazılım cihaza başarıyla yüklendi! (Flash PASS)"
                self.log_line.emit(f"\n✔ {msg}", "#10B981")
                self.flash_finished.emit(True, msg)
            else:
                self.log_line.emit(f"\n❌ İşlem başarısız oldu (Hata Kodu: {p.returncode})", "#EF4444")
                self.flash_finished.emit(False, f"Esptool hatası: Hata Kodu {p.returncode}")
        except Exception as e:
            self.log_line.emit(f"Flash çalıştırma hatası: {e}", "#EF4444")
            self.flash_finished.emit(False, str(e))


# =============================================================================
# MODERN DARK THEME STYLESHEET
# =============================================================================
STUDIO_STYLE = """
QMainWindow, QDialog {
    background-color: #0F172A;
    color: #F8FAFC;
    font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
}
QWidget {
    font-size: 13px;
    color: #E2E8F0;
}
QGroupBox {
    border: 1px solid #334155;
    border-radius: 8px;
    margin-top: 14px;
    padding-top: 12px;
    font-weight: bold;
    color: #94A3B8;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: #38BDF8;
}
QTabWidget::pane {
    border: 1px solid #334155;
    border-radius: 8px;
    background-color: #1E293B;
    top: -1px;
}
QTabBar::tab {
    background: #0F172A;
    border: 1px solid #334155;
    border-bottom-color: #334155;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
    padding: 8px 16px;
    margin-right: 4px;
    font-weight: 600;
    color: #94A3B8;
}
QTabBar::tab:selected {
    background: #1E293B;
    border-color: #334155;
    border-bottom-color: #1E293B;
    color: #38BDF8;
}
QTabBar::tab:hover:!selected {
    background: #1E293B;
    color: #F1F5F9;
}
QPushButton {
    background-color: #1E293B;
    border: 1px solid #475569;
    border-radius: 6px;
    padding: 7px 14px;
    font-weight: 600;
    color: #F1F5F9;
}
QPushButton:hover {
    background-color: #334155;
    border-color: #64748B;
}
QPushButton:pressed {
    background-color: #0F172A;
}
QPushButton:disabled {
    background-color: #1E293B;
    border-color: #334155;
    color: #64748B;
}
QPushButton#btnPrimary {
    background-color: #0284C7;
    border-color: #0369A1;
    color: white;
}
QPushButton#btnPrimary:hover {
    background-color: #0369A1;
}
QPushButton#btnSuccess {
    background-color: #16A34A;
    border-color: #15803D;
    color: white;
}
QPushButton#btnSuccess:hover {
    background-color: #15803D;
}
QPushButton#btnDanger {
    background-color: #DC2626;
    border-color: #B91C1C;
    color: white;
}
QPushButton#btnDanger:hover {
    background-color: #B91C1C;
}
QComboBox, QLineEdit {
    background-color: #0F172A;
    border: 1px solid #475569;
    border-radius: 6px;
    padding: 6px 10px;
    color: #F8FAFC;
    font-weight: 500;
}
QComboBox:focus, QLineEdit:focus {
    border-color: #38BDF8;
}
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 24px;
    border-left: 1px solid #334155;
    border-top-right-radius: 6px;
    border-bottom-right-radius: 6px;
    background-color: #1E293B;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #94A3B8;
    width: 0;
    height: 0;
}
QComboBox::down-arrow:hover {
    border-top: 5px solid #38BDF8;
}
QComboBox QAbstractItemView {
    background-color: #0F172A;
    border: 1px solid #38BDF8;
    border-radius: 6px;
    color: #F8FAFC;
    selection-background-color: #0284C7;
    selection-color: #FFFFFF;
    outline: none;
    padding: 4px;
}
QComboBox QAbstractItemView::item {
    min-height: 26px;
    padding: 6px 10px;
    color: #F8FAFC;
    border-radius: 4px;
}
QComboBox QAbstractItemView::item:hover {
    background-color: #334155;
    color: #38BDF8;
}
QComboBox QAbstractItemView::item:selected {
    background-color: #0284C7;
    color: #FFFFFF;
    font-weight: bold;
}
QPlainTextEdit, QTextEdit {
    background-color: #0B1120;
    border: 1px solid #1E293B;
    border-radius: 6px;
    font-family: 'Consolas', 'Courier New', monospace;
    font-size: 12px;
    color: #CBD5E1;
}
QProgressBar {
    border: 1px solid #334155;
    border-radius: 6px;
    text-align: center;
    background-color: #0F172A;
    color: white;
    font-weight: bold;
    height: 18px;
}
QProgressBar::chunk {
    background-color: #0284C7;
    border-radius: 5px;
}
"""


# =============================================================================
# MAIN WINDOW: ONLOI FIRMWARE STUDIO
# =============================================================================
class OnloiFirmwareStudio(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"Onloi Firmware Studio {STUDIO_VERSION} — Derleme, Yükleme & Seri Monitör")
        self.resize(1280, 800)
        self.setMinimumSize(1050, 680)

        self.serial_worker: Optional[SerialWorker] = None
        self.build_worker: Optional[BuildWorker] = None
        self.flash_worker: Optional[FlashWorker] = None
        self._was_monitoring_before_flash = False
        self.show_old_versions = False

        self.init_ui()
        self.refresh_ports()
        self.refresh_available_binaries()

    def init_ui(self):
        # Set application icon
        icon_path = os.path.join(RESOURCE_DIR, "studio_icon.ico")
        if not os.path.exists(icon_path):
            icon_path = os.path.join(APP_DIR, "studio_icon.ico")
        if os.path.exists(icon_path):
            self.setWindowIcon(QtGui.QIcon(icon_path))

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        main_layout = QtWidgets.QVBoxLayout(central)
        main_layout.setContentsMargins(12, 10, 12, 10)
        main_layout.setSpacing(10)

        # ---------------------------------------------------------------------
        # 1. TOP BAR: Global Connection, Port, Reset & Quick Actions
        # ---------------------------------------------------------------------
        top_bar = QtWidgets.QFrame()
        top_bar.setFixedHeight(54)
        top_bar.setStyleSheet("background-color: #1E293B; border-radius: 8px; padding: 4px;")
        top_layout = QtWidgets.QHBoxLayout(top_bar)
        top_layout.setContentsMargins(10, 4, 10, 4)
        top_layout.setSpacing(10)

        # Port Selector
        top_layout.addWidget(QtWidgets.QLabel("<b>Port:</b>"))
        self.combo_ports = QtWidgets.QComboBox()
        self.combo_ports.setMinimumWidth(140)
        top_layout.addWidget(self.combo_ports)

        btn_refresh = QtWidgets.QPushButton("🔄")
        btn_refresh.setToolTip("Portları Yenile")
        btn_refresh.setFixedWidth(36)
        btn_refresh.clicked.connect(self.refresh_ports)
        top_layout.addWidget(btn_refresh)

        # Serial Monitor Baudrate
        top_layout.addWidget(QtWidgets.QLabel("Baud:"))
        self.combo_mon_baud = QtWidgets.QComboBox()
        self.combo_mon_baud.addItems(["115200", "921600", "460800", "57600", "9600"])
        self.combo_mon_baud.setFixedWidth(90)
        top_layout.addWidget(self.combo_mon_baud)

        # Serial Connect Button
        self.btn_connect = QtWidgets.QPushButton("🔌 Monitörü Aç")
        self.btn_connect.setObjectName("btnPrimary")
        self.btn_connect.setMinimumWidth(130)
        self.btn_connect.clicked.connect(self.toggle_serial_connection)
        top_layout.addWidget(self.btn_connect)

        # ESP32 Reset Button
        self.btn_reset_esp = QtWidgets.QPushButton("⚡ ESP32 Reset")
        self.btn_reset_esp.setToolTip("ESP32 çipini DTR/RTS üzerinden donanımsal resetler")
        self.btn_reset_esp.clicked.connect(self.reset_esp32_device)
        top_layout.addWidget(self.btn_reset_esp)

        top_layout.addStretch()

        # Quick Links
        btn_open_outputs = QtWidgets.QPushButton("📁 outputs/ Klasörü")
        btn_open_outputs.clicked.connect(self.open_outputs_folder)
        top_layout.addWidget(btn_open_outputs)

        main_layout.addWidget(top_bar)

        # ---------------------------------------------------------------------
        # 2. MAIN BODY SPLITTER: Left (Build & Flash Controls) | Right (Consoles)
        # ---------------------------------------------------------------------
        splitter = QtWidgets.QSplitter(Qt.Horizontal)
        splitter.setHandleWidth(6)

        # --- LEFT PANEL: TABS FOR BUILDER & FLASHER ---
        left_widget = QtWidgets.QWidget()
        left_layout = QtWidgets.QVBoxLayout(left_widget)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(8)

        self.tabs_tools = QtWidgets.QTabWidget()

        # TAB 1: 🔨 DERLEME & PAKETLEME (BUILDER)
        tab_build = QtWidgets.QWidget()
        build_layout = QtWidgets.QVBoxLayout(tab_build)
        build_layout.setContentsMargins(10, 10, 10, 10)
        build_layout.setSpacing(10)

        grp_target = QtWidgets.QGroupBox("HEDEF MODEL VE DERLEME AYARLARI")
        grp_target_layout = QtWidgets.QVBoxLayout(grp_target)
        grp_target_layout.setSpacing(8)

        grp_target_layout.addWidget(QtWidgets.QLabel("<b>Derlenecek Model / Çevre:</b>"))
        self.combo_build_target = QtWidgets.QComboBox()
        for k, v in VARIANTS.items():
            self.combo_build_target.addItem(v['title'], k)
        grp_target_layout.addWidget(self.combo_build_target)

        # Version line
        ver_box = QtWidgets.QHBoxLayout()
        ver_box.addWidget(QtWidgets.QLabel("<b>Yazılım Versiyonu:</b>"))
        self.txt_version = QtWidgets.QLineEdit(DEFAULT_FW_VERSION)
        ver_box.addWidget(self.txt_version)
        grp_target_layout.addLayout(ver_box)

        self.chk_clean = QtWidgets.QCheckBox("Derlemeden önce önbelleği temizle (pio clean)")
        self.chk_clean.setChecked(False)
        grp_target_layout.addWidget(self.chk_clean)

        build_layout.addWidget(grp_target)

        # Build Action Buttons
        self.btn_start_build = QtWidgets.QPushButton("🔨 Seçiliyi Derle & Paketle (Merge 0x0000)")
        self.btn_start_build.setObjectName("btnPrimary")
        self.btn_start_build.setMinimumHeight(42)
        self.btn_start_build.setStyleSheet("font-size: 13.5px; font-weight: bold;")
        self.btn_start_build.clicked.connect(self.start_build_process)
        build_layout.addWidget(self.btn_start_build)

        # Summary box
        grp_summary = QtWidgets.QGroupBox("HAZIR DERLENMİŞ ÇIKTILAR (outputs/)")
        sum_layout = QtWidgets.QVBoxLayout(grp_summary)
        self.lbl_build_info = QtWidgets.QLabel("Henüz taranmadı...")
        self.lbl_build_info.setStyleSheet("color: #94A3B8; font-size: 12px; line-height: 1.4;")
        self.lbl_build_info.setWordWrap(True)
        sum_layout.addWidget(self.lbl_build_info)
        build_layout.addWidget(grp_summary)

        build_layout.addStretch()
        self.tabs_tools.addTab(tab_build, "🔨 Derleme (Build)")

        # TAB 2: ⚡ YAZILIM YÜKLEME (FLASHER)
        tab_flash = QtWidgets.QWidget()
        flash_layout = QtWidgets.QVBoxLayout(tab_flash)
        flash_layout.setContentsMargins(10, 10, 10, 10)
        flash_layout.setSpacing(10)

        grp_flash_src = QtWidgets.QGroupBox("YÜKLENECEK YAZILIM SEÇİMİ")
        flash_src_layout = QtWidgets.QVBoxLayout(grp_flash_src)
        flash_src_layout.setSpacing(8)

        self.radio_preset_bin = QtWidgets.QRadioButton("Hazır Model Çıktısından Yükle (Merged 0x0000)")
        self.radio_preset_bin.setChecked(True)
        self.radio_preset_bin.toggled.connect(self._on_flash_mode_toggled)
        flash_src_layout.addWidget(self.radio_preset_bin)

        self.preset_container = QtWidgets.QWidget()
        preset_layout = QtWidgets.QVBoxLayout(self.preset_container)
        preset_layout.setContentsMargins(14, 2, 0, 4)
        preset_layout.setSpacing(6)

        # 1. Model Selector Row
        model_row = QtWidgets.QHBoxLayout()
        lbl_model = QtWidgets.QLabel("<b>1. Model Seçimi:</b>")
        lbl_model.setFixedWidth(130)
        model_row.addWidget(lbl_model)
        self.combo_flash_model = QtWidgets.QComboBox()
        self.combo_flash_model.addItem("OE0111 (PIN + RFID Kontrolcü)", "OE0111")
        self.combo_flash_model.addItem("OE0112 (RFID ONLY Kontrolcü)", "OE0112")
        self.combo_flash_model.addItem("OE0113 (PIN + QR Kontrolcü)", "OE0113")
        self.combo_flash_model.currentIndexChanged.connect(self._on_flash_model_changed)
        model_row.addWidget(self.combo_flash_model)
        preset_layout.addLayout(model_row)

        # 2. Version Selector Row
        ver_row = QtWidgets.QHBoxLayout()
        lbl_ver = QtWidgets.QLabel("<b>2. Yazılım Sürümü:</b>")
        lbl_ver.setFixedWidth(130)
        ver_row.addWidget(lbl_ver)
        self.combo_flash_version = QtWidgets.QComboBox()
        self.combo_flash_version.currentIndexChanged.connect(self._on_flash_version_changed)
        ver_row.addWidget(self.combo_flash_version)
        preset_layout.addLayout(ver_row)

        # Selected Binary Info Label
        self.lbl_selected_bin_info = QtWidgets.QLabel("")
        self.lbl_selected_bin_info.setStyleSheet("color: #38BDF8; font-size: 11.5px; padding: 2px 0;")
        preset_layout.addWidget(self.lbl_selected_bin_info)

        btn_refresh_bins = QtWidgets.QPushButton("🔄 Çıktı Listesini Yenile")
        btn_refresh_bins.clicked.connect(self.refresh_available_binaries)
        preset_layout.addWidget(btn_refresh_bins)

        flash_src_layout.addWidget(self.preset_container)

        line_sep = QtWidgets.QFrame()
        line_sep.setFrameShape(QtWidgets.QFrame.HLine)
        line_sep.setStyleSheet("color: #334155;")
        flash_src_layout.addWidget(line_sep)

        self.radio_custom_bin = QtWidgets.QRadioButton("Özel .bin Dosyası Seç")
        self.radio_custom_bin.toggled.connect(self._on_flash_mode_toggled)
        flash_src_layout.addWidget(self.radio_custom_bin)

        custom_file_box = QtWidgets.QHBoxLayout()
        self.txt_custom_file = QtWidgets.QLineEdit()
        self.txt_custom_file.setPlaceholderText("C:\\...\\firmware.bin")
        self.txt_custom_file.setEnabled(False)
        custom_file_box.addWidget(self.txt_custom_file)

        self.btn_browse = QtWidgets.QPushButton("Gözat...")
        self.btn_browse.setEnabled(False)
        self.btn_browse.clicked.connect(self._browse_custom_binary)
        custom_file_box.addWidget(self.btn_browse)
        flash_src_layout.addLayout(custom_file_box)

        addr_box = QtWidgets.QHBoxLayout()
        addr_box.addWidget(QtWidgets.QLabel("Flash Adresi:"))
        self.txt_flash_addr = QtWidgets.QLineEdit("0x0000")
        self.txt_flash_addr.setFixedWidth(100)
        addr_box.addWidget(self.txt_flash_addr)
        addr_box.addStretch()
        flash_src_layout.addLayout(addr_box)

        flash_layout.addWidget(grp_flash_src)

        # Flasher Settings
        grp_flash_opt = QtWidgets.QGroupBox("YÜKLEME AYARLARI")
        flash_opt_layout = QtWidgets.QVBoxLayout(grp_flash_opt)

        baud_box = QtWidgets.QHBoxLayout()
        baud_box.addWidget(QtWidgets.QLabel("Yükleme Hızı (Flash Baud):"))
        self.combo_flash_baud = QtWidgets.QComboBox()
        self.combo_flash_baud.addItems(["921600 (Hızlı - Önerilen)", "460800", "115200 (Güvenli)"])
        baud_box.addWidget(self.combo_flash_baud)
        flash_opt_layout.addLayout(baud_box)

        self.chk_auto_monitor = QtWidgets.QCheckBox("Flash bitince Seri Monitör'ü otomatik aç")
        self.chk_auto_monitor.setChecked(True)
        flash_opt_layout.addWidget(self.chk_auto_monitor)

        flash_layout.addWidget(grp_flash_opt)

        # Flashing Progress Bar
        self.bar_flash = QtWidgets.QProgressBar()
        self.bar_flash.setValue(0)
        self.bar_flash.setFormat("Yükleme Durumu: %p%")
        flash_layout.addWidget(self.bar_flash)

        # Action Buttons
        self.btn_start_flash = QtWidgets.QPushButton("⚡ Yazılımı Cihaza Yükle (Write Flash)")
        self.btn_start_flash.setObjectName("btnSuccess")
        self.btn_start_flash.setMinimumHeight(44)
        self.btn_start_flash.setStyleSheet("font-size: 14px; font-weight: bold;")
        self.btn_start_flash.clicked.connect(self.start_flash_process)
        flash_layout.addWidget(self.btn_start_flash)

        self.btn_erase_flash = QtWidgets.QPushButton("🧹 Çipi Tamamen Sil (Erase Flash)")
        self.btn_erase_flash.setObjectName("btnDanger")
        self.btn_erase_flash.clicked.connect(self.start_erase_process)
        flash_layout.addWidget(self.btn_erase_flash)

        flash_layout.addStretch()
        self.tabs_tools.addTab(tab_flash, "⚡ Yazılım Yükleme (Flash)")

        left_layout.addWidget(self.tabs_tools)
        splitter.addWidget(left_widget)
        splitter.setStretchFactor(0, 36)

        # --- RIGHT PANEL: DUAL TABS CONSOLE (SERIAL MONITOR & BUILD/FLASH LOGS) ---
        right_widget = QtWidgets.QWidget()
        right_layout = QtWidgets.QVBoxLayout(right_widget)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(6)

        self.tabs_console = QtWidgets.QTabWidget()

        # TAB A: 📟 SERİ PORT MONİTÖRÜ
        tab_serial = QtWidgets.QWidget()
        serial_tab_layout = QtWidgets.QVBoxLayout(tab_serial)
        serial_tab_layout.setContentsMargins(8, 8, 8, 8)
        serial_tab_layout.setSpacing(6)

        # Top Bar for Serial
        ser_ctrl = QtWidgets.QHBoxLayout()
        ser_ctrl.addWidget(QtWidgets.QLabel("<b>Canlı Seri Port Akışı (UART)</b>"))
        ser_ctrl.addStretch()

        self.chk_autoscroll = QtWidgets.QCheckBox("Oto-Kaydır")
        self.chk_autoscroll.setChecked(True)
        ser_ctrl.addWidget(self.chk_autoscroll)

        btn_clear_ser = QtWidgets.QPushButton("Temizle")
        btn_clear_ser.setMinimumWidth(85)
        btn_clear_ser.setMaximumWidth(100)
        btn_clear_ser.clicked.connect(lambda: self.console_serial.clear())
        ser_ctrl.addWidget(btn_clear_ser)

        btn_save_log = QtWidgets.QPushButton("Log Kaydet")
        btn_save_log.setMinimumWidth(95)
        btn_save_log.clicked.connect(self.save_serial_log)
        ser_ctrl.addWidget(btn_save_log)

        serial_tab_layout.addLayout(ser_ctrl)

        # Serial Text Console
        self.console_serial = QtWidgets.QPlainTextEdit()
        self.console_serial.setReadOnly(True)
        self.console_serial.setMaximumBlockCount(5000)
        serial_tab_layout.addWidget(self.console_serial)

        # Quick CLI Buttons
        cli_box = QtWidgets.QHBoxLayout()
        cli_box.addWidget(QtWidgets.QLabel("<span style='color:#38BDF8;'><b>Hızlı CLI:</b></span>"))

        btn_cli_info = QtWidgets.QPushButton("OTA Info")
        btn_cli_info.clicked.connect(lambda: self.send_serial_cmd("onloi_ota_info"))
        cli_box.addWidget(btn_cli_info)

        btn_cli_users = QtWidgets.QPushButton("Kullanıcılar")
        btn_cli_users.clicked.connect(lambda: self.send_serial_cmd("onloi_user_list_view"))
        cli_box.addWidget(btn_cli_users)

        btn_cli_masters = QtWidgets.QPushButton("Masterlar")
        btn_cli_masters.clicked.connect(lambda: self.send_serial_cmd("onloi_master_list_view"))
        cli_box.addWidget(btn_cli_masters)

        btn_cli_osdp = QtWidgets.QPushButton("OSDP Info")
        btn_cli_osdp.clicked.connect(lambda: self.send_serial_cmd("onloi_osdp_info"))
        cli_box.addWidget(btn_cli_osdp)

        cli_box.addStretch()
        serial_tab_layout.addLayout(cli_box)

        # Serial Command Input
        cmd_box = QtWidgets.QHBoxLayout()
        self.txt_serial_cmd = QtWidgets.QLineEdit()
        self.txt_serial_cmd.setPlaceholderText("Seri port komutu yazın ve Enter'a basın...")
        self.txt_serial_cmd.returnPressed.connect(self._on_send_cmd_pressed)
        cmd_box.addWidget(self.txt_serial_cmd)

        btn_send = QtWidgets.QPushButton("Gönder")
        btn_send.clicked.connect(self._on_send_cmd_pressed)
        cmd_box.addWidget(btn_send)
        serial_tab_layout.addLayout(cmd_box)

        self.tabs_console.addTab(tab_serial, "📟 Canlı Seri Monitör")

        # TAB B: 🛠️ DERLEME VE FLASH İŞLEM LOGLARI
        tab_process = QtWidgets.QWidget()
        proc_layout = QtWidgets.QVBoxLayout(tab_process)
        proc_layout.setContentsMargins(8, 8, 8, 8)
        proc_layout.setSpacing(6)

        proc_top = QtWidgets.QHBoxLayout()
        proc_top.addWidget(QtWidgets.QLabel("<b>PlatformIO / Esptool Terminal Çıktısı</b>"))
        proc_top.addStretch()
        btn_clear_proc = QtWidgets.QPushButton("Temizle")
        btn_clear_proc.setMinimumWidth(85)
        btn_clear_proc.setMaximumWidth(100)
        btn_clear_proc.clicked.connect(lambda: self.console_process.clear())
        proc_top.addWidget(btn_clear_proc)
        proc_layout.addLayout(proc_top)

        self.console_process = QtWidgets.QPlainTextEdit()
        self.console_process.setReadOnly(True)
        self.console_process.setMaximumBlockCount(5000)
        proc_layout.addWidget(self.console_process)

        self.tabs_console.addTab(tab_process, "🛠️ Derleme & Flash Logları")

        right_layout.addWidget(self.tabs_console)
        splitter.addWidget(right_widget)
        splitter.setStretchFactor(1, 64)

        main_layout.addWidget(splitter, 1)

        # ---------------------------------------------------------------------
        # 3. STATUS BAR
        # ---------------------------------------------------------------------
        self.status_bar = QtWidgets.QStatusBar()
        self.setStatusBar(self.status_bar)
        self.lbl_status = QtWidgets.QLabel("Hazır. Cihaz bağlayabilir, derleme veya yükleme başlatabilirsiniz.")
        self.status_bar.addWidget(self.lbl_status)

        # Force custom styled QListView popup on all combo boxes
        for cb in self.findChildren(QtWidgets.QComboBox):
            cb.setView(QtWidgets.QListView())

    # -------------------------------------------------------------------------
    # PORT MANAGEMENT
    # -------------------------------------------------------------------------
    def refresh_ports(self):
        curr = self.combo_ports.currentData()
        self.combo_ports.clear()
        ports = serial.tools.list_ports.comports()
        found_idx = 0
        for i, p in enumerate(ports):
            desc = p.description if p.description else ""
            self.combo_ports.addItem(f"{p.device} ({desc})", p.device)
            if curr and p.device == curr:
                found_idx = i

        if not ports:
            self.combo_ports.addItem("Port Bulunamadı", "")
        else:
            self.combo_ports.setCurrentIndex(found_idx)

    def toggle_serial_connection(self):
        if self.serial_worker and self.serial_worker.isRunning():
            self.stop_serial_monitor()
        else:
            self.start_serial_monitor()

    def start_serial_monitor(self) -> bool:
        port = self.combo_ports.currentData()
        if not port:
            QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen geçerli bir COM port seçin!")
            return False

        try:
            baud = int(self.combo_mon_baud.currentText())
        except ValueError:
            baud = 115200

        self.serial_worker = SerialWorker(port, baud)
        self.serial_worker.line_received.connect(self._on_serial_line)
        self.serial_worker.connected.connect(self._on_serial_connected)
        self.serial_worker.disconnected.connect(self._on_serial_disconnected)
        self.serial_worker.error_occurred.connect(self._on_serial_error)
        self.serial_worker.start()
        return True

    def stop_serial_monitor(self):
        if self.serial_worker and self.serial_worker.isRunning():
            self.serial_worker.stop()
            self.serial_worker = None

    def _on_serial_connected(self, port: str):
        self.btn_connect.setText("❌ Bağlantıyı Kes")
        self.btn_connect.setStyleSheet("background-color: #B91C1C; border-color: #991B1B; color: white;")
        self.lbl_status.setText(f"Seri Monitör Bağlandı: {port} ({self.combo_mon_baud.currentText()} bps)")
        self.append_serial_console(f"=== BAĞLANDI: {port} ({self.combo_mon_baud.currentText()} bps) ===", "#38BDF8")
        self.tabs_console.setCurrentIndex(0)

    def _on_serial_disconnected(self):
        self.btn_connect.setText("🔌 Monitörü Aç")
        self.btn_connect.setObjectName("btnPrimary")
        self.btn_connect.setStyleSheet("")
        self.lbl_status.setText("Seri Monitör Kapalı.")
        self.append_serial_console("=== BAĞLANTI KAPATILDI ===", "#94A3B8")

    def _on_serial_error(self, err_msg: str):
        self.append_serial_console(f"[HATA] {err_msg}", "#EF4444")
        self.lbl_status.setText(err_msg)

    def reset_esp32_device(self):
        if self.serial_worker and self.serial_worker.isRunning():
            self.append_serial_console("⚡ Donanımsal ESP32 reset gönderiliyor (DTR/RTS)...", "#F59E0B")
            self.serial_worker.reset_esp32()
        else:
            port = self.combo_ports.currentData()
            if not port:
                QtWidgets.QMessageBox.warning(self, "Uyarı", "Lütfen önce bir COM port seçin!")
                return
            try:
                ser = serial.Serial(port)
                ser.dtr = False
                ser.rts = True
                time.sleep(0.1)
                ser.dtr = True
                ser.rts = False
                time.sleep(0.1)
                ser.dtr = False
                ser.close()
                self.append_serial_console(f"⚡ {port} üzerindeki ESP32 donanımsal olarak resetlendi.", "#10B981")
            except Exception as e:
                QtWidgets.QMessageBox.warning(self, "Hata", f"Reset başarısız: {e}")

    def send_serial_cmd(self, cmd: str):
        if self.serial_worker and self.serial_worker.isRunning():
            self.append_serial_console(f">> {cmd}", "#A855F7")
            self.serial_worker.send_line(cmd)
        else:
            QtWidgets.QMessageBox.warning(self, "Uyarı", "Seri monitör açık değil! Lütfen önce 'Monitörü Aç' butonuna basın.")

    def _on_send_cmd_pressed(self):
        cmd = self.txt_serial_cmd.text().strip()
        if cmd:
            self.send_serial_cmd(cmd)
            self.txt_serial_cmd.clear()

    # -------------------------------------------------------------------------
    # CONSOLE LOGGERS
    # -------------------------------------------------------------------------
    def append_serial_console(self, text: str, color: Optional[str] = None):
        escaped = html.escape(text)
        if not color:
            if "E (" in text or "ERROR" in text or "FAIL" in text:
                color = "#EF4444"
            elif "W (" in text or "WARN" in text:
                color = "#F59E0B"
            elif "I (" in text or "SAVED" in text or "Granted" in text:
                color = "#10B981"
            elif "[CLI]" in text or "---" in text:
                color = "#38BDF8"
            else:
                color = "#CBD5E1"

        self.console_serial.appendHtml(f"<span style='color: {color};'>{escaped}</span>")
        if self.chk_autoscroll.isChecked():
            self.console_serial.moveCursor(QtGui.QTextCursor.End)

    def append_process_console(self, text: str, color: str = "#CBD5E1"):
        escaped = html.escape(text)
        self.console_process.appendHtml(f"<span style='color: {color};'>{escaped}</span>")
        self.console_process.moveCursor(QtGui.QTextCursor.End)

    def _on_serial_line(self, line: str):
        self.append_serial_console(line)

    def save_serial_log(self):
        text = self.console_serial.toPlainText()
        if not text:
            QtWidgets.QMessageBox.information(self, "Bilgi", "Kaydedilecek log bulunamadı.")
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Log Dosyasını Kaydet",
            f"Onloi_Serial_Log_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.txt",
            "Metin Dosyaları (*.txt)"
        )
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            self.lbl_status.setText(f"Log kaydedildi: {path}")

    # -------------------------------------------------------------------------
    # BINARY SCANNER & OUTPUTS
    # -------------------------------------------------------------------------
    def _find_model_binaries(self, model_key: str) -> List[Any]:
        """Returns sorted list of (version, bin_path) for given model from outputs and archive."""
        if not model_key or model_key not in VARIANTS:
            return []
        v = VARIANTS[model_key]
        folder = v['folder']
        code = v['code']

        search_roots = get_outputs_search_roots()

        found_dict = {}
        for base in search_roots:
            fpath = os.path.join(base, folder)
            if os.path.exists(fpath):
                for d in os.listdir(fpath):
                    bin_file = find_merged_bin_in_dir(os.path.join(fpath, d), code)
                    if bin_file and d not in found_dict:
                        found_dict[d] = bin_file

        sorted_vers = sorted(found_dict.keys(), key=parse_version_tuple, reverse=True)
        return [(ver, found_dict[ver]) for ver in sorted_vers]

    def refresh_available_binaries(self):
        found_rows = []
        for model_key in ['OE0111', 'OE0112', 'OE0113']:
            bins = self._find_model_binaries(model_key)
            if bins:
                latest_ver, latest_path = bins[0]
                sz_mb = os.path.getsize(latest_path) / (1024 * 1024)
                v = VARIANTS[model_key]
                found_rows.append(f"• <b>{model_key}</b> (En Son: {latest_ver}): {v['merged']} ({sz_mb:.2f} MB - Toplam {len(bins)} sürüm)")

        if found_rows:
            self.lbl_build_info.setText("<br>".join(found_rows))
        else:
            self.lbl_build_info.setText("Henüz derlenmiş çıktı yok. 'Derle & Paketle' butonuna basarak derleyebilirsiniz.")

        self._on_flash_model_changed()

    def _on_flash_model_changed(self):
        model_key = self.combo_flash_model.currentData()
        self.show_old_versions = False
        self._populate_version_combo(model_key)

    def _populate_version_combo(self, model_key: str):
        available_versions = self._find_model_binaries(model_key)

        self.combo_flash_version.blockSignals(True)
        self.combo_flash_version.clear()

        if not available_versions:
            self.combo_flash_version.addItem(f"❌ {model_key} için derlenmiş firmware bulunamadı", "")
            self.combo_flash_version.blockSignals(False)
            self._update_selected_bin_info()
            return

        if not self.show_old_versions:
            # Sadece en son versiyon görünür + Eski versiyonlar seçeneği
            latest_ver, latest_path = available_versions[0]
            sz = os.path.getsize(latest_path) / (1024 * 1024)
            self.combo_flash_version.addItem(f"{latest_ver} (En Son / Güncel) — {sz:.2f} MB", latest_path)
            if len(available_versions) > 1:
                self.combo_flash_version.addItem(f"📂 [Daha Eski Versiyonları Göster ({len(available_versions)-1} adet)...]", "__SHOW_OLD__")
            self.combo_flash_version.setCurrentIndex(0)
        else:
            # Bütün versiyonlar listelenir, en başta gizle seçeneği
            self.combo_flash_version.addItem("📁 [Sadece En Son Versiyonu Göster]", "__HIDE_OLD__")
            for idx, (ver, path) in enumerate(available_versions):
                sz = os.path.getsize(path) / (1024 * 1024)
                tag = " (En Son / Güncel)" if idx == 0 else ""
                self.combo_flash_version.addItem(f"{ver}{tag} — {sz:.2f} MB", path)
            self.combo_flash_version.setCurrentIndex(1)

        self.combo_flash_version.blockSignals(False)
        self._update_selected_bin_info()

    def _on_flash_version_changed(self):
        data = self.combo_flash_version.currentData()
        if data == "__SHOW_OLD__":
            self.show_old_versions = True
            model_key = self.combo_flash_model.currentData()
            self._populate_version_combo(model_key)
            QTimer.singleShot(60, self.combo_flash_version.showPopup)
            return
        elif data == "__HIDE_OLD__":
            self.show_old_versions = False
            model_key = self.combo_flash_model.currentData()
            self._populate_version_combo(model_key)
            return

        self._update_selected_bin_info()

    def _update_selected_bin_info(self):
        if not self.radio_preset_bin.isChecked():
            return
        bin_path = self.combo_flash_version.currentData()
        if bin_path and os.path.exists(bin_path) and bin_path not in ["__SHOW_OLD__", "__HIDE_OLD__"]:
            sz = os.path.getsize(bin_path) / (1024 * 1024)
            name = os.path.basename(bin_path)
            ver = os.path.basename(os.path.dirname(bin_path))
            self.lbl_selected_bin_info.setText(
                f"✔ <b>Seçili Dosya:</b> <span style='color:#10B981;'>{name}</span> "
                f"({sz:.2f} MB) | <b>Sürüm:</b> {ver} | <b>Flash Adresi:</b> 0x0000"
            )
        else:
            self.lbl_selected_bin_info.setText("<span style='color:#EF4444;'>Geçerli bir binary seçilmedi.</span>")

    def _on_flash_mode_toggled(self):
        is_custom = self.radio_custom_bin.isChecked()
        self.preset_container.setEnabled(not is_custom)
        self.txt_custom_file.setEnabled(is_custom)
        self.btn_browse.setEnabled(is_custom)
        if not is_custom:
            self.txt_flash_addr.setText("0x0000")
            self._update_selected_bin_info()
        else:
            self.lbl_selected_bin_info.setText("💡 Özel seçilen harici binary dosyası yüklenecektir.")

    def _browse_custom_binary(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Yüklenecek Binary Dosyası Seç (.bin)",
            SOFTWARE_DIR,
            "Binary Dosyaları (*.bin);;Tüm Dosyalar (*.*)"
        )
        if path:
            self.txt_custom_file.setText(path)

    def open_outputs_folder(self):
        if not os.path.exists(OUTPUTS_DIR):
            os.makedirs(OUTPUTS_DIR, exist_ok=True)
        if sys.platform == 'win32':
            os.startfile(OUTPUTS_DIR)
        else:
            subprocess.Popen(['xdg-open', OUTPUTS_DIR])

    # -------------------------------------------------------------------------
    # BUILD PROCESS
    # -------------------------------------------------------------------------
    def start_build_process(self):
        if self.build_worker and self.build_worker.isRunning():
            QtWidgets.QMessageBox.warning(self, "Uyarı", "Zaten devam eden bir derleme işlemi var!")
            return

        target_key = self.combo_build_target.currentData()
        version = self.txt_version.text().strip() or DEFAULT_FW_VERSION
        clean_first = self.chk_clean.isChecked()

        self.btn_start_build.setEnabled(False)
        self.btn_start_build.setText("⏳ Derleniyor, Lütfen Bekleyin...")
        self.lbl_status.setText(f"Derleme başlatıldı: {target_key} ({version})...")

        # Switch console to process tab
        self.tabs_console.setCurrentIndex(1)
        self.console_process.clear()

        self.build_worker = BuildWorker(target_key, version, clean_first)
        self.build_worker.log_line.connect(self.append_process_console)
        self.build_worker.build_finished.connect(self._on_build_completed)
        self.build_worker.start()

    def _on_build_completed(self, success: bool, msg: str):
        self.btn_start_build.setEnabled(True)
        self.btn_start_build.setText("🔨 Seçiliyi Derle & Paketle (Merge 0x0000)")
        self.lbl_status.setText(msg)
        self.refresh_available_binaries()

        if success:
            QtWidgets.QMessageBox.information(self, "Derleme Başarılı", msg)
        else:
            QtWidgets.QMessageBox.critical(self, "Derleme Hatası", msg)

    # -------------------------------------------------------------------------
    # FLASH & ERASE PROCESS
    # -------------------------------------------------------------------------
    def start_flash_process(self):
        port = self.combo_ports.currentData()
        if not port:
            QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen bir COM Port seçin!")
            return

        if self.flash_worker and self.flash_worker.isRunning():
            QtWidgets.QMessageBox.warning(self, "Uyarı", "Zaten bir yükleme işlemi sürüyor!")
            return

        # Determine target binary
        if self.radio_preset_bin.isChecked():
            bin_path = self.combo_flash_version.currentData()
            if not bin_path or not os.path.exists(bin_path) or bin_path in ["__SHOW_OLD__", "__HIDE_OLD__"]:
                QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen geçerli bir derlenmiş ikili (merged binary) seçin!")
                return
            addr = "0x0000"
        else:
            bin_path = self.txt_custom_file.text().strip()
            if not bin_path or not os.path.exists(bin_path):
                QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen geçerli bir özel .bin dosyası seçin!")
                return
            addr = self.txt_flash_addr.text().strip() or "0x0000"

        # Determine baud
        baud_txt = self.combo_flash_baud.currentText().split()[0]
        baud = int(baud_txt)

        # Smart arbitration: Stop serial monitor if active
        if self.serial_worker and self.serial_worker.isRunning():
            self._was_monitoring_before_flash = True
            self.append_serial_console("⚡ Flash işlemi için seri monitör geçici olarak durduruldu.", "#F59E0B")
            self.stop_serial_monitor()
        else:
            self._was_monitoring_before_flash = False

        self.btn_start_flash.setEnabled(False)
        self.btn_erase_flash.setEnabled(False)
        self.btn_start_flash.setText("⏳ Yükleniyor... (%0)")
        self.bar_flash.setValue(0)
        self.lbl_status.setText(f"Yükleme başlatıldı: {os.path.basename(bin_path)}...")

        # Switch to process console
        self.tabs_console.setCurrentIndex(1)
        self.console_process.clear()

        self.flash_worker = FlashWorker(port, baud, bin_path, addr, is_erase=False)
        self.flash_worker.log_line.connect(self.append_process_console)
        self.flash_worker.progress.connect(self._on_flash_progress)
        self.flash_worker.flash_finished.connect(self._on_flash_completed)
        self.flash_worker.start()

    def start_erase_process(self):
        port = self.combo_ports.currentData()
        if not port:
            QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen bir COM Port seçin!")
            return

        ans = QtWidgets.QMessageBox.warning(
            self, "Çip Silme Onayı",
            f"{port} portundaki ESP32 çipinin TÜM FLASH HAFIZASI tamamen silinecek!\nDevam etmek istiyor musunuz?",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
        )
        if ans != QtWidgets.QMessageBox.Yes:
            return

        baud_txt = self.combo_flash_baud.currentText().split()[0]
        baud = int(baud_txt)

        if self.serial_worker and self.serial_worker.isRunning():
            self._was_monitoring_before_flash = True
            self.stop_serial_monitor()
        else:
            self._was_monitoring_before_flash = False

        self.btn_start_flash.setEnabled(False)
        self.btn_erase_flash.setEnabled(False)
        self.btn_erase_flash.setText("⏳ Siliniyor...")
        self.bar_flash.setValue(20)

        self.tabs_console.setCurrentIndex(1)
        self.console_process.clear()

        self.flash_worker = FlashWorker(port, baud, "", "0x0000", is_erase=True)
        self.flash_worker.log_line.connect(self.append_process_console)
        self.flash_worker.progress.connect(self._on_flash_progress)
        self.flash_worker.flash_finished.connect(self._on_flash_completed)
        self.flash_worker.start()

    def _on_flash_progress(self, pct: int):
        self.bar_flash.setValue(pct)
        if not self.btn_start_flash.isEnabled():
            self.btn_start_flash.setText(f"⏳ Yükleniyor... (%{pct})")

    def _on_flash_completed(self, success: bool, msg: str):
        self.btn_start_flash.setEnabled(True)
        self.btn_erase_flash.setEnabled(True)
        self.btn_start_flash.setText("⚡ Yazılımı Cihaza Yükle (Write Flash)")
        self.btn_erase_flash.setText("🧹 Çipi Tamamen Sil (Erase Flash)")
        self.lbl_status.setText(msg)

        if success:
            QtWidgets.QMessageBox.information(self, "İşlem Başarılı", msg)
            # Auto reconnect serial monitor if requested
            if self.chk_auto_monitor.isChecked() or self._was_monitoring_before_flash:
                QTimer.singleShot(800, self.start_serial_monitor)
        else:
            QtWidgets.QMessageBox.critical(self, "İşlem Hatası", msg)


# =============================================================================
# ENTRY POINT
# =============================================================================
def main():
    if sys.platform == "win32":
        try:
            import ctypes
            myappid = f'onloi.accesscontrol.firmwarestudio.{STUDIO_VERSION}'
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(myappid)
        except Exception:
            pass

    QtWidgets.QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    QtWidgets.QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName(f"Onloi Firmware Studio {STUDIO_VERSION}")
    app.setApplicationVersion(STUDIO_VERSION)
    app.setStyleSheet(STUDIO_STYLE)

    icon_path = os.path.join(RESOURCE_DIR, "studio_icon.ico")
    if not os.path.exists(icon_path):
        icon_path = os.path.join(APP_DIR, "studio_icon.ico")
    if os.path.exists(icon_path):
        app.setWindowIcon(QtGui.QIcon(icon_path))

    window = OnloiFirmwareStudio()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
