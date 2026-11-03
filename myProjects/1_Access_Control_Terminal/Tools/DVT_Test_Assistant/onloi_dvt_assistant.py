# -*- coding: utf-8 -*-
"""
ONLOI GEÇİŞ KONTROL TERMİNALİ
Saha Öncesi Doğrulama (DVT) İnteraktif Masaüstü Test Asistanı (v2.0.11)
"""
import sys
import os
import re
import time
import datetime
import html
import json
import base64
from typing import Dict, List, Optional, Tuple, Any

# Ensure local packages and current dir are in path
# Determine application and resource paths (supports PyInstaller frozen binary)
if getattr(sys, 'frozen', False):
    APP_DIR = os.path.dirname(sys.executable)
    RESOURCE_DIR = getattr(sys, '_MEIPASS', APP_DIR)
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))
    RESOURCE_DIR = APP_DIR

SCRIPT_DIR = APP_DIR
SCRATCH_PKGS = r"C:\Users\gentek-ideapad\.gemini\antigravity\brain\4d9141ec-33a5-49a4-a9a1-8f5b3a3f9e1a\scratch\pypkgs"
if os.path.exists(SCRATCH_PKGS):
    sys.path.insert(0, SCRATCH_PKGS)
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

import serial
import serial.tools.list_ports
from PyQt5 import QtCore, QtGui, QtWidgets
from PyQt5.QtCore import Qt, pyqtSignal, pyqtSlot, QThread, QTimer

from test_steps import Step, MODELS, build_steps

APP_VERSION = "v1.0"
TARGET_FW_VERSION = "v2.0.13"

# =============================================================================
# SERIAL WORKER THREAD
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
            except:
                pass
        self.disconnected.emit()

    def send_line(self, cmd: str):
        if self._ser and self._ser.is_open:
            try:
                self._ser.write((cmd.strip() + "\r\n").encode('utf-8'))
                self._ser.flush()
            except Exception as e:
                self.error_occurred.emit(f"Komut gönderilemedi: {e}")

    def pulse_reset(self):
        if self._ser and self._ser.is_open:
            try:
                self._ser.dtr = False
                self._ser.rts = True
                time.sleep(0.1)
                self._ser.rts = False
            except Exception as e:
                self.error_occurred.emit(f"Reset sinyali gönderilemedi: {e}")

    def stop(self):
        self._running = False
        self.wait(1500)


# =============================================================================
# VISUAL KEYCAP & STEP BADGE GENERATOR (QPainter -> Base64 PNG)
# =============================================================================
_BADGE_CACHE: Dict[Tuple[str, str], str] = {}

_BADGE_STYLES: Dict[str, Dict[str, Any]] = {
    "circle_num":    {"bg": "#0284C7", "border": "#38BDF8", "fg": "#FFFFFF", "shape": "circle", "h": 22, "min_w": 22, "pad_x": 0, "fs": 11},
    "key_star":      {"bg": "#451A03", "border": "#F59E0B", "fg": "#FDE68A", "shape": "rect",   "h": 25, "min_w": 25, "pad_x": 7, "fs": 13},
    "key_hash":      {"bg": "#064E3B", "border": "#10B981", "fg": "#A7F3D0", "shape": "rect",   "h": 25, "min_w": 25, "pad_x": 7, "fs": 12},
    "key_digit":     {"bg": "#1E293B", "border": "#38BDF8", "fg": "#F8FAFC", "shape": "rect",   "h": 25, "min_w": 25, "pad_x": 7, "fs": 12},
    "badge_master":  {"bg": "#2E1065", "border": "#A855F7", "fg": "#F3E8FF", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
    "badge_user":    {"bg": "#083344", "border": "#06B6D4", "fg": "#CFFAFE", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
    "badge_foreign": {"bg": "#4C0519", "border": "#F43F5E", "fg": "#FFE4E6", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
    "badge_hw":      {"bg": "#451A03", "border": "#F59E0B", "fg": "#FDE68A", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
    "badge_wait":    {"bg": "#1E293B", "border": "#EAB308", "fg": "#FEF08A", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
    "badge_action":  {"bg": "#0C4A6E", "border": "#38BDF8", "fg": "#E0F2FE", "shape": "rect",   "h": 25, "min_w": 32, "pad_x": 9, "fs": 11},
}


def get_badge_img(text: str, kind: str = "key_digit") -> str:
    key = (text, kind)
    if key in _BADGE_CACHE:
        return _BADGE_CACHE[key]

    st = _BADGE_STYLES.get(kind, _BADGE_STYLES["key_digit"])
    font = QtGui.QFont("Segoe UI")
    font.setPixelSize(st["fs"])
    font.setBold(True)
    fm = QtGui.QFontMetrics(font)
    tw = fm.horizontalAdvance(text)

    h = st["h"]
    if st["shape"] == "circle":
        w = max(st["min_w"], h) + 2
    else:
        w = max(st["min_w"], tw + st["pad_x"] * 2) + 3

    pix = QtGui.QPixmap(w, h)
    pix.fill(Qt.transparent)

    p = QtGui.QPainter(pix)
    p.setRenderHint(QtGui.QPainter.Antialiasing, True)
    p.setRenderHint(QtGui.QPainter.TextAntialiasing, True)

    pen = QtGui.QPen(QtGui.QColor(st["border"]))
    pen.setWidthF(1.4)
    p.setPen(pen)
    p.setBrush(QtGui.QBrush(QtGui.QColor(st["bg"])))

    if st["shape"] == "circle":
        rect = QtCore.QRectF(1.5, 1.0, h - 2.0, h - 2.0)
        p.drawEllipse(rect)
    else:
        rect = QtCore.QRectF(1.5, 1.0, w - 4.0, h - 2.0)
        p.drawRoundedRect(rect, 5.5, 5.5)

    p.setFont(font)
    p.setPen(QtGui.QColor(st["fg"]))
    p.drawText(rect, Qt.AlignCenter, text)
    p.end()

    ba = QtCore.QByteArray()
    buf = QtCore.QBuffer(ba)
    buf.open(QtCore.QIODevice.WriteOnly)
    pix.save(buf, "PNG")
    b64 = base64.b64encode(bytes(ba)).decode("ascii")
    tag = f'<img src="data:image/png;base64,{b64}" style="vertical-align: middle;" />'
    _BADGE_CACHE[key] = tag
    return tag


# =============================================================================
# STYLESHEET (PROFESYONEL KOYU MAVİ / SLATE TEMASI)
# =============================================================================
MODERN_STYLE = """
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
QPushButton {
    background-color: #1E293B;
    border: 1px solid #475569;
    border-radius: 6px;
    padding: 6px 12px;
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
QPushButton#btnConnect {
    background-color: #0284C7;
    border-color: #0369A1;
    color: white;
}
QPushButton#btnConnect:hover {
    background-color: #0369A1;
}
QPushButton#btnPass {
    background-color: #15803D;
    border-color: #166534;
    color: white;
    font-size: 14px;
    padding: 10px 18px;
}
QPushButton#btnPass:hover {
    background-color: #166534;
}
QPushButton#btnFail {
    background-color: #B91C1C;
    border-color: #991B1B;
    color: white;
    font-size: 14px;
    padding: 10px 18px;
}
QPushButton#btnFail:hover {
    background-color: #991B1B;
}
QPushButton#btnResetStep {
    background-color: #D97706;
    border-color: #B45309;
    color: white;
}
QPushButton#btnResetStep:hover {
    background-color: #B45309;
}
QComboBox, QLineEdit {
    background-color: #1E293B;
    border: 1px solid #475569;
    border-radius: 6px;
    padding: 5px 10px;
    color: #F8FAFC;
}
QComboBox#comboPorts {
    min-width: 62px;
    max-width: 82px;
}
QComboBox#comboModel {
    min-width: 215px;
}
QLineEdit#txtTester {
    min-width: 95px;
    max-width: 120px;
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
QListWidget {
    background-color: #1E293B;
    border: 1px solid #334155;
    border-radius: 8px;
    padding: 4px;
    outline: none;
}
QListWidget::item {
    border-radius: 5px;
    padding: 6px 8px;
    margin-bottom: 2px;
}
QListWidget::item:hover {
    background-color: #334155;
}
QListWidget::item:selected {
    background-color: #0369A1;
    color: white;
    font-weight: bold;
}
QTextEdit, QPlainTextEdit {
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
    background-color: #1E293B;
    color: white;
    font-weight: bold;
    height: 18px;
}
QProgressBar::chunk {
    background-color: #0284C7;
    border-radius: 5px;
}
QScrollBar:vertical {
    border: none;
    background-color: #0F172A;
    width: 10px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background-color: #334155;
    min-height: 20px;
    border-radius: 5px;
}
QScrollBar::handle:vertical:hover {
    background-color: #475569;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
}
"""


# =============================================================================
# MAIN WINDOW
# =============================================================================
class OnloiDvtApp(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"Onloi DVT Test Asistanı {APP_VERSION} — Geçiş Kontrol Terminali (Hedef FW: {TARGET_FW_VERSION})")
        self.resize(1340, 840)
        self.setMinimumSize(1100, 700)

        self.serial_worker: Optional[SerialWorker] = None
        self.current_model = "OE0111"
        self.steps: List[Step] = []
        self.current_step_idx = 0

        # Test state
        self.step_results: Dict[str, Dict[str, Any]] = {}
        self.captured_vars: Dict[str, str] = {}
        self.detected_serial: str = ""
        self.detected_mac6: str = ""
        self.current_expect_counts: Dict[int, int] = {}
        self.measure_start_time: Optional[float] = None
        self.window_deadline: Optional[float] = None
        self.step_start_time: float = time.time()

        # Timers
        self.ui_timer = QTimer(self)
        self.ui_timer.setInterval(200)
        self.ui_timer.timeout.connect(self._on_ui_tick)

        self.init_ui()
        self.refresh_ports()
        self.load_model_steps(self.current_model)

    # -------------------------------------------------------------------------
    # UI SETUP
    # -------------------------------------------------------------------------
    def init_ui(self):
        # Set application icon
        icon_path = os.path.join(RESOURCE_DIR, "app_icon.ico")
        if not os.path.exists(icon_path):
            icon_path = os.path.join(APP_DIR, "app_icon.ico")
        if os.path.exists(icon_path):
            self.setWindowIcon(QtGui.QIcon(icon_path))

        central = QtWidgets.QWidget()
        self.setCentralWidget(central)
        main_layout = QtWidgets.QVBoxLayout(central)
        main_layout.setContentsMargins(12, 10, 12, 10)
        main_layout.setSpacing(10)

        # 1. TOP BAR: Port Connection, Model, Tester Info & Quick CLI (Fixed Height to prevent jumping)
        top_bar = QtWidgets.QFrame()
        top_bar.setObjectName("topBar")
        top_bar.setFixedHeight(54)
        top_bar.setSizePolicy(QtWidgets.QSizePolicy.Expanding, QtWidgets.QSizePolicy.Fixed)
        top_bar.setStyleSheet("#topBar { background-color: #1E293B; border: 1px solid #334155; border-radius: 8px; }")
        top_layout = QtWidgets.QHBoxLayout(top_bar)
        top_layout.setContentsMargins(10, 4, 10, 4)
        top_layout.setSpacing(6)

        # Port selection (only port name shown, no parenthetical description)
        lbl_port = QtWidgets.QLabel("<b>Port:</b>")
        lbl_port.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Preferred)
        top_layout.addWidget(lbl_port)

        self.combo_ports = QtWidgets.QComboBox()
        self.combo_ports.setObjectName("comboPorts")
        self.combo_ports.setSizePolicy(QtWidgets.QSizePolicy.Minimum, QtWidgets.QSizePolicy.Preferred)
        top_layout.addWidget(self.combo_ports)

        btn_refresh = QtWidgets.QPushButton("🔄")
        btn_refresh.setToolTip("Portları Yenile")
        btn_refresh.clicked.connect(self.refresh_ports)
        top_layout.addWidget(btn_refresh)

        self.btn_connect = QtWidgets.QPushButton("🔌 Bağlan")
        self.btn_connect.setObjectName("btnConnect")
        self.btn_connect.clicked.connect(self.toggle_connection)
        top_layout.addWidget(self.btn_connect)

        # Separator
        line1 = QtWidgets.QFrame()
        line1.setFrameShape(QtWidgets.QFrame.VLine)
        line1.setStyleSheet("color: #475569;")
        top_layout.addWidget(line1)

        # Model selection (CSS min-width ensures full model title fits without layout mismatch)
        lbl_model = QtWidgets.QLabel("<b>Model:</b>")
        lbl_model.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Preferred)
        top_layout.addWidget(lbl_model)

        self.combo_model = QtWidgets.QComboBox()
        self.combo_model.setObjectName("comboModel")
        self.combo_model.setSizePolicy(QtWidgets.QSizePolicy.Minimum, QtWidgets.QSizePolicy.Preferred)
        for k, v in MODELS.items():
            self.combo_model.addItem(f"{k} ({v['title']})", k)
        self.combo_model.currentIndexChanged.connect(self._on_model_changed)
        top_layout.addWidget(self.combo_model)

        # Tester Info (Serial No is automatically captured from UART debug logs)
        lbl_tester = QtWidgets.QLabel("<b>Personel:</b>")
        lbl_tester.setSizePolicy(QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Preferred)
        top_layout.addWidget(lbl_tester)

        self.txt_tester = QtWidgets.QLineEdit()
        self.txt_tester.setObjectName("txtTester")
        self.txt_tester.setPlaceholderText("Ad Soyad")
        self.txt_tester.textChanged.connect(lambda: self._save_session())
        top_layout.addWidget(self.txt_tester)

        top_layout.addStretch()

        # Quick CLI Buttons
        btn_cli_info = QtWidgets.QPushButton("Info")
        btn_cli_info.setToolTip("Cihaz model, versiyon ve seri no sorgula (onloi_ota_info)")
        btn_cli_info.clicked.connect(lambda: self.send_cli("onloi_ota_info"))
        top_layout.addWidget(btn_cli_info)

        btn_cli_users = QtWidgets.QPushButton("Kullanıcılar")
        btn_cli_users.setToolTip("Kayıtlı kullanıcıları listele (onloi_user_list_view)")
        btn_cli_users.clicked.connect(lambda: self.send_cli("onloi_user_list_view"))
        top_layout.addWidget(btn_cli_users)

        btn_cli_masters = QtWidgets.QPushButton("Masterlar")
        btn_cli_masters.setToolTip("Kayıtlı master kart/QR listele (onloi_master_list_view)")
        btn_cli_masters.clicked.connect(lambda: self.send_cli("onloi_master_list_view"))
        top_layout.addWidget(btn_cli_masters)

        btn_hw_reset = QtWidgets.QPushButton("⚡ Reset")
        btn_hw_reset.setToolTip("Cihazı yeniden başlat (DTR/RTS donanım reset - NVS silinmez)")
        btn_hw_reset.clicked.connect(self._pulse_device_reset)
        top_layout.addWidget(btn_hw_reset)

        btn_cli_reset = QtWidgets.QPushButton("⚠️ Fabrika Reset")
        btn_cli_reset.setObjectName("btnResetStep")
        btn_cli_reset.setToolTip("Tüm hafızayı fabrika ayarlarına sıfırla (onloi_fab_reset)")
        btn_cli_reset.clicked.connect(self._confirm_cli_fab_reset)
        top_layout.addWidget(btn_cli_reset)

        main_layout.addWidget(top_bar, 0)

        # 2. BODY SPLITTER: Left (Step List), Center (Current Step Guide), Right (Serial Console)
        splitter = QtWidgets.QSplitter(Qt.Horizontal)
        splitter.setHandleWidth(6)

        # --- LEFT PANEL: Step List & Progress ---
        left_widget = QtWidgets.QWidget()
        left_layout = QtWidgets.QVBoxLayout(left_widget)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(6)

        lbl_steps = QtWidgets.QLabel("<b>Test Adımları ve İlerleme</b>")
        left_layout.addWidget(lbl_steps)

        self.progress_bar = QtWidgets.QProgressBar()
        self.progress_bar.setFormat("%v / %m (%p%)")
        left_layout.addWidget(self.progress_bar)

        self.list_steps = QtWidgets.QListWidget()
        self.list_steps.setWordWrap(True)
        self.list_steps.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.list_steps.currentRowChanged.connect(self._on_step_list_selected)
        left_layout.addWidget(self.list_steps)

        btn_export = QtWidgets.QPushButton("📄 Test Raporu Oluştur (HTML / CSV)")
        btn_export.clicked.connect(self.export_report)
        left_layout.addWidget(btn_export)

        self.btn_reset_session = QtWidgets.QPushButton("🔄 Bu Modelin Testini Sıfırla")
        self.btn_reset_session.setToolTip("Kaydedilmiş test ilerlemesini siler ve testi en baştan başlatır")
        self.btn_reset_session.setStyleSheet("background-color: #1E293B; border: 1px solid #7F1D1D; color: #FCA5A5; font-size: 12px;")
        self.btn_reset_session.clicked.connect(self.reset_model_session)
        left_layout.addWidget(self.btn_reset_session)

        splitter.addWidget(left_widget)
        splitter.setStretchFactor(0, 24)

        # --- CENTER PANEL: Interactive Step Guide ---
        center_widget = QtWidgets.QWidget()
        center_layout = QtWidgets.QVBoxLayout(center_widget)
        center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.setSpacing(8)

        # Step Header Card
        self.group_step = QtWidgets.QGroupBox("ŞU ANKİ ADIM TALİMATI")
        step_group_layout = QtWidgets.QVBoxLayout(self.group_step)
        step_group_layout.setSpacing(8)

        self.lbl_step_id_title = QtWidgets.QLabel("<b>ADIM X: Başlık</b>")
        self.lbl_step_id_title.setStyleSheet("font-size: 15px; color: #38BDF8; font-weight: bold;")
        self.lbl_step_id_title.setWordWrap(True)
        step_group_layout.addWidget(self.lbl_step_id_title)

        # Instruction Box
        self.txt_instruction = QtWidgets.QTextBrowser()
        self.txt_instruction.setStyleSheet(
            "background-color: #1E293B; border: 1px solid #334155; border-radius: 6px; "
            "padding: 10px; font-size: 13.5px; line-height: 1.4; color: #F8FAFC;"
        )
        self.txt_instruction.setOpenExternalLinks(True)
        step_group_layout.addWidget(self.txt_instruction, stretch=4)

        # Observation Box
        self.lbl_observe = QtWidgets.QLabel("")
        self.lbl_observe.setStyleSheet(
            "background-color: #0F172A; border-left: 4px solid #38BDF8; "
            "padding: 6px 10px; border-radius: 4px; color: #94A3B8; font-size: 12.5px;"
        )
        self.lbl_observe.setWordWrap(True)
        step_group_layout.addWidget(self.lbl_observe)

        # Step Action Button (For steps with CLI commands like onloi_ota_info or onloi_fab_reset)
        self.btn_step_action = QtWidgets.QPushButton("🚀 Komutu Çalıştır")
        self.btn_step_action.setObjectName("btnStepAction")
        self.btn_step_action.clicked.connect(self._on_step_action_clicked)
        self.btn_step_action.setVisible(False)
        step_group_layout.addWidget(self.btn_step_action)

        # Live Countdown / Window Warning Bar (for IN2 or 10s boot master window)
        self.bar_window = QtWidgets.QProgressBar()
        self.bar_window.setStyleSheet(
            "QProgressBar { height: 16px; border: 1px solid #D97706; background-color: #1E293B; } "
            "QProgressBar::chunk { background-color: #F59E0B; }"
        )
        self.bar_window.setVisible(False)
        step_group_layout.addWidget(self.bar_window)

        # Live Verification Status
        self.group_expect = QtWidgets.QGroupBox("SERİ PORT OTOMATİK DOĞRULAMA (UART CANLI YAKALAMA)")
        expect_layout = QtWidgets.QVBoxLayout(self.group_expect)
        expect_layout.setSpacing(4)
        self.lbl_expect_status = QtWidgets.QLabel("Beklenen log satırları...")
        self.lbl_expect_status.setWordWrap(True)
        self.lbl_expect_status.setStyleSheet("font-size: 12.5px; color: #E2E8F0;")
        expect_layout.addWidget(self.lbl_expect_status)
        step_group_layout.addWidget(self.group_expect)

        # Manual Question Banner
        self.frame_manual = QtWidgets.QFrame()
        self.frame_manual.setStyleSheet("background-color: #312E81; border: 1px solid #4F46E5; border-radius: 6px; padding: 8px;")
        manual_layout = QtWidgets.QVBoxLayout(self.frame_manual)
        manual_layout.setContentsMargins(8, 6, 8, 6)
        self.lbl_manual_q = QtWidgets.QLabel("<b>Fiziksel Gözlem Sorusu:</b> ...")
        self.lbl_manual_q.setStyleSheet("font-size: 13.5px; color: #EEF2FF;")
        self.lbl_manual_q.setWordWrap(True)
        manual_layout.addWidget(self.lbl_manual_q)
        step_group_layout.addWidget(self.frame_manual)
        self.frame_manual.setVisible(False)

        # Auto-advance checkbox
        advance_bar = QtWidgets.QHBoxLayout()
        self.chk_autoadvance = QtWidgets.QCheckBox("Otomatik İlerle (Doğrulanınca 2 sn sonra sonraki adıma geç)")
        self.chk_autoadvance.setChecked(False)
        self.chk_autoadvance.setStyleSheet("color: #94A3B8; font-size: 12px;")
        advance_bar.addWidget(self.chk_autoadvance)
        advance_bar.addStretch()
        step_group_layout.addLayout(advance_bar)

        # Action Buttons
        btn_bar = QtWidgets.QHBoxLayout()
        btn_bar.setSpacing(8)

        self.btn_pass = QtWidgets.QPushButton("✅ DOĞRULANDI (PASS)")
        self.btn_pass.setObjectName("btnPass")
        self.btn_pass.clicked.connect(lambda: self.record_step_result(passed=True))
        btn_bar.addWidget(self.btn_pass, stretch=2)

        self.btn_fail = QtWidgets.QPushButton("❌ HATA VAR (FAIL)")
        self.btn_fail.setObjectName("btnFail")
        self.btn_fail.clicked.connect(lambda: self.record_step_result(passed=False))
        btn_bar.addWidget(self.btn_fail, stretch=2)

        self.btn_retry = QtWidgets.QPushButton("🔄 Adımı Sıfırla")
        self.btn_retry.setObjectName("btnResetStep")
        self.btn_retry.clicked.connect(self.retry_current_step)
        btn_bar.addWidget(self.btn_retry, stretch=1)

        self.btn_skip = QtWidgets.QPushButton("⏭️ Atla")
        self.btn_skip.clicked.connect(self.skip_current_step)
        btn_bar.addWidget(self.btn_skip, stretch=1)

        step_group_layout.addLayout(btn_bar)
        center_layout.addWidget(self.group_step)

        splitter.addWidget(center_widget)
        splitter.setStretchFactor(1, 48)

        # --- RIGHT PANEL: Live Serial Terminal ---
        right_widget = QtWidgets.QWidget()
        right_layout = QtWidgets.QVBoxLayout(right_widget)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(6)

        right_top = QtWidgets.QHBoxLayout()
        right_top.addWidget(QtWidgets.QLabel("<b>Seri Port Logu (115200)</b>"))
        right_top.addStretch()

        self.chk_autoscroll = QtWidgets.QCheckBox("Oto-Kaydır")
        self.chk_autoscroll.setChecked(True)
        right_top.addWidget(self.chk_autoscroll)

        btn_clear_log = QtWidgets.QPushButton("Temizle")
        btn_clear_log.setMinimumWidth(85)
        btn_clear_log.setMaximumWidth(100)
        btn_clear_log.clicked.connect(self._clear_console)
        right_top.addWidget(btn_clear_log)
        right_layout.addLayout(right_top)

        self.console = QtWidgets.QPlainTextEdit()
        self.console.setReadOnly(True)
        self.console.setMaximumBlockCount(3000)
        right_layout.addWidget(self.console)

        # Command input
        cmd_bar = QtWidgets.QHBoxLayout()
        self.txt_cmd = QtWidgets.QLineEdit()
        self.txt_cmd.setPlaceholderText("Seri port komutu yazın (Enter)...")
        self.txt_cmd.returnPressed.connect(self._on_send_cmd_pressed)
        cmd_bar.addWidget(self.txt_cmd)

        btn_send = QtWidgets.QPushButton("Gönder")
        btn_send.clicked.connect(self._on_send_cmd_pressed)
        cmd_bar.addWidget(btn_send)
        right_layout.addLayout(cmd_bar)

        splitter.addWidget(right_widget)
        splitter.setStretchFactor(2, 28)

        main_layout.addWidget(splitter, 1)

        # 3. STATUS BAR
        self.status_bar = QtWidgets.QStatusBar()
        self.setStatusBar(self.status_bar)
        self.lbl_status = QtWidgets.QLabel("Bağlantı bekleniyor...")
        self.status_bar.addWidget(self.lbl_status)

        self.lbl_serial_badge = QtWidgets.QLabel("🆔 Cihaz Seri No: Otomatik Algılanacak (Log bekleniyor)")
        self.lbl_serial_badge.setStyleSheet(
            "color: #38BDF8; font-weight: bold; padding: 2px 10px; "
            "background-color: #1E293B; border: 1px solid #334155; border-radius: 4px;"
        )
        self.status_bar.addPermanentWidget(self.lbl_serial_badge)

        # Force custom styled QListView popup on all combo boxes and apply stylesheet after all objectNames are set
        for cb in self.findChildren(QtWidgets.QComboBox):
            cb.setView(QtWidgets.QListView())
        self.setStyleSheet(MODERN_STYLE)

    def _update_serial_badge(self):
        if self.detected_serial:
            self.lbl_serial_badge.setText(f"🆔 Cihaz Seri No: {self.detected_serial}")
            self.lbl_serial_badge.setStyleSheet(
                "color: #34D399; font-weight: bold; padding: 2px 10px; "
                "background-color: #064E3B; border: 1px solid #10B981; border-radius: 4px;"
            )
        else:
            self.lbl_serial_badge.setText("🆔 Cihaz Seri No: Otomatik Algılanacak (Log bekleniyor)")
            self.lbl_serial_badge.setStyleSheet(
                "color: #38BDF8; font-weight: bold; padding: 2px 10px; "
                "background-color: #1E293B; border: 1px solid #334155; border-radius: 4px;"
            )

    # -------------------------------------------------------------------------
    # SERIAL PORT HANDLING
    # -------------------------------------------------------------------------
    def refresh_ports(self):
        self.combo_ports.clear()
        ports = serial.tools.list_ports.comports()
        for idx, p in enumerate(ports):
            desc = p.description if p.description else p.device
            self.combo_ports.addItem(p.device, p.device)
            self.combo_ports.setItemData(idx, f"{p.device} — {desc}", Qt.ToolTipRole)
        if not ports:
            self.combo_ports.addItem("Port Yok", "")

    def toggle_connection(self):
        if self.serial_worker and self.serial_worker.isRunning():
            self.serial_worker.stop()
            self.serial_worker = None
            self.btn_connect.setText("🔌 Bağlan")
            self.btn_connect.setObjectName("btnConnect")
            self.btn_connect.setStyleSheet("")
            self.lbl_status.setText("Bağlantı kesildi.")
            self.ui_timer.stop()
        else:
            port = self.combo_ports.currentData()
            if not port:
                QtWidgets.QMessageBox.warning(self, "Hata", "Lütfen geçerli bir COM port seçin!")
                return
            self.serial_worker = SerialWorker(port, 115200)
            self.serial_worker.line_received.connect(self._on_serial_line)
            self.serial_worker.connected.connect(self._on_serial_connected)
            self.serial_worker.disconnected.connect(self._on_serial_disconnected)
            self.serial_worker.error_occurred.connect(self._on_serial_error)
            self.serial_worker.start()
            self.ui_timer.start()

    def _on_serial_connected(self, port: str):
        self.btn_connect.setText("❌ Bağlantıyı Kes")
        self.btn_connect.setStyleSheet("background-color: #B91C1C; border-color: #991B1B;")
        self.lbl_status.setText(f"Bağlandı: {port} (115200 bps)")
        self.append_console(f"=== BAĞLANDI: {port} (115200) ===", "#38BDF8")
        # Trigger step 0 if ready
        self._activate_step(self.current_step_idx)

    def _on_serial_disconnected(self):
        self.btn_connect.setText("🔌 Bağlan")
        self.btn_connect.setStyleSheet("")
        self.lbl_status.setText("Bağlantı kapalı.")
        self.append_console("=== BAĞLANTI KAPATILDI ===", "#94A3B8")

    def _on_serial_error(self, err_msg: str):
        self.append_console(f"[HATA] {err_msg}", "#EF4444")
        self.lbl_status.setText(err_msg)

    def send_cli(self, cmd: str):
        if self.serial_worker and self.serial_worker.isRunning():
            self.append_console(f">> {cmd}", "#A855F7")
            self.serial_worker.send_line(cmd)
        else:
            QtWidgets.QMessageBox.warning(self, "Uyarı", "Cihaza bağlı değilsiniz! Önce 'Bağlan' butonuna basın.")

    def _pulse_device_reset(self):
        if self.serial_worker and self.serial_worker.isRunning():
            self.append_console("⚡ Cihaza donanım reset sinyali (DTR/RTS) gönderiliyor...", "#F59E0B")
            self.serial_worker.pulse_reset()
        else:
            QtWidgets.QMessageBox.warning(self, "Uyarı", "Cihaza bağlı değilsiniz! Önce 'Bağlan' butonuna basın.")

    def _on_send_cmd_pressed(self):
        cmd = self.txt_cmd.text().strip()
        if cmd:
            self.send_cli(cmd)
            self.txt_cmd.clear()

    def _confirm_cli_fab_reset(self):
        ans = QtWidgets.QMessageBox.question(
            self, "Fabrika Sıfırlama Onayı",
            "Cihaz seri porttan (onloi_fab_reset) fabrika ayarlarına döndürülecek!\nOnaylıyor musunuz?",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
        )
        if ans == QtWidgets.QMessageBox.Yes:
            self.send_cli("onloi_fab_reset")

    # -------------------------------------------------------------------------
    # CONSOLE & FORMATTING
    # -------------------------------------------------------------------------
    def append_console(self, text: str, color: Optional[str] = None):
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

        self.console.appendHtml(f"<span style='color: {color};'>{escaped}</span>")
        if self.chk_autoscroll.isChecked():
            self.console.moveCursor(QtGui.QTextCursor.End)

    def _clear_console(self):
        self.console.clear()

    # -------------------------------------------------------------------------
    # MODEL & STEP MANAGEMENT & SESSION PERSISTENCE
    # -------------------------------------------------------------------------
    def _get_session_file(self, model: str) -> str:
        sessions_dir = os.path.join(SCRIPT_DIR, "Sessions")
        os.makedirs(sessions_dir, exist_ok=True)
        return os.path.join(sessions_dir, f"session_{model}.json")

    def _save_session(self):
        if not hasattr(self, "steps") or not self.steps:
            return
        session_file = self._get_session_file(self.current_model)
        data = {
            "model": self.current_model,
            "current_step_idx": self.current_step_idx,
            "tester": self.txt_tester.text(),
            "serial": self.detected_serial,
            "detected_mac6": self.detected_mac6,
            "captured_vars": self.captured_vars,
            "step_results": self.step_results,
            "updated_at": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        }
        try:
            with open(session_file, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    def _load_session(self, model: str) -> bool:
        session_file = self._get_session_file(model)
        if not os.path.exists(session_file):
            return False
        try:
            with open(session_file, "r", encoding="utf-8") as f:
                data = json.load(f)

            self.txt_tester.blockSignals(True)
            if "tester" in data:
                self.txt_tester.setText(data["tester"])
            self.txt_tester.blockSignals(False)

            self.detected_mac6 = data.get("detected_mac6", "")
            self.detected_serial = data.get("serial", "")
            if self.detected_mac6:
                self.detected_serial = f"{model}_{self.detected_mac6}"
            self._update_serial_badge()

            self.captured_vars = data.get("captured_vars", {})
            self.step_results = data.get("step_results", {})

            # Update list items
            for idx, s in enumerate(self.steps):
                item = self.list_steps.item(idx)
                if not item:
                    continue
                if s.id in self.step_results:
                    res = self.step_results[s.id]
                    st = res.get("status")
                    if st == "PASS":
                        item.setText(f"✅ {s.id}: {s.title}")
                        item.setForeground(QtGui.QBrush(QtGui.QColor("#15803D")))
                    elif st == "FAIL":
                        item.setText(f"❌ {s.id}: {s.title}")
                        item.setForeground(QtGui.QBrush(QtGui.QColor("#B91C1C")))
                    elif st == "SKIP":
                        item.setText(f"⏭️ {s.id}: {s.title}")
                        item.setForeground(QtGui.QBrush(QtGui.QColor("#94A3B8")))

            completed = sum(1 for v in self.step_results.values() if v.get("status") in ["PASS", "FAIL", "SKIP"])
            self.progress_bar.setValue(completed)

            target_idx = data.get("current_step_idx", 0)
            if target_idx < 0 or target_idx >= len(self.steps):
                target_idx = 0

            # If target_idx is already completed and there are remaining steps, jump to first uncompleted step
            if completed < len(self.steps) and self.steps[target_idx].id in self.step_results:
                for i, s in enumerate(self.steps):
                    if s.id not in self.step_results:
                        target_idx = i
                        break

            self._activate_step(target_idx)
            self.append_console(f"💾 Önceki test oturumu yüklendi ({completed}/{len(self.steps)} adım tamamlanmış). Kaldığınız yerden devam edebilirsiniz.", "#38BDF8")
            return True
        except Exception as e:
            self.append_console(f"⚠️ Oturum dosyası okunamadı: {e}", "#EF4444")
            return False

    def reset_model_session(self):
        ans = QtWidgets.QMessageBox.question(
            self, "Testi Sıfırla",
            f"{self.current_model} modeline ait kaydedilmiş test ilerlemesi silinecek ve test ilk adımdan başlatılacak.\nEmin misiniz?",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
        )
        if ans == QtWidgets.QMessageBox.Yes:
            session_file = self._get_session_file(self.current_model)
            if os.path.exists(session_file):
                try:
                    os.remove(session_file)
                except Exception as e:
                    QtWidgets.QMessageBox.warning(self, "Hata", f"Oturum dosyası silinemedi: {e}")
            self.append_console(f"🔄 {self.current_model} testi sıfırlandı. 1. adımdan başlanıyor.", "#F59E0B")
            self.load_model_steps(self.current_model)

    def _on_model_changed(self):
        model = self.combo_model.currentData()
        if model != self.current_model:
            self._save_session()
            self.current_model = model
            self.load_model_steps(model)

    def load_model_steps(self, model: str):
        self.current_model = model
        self.steps = build_steps(model, TARGET_FW_VERSION)
        self.step_results.clear()
        self.captured_vars.clear()
        self.detected_serial = f"{model}_{self.detected_mac6}" if self.detected_mac6 else ""
        self._update_serial_badge()
        self.current_step_idx = 0

        self.list_steps.clear()
        for idx, s in enumerate(self.steps):
            item = QtWidgets.QListWidgetItem(f"⏳ {s.id}: {s.title}")
            item.setData(Qt.UserRole, idx)
            self.list_steps.addItem(item)

        self.progress_bar.setMaximum(len(self.steps))
        self.progress_bar.setValue(0)

        # Attempt to resume from session; if none exists, activate step 0
        if not self._load_session(model):
            self._activate_step(0)

    def _activate_step(self, idx: int):
        if idx < 0 or idx >= len(self.steps):
            return
        self.current_step_idx = idx
        step = self.steps[idx]
        self.step_start_time = time.time()
        self.current_expect_counts.clear()
        self.measure_start_time = None
        self.window_deadline = None

        # Reset action button state
        self.btn_pass.setText("✅ DOĞRULANDI (PASS)")
        self.btn_pass.setStyleSheet("")

        # NOTE: Commands (step.send) are NOT sent automatically on activation
        # to prevent accidental factory resets or unexpected auto-jumps!
        # The operator initiates it via self.btn_step_action.

        self._update_step_ui()

        # Update List Selection
        self.list_steps.blockSignals(True)
        self.list_steps.setCurrentRow(idx)
        self.list_steps.blockSignals(False)
        self._save_session()

    def _on_step_action_clicked(self):
        step = self.steps[self.current_step_idx]
        if not step.send:
            return
        if not (self.serial_worker and self.serial_worker.isRunning()):
            QtWidgets.QMessageBox.warning(self, "Bağlantı Yok", "Lütfen önce cihaza seri porttan bağlanın!")
            return

        if any("onloi_fab_reset" in cmd for cmd in step.send):
            ans = QtWidgets.QMessageBox.question(
                self, "Fabrika Sıfırlama Onayı",
                "Bu işlem cihaz hafızasını fabrika ayarlarına sıfırlayacaktır!\nKomutu göndermek istiyor musunuz?",
                QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
            )
            if ans != QtWidgets.QMessageBox.Yes:
                return

        for cmd in step.send:
            self.send_cli(cmd)

    def _substitute_vars(self, text: str, for_regex: bool = False) -> str:
        for k, v in self.captured_vars.items():
            text = text.replace(f"<<{k}>>", v)
        if not for_regex:
            defaults = {
                "PINID": "3",
                "QLID": "2",
                "BLE": self.detected_serial or f"{self.current_model}_XXXX",
                "FW": TARGET_FW_VERSION,
                "MODEL": self.current_model,
            }
            for k, v in defaults.items():
                text = text.replace(f"<<{k}>>", v)
        else:
            regex_defaults = {
                "PINID": r"\d+",
                "QLID": r"\d+",
                "BLE": rf"{self.current_model}_[0-9A-Fa-f]+",
                "FW": re.escape(TARGET_FW_VERSION),
                "MODEL": re.escape(self.current_model),
                "M1": r"\S+",
                "M2": r"\S+",
                "MQ1": r"\S+",
                "MQ2": r"\S+",
                "C1": r"\S+",
                "C2": r"\S+",
                "Q1": r"\S+",
                "QL": r"\S+",
            }
            for k, v in regex_defaults.items():
                text = text.replace(f"<<{k}>>", v)
        return text

    def _format_talimat_tokens(self, raw_cmd: str) -> str:
        cmd_str = self._substitute_vars(raw_cmd, for_regex=False)
        parts = re.split(r'(\[[^\]]+\]|->)', cmd_str)
        out: List[str] = []
        for part in parts:
            p = part.strip()
            if not p:
                continue
            if p == "->":
                out.append('<span style="color:#64748B; font-weight:bold; font-size:13px;">&nbsp;➜&nbsp;</span>')
            elif p.startswith("[") and p.endswith("]"):
                inner = p[1:-1].strip()
                if inner == "*":
                    out.append(get_badge_img("*", "key_star"))
                elif inner == "#":
                    out.append(get_badge_img("#", "key_hash"))
                elif inner.isdigit():
                    if len(inner) == 1:
                        out.append(get_badge_img(inner, "key_digit"))
                    else:
                        out.append("".join(get_badge_img(ch, "key_digit") for ch in inner))
                elif inner.startswith("MASTER"):
                    out.append(get_badge_img(inner, "badge_master"))
                elif any(inner.startswith(pfx) for pfx in ("KART-YABANCI", "QR-YABANCI")):
                    out.append(get_badge_img(inner, "badge_foreign"))
                elif any(inner.startswith(pfx) for pfx in ("KART-", "QR-")):
                    out.append(get_badge_img(inner, "badge_user"))
                elif any(hw in inner for hw in ("+12V", "GND", "COM", "NO", "IN1", "IN2", "D0 / D1")):
                    out.append(get_badge_img(inner, "badge_hw"))
                elif inner.startswith("⏳"):
                    out.append(get_badge_img(inner, "badge_wait"))
                else:
                    out.append(get_badge_img(inner, "badge_action"))
            else:
                out.append(f'<span style="color:#94A3B8; font-weight:600; font-size:12px;">&nbsp;{html.escape(p)}&nbsp;</span>')
        return "".join(out)

    def _render_step_instruction_html(self, step: Step) -> str:
        if not step.rows:
            instr_html = self._substitute_vars(step.instruction).replace("\n", "<br>")
            return f"<div style='font-family:Segoe UI;'>{instr_html}</div>"

        rows_tr = ""
        for idx, (raw_cmd, raw_desc) in enumerate(step.rows, 1):
            bg = "#162032" if idx % 2 == 1 else "#1E293B"
            num_badge = get_badge_img(str(idx), "circle_num")
            talimat_html = self._format_talimat_tokens(raw_cmd)
            desc_html = self._substitute_vars(raw_desc, for_regex=False)
            rows_tr += f"""
            <tr style="background-color: {bg};">
                <td width="34" align="center" valign="middle" style="padding: 7px 4px; border-bottom: 1px solid #334155;">{num_badge}</td>
                <td width="48%" valign="middle" style="padding: 7px 8px; border-bottom: 1px solid #334155;">{talimat_html}</td>
                <td width="47%" valign="middle" style="padding: 7px 10px; border-bottom: 1px solid #334155; color: #E2E8F0; font-size: 12.5px;">{desc_html}</td>
            </tr>
            """

        note_html = ""
        if step.instruction and step.instruction.strip():
            note_text = self._substitute_vars(step.instruction.strip(), for_regex=False).replace("\n", "<br>")
            note_html = f"""
            <div style="margin-top: 8px; background-color: #0F172A; border-left: 3px solid #38BDF8; padding: 6px 10px; color: #CBD5E1; font-size: 12px;">
                💡 <b>Not / İpucu:</b> {note_text}
            </div>
            """

        return f"""
        <div style="font-family: 'Segoe UI', sans-serif;">
            <table width="100%" cellspacing="0" cellpadding="0" style="border-collapse: collapse; border: 1px solid #334155;">
                <thead>
                    <tr style="background-color: #0F172A;">
                        <th width="34" align="center" style="padding: 6px 4px; color: #38BDF8; font-size: 11px; font-weight: bold; border-bottom: 2px solid #334155;">#</th>
                        <th width="48%" align="left" style="padding: 6px 8px; color: #38BDF8; font-size: 11px; font-weight: bold; border-bottom: 2px solid #334155;">TALİMAT (TUŞLAMA / İŞLEM)</th>
                        <th width="47%" align="left" style="padding: 6px 10px; color: #38BDF8; font-size: 11px; font-weight: bold; border-bottom: 2px solid #334155;">AÇIKLAMA (NE YAPILIYOR?)</th>
                    </tr>
                </thead>
                <tbody>
                    {rows_tr}
                </tbody>
            </table>
            {note_html}
        </div>
        """

    def _update_step_ui(self):
        step = self.steps[self.current_step_idx]

        # Title
        self.lbl_step_id_title.setText(
            f"<b>ADIM {self.current_step_idx + 1} / {len(self.steps)} — [{step.id}] {step.title}</b>"
        )

        # Instruction Table + Badges
        self.txt_instruction.setHtml(self._render_step_instruction_html(step))

        # Step Action Button
        if step.send:
            self.btn_step_action.setVisible(True)
            if any("onloi_fab_reset" in cmd for cmd in step.send):
                self.btn_step_action.setText("⚠️ Fabrika Sıfırlamayı Başlat (onloi_fab_reset)")
                self.btn_step_action.setStyleSheet(
                    "background-color: #D97706; border: 1px solid #B45309; border-radius: 6px; "
                    "padding: 10px 14px; font-size: 13.5px; font-weight: bold; color: white;"
                )
            elif any("onloi_ota_info" in cmd for cmd in step.send):
                self.btn_step_action.setText("🔍 Model ve Sürümü Sorgula (onloi_ota_info)")
                self.btn_step_action.setStyleSheet(
                    "background-color: #0284C7; border: 1px solid #0369A1; border-radius: 6px; "
                    "padding: 10px 14px; font-size: 13.5px; font-weight: bold; color: white;"
                )
            elif any("onloi_osdp_info" in cmd for cmd in step.send):
                self.btn_step_action.setText("📡 OSDP Durumunu Sorgula (onloi_osdp_info)")
                self.btn_step_action.setStyleSheet(
                    "background-color: #0284C7; border: 1px solid #0369A1; border-radius: 6px; "
                    "padding: 10px 14px; font-size: 13.5px; font-weight: bold; color: white;"
                )
            elif any("onloi_ota_req" in cmd for cmd in step.send):
                self.btn_step_action.setText("🔄 Cihazdan OTA Güncelleme İsteği Gönder (onloi_ota_req)")
                self.btn_step_action.setStyleSheet(
                    "background-color: #7C3AED; border: 1px solid #6D28D9; border-radius: 6px; "
                    "padding: 10px 14px; font-size: 13.5px; font-weight: bold; color: white;"
                )
            else:
                self.btn_step_action.setText(f"🚀 Komutu Çalıştır: {', '.join(step.send)}")
                self.btn_step_action.setStyleSheet(
                    "background-color: #0284C7; border: 1px solid #0369A1; border-radius: 6px; "
                    "padding: 10px 14px; font-size: 13.5px; font-weight: bold; color: white;"
                )
        else:
            self.btn_step_action.setVisible(False)

        # Observation
        if step.observe:
            self.lbl_observe.setVisible(True)
            self.lbl_observe.setText(f"👁️ <b>Gözlem / Beklenen Tepki:</b> {self._substitute_vars(step.observe)}")
        else:
            self.lbl_observe.setVisible(False)

        # Window Bar
        self.bar_window.setVisible(self.window_deadline is not None)

        # Expect Items Status
        if step.expect:
            self.group_expect.setVisible(True)
            lines = []
            for e_idx, item in enumerate(step.expect):
                label = item[0]
                needed = item[2] if len(item) > 2 else 1
                got = self.current_expect_counts.get(e_idx, 0)
                if got >= needed:
                    lines.append(f"<span style='color:#10B981;'>✔ [{got}/{needed}] {label}</span>")
                else:
                    lines.append(f"<span style='color:#F59E0B;'>⏳ [{got}/{needed}] {label}</span>")
            self.lbl_expect_status.setText("<br>".join(lines))
        else:
            self.group_expect.setVisible(False)

        # Manual Question
        if step.manual:
            self.frame_manual.setVisible(True)
            q_text = self._substitute_vars(step.manual)
            self.lbl_manual_q.setText(f"❓ <b>Fiziksel Doğrulama Sorusu:</b><br>{q_text}")
        else:
            self.frame_manual.setVisible(False)

    def _on_step_list_selected(self, row: int):
        if row >= 0 and row < len(self.steps) and row != self.current_step_idx:
            self._activate_step(row)

    # -------------------------------------------------------------------------
    # SERIAL LOG MATCHING & STEP AUTOMATION ENGINE
    # -------------------------------------------------------------------------
    def _extract_device_metadata_from_line(self, line: str):
        # 1. Check 6-byte MAC address in boot logs (e.g. bluetooth mac: xx:xx:xx:aa:bb:cc or mode : sta (xx:xx:xx:aa:bb:cc))
        m_mac = re.search(
            r"(?:bluetooth mac|mode\s*:\s*sta\s*\(|MAC\s*:)\s*[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}:([0-9a-fA-F]{2}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})",
            line, re.IGNORECASE
        )
        if m_mac:
            mac6 = (m_mac.group(1) + m_mac.group(2) + m_mac.group(3)).upper()
            if mac6 != self.detected_mac6:
                self.detected_mac6 = mac6
                self.detected_serial = f"{self.current_model}_{mac6}"
                self._update_serial_badge()
                self._save_session()
                self.append_console(f"🆔 Cihaz Seri Numarası Algılandı: {self.detected_serial}", "#34D399")

        # 2. Check SMART_XXXXXX (6 hex digits of BT MAC)
        m_smart = re.search(r"SMART_([0-9a-fA-F]{6})", line)
        if m_smart:
            mac6 = m_smart.group(1).upper()
            if mac6 != self.detected_mac6:
                self.detected_mac6 = mac6
                self.detected_serial = f"{self.current_model}_{mac6}"
                self._update_serial_badge()
                self._save_session()
                self.append_console(f"🆔 Cihaz Seri Numarası Algılandı: {self.detected_serial}", "#34D399")

        # 3. Check BLE Name (e.g. Project Code: OE0113, BLE Name: OE0113_A1B2 or Using BLE Name: OE0113_A1B2)
        m_ble = re.search(r"(?:BLE Name|Using BLE Name)\s*:\s*(OE\d+_[0-9a-fA-F]+)", line)
        if m_ble:
            ble_name = m_ble.group(1).strip()
            self.captured_vars["BLE"] = ble_name
            if not self.detected_mac6 and ble_name != self.detected_serial:
                self.detected_serial = ble_name
                self._update_serial_badge()
                self._save_session()
                self.append_console(f"🆔 Cihaz Seri Numarası (BLE) Algılandı: {self.detected_serial}", "#34D399")

        # 4. Sync variables from onloi_master_list_view
        m_master = re.search(r"Master\s+(\d+)\s+\(ID\s+(\d+)\):\s+(\S+)", line)
        if m_master:
            m_idx, m_id, m_val = m_master.group(1), m_master.group(2), m_master.group(3)
            if m_id == "10000" or m_idx == "1":
                self.captured_vars["M1"] = m_val
                self.captured_vars["MQ1"] = m_val
            elif m_id == "10005" or m_idx == "2":
                self.captured_vars["M2"] = m_val
                self.captured_vars["MQ2"] = m_val

        # 5. Sync variables from onloi_user_list_view
        m_user = re.search(r"User\s+(\d+):\s+PIN=(\S*),\s+CARD=(\S*),\s+QR=(\S*)", line)
        if m_user:
            u_id, u_pin, u_card, u_qr = m_user.group(1), m_user.group(2), m_user.group(3), m_user.group(4)
            if u_pin in ("2580", "3690"):
                self.captured_vars["PINID"] = u_id
            if len(u_qr) >= 24:
                self.captured_vars["QLID"] = u_id
                self.captured_vars["QL"] = u_qr

    def _on_serial_line(self, line: str):
        self.append_console(line)
        self._extract_device_metadata_from_line(line)

        if self.current_step_idx >= len(self.steps):
            return

        step = self.steps[self.current_step_idx]

        # 1. Fail-On Check
        for f_label, f_regex in step.fail_on:
            pat = self._substitute_vars(f_regex, for_regex=True)
            if re.search(pat, line):
                self.append_console(f"❌ ADIM BAŞARISIZ OLDU: {f_label} (Eşleşti: '{pat}')", "#EF4444")
                self.record_step_result(passed=False, note=f"Hata Kriteri Tetiklendi: {f_label}")
                return

        # 2. Measurement Check (Duration between two events)
        if step.measure:
            start_re, end_re, target_sec, tol_sec = step.measure
            if re.search(start_re, line) and self.measure_start_time is None:
                self.measure_start_time = time.time()
                self.append_console(f"⏱️ Süre ölçümü başladı: {start_re}", "#38BDF8")
            elif re.search(end_re, line) and self.measure_start_time is not None:
                dur = time.time() - self.measure_start_time
                self.measure_start_time = None
                self.append_console(f"⏱️ Süre ölçüldü: {dur:.2f} sn (Hedef: {target_sec} ± {tol_sec} sn)", "#38BDF8")
                if abs(dur - target_sec) > tol_sec:
                    self.append_console(f"❌ Süre tolerans dışı: {dur:.2f} sn!", "#EF4444")
                    self.record_step_result(passed=False, note=f"Süre Hatası: {dur:.2f} sn (Hedef: {target_sec}±{tol_sec})")
                    return

        # 3. Window Check (Trigger visual countdown)
        if step.window and self.window_deadline is None:
            win_re, win_sec, win_title = step.window
            if re.search(win_re, line):
                self.window_deadline = time.time() + win_sec
                self.bar_window.setMaximum(win_sec * 10)
                self.bar_window.setValue(win_sec * 10)
                self.bar_window.setFormat(f"{win_title}: %v sn kaldı")
                self.bar_window.setVisible(True)
                self.append_console(f"⏳ Zaman Penceresi Başladı: {win_title} ({win_sec} sn)", "#F59E0B")

        # 4. Expect Items Matching
        if step.expect:
            all_satisfied = True
            any_updated = False
            for e_idx, item in enumerate(step.expect):
                label = item[0]
                pat = self._substitute_vars(item[1], for_regex=True)
                needed = item[2] if len(item) > 2 else 1
                current_cnt = self.current_expect_counts.get(e_idx, 0)

                if current_cnt < needed:
                    m = re.search(pat, line)
                    if m:
                        # Capture named groups if any
                        for k, v in m.groupdict().items():
                            if v:
                                self.captured_vars[k] = v
                                self.append_console(f"🔑 Değişken Yakalandı: <<{k}>> = '{v}'", "#A855F7")
                        self.current_expect_counts[e_idx] = current_cnt + 1
                        any_updated = True

                if self.current_expect_counts.get(e_idx, 0) < needed:
                    all_satisfied = False

            if any_updated:
                self._update_step_ui()

            # If all expected lines are captured:
            if any_updated and all_satisfied:
                self.append_console(f"✔ Adım {step.id} için tüm seri port şartları doğrulandı!", "#10B981")
                self.btn_pass.setText("✅ DOĞRULANDI - GEÇİŞ İÇİN TIKLAYIN (PASS) ➔")
                self.btn_pass.setStyleSheet(
                    "background-color: #16A34A; border: 2px solid #4ADE80; font-size: 14px; font-weight: bold; color: white;"
                )
                if step.verify and self.serial_worker and self.serial_worker.isRunning():
                    verify_cmd = step.verify[0]
                    self.serial_worker.send_line(verify_cmd)
                if not step.manual and self.chk_autoadvance.isChecked():
                    QTimer.singleShot(1500, lambda: self.record_step_result(passed=True, note="Seri porttan tam otomatik doğrulandı."))

    # -------------------------------------------------------------------------
    # TIMER TICK (Timeouts, countdowns)
    # -------------------------------------------------------------------------
    def _on_ui_tick(self):
        if self.current_step_idx >= len(self.steps):
            return

        step = self.steps[self.current_step_idx]
        now = time.time()

        # Update window countdown
        if self.window_deadline:
            rem = max(0.0, self.window_deadline - now)
            self.bar_window.setValue(int(rem * 10))
            if rem <= 0:
                self.window_deadline = None
                self.bar_window.setVisible(False)

        # Timeout Check: Normal test adımlarında süre sınırı yoktur (operatör istediği kadar bekleyebilir).
        # Sadece inaktivite veya negatif koruma testlerinde (pass_on_timeout == True) süre dolduğunda otomatik PASS verilir.
        elapsed = now - self.step_start_time
        if step.pass_on_timeout and elapsed > step.timeout:
            self.append_console(f"✔ Bekleme süresi doldu ve hiçbir hata tetiklenmedi ({step.id} GEÇTİ).", "#10B981")
            self.record_step_result(passed=True, note="Zaman aşımı koruması / inaktivite testi başarıyla sağlandı.")

    # -------------------------------------------------------------------------
    # STEP COMPLETION & NAVIGATION
    # -------------------------------------------------------------------------
    def record_step_result(self, passed: bool, note: str = ""):
        step = self.steps[self.current_step_idx]
        status_str = "PASS" if passed else "FAIL"
        icon_str = "✅" if passed else "❌"

        self.step_results[step.id] = {
            "title": step.title,
            "section": step.section,
            "status": status_str,
            "timestamp": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "note": note
        }

        # Update List Item
        item = self.list_steps.item(self.current_step_idx)
        if item:
            item.setText(f"{icon_str} {step.id}: {step.title}")
            color = QtGui.QColor("#15803D" if passed else "#B91C1C")
            item.setForeground(QtGui.QBrush(color))

        # Advance Progress Bar
        completed = sum(1 for v in self.step_results.values() if v["status"] in ["PASS", "FAIL", "SKIP"])
        self.progress_bar.setValue(completed)

        self._save_session()

        # Advance to next step
        next_idx = self.current_step_idx + 1
        if next_idx < len(self.steps):
            self._activate_step(next_idx)
        else:
            self._on_all_steps_completed()

    def retry_current_step(self):
        self.append_console(f"🔄 Adım {self.steps[self.current_step_idx].id} sıfırlandı.", "#F59E0B")
        self._activate_step(self.current_step_idx)

    def skip_current_step(self):
        step = self.steps[self.current_step_idx]
        self.step_results[step.id] = {
            "title": step.title,
            "section": step.section,
            "status": "SKIP",
            "timestamp": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "note": "Kullanıcı tarafından atlandı."
        }
        item = self.list_steps.item(self.current_step_idx)
        if item:
            item.setText(f"⏭️ {step.id}: {step.title}")
            item.setForeground(QtGui.QBrush(QtGui.QColor("#94A3B8")))

        completed = sum(1 for v in self.step_results.values() if v["status"] in ["PASS", "FAIL", "SKIP"])
        self.progress_bar.setValue(completed)

        self._save_session()

        next_idx = self.current_step_idx + 1
        if next_idx < len(self.steps):
            self._activate_step(next_idx)
        else:
            self._on_all_steps_completed()

    def _on_all_steps_completed(self):
        passed = sum(1 for v in self.step_results.values() if v["status"] == "PASS")
        failed = sum(1 for v in self.step_results.values() if v["status"] == "FAIL")
        skipped = sum(1 for v in self.step_results.values() if v["status"] == "SKIP")

        msg = (
            f"Tüm test adımları tamamlandı!\n\n"
            f"✅ Başarılı (PASS) : {passed}\n"
            f"❌ Hatalı (FAIL)   : {failed}\n"
            f"⏭️ Atlanan (SKIP)  : {skipped}\n\n"
            f"Test raporunu kaydetmek ister misiniz?"
        )
        ans = QtWidgets.QMessageBox.information(
            self, "Test Tamamlandı", msg,
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
        )
        if ans == QtWidgets.QMessageBox.Yes:
            self.export_report()

    # -------------------------------------------------------------------------
    # REPORT EXPORT (HTML & CSV)
    # -------------------------------------------------------------------------
    def export_report(self):
        reports_dir = os.path.join(SCRIPT_DIR, "Reports")
        os.makedirs(reports_dir, exist_ok=True)

        now_str = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        tester = self.txt_tester.text().strip() or "Bilinmiyor"
        sn = self.detected_serial or self.captured_vars.get("BLE") or f"{self.current_model}_Bilinmiyor"
        model = self.current_model

        html_path = os.path.join(reports_dir, f"Onloi_Test_Raporu_{model}_{now_str}.html")
        csv_path = os.path.join(reports_dir, f"Onloi_Test_Raporu_{model}_{now_str}.csv")

        # HTML Generation
        rows_html = ""
        total_p = 0
        total_f = 0
        total_s = 0

        for s in self.steps:
            r = self.step_results.get(s.id, {
                "status": "BEKLIYOR",
                "timestamp": "-",
                "note": "Uygulanmadı"
            })
            st = r["status"]
            if st == "PASS":
                bg = "#DCFCE7"
                col = "#15803D"
                total_p += 1
            elif st == "FAIL":
                bg = "#FEE2E2"
                col = "#B91C1C"
                total_f += 1
            elif st == "SKIP":
                bg = "#F1F5F9"
                col = "#64748B"
                total_s += 1
            else:
                bg = "#FEF3C7"
                col = "#D97706"

            rows_html += f"""
            <tr>
                <td style="font-weight:bold; font-family:monospace;">{s.id}</td>
                <td>{s.section}</td>
                <td><b>{s.title}</b></td>
                <td style="background-color:{bg}; color:{col}; font-weight:bold; text-align:center;">{st}</td>
                <td>{r['timestamp']}</td>
                <td style="color:#475569; font-size:12px;">{r.get('note', '')}</td>
            </tr>
            """

        overall_result = "ONAYLANDI (PASS)" if total_f == 0 and total_p > 0 else "RED (FAIL)"
        overall_color = "#15803D" if overall_result.startswith("ONAY") else "#B91C1C"

        html_content = f"""<!DOCTYPE html>
<html lang="tr">
<head>
    <meta charset="UTF-8">
    <title>Onloi DVT Test Raporu - {model} ({APP_VERSION})</title>
    <style>
        body {{ font-family: 'Segoe UI', Arial, sans-serif; margin: 30px; background-color: #F8FAFC; color: #0F172A; }}
        .header {{ border-bottom: 3px solid #003366; padding-bottom: 12px; margin-bottom: 20px; }}
        h1 {{ color: #003366; margin: 0 0 6px 0; font-size: 24px; }}
        .sub {{ color: #64748B; font-size: 14px; margin: 0; }}
        .info-card {{ background-color: white; border: 1px solid #E2E8F0; border-radius: 8px; padding: 15px; margin-bottom: 20px; display: flex; justify-content: space-between; }}
        .badge {{ padding: 6px 14px; border-radius: 6px; font-weight: bold; color: white; display: inline-block; font-size: 16px; background-color: {overall_color}; }}
        table {{ width: 100%; border-collapse: collapse; background-color: white; border-radius: 8px; overflow: hidden; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }}
        th {{ background-color: #003366; color: white; text-align: left; padding: 10px 12px; font-size: 13px; }}
        td {{ padding: 9px 12px; border-bottom: 1px solid #E2E8F0; font-size: 13px; }}
        tr:hover {{ background-color: #F8FAFC; }}
        .footer {{ margin-top: 30px; padding-top: 15px; border-top: 1px solid #CBD5E1; color: #64748B; font-size: 12px; display: flex; justify-content: space-between; }}
    </style>
</head>
<body>
    <div class="header">
        <h1>ONLOI GEÇİŞ KONTROL TERMİNALİ — DVT TEST DOĞRULAMA RAPORU</h1>
        <p class="sub">Otomatik Oluşturulan Saha Öncesi Test ve Kalite Güvence Belgesi</p>
    </div>

    <div class="info-card">
        <div>
            <b>Model:</b> {model} ({MODELS[model]['title']})<br>
            <b>Yazılım Versiyonu:</b> Test Asistanı {APP_VERSION}<br>
            <b>Firmware Versiyonu:</b> {TARGET_FW_VERSION}<br>
            <b>Cihaz Seri No / MAC:</b> {sn}
        </div>
        <div>
            <b>Test Personeli:</b> {tester}<br>
            <b>Test Tarihi:</b> {datetime.datetime.now().strftime("%d.%m.%Y %H:%M:%S")}<br>
            <b>Özet:</b> {total_p} Başarılı / {total_f} Hatalı / {total_s} Atlanan
        </div>
        <div style="text-align:right;">
            <b>Genel Sonuç:</b><br>
            <span class="badge">{overall_result}</span>
        </div>
    </div>

    <table>
        <thead>
            <tr>
                <th style="width:75px;">Adım No</th>
                <th style="width:160px;">Bölüm</th>
                <th>Test Başlığı</th>
                <th style="width:90px; text-align:center;">Sonuç</th>
                <th style="width:150px;">Zaman</th>
                <th>Gözlem ve Doğrulama Notları</th>
            </tr>
        </thead>
        <tbody>
            {rows_html}
        </tbody>
    </table>

    <div class="footer">
        <div>Onloi DVT Test Asistanı {APP_VERSION} (Hedef Cihaz FW: {TARGET_FW_VERSION})</div>
        <div>İmza: ......................................................</div>
    </div>
</body>
</html>
"""
        with open(html_path, "w", encoding="utf-8") as f:
            f.write(html_content)

        # CSV Generation
        with open(csv_path, "w", encoding="utf-8-sig") as f:
            f.write(f"# Onloi DVT Test Asistani {APP_VERSION} | Cihaz FW: {TARGET_FW_VERSION} | Model: {model} | Seri No: {sn} | Personel: {tester}\n")
            f.write("Adim_No;Bolum;Baslik;Sonuc;Zaman;Not\n")
            for s in self.steps:
                r = self.step_results.get(s.id, {"status": "BEKLIYOR", "timestamp": "-", "note": ""})
                clean_note = r.get("note", "").replace(";", " ")
                f.write(f"{s.id};{s.section};{s.title};{r['status']};{r['timestamp']};{clean_note}\n")

        QtWidgets.QMessageBox.information(
            self, "Rapor Kaydedildi",
            f"Test raporları başarıyla oluşturuldu:\n\n"
            f"HTML: {html_path}\n"
            f"CSV: {csv_path}"
        )
        self.append_console(f"📄 Rapor kaydedildi: {html_path}", "#10B981")


# =============================================================================
# ENTRY POINT
# =============================================================================
def main():
    # Explicit Windows AppUserModelID so taskbar icon displays our custom Tester icon
    if sys.platform == "win32":
        try:
            import ctypes
            myappid = f'onloi.accesscontrol.dvtassistant.{APP_VERSION}'
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(myappid)
        except Exception:
            pass

    QtWidgets.QApplication.setAttribute(Qt.AA_EnableHighDpiScaling, True)
    QtWidgets.QApplication.setAttribute(Qt.AA_UseHighDpiPixmaps, True)
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName(f"Onloi DVT Test Asistanı {APP_VERSION}")
    app.setApplicationVersion(APP_VERSION)
    app.setStyleSheet(MODERN_STYLE)

    icon_path = os.path.join(RESOURCE_DIR, "app_icon.ico")
    if not os.path.exists(icon_path):
        icon_path = os.path.join(APP_DIR, "app_icon.ico")
    if os.path.exists(icon_path):
        app.setWindowIcon(QtGui.QIcon(icon_path))

    window = OnloiDvtApp()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
