# -*- coding: utf-8 -*-
"""
Onloi DVT Test Asistanı - Test Adımı Tanımları (Sürümden Bağımsız / v2.0.27+ Uyumlu)

Her adım bir Step nesnesidir. Motor (onloi_dvt_assistant.py) şu alanları kullanır:

  id, section, title : Ekranda görünen kimlik / bölüm / başlık
  rows               : [(talimat_tokenlari, aciklama_metni)] -> Görsel Talimat - Açıklama tablosu satırları
  instruction        : Tablo altında gösterilecek ek not / ipucu (opsiyonel)
  observe            : Cihazda görülmesi/duyulması gereken fiziksel tepki (personel için açıklama)
  send               : Adım butonuna basıldığında seri porttan gönderilecek CLI komutları
  expect             : [(etiket, regex, adet)] -> Seri port logunda görülmesi gereken satırlar
  fail_on            : [(etiket, regex)] -> Görülürse adım anında BAŞARISIZ olur
  verify             : (komut, [(etiket, regex)]) -> expect tamamlanınca CLI komutu gönderilip
                       çıktısı kontrol edilir (ör. onloi_user_list_view)
  measure            : (başlangıç_regex, bitiş_regex, beklenen_sn, tolerans_sn) -> süre ölçümü
  window             : (regex, saniye, etiket) -> regex görülünce ekranda geri sayım başlatır
  manual             : Otomatik doğrulamadan sonra personele sorulacak EVET/HAYIR sorusu
  timeout            : Adım için maksimum bekleme süresi (sn)
  pass_on_timeout    : True ise süre dolduğunda fail_on görülmediyse adım GEÇER
"""
from dataclasses import dataclass, field
import re
from typing import List, Optional, Tuple

ENTER_MENU_TOKENS = "[*] [1] [2] [3] [4] [5] [6] [#]"


@dataclass
class Step:
    id: str
    section: str
    title: str
    instruction: str = ""
    rows: List[Tuple[str, str]] = field(default_factory=list)
    observe: str = ""
    send: List[str] = field(default_factory=list)
    expect: List[Tuple] = field(default_factory=list)
    fail_on: List[Tuple[str, str]] = field(default_factory=list)
    verify: Optional[Tuple[str, List[Tuple[str, str]]]] = None
    measure: Optional[Tuple[str, str, float, float]] = None
    window: Optional[Tuple[str, int, str]] = None
    manual: Optional[str] = None
    timeout: int = 120
    pass_on_timeout: bool = False
    need_master: bool = False
    need_user: bool = False


MODELS = {
    "OE0111": {"pid": "0111", "title": "PIN + RFID", "keypad": True, "rfid": True, "qr": False},
    "OE0112": {"pid": "0112", "title": "Sadece RFID (Tuşsuz)", "keypad": False, "rfid": True, "qr": False},
    "OE0113": {"pid": "0113", "title": "PIN + QR", "keypad": True, "qr": True, "rfid": False},
}

# Sık kullanılan log kalıpları
T_IDLE_PROG = ("Programlama moduna girildi", r"State Transition: IDLE -> PROGRAMMING")
T_PROG_IDLE = ("Programlama modundan çıkıldı", r"State Transition: PROGRAMMING -> IDLE")
T_DOOR_OPEN = ("Kapı açıldı (DOOR_OPEN)", r"State Transition: IDLE -> DOOR_OPEN")
T_DOOR_CLOSE = ("Kapı kapandı (IDLE)", r"State Transition: DOOR_OPEN -> IDLE")
T_BANNER = ("Cihaz yeniden başladı (açılış banner)", r"Access Control Terminal Firmware")
F_HW_WARN = ("Donanım modülü yanıt vermedi!", r"WARNING: .* did NOT respond")


# =============================================================================
# ORTAK BAŞLANGIÇ ADIMLARI
# =============================================================================
def prep_steps(model: str, fw: str) -> List[Step]:
    m = MODELS[model]
    fw_escaped = re.escape(fw)
    if m["rfid"]:
        card_tokens = "[MASTER-KART-1] [MASTER-KART-2] [KART-1] [KART-2] [KART-YABANCI]"
        card_desc = "Masanıza 5 adet 13.56 MHz Mifare kart koyun ve üzerlerini kalemle etiketleyin."
        extra_note = ""
    else:
        card_tokens = "[MASTER-QR-1] [MASTER-QR-2] [QR-KISA-1] [QR-UZUN-1] [QR-YABANCI]"
        card_desc = "Masanıza 5 adet test QR kod çıktısını hazırlayın ve isimlerini yazın."
        extra_note = (
            "💡 <b>QR İçerik Önerileri:</b> "
            "MASTER-QR-1 = <b>99887766</b> &nbsp;|&nbsp; MASTER-QR-2 = <b>55443322</b> &nbsp;|&nbsp; "
            "QR-KISA-1 = <b>12345678</b> &nbsp;|&nbsp; QR-YABANCI = <b>11112222</b> &nbsp;|&nbsp; "
            "QR-UZUN-1 = <b>ONLOI_LONG_QR_CREDENTIAL_2026_TEST_STRING_001</b> <i>(>31 karakter)</i>"
        )

    steps = [
        Step(
            id="P-00", section="0. Hazırlık", title="Test masası ve etiketli kartların hazırlanması",
            rows=[
                (card_tokens, card_desc),
                ("[+12V] [GND]", "Cihazın besleme uçlarını <b>12V DC (en az 1A)</b> adaptöre bağlayın."),
                ("[COM] [NO]", "Röle COM ve NO uçlarına multimetreyi <b>Kısa Devre / Buzzer</b> modunda bağlayın."),
                ("[IN1] [IN2] -> [+12V]", "Exit (IN1) ve Reset (IN2) testleri için +12V'a bağlı bir tetik kablosu hazır bulundurun <i>(şimdilik değdirmeyin)</i>."),
                ("[🔌 Bağlan]", "USB-UART kablosunu bilgisayara takın, üstten COM portu seçip <b>Bağlan</b> butonuna basın."),
            ],
            instruction=extra_note,
            manual="Kartlar/QR kodlar etiketlendi, bağlantılar yapıldı ve program cihaza bağlandı mı?",
        ),
        Step(
            id="P-01", section="0. Hazırlık", title="Model ve firmware kimlik doğrulaması",
            rows=[
                ("[🔍 Model ve Sürümü Sorgula]", "Aşağıdaki mavi sorgulama butonuna tıklayarak <b>onloi_ota_info</b> komutunu gönderin."),
                ("[UART Doğrulama]", f"Cihazın <b>{model} (0x{m['pid']})</b> modelini ve güncel firmware sürümünü (<b><<FW>></b>) raporladığını doğrulayın."),
            ],
            send=["onloi_ota_info"],
            expect=[
                (f"Model: {model} (0x{m['pid']})", rf"Product ID\s*:\s*0x{m['pid']}\s*\({model}\)"),
                ("Firmware sürümü algılandı", r"Firmware Version\s*:.*\((?P<FW_VER>v\d+\.\d+\.\d+)\)"),
            ],
            manual="Model ve firmware sürümü seri port logunda doğrulandı mı?",
            timeout=20,
        ),
        Step(
            id="P-02", section="0. Hazırlık", title="Fabrika ayarlarına sıfırlama ve açılış donanım kontrolü",
            rows=[
                ("[⚠️ Fabrika Sıfırlamayı Başlat]", "Aşağıdaki turuncu sıfırlama butonuna tıklayın ve onay penceresinde <b>Evet</b>'e basın."),
                ("[⏳ Bekleyin]", "Cihaz hafızayı silip yeniden başlayacaktır. <b>Açılış melodisi bitene kadar hiçbir tuşa veya karta dokunmayın.</b>"),
                ("[LED & Buzzer]", "Açılışta melodi duyulduğunu, seri numarasının (BLE Name) algılandığını ve LED'in <b>Kırmızı Yavaş</b> yanıp söndüğünü gözlemleyin."),
            ],
            observe="Turuncu LED + 1 sn uzun bip → yeniden başlama → açılış melodisi → LED kırmızı yavaş yanıp söner.",
            send=["onloi_fab_reset"],
            expect=[
                ("Fabrika reset komutu alındı", r"\[CLI\] Triggering Factory Reset"),
                ("Hafıza silindi", r"Wipe complete\. Restarting system"),
                T_BANNER,
                ("Donanım versiyonu raporlandı", r"FIRMWARE COMPILED FOR HARDWARE VERSION"),
                ("BLE adı ve Seri No oluşturuldu", rf"Project Code: {model}, BLE Name: (?P<BLE>\S+)"),
            ],
            fail_on=[F_HW_WARN],
            manual="Açılışta melodi duyuldu mu ve LED şu an KIRMIZI YAVAŞ yanıp sönüyor mu?",
            timeout=40,
        ),
    ]
    if m["keypad"]:
        steps.append(Step(
            id="P-03", section="0. Hazırlık", title="Dokunmatik tuş takımı - 12 tuşun tek tek kontrolü",
            rows=[
                ("[1] [2] [3] [4] [5] [6] [7] [8] [9] [0]", "Rakam tuşlarına sırayla tek tek basın. <i>(İlk dokunuş uyandırma için kullanılırsa o tuşa tekrar basın)</i>."),
                ("[*] -> [#]", "Son olarak Yıldız ve Kare tuşlarına basın. <i>(Sonda 3 hata bipi duyulması normaldir)</i>."),
            ],
            observe="Her tuşta kısa bip ve tuş arka ışığı.",
            expect=[(f"Tuş '{k}' algılandı", r"\[IDLE\] Key pressed: " + ("\\" + k if k in "*#" else k))
                    for k in "1234567890*#"],
            manual="Her tuşa basışta kısa bip duyuldu ve tuş ışıkları yandı mı?",
        ))
    return steps


# =============================================================================
# MENÜ 1 (Master Kod) - tuşlu modeller için ortak
# =============================================================================
def menu1_steps(px: str) -> List[Step]:
    sec = "1. Programlama Girişi ve Menü 1 (Master Kod)"
    return [
        Step(
            id=f"{px}-01", section=sec, title="Varsayılan Master Kod (123456) ile programlama girişi ve çıkış",
            rows=[
                (ENTER_MENU_TOKENS, "Varsayılan Master Kod (123456) ile <b>Programlama Moduna</b> girin <i>(2 kısa bip + Mavi LED hızlı yanıp söner)</i>."),
                ("[*]", "Yıldız tuşuna basarak Programlama Modundan çıkın ve <b>Bekleme Moduna (IDLE)</b> dönün."),
            ],
            observe="Giriş: 2 kısa bip + MAVİ LED HIZLI yanıp söner. Çıkış: kısa bip, kırmızı yavaş yanıp sönmeye döner.",
            expect=[T_IDLE_PROG, T_PROG_IDLE],
            manual="Programlama modunda MAVİ LED HIZLI yanıp söndü mü?",
        ),
        Step(
            id=f"{px}-02", section=sec, title="Menü 1: Master Kodu 654321 yapma",
            rows=[
                (ENTER_MENU_TOKENS, "Mevcut Master Kod (123456) ile <b>Programlama Moduna</b> girin."),
                ("[1]", "<b>Menü 1</b>'i (Master Kod Değiştirme) seçin <i>(1 kısa bip)</i>."),
                ("[6] [5] [4] [3] [2] [1] [#]", "Yeni Master Kodu (<b>654321</b>) girip kare ile onaylayın <i>(1 kısa bip)</i>."),
                ("[6] [5] [4] [3] [2] [1] [#]", "Doğrulama için yeni kodu tekrar girip kaydedin <i>(Yeşil LED + 2 kısa bip)</i>."),
                ("[*]", "Programlama modundan çıkıp bekleme moduna dönün."),
            ],
            observe="İkinci # sonrası YEŞİL LED + 2 kısa bip.",
            expect=[T_IDLE_PROG, ("Menü 1 açıldı", r"Menu 1: Update Master Code"), T_PROG_IDLE],
            manual="İkinci # sonrası YEŞİL LED yandı ve 2 kısa bip duyuldu mu?",
        ),
        Step(
            id=f"{px}-03", section=sec, title="Eski kodun reddi, yeni kodun kabulü, kodu 123456'ya geri alma",
            rows=[
                ("[*] [1] [2] [3] [4] [5] [6] [#]", "Eski Master Kodu deneyin → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 yavaş hata bipi)</i>."),
                ("[*] [6] [5] [4] [3] [2] [1] [#]", "Yeni Master Kod (654321) ile <b>Programlama Moduna</b> girin <i>(Kabul edilmeli)</i>."),
                ("[1] -> [1] [2] [3] [4] [5] [6] [#] -> [1] [2] [3] [4] [5] [6] [#]", "<b>Menü 1</b>'e girip Master Kodu tekrar varsayılan <b>123456</b> değerine geri alın."),
                ("[*]", "Programlama modundan çıkın."),
                ("[*] [1] [2] [3] [4] [5] [6] [#] -> [*]", "Varsayılan <b>123456</b> koduyla tekrar giriş yapılabildiğini doğrulayıp <b>*</b> ile çıkın."),
            ],
            observe="1. adımda KIRMIZI LED + 3 yavaş bip. Diğerlerinde onay bipleri.",
            expect=[
                ("Eski kod reddedildi", r"Master code incorrect"),
                ("Programlama girişi (2 kez)", r"State Transition: IDLE -> PROGRAMMING", 2),
                ("Menü 1 açıldı", r"Menu 1: Update Master Code"),
                ("Programlamadan çıkış (2 kez)", r"State Transition: PROGRAMMING -> IDLE", 2),
            ],
        ),
    ]


# =============================================================================
# OE0111 (PIN + RFID)
# =============================================================================
def oe0111_steps() -> List[Step]:
    s = menu1_steps("A")
    s2 = "2. Menü 2: Master Kart Ekleme"
    s3 = "3. Menü 3: Kullanıcı Ekleme ve Geçiş"
    s4 = "4. Master Kart ile Yönetim"
    s5 = "5. Menü 5 ve Kullanıcı PIN Değiştirme"
    s6 = "6. Menü 6 / Menü 7 Ayarları"
    s7 = "7. Menü 4: Kullanıcı Silme"
    s8 = "8. Menü 8: Master Kart Silme"
    s9 = "9. Menü 9: Fabrika Ayarı"
    s += [
        Step(
            id="A-04", section=s2, title="Menü 2: MASTER-KART-1 ekleme (otomatik ID)",
            rows=[
                (ENTER_MENU_TOKENS, "Varsayılan Master Kod ile <b>Programlama Moduna</b> girin."),
                ("[2]", "<b>Menü 2</b>'yi (Master Kart Ekleme) seçin."),
                ("[MASTER-KART-1]", "<b>MASTER-KART-1</b>'i okuyucuya okutun <i>(Otomatik ID 10000 ile kaydedilir, Yeşil LED + 2 bip)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            observe="Kart okutulunca YEŞİL LED + 2 kısa bip.",
            expect=[("Menü 2 açıldı", r"Menu 2: Add Master Card/QR"),
                    ("Master kart kaydedildi", r"\[PROG\] Master Card SAVED: (?P<M1>\d+)"), T_PROG_IDLE],
            verify=("onloi_master_list_view", [("Master listesinde MASTER-KART-1", r"Master 1 \(ID \d+\): <<M1>>"),
                                               ("Toplam Master = 1", r"Total Masters: 1\b")]),
        ),
        Step(
            id="A-05", section=s2, title="Menü 2: MASTER-KART-2'yi özel ID (10005) ile ekleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [1] [0] [0] [0] [5] [#]", "<b>Menü 2</b>'yi seçip özel Master ID olarak <b>10005#</b> tuşlayın."),
                ("[MASTER-KART-2]", "<b>MASTER-KART-2</b>'yi okutarak ID 10005'e kaydedin <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Master kart ID=10005 kaydedildi", r"\[PROG\] Master Card SAVED: ID=10005, Card=(?P<M2>\d+)"), T_PROG_IDLE],
            verify=("onloi_master_list_view", [("ID 10005 listede", r"\(ID 10005\): <<M2>>"),
                                               ("Toplam Master = 2", r"Total Masters: 2\b")]),
        ),
        Step(
            id="A-06", section=s2, title="Menü 2 negatif: aynı Master kartı tekrar ekleme reddi",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-KART-1]", "<b>Menü 2</b>'de zaten kayıtlı olan <b>MASTER-KART-1</b>'i tekrar okutun → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            observe="KIRMIZI LED + 3 yavaş hata bipi.",
            expect=[("Mükerrer master reddedildi", r"Master Card FAILED \(already exists or full\)"), T_PROG_IDLE],
            manual="KIRMIZI LED yandı ve 3 yavaş hata bipi duyuldu mu?",
        ),
        Step(
            id="A-07", section=s3, title="Menü 3: KART-1 ekleme ve aynı kartın tekrar eklenme reddi",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [KART-1]", "<b>Menü 3</b>'e (Kullanıcı Ekleme) girip <b>KART-1</b>'i okutun → <b>KABUL</b> <i>(Yeşil LED + 2 bip)</i>."),
                ("[KART-1]", "Menüden çıkmadan <b>KART-1</b>'i tekrar okutun → <b>MÜKERRER REDDİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Menü 3 açıldı", r"Menu 3: Add User"),
                    ("KART-1 kaydedildi", r"\[PROG\] User SAVED: UserID=\d+, Card=(?P<K1>\d+)"),
                    ("Mükerrer kart reddedildi", r"Card <<K1>> ALREADY registered"),
                    T_PROG_IDLE],
        ),
        Step(
            id="A-08", section=s3, title="Menü 3: KART-2 ekleme ve Master kart okutarak menüden çıkış",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [KART-2]", "<b>Menü 3</b>'te <b>KART-2</b>'yi okutarak kullanıcı olarak kaydedin <i>(Yeşil LED + 2 bip)</i>."),
                ("[MASTER-KART-1]", "Hiçbir tuşa basmadan <b>MASTER-KART-1</b>'i okutun → Cihaz otomatik olarak programlamadan çıkıp <b>IDLE</b> moduna dönmelidir."),
            ],
            need_master=True,
            expect=[("KART-2 kaydedildi", r"\[PROG\] User SAVED: UserID=\d+, Card=(?P<K2>\d+)"),
                    ("Master kart ile çıkış", r"Master card scanned in Add User -> Exit to IDLE")],
        ),
        Step(
            id="A-09", section=s3, title="Menü 3: PIN kullanıcı ekleme (2580) ve hatalı girişler",
            rows=[
                (ENTER_MENU_TOKENS + " -> [3]", "Programlama moduna girip <b>Menü 3</b>'ü (Kullanıcı Ekleme) seçin."),
                ("[2] [5] [8] [0] [#] -> [2] [5] [8] [0] [#]", "<b>2580</b> PIN kodunu iki kez girip onaylayın → <b>KABUL</b> <i>(Yeşil LED + 2 bip)</i>."),
                ("[1] [1] [1] [1] [#] -> [2] [2] [2] [2] [#]", "Birbiriyle uyuşmayan PIN girin → <b>ONAY UYUŞMAZLIĞI REDDİ</b> <i>(3 hata bipi)</i>."),
                ("[1] [2] [#]", "4 haneden kısa (geçersiz) PIN girin → <b>UZUNLUK REDDİ</b> <i>(3 hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("PIN onay bekleniyor (2 kez)", r"PIN entered\. Waiting for confirmation", 2),
                    ("PIN 2580 kaydedildi", r"User ADDED \(AutoID\) - UserID: (?P<PINID>\d+), PIN: 2580"),
                    ("Uyuşmayan PIN reddedildi", r"PIN Confirm failed"),
                    ("Geçersiz uzunluk reddedildi", r"Invalid input for Menu \d"),
                    T_PROG_IDLE],
        ),
        Step(
            id="A-10", section=s3, title="Kayıtlı KART-1 ile kapı açma (röle süresi 5 sn ölçümü)",
            rows=[
                ("[KART-1]", "Bekleme modunda <b>KART-1</b>'i okutun <i>(Yeşil LED yanar, röle çeker ve multimetre öter)</i>."),
                ("[⏳ 5 sn Bekle]", "Kapı rölesinin <b>~5 saniye</b> sonunda otomatik kapanmasını bekleyin <i>(Süre otomatik ölçülür)</i>."),
            ],
            need_user=True,
            observe="YEŞİL LED + röle 'TIK' + multimetre ~5 sn öter.",
            expect=[("KART-1 kabul edildi", r"Offline Mode Granted Access for RFID:\d+"), T_DOOR_OPEN, T_DOOR_CLOSE],
            measure=(r"IDLE -> DOOR_OPEN", r"DOOR_OPEN -> IDLE", 5.0, 1.0),
            manual="Röle 'TIK' sesiyle çekti, multimetre ~5 sn öttü ve YEŞİL LED yandı mı?",
        ),
        Step(
            id="A-11", section=s3, title="Kayıtlı PIN (2580) ile kapı açma",
            rows=[
                ("[2] [5] [8] [0] [#]", "Bekleme modunda kayıtlı <b>2580#</b> PIN kodunu tuşlayın <i>(Kapı açılır)</i>."),
                ("[⏳ 5 sn Bekle]", "Kapının ~5 saniye sonunda kapanmasını bekleyin."),
            ],
            need_user=True,
            expect=[("PIN 2580 kabul edildi", r"Offline Mode Granted Access for KEYPAD:2580"), T_DOOR_OPEN, T_DOOR_CLOSE],
        ),
        Step(
            id="A-12", section=s3, title="Yetkisiz kart ve yanlış PIN reddi",
            rows=[
                ("[KART-YABANCI]", "Kayıtsız <b>KART-YABANCI</b>'yı okutun → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi, röle çekmez)</i>."),
                ("[9] [9] [9] [9] [#]", "Kayıtsız <b>9999#</b> PIN kodunu tuşlayın → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi, röle çekmez)</i>."),
            ],
            observe="Her ikisinde KIRMIZI LED + 3 yavaş bip. Röle ÇEKMEMELİ.",
            expect=[("Yabancı kart reddedildi", r"Offline Mode Denied Access for RFID:"),
                    ("Yanlış PIN reddedildi", r"Offline Mode Denied Access for KEYPAD:9999")],
            fail_on=[("Kapı yetkisiz açıldı!", r"IDLE -> DOOR_OPEN")],
        ),
        Step(
            id="A-13", section=s4, title="Master Ekleme Modu: Master kart ile KART-YABANCI ekleme",
            rows=[
                ("[MASTER-KART-1]", "Bekleme modunda <b>MASTER-KART-1</b>'i 1 kez okutup çekin → <b>Master Ekleme Modu</b> <i>(Mavi LED yavaş yanıp söner)</i>."),
                ("[KART-YABANCI]", "<b>KART-YABANCI</b>'yı okutup çekin → Kullanıcı olarak kaydedilir <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "<b>*</b> tuşuna basarak <i>(veya 6 sn bekleyip MASTER-KART-1 okutarak)</i> moddan çıkın."),
            ],
            need_master=True,
            expect=[("Master Ekleme Moduna girildi", r"Master Card \(ID: -?\d+\) scanned in IDLE\. Entering Master Add Mode"),
                    ("KART-YABANCI eklendi", r"\[MASTER_MODE\] ADD: UserID=\d+, Card=(?P<KY>\d+)"),
                    ("Moddan çıkıldı", r"State Transition: MASTER_ADD_MODE -> IDLE")],
            manual="Master Ekleme Modunda MAVİ LED YAVAŞ yanıp söndü mü?",
        ),
        Step(
            id="A-14", section=s4, title="Master Silme Modu: Master kartı 2 kez okutarak KART-YABANCI silme",
            rows=[
                ("[MASTER-KART-1] -> [MASTER-KART-1]", "Bekleme modunda <b>MASTER-KART-1</b>'i 5 sn içinde art arda <b>2 kez</b> okutun → <b>Silme Modu</b> <i>(Kırmızı LED yavaş yanıp söner)</i>."),
                ("[KART-YABANCI]", "<b>KART-YABANCI</b>'yı okutun → Kullanıcı silinir <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "<b>*</b> tuşuna basarak Silme Modundan güvenli şekilde çıkın <i>(3. kez Master kart okutmayın, tüm kullanıcıları siler!)</i>."),
            ],
            need_master=True,
            expect=[("Silme moduna geçildi", r"-> switch to DELETE mode"),
                    ("KART-YABANCI silindi", r"\[MASTER_MODE\] DELETE: UserID=\d+, Card=\d+"),
                    ("Moddan çıkıldı", r"State Transition: MASTER_DELETE_MODE -> IDLE")],
            fail_on=[("3. okutma ile tüm kullanıcılar silindi!", r"3rd scan -> Delete ALL users")],
            manual="Silme modunda KIRMIZI LED YAVAŞ yanıp söndü ve 3 kısa bip duyuldu mu?",
        ),
        Step(
            id="A-15", section=s4, title="* + Master kart ile programlama moduna giriş",
            rows=[
                ("[*] -> [MASTER-KART-1]", "Bekleme modunda <b>*</b> tuşuna basın ve hemen <b>MASTER-KART-1</b>'i okutun → Şifresiz <b>Programlama Moduna</b> girilir."),
                ("[*]", "<b>*</b> tuşuna basarak Programlama Modundan çıkın."),
            ],
            need_master=True,
            expect=[("* + Master kart ile giriş", r"Master Card read after \*! Entering Programming Mode"), T_IDLE_PROG, T_PROG_IDLE],
        ),
        Step(
            id="A-16", section=s5, title="Menü 5: Özel ID'ye kart (ID 50) ve PIN (ID 60) ekleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[5] -> [5] [0] [#] -> [KART-YABANCI]", "<b>Menü 5</b>'i seçin, User ID olarak <b>50#</b> girin ve <b>KART-YABANCI</b>'yı okutarak ID 50'ye kaydedin."),
                ("[5] -> [6] [0] [#] -> [4] [5] [6] [7] [#]", "Tekrar <b>Menü 5</b>'i seçin, User ID <b>60#</b> girin ve <b>4567#</b> PIN kodunu ID 60'a kaydedin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Menü 5 açıldı", r"Menu 5: Block Card Add"),
                    ("ID 50'ye kart okutuldu", r"\[PROG\] RFID read: \d+ -> Specific ID Card Add"),
                    ("ID 60'a PIN eklendi", r"PIN added to Specific ID 60"),
                    T_PROG_IDLE],
            verify=("onloi_user_list_view", [("User 50 kartlı", r"User 50: PIN=, CARD=\d+"),
                                             ("User 60 PIN=4567", r"User 60: PIN=4567,")]),
        ),
        Step(
            id="A-17", section=s5, title="Menü 5: Seri (blok) kart ekleme - ID 100'den 3 adet",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[5] -> [1] [0] [0] [#]", "<b>Menü 5</b>'i seçip başlangıç User ID olarak <b>100#</b> girin."),
                ("[3] [#]", "Eklenecek ardışık kart adedi olarak <b>3#</b> girin."),
                ("[1] [2] [3] [4] [5] [6] [7] [8] [#]", "Başlangıç kart numarası olarak <b>12345678#</b> girin <i>(ID 100, 101 ve 102 otomatik oluşturulur)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("3 kullanıcı blok eklendi", r"Block Added 3 users starting from ID 100"), T_PROG_IDLE],
            verify=("onloi_user_list_view", [("User 100 = 12345678", r"User 100: PIN=, CARD=12345678"),
                                             ("User 102 = 12345680", r"User 102: PIN=, CARD=12345680")]),
        ),
        Step(
            id="A-18", section=s5, title="Kullanıcının kendi PIN'ini değiştirmesi (2580 → 3690)",
            rows=[
                ("[*] [<<PINID>>] [#]", "Bekleme modunda <b>*</b> tuşu, PIN kullanıcısının ID numarası (<b><<PINID>></b>) ve <b>#</b> tuşlayın."),
                ("[2] [5] [8] [0] [#]", "Mevcut (eski) PIN kodu olan <b>2580#</b> değerini girin."),
                ("[3] [6] [9] [0] [#] -> [3] [6] [9] [0] [#]", "Yeni PIN kodu <b>3690#</b> değerini iki kez girerek kaydedin <i>(Yeşil LED + 2 bip)</i>."),
                ("[3] [6] [9] [0] [#]", "Yeni PIN kodu <b>3690#</b> ile kapıyı açarak değişimi doğrulayın."),
            ],
            need_user=True,
            expect=[("PIN değiştirme moduna girildi", r"State Transition: IDLE -> USER_PIN_CHANGE"),
                    ("PIN değiştirme tamamlandı", r"State Transition: USER_PIN_CHANGE -> IDLE"),
                    ("Yeni PIN 3690 kabul edildi", r"Offline Mode Granted Access for KEYPAD:3690")],
        ),
        Step(
            id="A-19", section=s6, title="Menü 6: Röle süresini 2 sn yapma ve ölçme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[6] -> [2] [#]", "<b>Menü 6</b>'ya (Röle Süresi) girip kapı açık kalma süresini <b>2 saniye</b> yapın."),
                ("[*]", "Programlama modundan çıkın."),
                ("[3] [6] [9] [0] [#]", "<b>3690#</b> <i>(veya kayıtlı bir kart)</i> ile kapıyı açın ve ~2 sn içinde kapandığını gözlemleyin."),
            ],
            expect=[("Röle süresi 2 sn", r"Config: Relay Time = 2\b"), T_PROG_IDLE, T_DOOR_OPEN, T_DOOR_CLOSE],
            measure=(r"IDLE -> DOOR_OPEN", r"DOOR_OPEN -> IDLE", 2.0, 0.8),
        ),
        Step(
            id="A-20", section=s6, title="Menü 6: Toggle (Aç/Kapa) modu - 0 sn",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[6] -> [0] [#]", "<b>Menü 6</b>'da röle süresini <b>0 (Toggle / Aç-Kapa modu)</b> olarak kaydedin."),
                ("[3] -> [KART-2]", "<b>Menü 3</b>'e basıp <b>KART-2</b>'yi okutun <i>(Önceki testlerde silinmişse yeniden ekler, kayıtlıysa korur)</i>."),
                ("[*]", "Programlama modundan çıkın."),
                ("[KART-2]", "<b>KART-2</b>'yi 1. kez okutun → Kapı <b>AÇILIR</b> ve sürekli <b>AÇIK KALMALI</b>."),
                ("[⏳ 5 sn Bekle] -> [KART-2]", "En az 5 sn bekleyip <b>KART-2</b>'yi 2. kez okutun → Kapı <b>KAPANMALI</b>."),
            ],
            instruction="<i>Not: Kapı kapanmazsa üstteki <b>⚡ Cihazı Resetle</b> butonuna basın, bu adımı HATA VAR olarak işaretleyip devam edin.</i>",
            expect=[("Röle süresi 0 (toggle)", r"Config: Relay Time = 0\b"), T_PROG_IDLE, T_DOOR_OPEN, T_DOOR_CLOSE],
            fail_on=[("Toggle yerine süre uzatıldı (kapı kapanmadı)", r"Valid card scanned during DOOR_OPEN\. Extending timer")],
        ),
        Step(
            id="A-21", section=s6, title="Menü 6: Röle süresini tekrar 5 sn yapma",
            rows=[
                (ENTER_MENU_TOKENS, "Varsayılan Master Kod ile <b>Programlama Moduna</b> girin."),
                ("[6] -> [5] [#]", "<b>Menü 6</b>'ya (Röle Süresi) girip kapı açık kalma süresini <b>5 saniye</b> yapın."),
                ("[*]", "Programlama modundan çıkıp <b>Bekleme Moduna (IDLE)</b> dönün."),
            ],
            expect=[("Röle süresi 5 sn", r"Config: Relay Time = 5\b"), T_PROG_IDLE],
        ),
        Step(
            id="A-22", section=s6, title="Menü 7: Ses kapat/aç, LED kapat/aç ve Menü 0: Wiegand formatı (26 / 34 bit)",
            rows=[
                (ENTER_MENU_TOKENS + " -> [7] -> [0] [#] -> [*]", "Programlamaya girip <b>Menü 7 → 0#</b> ile buzzer sesini kapatın ve <b>*</b> ile çıkın."),
                ("[1] [2] [3]", "Bekleme modunda tuşlara basın → Buzzer'dan <b>BİP SESİ DUYULMAMALI</b>."),
                (ENTER_MENU_TOKENS + " -> [7] -> [1] [#]", "Tekrar programlamaya girip <b>Menü 7 → 1#</b> ile buzzer sesini geri açın."),
                ("[7] -> [2] [#] -> [7] -> [3] [#]", "<b>Menü 7 → 2#</b> ile bekleme LED'ini kapatıp <b>Menü 7 → 3#</b> ile tekrar açın."),
                ("[0] -> [3] [4] [#] -> [0] -> [2] [6] [#] -> [*]", "<b>Menü 0 → 34#</b> ile Wiegand 34-bit modunu test edin, ardından <b>Menü 0 → 26#</b> ile varsayılan 26-bit moduna geri alıp <b>*</b> ile çıkın."),
            ],
            expect=[("Ses kapatıldı", r"Config: Sound Disabled"), ("Ses açıldı", r"Config: Sound Enabled"),
                    ("LED kapatıldı", r"Config: LED Always On Disabled"), ("LED açıldı", r"Config: LED Always On Enabled"),
                    ("Wiegand 34-bit seçildi", r"Config: Wiegand/RFID Format set to 34-bit"),
                    ("Wiegand 26-bit (varsayılan) seçildi", r"Config: Wiegand/RFID Format set to 26-bit"),
                    T_PROG_IDLE],
            manual="Ses kapalıyken tuşlarda bip DUYULMADI ve ses açılınca geri geldi mi?",
        ),
        Step(
            id="A-23", section=s7, title="Menü 4: Kart okutarak ve ID ile tekil silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [KART-1] -> [*]", "<b>KART-1</b> önceki adımlarda silinmişse <b>Menü 3</b>'te okutarak tanımlı olduğundan emin olun ve <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS + " -> [4]", "Tekrar programlama moduna girip <b>Menü 4</b>'ü (Kullanıcı Silme) seçin."),
                ("[KART-1]", "<b>KART-1</b>'i okutarak tekil olarak silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[5] [0] [#]", "<b>50#</b> tuşlayarak ID 50'deki kullanıcıyı silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "Programlama modundan çıkın."),
                ("[KART-1]", "Bekleme modunda <b>KART-1</b>'i okutun → Silindiği için <b>REDDEDİLMELİ</b>."),
            ],
            expect=[("Menü 4 açıldı", r"Menu 4: Delete User"),
                    ("KART-1 silindi", r"\[PROG\] User DELETED: UserID=\d+, Card=\d+"),
                    ("ID 50 silindi", r"User DELETED by ID - UserID: 50\b"),
                    ("KART-1 artık reddediliyor", r"Offline Mode Denied Access for RFID:\d+")],
            fail_on=[("Silinen kart kapıyı açtı!", r"Granted Access for RFID:<<K1>>")],
        ),
        Step(
            id="A-24", section=s7, title="Menü 4: Kart numarası yazarak silme (12345678)",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[4] -> [1] [2] [3] [4] [5] [6] [7] [8] [#]", "<b>Menü 4</b>'te kart numarası <b>12345678#</b> tuşlayarak ID 100 kullanıcısını silin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Kart no ile silindi (ID 100)", r"User DELETED by Card - UserID: 100\b"), T_PROG_IDLE],
        ),
        Step(
            id="A-25", section=s7, title="Menü 4: Master Kod ile TÜM kullanıcıları silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [KART-1] -> [*]", "Silinecek en az 1 kullanıcı bulunması için <b>Menü 3</b>'te <b>KART-1</b>'i okutup <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS, "Tekrar Programlama Moduna girin."),
                ("[4] -> [1] [2] [3] [4] [5] [6] [#]", "<b>Menü 4</b>'te Master Kod (<b>123456#</b>) girerek cihazdaki <b>TÜM kullanıcıları</b> silin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Tüm kullanıcılar silindi", r"ALL USERS DELETED via Master Code"), T_PROG_IDLE],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 0", r"Total Users: 0\b")]),
        ),
        Step(
            id="A-26", section=s7, title="Menü 4: Master kart + # ile TÜM kullanıcıları silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-KART-1]", "<b>Menü 2</b>'de <b>MASTER-KART-1</b>'i okutun <i>(Önceki testlerde silinmişse Master olarak ekler, kayıtlıysa korur)</i>."),
                ("[3] -> [KART-1] -> [*]", "<b>Menü 3</b>'te silme testi için <b>KART-1</b>'i kullanıcı olarak ekleyip <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS, "Tekrar Programlama Moduna girin."),
                ("[4] -> [MASTER-KART-1] -> [#]", "<b>Menü 4</b>'te <b>MASTER-KART-1</b>'i okutup <b>#</b> tuşuna basarak tüm kullanıcıları silin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            need_master=True,
            expect=[("Kullanıcı eklendi", r"\[PROG\] User SAVED: UserID=\d+, Card=\d+"),
                    ("Master kart ile toplu silme onayı bekleniyor", r"Delete ALL users \(Waiting for #\)"),
                    ("Tüm kullanıcılar silindi", r"ALL USERS DELETED via Master Card Confirm"),
                    T_PROG_IDLE],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 0", r"Total Users: 0\b")]),
        ),
        Step(
            id="A-27", section=s8, title="Menü 8: Master kartı okutarak ve Master ID ile silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-KART-1]", "<b>MASTER-KART-1</b>'in tanımlı olduğundan emin olmak için <b>Menü 2</b>'de okutun."),
                ("[2] -> [1] [0] [0] [0] [5] [#] -> [MASTER-KART-2]", "<b>MASTER-KART-2</b>'nin ID 10005'te tanımlı olduğundan emin olmak için <b>Menü 2</b>'de okutun."),
                ("[8] -> [KART-2]", "<b>Menü 8</b>'e (Master Silme) girip normal <b>KART-2</b>'yi okutun → Master olmadığı için <b>REDDEDİLMELİ</b>."),
                ("[MASTER-KART-1]", "<b>MASTER-KART-1</b>'i okutarak Master listesinden silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[1] [0] [0] [0] [5] [#]", "<b>10005#</b> tuşlayarak <b>MASTER-KART-2</b>'yi ID ile silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[#] -> [*]", "Alt menüden ve Programlama Modundan çıkın."),
            ],
            expect=[("Menü 8 açıldı", r"Menu 8: Delete Master Card/QR"),
                    ("Master olmayan kart reddedildi", r"Delete Master FAILED: Card \d+ is not a Master"),
                    ("MASTER-KART-1 silindi", r"\[PROG\] Master Card DELETED: \d+"),
                    ("ID 10005 silindi", r"Master DELETED by ID - 10005"),
                    T_PROG_IDLE],
            verify=("onloi_master_list_view", [("Toplam Master = 0", r"Total Masters: 0\b")]),
        ),
        Step(
            id="A-28", section=s8, title="Menü 8: Kart numarası ile silme, Master Kod ile TÜM masterları silme ve Master'ı geri yükleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-KART-1]", "<b>Menü 2</b>'de <b>MASTER-KART-1</b>'i yeniden ekleyin."),
                ("[2] -> [MASTER-KART-2]", "<b>Menü 2</b>'de <b>MASTER-KART-2</b>'yi yeniden ekleyin."),
                ("[8] -> [<<M1>>] [#]", "<b>Menü 8</b>'e girip <b>MASTER-KART-1</b>'in kart numarasını (<b><<M1>>#</b>) tuşlayarak silin."),
                ("[1] [2] [3] [4] [5] [6] [#]", "Menü 8 içindeyken Master Kod (<b>123456#</b>) girerek <b>TÜM Masterları</b> silin."),
                ("[#] -> [2] -> [MASTER-KART-1] -> [*]", "Cihazda Master kart tanımsız kalmaması için <b>Menü 2</b>'den <b>MASTER-KART-1</b>'i tekrar tanıtıp <b>*</b> ile çıkın."),
            ],
            expect=[("Master eklendi", r"\[PROG\] Master Card SAVED: (?P<M1>\d+)"),
                    ("Numara ile silindi", r"Master Credential DELETED - \d+"),
                    ("Tüm masterlar silindi", r"ALL MASTER CREDENTIALS DELETED via Master Code"),
                    T_PROG_IDLE],
            verify=("onloi_master_list_view", [("MASTER-KART-1 yeniden tanımlı (Toplam Master = 1)", r"Total Masters: 1\b")]),
        ),
        Step(
            id="A-29", section=s9, title="Menü 9: Tuş takımından fabrika ayarına dönüş (9 9 #)",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[9] -> [1] [#]", "<b>Menü 9</b>'a girip yanlış onay kodu (<b>1#</b>) tuşlayın → <b>REDDEDİLMELİ</b> <i>(Sıfırlama yapılmaz)</i>."),
                ("[9] -> [9] [#]", "Tekrar <b>Menü 9</b>'a girip <b>9#</b> ile fabrika ayarlarına sıfırlamayı başlatın."),
            ],
            observe="Turuncu LED + uzun bip, ~3 sn sonra cihaz yeniden başlar.",
            expect=[("Menü 9 açıldı", r"Menu 9: Factory Reset"),
                    ("99# ile reset tetiklendi", r"Factory Reset triggered from Keypad \(99#\)"),
                    ("Hafıza silindi", r"Wipe complete"), T_BANNER],
            timeout=90,
        ),
    ]
    return s


# =============================================================================
# OE0113 (PIN + QR)
# =============================================================================
def oe0113_steps() -> List[Step]:
    s = menu1_steps("B")
    s2 = "2. Menü 2: Master QR Ekleme"
    s3 = "3. Menü 3: QR / PIN Kullanıcı ve Geçiş"
    s4 = "4. Master QR ile Yönetim"
    s5 = "5. Menü 5 Kilidi ve PIN Değiştirme"
    s6 = "6. Menü 6 / Menü 7 Ayarları"
    s7 = "7. Menü 4: Kullanıcı Silme"
    s8 = "8. Menü 8: Master QR Silme"
    s9 = "9. Menü 9: Fabrika Ayarı"
    qr_tip = "💡 <i>QR kodu okuyucuya 5-15 cm mesafeden gösterin ve bip sesini duyunca <b>HEMEN</b> geri çekin.</i>"
    s += [
        Step(
            id="B-04", section=s2, title="Menü 2: MASTER-QR-1 ekleme (otomatik ID)",
            rows=[
                (ENTER_MENU_TOKENS, "Varsayılan Master Kod ile <b>Programlama Moduna</b> girin."),
                ("[2]", "<b>Menü 2</b>'yi (Master QR Ekleme) seçin."),
                ("[MASTER-QR-1]", "<b>MASTER-QR-1</b> kodunu okutun <i>(Otomatik ID 10000 ile kaydedilir, Yeşil LED + 2 bip)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            instruction=qr_tip,
            expect=[("Menü 2 açıldı", r"Menu 2: Add Master Card/QR"),
                    ("Master QR kaydedildi", r"\[PROG\] Master QR SAVED: (?P<MQ1>[^\s,]+)$"), T_PROG_IDLE],
            verify=("onloi_master_list_view", [("Toplam Master = 1", r"Total Masters: 1\b")]),
        ),
        Step(
            id="B-05", section=s2, title="Menü 2: MASTER-QR-2'yi özel ID (10005) ile ekleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [1] [0] [0] [0] [5] [#]", "<b>Menü 2</b>'ye girip özel Master ID olarak <b>10005#</b> tuşlayın."),
                ("[MASTER-QR-2]", "<b>MASTER-QR-2</b> kodunu göstererek ID 10005'e kaydedin <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            instruction=qr_tip,
            expect=[("Master QR ID=10005 kaydedildi", r"\[PROG\] Master QR SAVED: ID=10005, QR=(?P<MQ2>\S+)"), T_PROG_IDLE],
            verify=("onloi_master_list_view", [("Toplam Master = 2", r"Total Masters: 2\b")]),
        ),
        Step(
            id="B-06", section=s3, title="Menü 3: Kısa QR (QR-KISA-1) ekleme ve mükerrer red",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [QR-KISA-1]", "<b>Menü 3</b>'e (Kullanıcı Ekleme) girip <b>QR-KISA-1</b>'i okutun → <b>KABUL</b> <i>(Yeşil LED + 2 bip)</i>."),
                ("[QR-KISA-1]", "Menüden çıkmadan <b>QR-KISA-1</b>'i tekrar gösterin → <b>MÜKERRER REDDİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            instruction=qr_tip,
            expect=[("Menü 3 açıldı", r"Menu 3: Add User"),
                    ("QR-KISA-1 kaydedildi", r"\[PROG\] User SAVED: UserID=\d+, QR=(?P<Q1>\S+)"),
                    ("Mükerrer QR reddedildi", r"QR <<Q1>> ALREADY registered"),
                    T_PROG_IDLE],
        ),
        Step(
            id="B-07", section=s3, title="Menü 3: Uzun QR (>31 karakter, SHA-256) ekleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [QR-UZUN-1]", "<b>Menü 3</b>'te 31 karakterden uzun <b>QR-UZUN-1</b> kodunu gösterin <i>(SHA-256 ile özetlenip kaydedilir)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            instruction=qr_tip,
            expect=[("Uzun QR SHA-256 ile özetlendi", r"Long QR \(>31 chars\) mapped to SHA256-31hex: (?P<QL>[0-9a-f]+)"),
                    ("Uzun QR kaydedildi", r"\[PROG\] User SAVED: UserID=(?P<QLID>\d+), QR=<<QL>>"),
                    T_PROG_IDLE],
        ),
        Step(
            id="B-08", section=s3, title="Menü 3: PIN kullanıcı ekleme (2580) ve hatalı girişler",
            rows=[
                (ENTER_MENU_TOKENS + " -> [3]", "Programlama moduna girip <b>Menü 3</b>'ü (Kullanıcı Ekleme) seçin."),
                ("[2] [5] [8] [0] [#] -> [2] [5] [8] [0] [#]", "<b>2580</b> PIN kodunu iki kez girip onaylayın → <b>KABUL</b> <i>(Yeşil LED + 2 bip)</i>."),
                ("[1] [1] [1] [1] [#] -> [2] [2] [2] [2] [#]", "Uyuşmayan PIN girin → <b>ONAY UYUŞMAZLIĞI REDDİ</b> <i>(3 hata bipi)</i>."),
                ("[1] [2] [#]", "4 haneden kısa (geçersiz) PIN girin → <b>UZUNLUK REDDİ</b> <i>(3 hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("PIN onay bekleniyor (2 kez)", r"PIN entered\. Waiting for confirmation", 2),
                    ("PIN 2580 kaydedildi", r"User ADDED \(AutoID\) - UserID: (?P<PINID>\d+), PIN: 2580"),
                    ("Uyuşmayan PIN reddedildi", r"PIN Confirm failed"),
                    ("Geçersiz uzunluk reddedildi", r"Invalid input for Menu \d"),
                    T_PROG_IDLE],
        ),
        Step(
            id="B-09", section=s3, title="Kısa ve uzun QR ile kapı açma",
            rows=[
                ("[QR-KISA-1] -> [⏳ 5 sn Bekle]", "Bekleme modunda <b>QR-KISA-1</b>'i gösterin, kapının açılıp ~5 sn sonra kapanmasını bekleyin."),
                ("[QR-UZUN-1] -> [⏳ 5 sn Bekle]", "Ardından <b>QR-UZUN-1</b>'i gösterin, kapının açılıp ~5 sn sonra kapanmasını bekleyin."),
            ],
            need_user=True,
            expect=[("Kısa QR kabul edildi", r"Offline Mode Granted Access for QR:\S+"),
                    ("Uzun QR kabul edildi", r"Offline Mode Granted Access for QR:\S{32,}"),
                    ("Kapı 2 kez açıldı", r"State Transition: IDLE -> DOOR_OPEN", 2)],
            measure=(r"IDLE -> DOOR_OPEN", r"DOOR_OPEN -> IDLE", 5.0, 1.0),
            manual="Her iki QR'da röle 'TIK' sesiyle çekti ve YEŞİL LED yandı mı?",
        ),
        Step(
            id="B-10", section=s3, title="PIN 2580 ile kapı açma; yabancı QR ve yanlış PIN reddi",
            rows=[
                ("[2] [5] [8] [0] [#] -> [⏳ 5 sn Bekle]", "Kayıtlı <b>2580#</b> PIN kodunu tuşlayın, kapının açılıp kapanmasını bekleyin."),
                ("[QR-YABANCI]", "Kayıtsız <b>QR-YABANCI</b> kodunu gösterin → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
                ("[9] [9] [9] [9] [#]", "Kayıtsız <b>9999#</b> PIN kodunu tuşlayın → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
            ],
            need_user=True,
            expect=[("PIN kabul edildi", r"Offline Mode Granted Access for KEYPAD:2580"),
                    ("Yabancı QR reddedildi", r"Offline Mode Denied Access for QR:"),
                    ("Yanlış PIN reddedildi", r"Offline Mode Denied Access for KEYPAD:9999")],
        ),
        Step(
            id="B-11", section=s4, title="Master Ekleme Modu: MASTER-QR-1 ile QR-YABANCI ekleme",
            rows=[
                ("[MASTER-QR-1]", "Bekleme modunda <b>MASTER-QR-1</b>'i 1 kez gösterip <b>HEMEN</b> çekin → <b>Master Ekleme Modu</b> <i>(Mavi LED yavaş yanıp söner)</i>."),
                ("[QR-YABANCI]", "<b>QR-YABANCI</b> kodunu gösterip çekin → Kullanıcı olarak kaydedilir <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "<b>*</b> tuşuna basarak <i>(veya 6 sn bekleyip MASTER-QR-1 göstererek)</i> moddan çıkın."),
            ],
            instruction=qr_tip,
            need_master=True,
            expect=[("Master Ekleme Moduna girildi", r"Master QR \(ID: -?\d+\) scanned in IDLE\. Entering Master Add Mode"),
                    ("Moddan çıkıldı", r"State Transition: MASTER_ADD_MODE -> IDLE")],
            fail_on=[("Yanlışlıkla silme moduna geçildi", r"MASTER_ADD_MODE -> MASTER_DELETE_MODE")],
            manual="QR-YABANCI gösterildiğinde YEŞİL LED + 2 bip oldu ve modda MAVİ LED YAVAŞ yanıp söndü mü?",
        ),
        Step(
            id="B-12", section=s4, title="Master Silme Modu: MASTER-QR-1'i 2 kez göstererek QR-YABANCI silme",
            rows=[
                ("[MASTER-QR-1] -> [MASTER-QR-1]", "Bekleme modunda <b>MASTER-QR-1</b>'i 5 sn içinde <b>2 kez</b> gösterip çekin → <b>Silme Modu</b> <i>(Kırmızı LED yavaş yanıp söner)</i>."),
                ("[QR-YABANCI]", "<b>QR-YABANCI</b>'yı gösterin → Kullanıcı silinir <i>(Yeşil LED + 2 bip)</i>."),
                ("[*]", "<b>*</b> tuşuna basarak Silme Modundan güvenli şekilde çıkın <i>(3. kez Master QR göstermeyin, tüm kullanıcıları siler!)</i>."),
                ("[QR-YABANCI]", "Bekleme modunda <b>QR-YABANCI</b>'yı tekrar gösterin → Silindiği için <b>REDDEDİLMELİ</b>."),
            ],
            instruction=qr_tip,
            need_master=True,
            expect=[("Silme moduna geçildi", r"State Transition: MASTER_ADD_MODE -> MASTER_DELETE_MODE"),
                    ("Moddan çıkıldı", r"State Transition: MASTER_DELETE_MODE -> IDLE"),
                    ("QR-YABANCI artık reddediliyor", r"Offline Mode Denied Access for QR:")],
            manual="Silme modunda KIRMIZI LED YAVAŞ yanıp söndü mü?",
        ),
        Step(
            id="B-13", section=s4, title="* + Master QR ile programlama girişi",
            rows=[
                ("[*] -> [MASTER-QR-1]", "Bekleme modunda <b>*</b> tuşuna basın ve hemen <b>MASTER-QR-1</b>'i gösterin → Şifresiz <b>Programlama Moduna</b> girilir."),
                ("[*]", "<b>*</b> tuşuna basarak Programlama Modundan çıkın."),
            ],
            need_master=True,
            expect=[("* + Master QR ile giriş", r"Master QR read after \*! Entering Programming Mode"), T_IDLE_PROG, T_PROG_IDLE],
        ),
        Step(
            id="B-14", section=s5, title="Menü 5 kilidi (bu modelde RFID yok)",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[5]", "<b>Menü 5</b>'e basın → Bu modelde RFID donanımı olmadığı için <b>REDDEDİLMELİ</b> <i>(3 yavaş hata bipi)</i>."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            observe="3 yavaş hata bipi.",
            expect=[("Menü 5 kilitli", r"Menu 5: Locked \(PIN\+QR has no RFID hardware\)"), T_PROG_IDLE],
        ),
        Step(
            id="B-15", section=s5, title="Kullanıcının kendi PIN'ini değiştirmesi (2580 → 3690)",
            rows=[
                ("[*] [<<PINID>>] [#]", "Bekleme modunda <b>*</b> tuşu, PIN kullanıcısının ID numarası (<b><<PINID>></b>) ve <b>#</b> tuşlayın."),
                ("[2] [5] [8] [0] [#]", "Mevcut (eski) PIN kodu olan <b>2580#</b> değerini girin."),
                ("[3] [6] [9] [0] [#] -> [3] [6] [9] [0] [#]", "Yeni PIN kodu <b>3690#</b> değerini iki kez girerek kaydedin <i>(Yeşil LED + 2 bip)</i>."),
                ("[3] [6] [9] [0] [#]", "Yeni PIN kodu <b>3690#</b> ile kapıyı açarak değişimi doğrulayın."),
            ],
            need_user=True,
            expect=[("PIN değiştirme moduna girildi", r"State Transition: IDLE -> USER_PIN_CHANGE"),
                    ("PIN değiştirme tamamlandı", r"State Transition: USER_PIN_CHANGE -> IDLE"),
                    ("Yeni PIN 3690 kabul edildi", r"Offline Mode Granted Access for KEYPAD:3690")],
        ),
        Step(
            id="B-16", section=s6, title="Menü 6: Röle süresini 2 sn yapma ve ölçme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[6] -> [2] [#]", "<b>Menü 6</b>'ya (Röle Süresi) girip kapı açık kalma süresini <b>2 saniye</b> yapın."),
                ("[*]", "Programlama modundan çıkın."),
                ("[3] [6] [9] [0] [#]", "<b>3690#</b> <i>(veya QR-KISA-1)</i> ile kapıyı açın ve ~2 sn içinde kapandığını gözlemleyin."),
            ],
            expect=[("Röle süresi 2 sn", r"Config: Relay Time = 2\b"), T_PROG_IDLE, T_DOOR_OPEN, T_DOOR_CLOSE],
            measure=(r"IDLE -> DOOR_OPEN", r"DOOR_OPEN -> IDLE", 2.0, 0.8),
        ),
        Step(
            id="B-17", section=s6, title="Menü 6: Toggle (Aç/Kapa) modu - 0 sn",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[6] -> [0] [#]", "<b>Menü 6</b>'da röle süresini <b>0 (Toggle / Aç-Kapa modu)</b> olarak kaydedin."),
                ("[3] -> [QR-KISA-1]", "<b>Menü 3</b>'e basıp <b>QR-KISA-1</b>'i okutun <i>(Önceki testlerde silinmişse yeniden ekler, kayıtlıysa korur)</i>."),
                ("[*]", "Programlama modundan çıkın."),
                ("[QR-KISA-1]", "<b>QR-KISA-1</b>'i 1. kez gösterip çekin → Kapı <b>AÇILIR</b> ve sürekli <b>AÇIK KALMALI</b>."),
                ("[⏳ 5 sn Bekle] -> [QR-KISA-1]", "En az 5 sn bekleyip <b>QR-KISA-1</b>'i 2. kez gösterin → Kapı <b>KAPANMALI</b>."),
            ],
            instruction="<i>Not: Kapı kapanmazsa üstteki <b>⚡ Cihazı Resetle</b> butonuna basın, bu adımı HATA VAR olarak işaretleyip devam edin.</i>",
            expect=[("Röle süresi 0 (toggle)", r"Config: Relay Time = 0\b"), T_PROG_IDLE, T_DOOR_OPEN, T_DOOR_CLOSE],
            fail_on=[("Toggle yerine süre uzatıldı (kapı kapanmadı)", r"Valid QR scanned during DOOR_OPEN\. Extending timer")],
        ),
        Step(
            id="B-18", section=s6, title="Menü 6: Röle süresini tekrar 5 sn yapma",
            rows=[
                (ENTER_MENU_TOKENS, "Varsayılan Master Kod ile <b>Programlama Moduna</b> girin."),
                ("[6] -> [5] [#]", "<b>Menü 6</b>'ya (Röle Süresi) girip kapı açık kalma süresini <b>5 saniye</b> yapın."),
                ("[*]", "Programlama modundan çıkıp <b>Bekleme Moduna (IDLE)</b> dönün."),
            ],
            expect=[("Röle süresi 5 sn", r"Config: Relay Time = 5\b"), T_PROG_IDLE],
        ),
        Step(
            id="B-19", section=s6, title="Menü 7: Ses kapat/aç, LED kapat/aç ve Menü 0: Wiegand formatı (26 / 34 bit)",
            rows=[
                (ENTER_MENU_TOKENS + " -> [7] -> [0] [#] -> [*]", "Programlamaya girip <b>Menü 7 → 0#</b> ile buzzer sesini kapatın ve <b>*</b> ile çıkın."),
                ("[1] [2] [3]", "Bekleme modunda tuşlara basın → Buzzer'dan <b>BİP SESİ DUYULMAMALI</b>."),
                (ENTER_MENU_TOKENS + " -> [7] -> [1] [#]", "Tekrar programlamaya girip <b>Menü 7 → 1#</b> ile buzzer sesini geri açın."),
                ("[7] -> [2] [#] -> [7] -> [3] [#]", "<b>Menü 7 → 2#</b> ile bekleme LED'ini kapatıp <b>Menü 7 → 3#</b> ile tekrar açın."),
                ("[0] -> [3] [4] [#] -> [0] -> [2] [6] [#] -> [*]", "<b>Menü 0 → 34#</b> ile Wiegand 34-bit modunu test edin, ardından <b>Menü 0 → 26#</b> ile varsayılan 26-bit moduna geri alıp <b>*</b> ile çıkın."),
            ],
            expect=[("Ses kapatıldı", r"Config: Sound Disabled"), ("Ses açıldı", r"Config: Sound Enabled"),
                    ("LED kapatıldı", r"Config: LED Always On Disabled"), ("LED açıldı", r"Config: LED Always On Enabled"),
                    ("Wiegand 34-bit seçildi", r"Config: Wiegand/RFID Format set to 34-bit"),
                    ("Wiegand 26-bit (varsayılan) seçildi", r"Config: Wiegand/RFID Format set to 26-bit"),
                    T_PROG_IDLE],
            manual="Ses kapalıyken tuşlarda bip DUYULMADI ve ses açılınca geri geldi mi?",
        ),
        Step(
            id="B-20", section=s7, title="Menü 4: QR göstererek ve ID ile tekil silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [QR-KISA-1] -> [QR-UZUN-1] -> [*]", "Kullanıcılar önceki adımlarda silinmişse <b>Menü 3</b>'te <b>QR-KISA-1</b> ve <b>QR-UZUN-1</b>'i okutup <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS + " -> [4]", "Tekrar programlama moduna girip <b>Menü 4</b>'ü (Kullanıcı Silme) seçin."),
                ("[QR-KISA-1]", "<b>QR-KISA-1</b>'i göstererek tekil olarak silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[<<QLID>>] [#]", "Uzun QR kullanıcısının ID numarasını (<b><<QLID>>#</b>) tuşlayarak ID ile silin."),
                ("[*]", "Programlama modundan çıkın."),
                ("[QR-KISA-1]", "Bekleme modunda <b>QR-KISA-1</b>'i gösterin → Silindiği için <b>REDDEDİLMELİ</b>."),
            ],
            expect=[("Menü 4 açıldı", r"Menu 4: Delete User"),
                    ("QR-KISA-1 silindi", r"\[PROG\] User DELETED: UserID=\d+, QR=\S+"),
                    ("Uzun QR kullanıcısı ID ile silindi", r"User DELETED by ID - UserID: \d+\b"),
                    ("QR-KISA-1 artık reddediliyor", r"Offline Mode Denied Access for QR:\S+")],
        ),
        Step(
            id="B-21", section=s7, title="Menü 4: Master Kod ile TÜM kullanıcıları silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[3] -> [QR-KISA-1] -> [*]", "Silinecek en az 1 kullanıcı bulunması için <b>Menü 3</b>'te <b>QR-KISA-1</b>'i okutup <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS, "Tekrar Programlama Moduna girin."),
                ("[4] -> [1] [2] [3] [4] [5] [6] [#]", "<b>Menü 4</b>'te Master Kod (<b>123456#</b>) girerek cihazdaki <b>TÜM kullanıcıları</b> silin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            expect=[("Tüm kullanıcılar silindi", r"ALL USERS DELETED via Master Code"), T_PROG_IDLE],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 0", r"Total Users: 0\b")]),
        ),
        Step(
            id="B-22", section=s7, title="Menü 4: Master QR + # ile TÜM kullanıcıları silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-QR-1]", "<b>Menü 2</b>'de <b>MASTER-QR-1</b>'i okutun <i>(Önceki testlerde silinmişse Master olarak ekler, kayıtlıysa korur)</i>."),
                ("[3] -> [QR-KISA-1] -> [*]", "<b>Menü 3</b>'te silme testi için <b>QR-KISA-1</b>'i kullanıcı olarak ekleyip <b>*</b> ile çıkın."),
                (ENTER_MENU_TOKENS, "Tekrar Programlama Moduna girin."),
                ("[4] -> [MASTER-QR-1] -> [#]", "<b>Menü 4</b>'te <b>MASTER-QR-1</b>'i gösterip <b>#</b> tuşuna basarak tüm kullanıcıları silin."),
                ("[*]", "Programlama modundan çıkın."),
            ],
            need_master=True,
            expect=[("Kullanıcı eklendi", r"\[PROG\] User SAVED: UserID=\d+, QR="),
                    ("Master QR ile toplu silme onayı bekleniyor", r"Master QR -> Delete ALL users \(Waiting for #\)"),
                    ("Tüm kullanıcılar silindi", r"ALL USERS DELETED via Master Card Confirm"),
                    T_PROG_IDLE],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 0", r"Total Users: 0\b")]),
        ),
        Step(
            id="B-23", section=s8, title="Menü 8: Master QR göstererek ve Master ID ile silme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-QR-1]", "<b>MASTER-QR-1</b>'in tanımlı olduğundan emin olmak için <b>Menü 2</b>'de gösterin."),
                ("[2] -> [1] [0] [0] [0] [5] [#] -> [MASTER-QR-2]", "<b>MASTER-QR-2</b>'nin ID 10005'te tanımlı olduğundan emin olmak için <b>Menü 2</b>'de gösterin."),
                ("[8] -> [QR-YABANCI]", "<b>Menü 8</b>'e (Master Silme) girip <b>QR-YABANCI</b>'yı gösterin → Master olmadığı için <b>REDDEDİLMELİ</b>."),
                ("[MASTER-QR-1]", "<b>MASTER-QR-1</b>'i göstererek Master listesinden silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[1] [0] [0] [0] [5] [#]", "<b>10005#</b> tuşlayarak <b>MASTER-QR-2</b>'yi ID ile silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[#] -> [*]", "Alt menüden ve Programlama Modundan çıkın."),
            ],
            expect=[("Menü 8 açıldı", r"Menu 8: Delete Master Card/QR"),
                    ("Master olmayan QR reddedildi", r"Delete Master FAILED: QR '.*' is not a Master"),
                    ("MASTER-QR-1 silindi", r"\[PROG\] Master QR DELETED: \S+"),
                    ("ID 10005 silindi", r"Master DELETED by ID - 10005"),
                    T_PROG_IDLE],
            verify=("onloi_master_list_view", [("Toplam Master = 0", r"Total Masters: 0\b")]),
        ),
        Step(
            id="B-24", section=s8, title="Menü 8: Master Kod ile TÜM master QR'ları silme ve Master QR'ı geri yükleme",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[2] -> [MASTER-QR-1]", "<b>Menü 2</b>'de <b>MASTER-QR-1</b>'i yeniden ekleyin."),
                ("[2] -> [MASTER-QR-2]", "<b>Menü 2</b>'de <b>MASTER-QR-2</b>'yi yeniden ekleyin."),
                ("[8] -> [1] [2] [3] [4] [5] [6] [#]", "<b>Menü 8</b>'e girip Master Kod (<b>123456#</b>) tuşlayarak <b>TÜM Master QR'ları</b> silin."),
                ("[#] -> [2] -> [MASTER-QR-1] -> [*]", "Cihazda Master QR tanımsız kalmaması için <b>Menü 2</b>'den <b>MASTER-QR-1</b>'i tekrar tanıtıp <b>*</b> ile çıkın."),
            ],
            expect=[("Master QR eklendi", r"\[PROG\] Master QR SAVED:"),
                    ("Tüm masterlar silindi", r"ALL MASTER CREDENTIALS DELETED via Master Code"),
                    T_PROG_IDLE],
            verify=("onloi_master_list_view", [("MASTER-QR-1 yeniden tanımlı (Toplam Master = 1)", r"Total Masters: 1\b")]),
        ),
        Step(
            id="B-25", section=s9, title="Menü 9: Tuş takımından fabrika ayarına dönüş (9 9 #)",
            rows=[
                (ENTER_MENU_TOKENS, "Programlama moduna girin."),
                ("[9] -> [1] [#]", "<b>Menü 9</b>'a girip yanlış onay kodu (<b>1#</b>) tuşlayın → <b>REDDEDİLMELİ</b> <i>(Sıfırlama yapılmaz)</i>."),
                ("[9] -> [9] [#]", "Tekrar <b>Menü 9</b>'a girip <b>9#</b> ile fabrika ayarlarına sıfırlamayı başlatın."),
            ],
            expect=[("Menü 9 açıldı", r"Menu 9: Factory Reset"),
                    ("99# ile reset tetiklendi", r"Factory Reset triggered from Keypad \(99#\)"),
                    ("Hafıza silindi", r"Wipe complete"), T_BANNER],
            timeout=90,
        ),
    ]
    return s


# =============================================================================
# OE0112 (Sadece RFID - Tuşsuz)
# =============================================================================
def oe0112_steps() -> List[Step]:
    s1 = "1. İlk Açılış Master Kart Tanımlama (10 sn)"
    s2 = "2. Master Kart ile Kullanıcı Yönetimi"
    return [
        Step(
            id="C-01", section=s1, title="Fabrika reset sonrası ilk 10 sn içinde 2 Master kart tanımlama",
            rows=[
                ("[MASTER-KART-1] [MASTER-KART-2]", "İki Master kartı şimdiden elinize hazırlayın."),
                ("[⚠️ Fabrika Sıfırlamayı Başlat]", "Aşağıdaki turuncu sıfırlama butonuna basın ve cihazın yeniden başlamasını bekleyin."),
                ("[MASTER-KART-1] -> [MASTER-KART-2]", "Ekranda <b>10 sn geri sayım</b> başlayınca süre bitmeden iki kartı sırayla okutun <i>(Her kartta Yeşil LED + 2 bip)</i>."),
                ("[⏳ 10 sn Bekle]", "10 saniyelik tanımlama penceresinin kilitlenmesini bekleyin."),
            ],
            observe="Her kartta YEŞİL LED + 2 kısa bip.",
            send=["onloi_fab_reset"],
            window=(r"Access Control Terminal Firmware", 9, "Master kart tanımlama penceresi"),
            expect=[T_BANNER,
                    ("MASTER-KART-1 tanımlandı", r"\[BOOT_MASTER\] Master Card 1/10 ADDED: (?P<M1>\d+)"),
                    ("MASTER-KART-2 tanımlandı", r"\[BOOT_MASTER\] Master Card 2/10 ADDED: (?P<M2>\d+)"),
                    ("10 sn penceresi kilitlendi", r"10s window closed with 2 Master Cards\. Enrollment locked")],
            fail_on=[F_HW_WARN],
            verify=("onloi_master_list_view", [("Toplam Master = 2", r"Total Masters: 2\b")]),
            timeout=60,
        ),
        Step(
            id="C-02", section=s1, title="Pencere kilitlendikten sonra yeni master tanımlanamaması",
            rows=[
                ("[⚡ Cihazı Resetle]", "Üst menüdeki <b>⚡ Cihazı Resetle</b> butonuna basın <i>(veya cihazın enerjisini kesip tekrar verin — Fabrika Reset'e basmayın!)</i>."),
                ("[KART-YABANCI]", "Cihaz açılır açılmaz ilk 10 saniye içinde <b>KART-YABANCI</b>'yı okutun → Master OLMAMALI, <b>REDDEDİLMELİ</b>."),
            ],
            expect=[T_BANNER, ("KART-YABANCI reddedildi", r"Offline Mode Denied Access for RFID:")],
            fail_on=[("Kilitli pencerede yeni master eklendi!", r"\[BOOT_MASTER\] Master Card \d+/10 ADDED")],
            timeout=90,
        ),
        Step(
            id="C-03", section=s2, title="Master kartı 1 kez okutma: KART-1 ve KART-2 ekleme",
            rows=[
                ("[MASTER-KART-1]", "Bekleme modunda <b>MASTER-KART-1</b>'i 1 kez okutup çekin → <b>Master Ekleme Modu</b> <i>(Mavi LED yavaş yanıp söner)</i>."),
                ("[KART-1] -> [KART-2]", "Sırayla <b>KART-1</b> ve <b>KART-2</b>'yi okutarak kullanıcı olarak ekleyin <i>(Her birinde Yeşil LED + 2 bip)</i>."),
                ("[⏳ 6 sn Bekle] -> [MASTER-KART-1]", "<b>En az 6 saniye bekleyin</b> ve <b>MASTER-KART-1</b>'i okutarak moddan çıkın."),
            ],
            need_master=True,
            expect=[("Master Ekleme Moduna girildi", r"scanned in IDLE\. Entering Master Add Mode"),
                    ("KART-1 eklendi", r"\[MASTER_MODE\] ADD: UserID=\d+, Card=(?P<K1>\d+)"),
                    ("2 kullanıcı kaydedildi", r"\[MASTER_MODE\] User SAVED: UserID=\d+", 2),
                    ("Çıkış", r"-> exit to IDLE")],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 2", r"Total Users: 2\b")]),
            manual="Master Ekleme Modunda MAVİ LED YAVAŞ yanıp söndü mü?",
        ),
        Step(
            id="C-04", section=s2, title="KART-1 ile kapı açma (5 sn) ve yabancı kart reddi",
            rows=[
                ("[KART-1] -> [⏳ 5 sn Bekle]", "<b>KART-1</b>'i okutun, kapının açılıp ~5 sn sonra kapanmasını bekleyin."),
                ("[KART-YABANCI]", "<b>KART-YABANCI</b>'yı okutun → <b>REDDEDİLMELİ</b> <i>(Kırmızı LED + 3 hata bipi)</i>."),
            ],
            need_user=True,
            expect=[("KART-1 kabul edildi", r"Offline Mode Granted Access for RFID:\d+"), T_DOOR_OPEN, T_DOOR_CLOSE,
                    ("Yabancı kart reddedildi", r"Offline Mode Denied Access for RFID:")],
            measure=(r"IDLE -> DOOR_OPEN", r"DOOR_OPEN -> IDLE", 5.0, 1.0),
            manual="Röle 'TIK' sesiyle çekti, YEŞİL LED yandı; yabancı kartta KIRMIZI + 3 bip oldu mu?",
        ),
        Step(
            id="C-05", section=s2, title="Master kartı 2 kez okutma: KART-1 silme",
            rows=[
                ("[MASTER-KART-1] -> [MASTER-KART-1]", "<b>MASTER-KART-1</b>'i 5 sn içinde art arda <b>2 kez</b> okutun → <b>Silme Modu</b> <i>(Kırmızı LED yavaş yanıp söner)</i>."),
                ("[KART-1]", "<b>KART-1</b>'i okutarak silin <i>(Yeşil LED + 2 bip)</i>."),
                ("[⏳ 6 sn Bekle] -> [MASTER-KART-1]", "<b>En az 6 saniye bekleyin</b> ve <b>MASTER-KART-1</b>'i okutarak moddan çıkın."),
                ("[KART-1]", "Bekleme modunda <b>KART-1</b>'i okutun → Silindiği için <b>REDDEDİLMELİ</b>."),
            ],
            need_master=True,
            expect=[("Silme moduna geçildi", r"-> switch to DELETE mode"),
                    ("KART-1 silindi", r"\[MASTER_MODE\] DELETE: UserID=\d+, Card=\d+"),
                    ("KART-1 artık reddediliyor", r"Offline Mode Denied Access for RFID:\d+")],
            manual="Silme modunda KIRMIZI LED YAVAŞ yanıp söndü mü?",
        ),
        Step(
            id="C-06", section=s2, title="İkinci Master kart (MASTER-KART-2) ile KART-1'i geri ekleme ve çıkış",
            rows=[
                ("[MASTER-KART-2]", "<b>MASTER-KART-2</b>'yi 1 kez okutarak Master Ekleme Moduna girin."),
                ("[KART-1]", "Bir önceki adımda silinen <b>KART-1</b>'i okutarak yeniden kullanıcı olarak ekleyin."),
                ("[⏳ 6 sn Bekle] -> [MASTER-KART-2]", "<b>En az 6 saniye bekleyin</b> ve <b>MASTER-KART-2</b>'yi okutarak moddan çıkın."),
            ],
            need_master=True,
            expect=[("MASTER-KART-2 ile moda girildi", r"Master Card \(ID: -?\d+\) scanned in IDLE"),
                    ("Çıkış", r"-> exit to IDLE")],
        ),
        Step(
            id="C-07", section=s2, title="Master kartı 3 kez okutma: TÜM kullanıcıları silme",
            rows=[
                ("[MASTER-KART-1] -> [MASTER-KART-1] -> [MASTER-KART-1]", "<b>MASTER-KART-1</b>'i her okutma arası 5 sn'den kısa olacak şekilde art arda <b>3 kez</b> okutun."),
            ],
            need_master=True,
            observe="3. okutmada YEŞİL LED + 2 bip, tüm kullanıcılar silinir ve cihaz bekleme moduna döner.",
            expect=[("3. okutma algılandı", r"3rd scan -> Delete ALL users"),
                    ("Tüm kullanıcılar silindi", r"\[MASTER_MODE\] ALL users DELETED")],
            verify=("onloi_user_list_view", [("Toplam kullanıcı = 0", r"Total Users: 0\b")]),
        ),
    ]


# =============================================================================
# ORTAK DONANIM / PROTOKOL ADIMLARI (tüm modeller, en sonda)
# =============================================================================
def common_hw_steps(model: str) -> List[Step]:
    sec = "D. Ortak Donanım ve Protokol"
    return [
        Step(
            id="D-01", section=sec, title="IN1 (Çıkış Butonu) ile kapı açma",
            rows=[
                ("[IN1] -> [+12V]", "<b>IN1</b> girişine <b>+12V</b> kablosunu 1 saniye değdirip çekin <i>(veya bağlı çıkış butonuna basın)</i>."),
                ("[⏳ 5 sn Bekle]", "Kapı rölesinin çektiğini ve ~5 saniye sonra kapandığını gözlemleyin."),
            ],
            observe="YEŞİL LED + röle 'TIK' ~5 sn.",
            expect=[("Çıkış butonu algılandı", r"Exit Button Pressed - Opening Door"), T_DOOR_OPEN, T_DOOR_CLOSE],
            manual="Röle çekti ve YEŞİL LED yandı mı?",
        ),
        Step(
            id="D-02", section=sec, title="Harici Wiegand okuyucu girişi (D0/D1)",
            rows=[
                ("[D0 / D1] [GND]", "Harici Wiegand okuyucuyu D0, D1 ve GND uçlarına bağlayıp bir kart okutun."),
                ("[⏭️ Atla]", "Harici Wiegand okuyucu kullanılmıyorsa aşağıdaki <b>Atla</b> butonuna tıklayın."),
            ],
            expect=[("Wiegand verisi işlendi", r"Offline Mode (Granted|Denied) Access for (RFID|QR):|Wiegand QR Received")],
        ),
        Step(
            id="D-03", section=sec, title="RS-485 / OSDP durum bilgisi",
            rows=[
                ("[📡 OSDP Durumunu Sorgula]", "Aşağıdaki mavi sorgulama butonuna tıklayarak <b>onloi_osdp_info</b> komutunu gönderin."),
                ("[UART Doğrulama]", "Seri portta OSDP PD adresinin ve baudrate hızının raporlandığını doğrulayın."),
            ],
            send=["onloi_osdp_info"],
            expect=[("OSDP durumu alındı", r"--- OSDP PD STATUS ---"),
                    ("OSDP adresi", r"Address\s*:\s*0x[0-9A-Fa-f]{2}"),
                    ("OSDP baudrate", r"Baudrate\s*:\s*\d+")],
            manual="OSDP v2.2 adresi ve baudrate seri port ekranında görüntülendi mi?",
            timeout=15,
        ),
        Step(
            id="D-04", section=sec, title="Bluetooth (BLE) yayın kontrolü",
            rows=[
                ("[📱 Bluetooth Tarama]", "Telefonunuzda Bluetooth taramasını (veya <b>nRF Connect</b> uygulamasını) açın."),
                ("[<<BLE>>]", "Listede <b><<BLE>></b> isimli cihaz yayınını gördüğünüzü doğrulayın."),
            ],
            manual="Telefonda <<BLE>> isimli cihaz göründü mü?",
        ),
        Step(
            id="D-05", section=sec, title="IN2: İlk 10 sn penceresinden sonra uygulanan +12V'un DİKKATE ALINMAMASI",
            rows=[
                ("[⏳ 15 sn Kontrolü]", "Cihaz açılalı en az 15 saniye olduğundan emin olun <i>(10 sn'lik IN2 açılış penceresi kapanmış olmalı)</i>."),
                ("[IN2] -> [+12V] -> [⏳ 10 sn Tut]", "<b>IN2</b> girişine <b>+12V</b>'u bağlayın, <b>10 saniye</b> basılı tutup çekin → <b>HİÇBİR ŞEY OLMAMALI</b>."),
            ],
            instruction="<i>Süre dolduğunda herhangi bir sıfırlama tetiklenmezse adım otomatik olarak GEÇER.</i>",
            fail_on=[("Pencere kapandıktan sonra IN2 reset sayacı başladı!", r"\[IN2_RESET\] IN2 is HIGH \(12V\)!|IN2 held at 12V"), T_BANNER],
            timeout=20, pass_on_timeout=True,
        ),
        Step(
            id="D-06", section=sec, title="IN2: +12V bırakılmazsa resetin İPTAL edilmesi (kısa devre / arıza koruması)",
            rows=[
                ("[⚡ Cihazı Resetle]", "Üst menüdeki <b>⚡ Reset</b> butonuna basın <i>(veya cihazın enerjisini kesip verin)</i>."),
                ("[IN2] -> [+12V] -> [⏳ 12 sn Tut]", "Cihaz açılır açılmaz (ilk 10 sn içinde) <b>IN2</b>'ye <b>+12V</b> bağlayın ve <b>en az 12 saniye</b> KESİNLİKLE BIRAKMAYIN."),
                ("[🔊 2 Hata Bipi]", "5. saniyede uzun bip + hızlı sarı LED başlar; yine bırakmayın → 11. saniyede (6 sn sonra) <b>2 hata bipi</b> duyulup reset İPTAL olunca kabloyu çekin."),
            ],
            observe="5. sn: uzun bip + hızlı sarı LED. 11. sn: 2 hata bipi, reset İPTAL (kırmızı yavaş yanıp sönmeye döner).",
            window=(r"Access Control Terminal Firmware", 12, "IN2 için 10 sn açılış penceresi"),
            expect=[("5 sn +12V algılandı", r"\[IN2_RESET\] 5s HIGH completed!|IN2 held at 12V"),
                    ("Reset iptal edildi (kısa devre koruması)", r"\[IN2_RESET\] Timeout! IN2 was NOT disconnected within [36]s|NOT released within [36]s! Aborting reset")],
            fail_on=[("Bırakılmadığı halde reset yapıldı!", r"\[IN2_RESET\] 3s LOW confirmed!|IN2 released! Confirmation acknowledged")],
            timeout=90,
        ),
        Step(
            id="D-07", section=sec, title="IN2: Donanımsal fabrika reseti (5 sn tut → 6 sn içinde bırak → 3 sn bekle)",
            rows=[
                ("[⚡ Cihazı Resetle]", "Üst menüdeki <b>⚡ Reset</b> butonuna basın <i>(veya cihazın enerjisini kesip verin)</i>."),
                ("[IN2] -> [+12V]", "Cihaz açılır açılmaz (ilk 10 sn içinde) <b>IN2</b>'ye <b>+12V</b> bağlayın ve tutmaya başlayın."),
                ("[🔊 Uzun Bip] -> [Kabloyu Çek]", "5. saniyede <b>uzun bip + hızlı sarı LED</b> başladığı anda <b>6 saniye içinde</b> +12V kablosunu çekin ve 3 onay bipi boyunca (3 sn) boşta bırakın."),
            ],
            observe="5. sn'de çekince 3 onay bipi (3 sn), ardından turuncu LED + uzun bip ile cihaz kendini sıfırlayıp yeniden başlar.",
            window=(r"Access Control Terminal Firmware", 12, "IN2 için 10 sn açılış penceresi"),
            expect=[("5 sn +12V algılandı", r"\[IN2_RESET\] 5s HIGH completed!|IN2 held at 12V"),
                    ("Bırakma ve 3 sn LOW onayı tamamlandı", r"\[IN2_RESET\] 3s LOW confirmed! Triggering Factory Reset|IN2 released! Confirmation acknowledged"),
                    ("Hafıza silindi", r"Wipe complete")],
            fail_on=[("Reset iptal oldu (geç bırakıldı veya tekrar değdi)", r"NOT disconnected within [36]s|re-asserted HIGH|NOT released within [36]s")],
            timeout=90,
        ),
    ]


# =============================================================================
# ONLINE MOD VE OTA GÜNCELLEME ADIMLARI (Offline ve Donanım testlerinden sonra)
# =============================================================================
def online_steps(model: str, new_fw: str = "v2.0.27", old_fw: str = "v2.0.26") -> List[Step]:
    sec = "E. Online Mod ve OTA Güncelleme Testleri"
    steps: List[Step] = [
        Step(
            id="E-01", section=sec, title="BLE Provisioning, Wi-Fi ve Bulut (MQTT) Bağlantısı (Online Moda Geçiş)",
            rows=[
                ("[📱 Onloi Mobil Uygulama] -> [<<BLE>>]", "Onloi mobil uygulamasından <b><<BLE>></b> cihazını seçip eşleştirmeyi başlatın <i>(Handshake 0x01 ve Bulut Ayarları 0x06 gönderilir)</i>."),
                ("[📶 Wi-Fi Seçimi] -> [Şifre Gir]", "Wi-Fi ağını seçip şifreyi göndererek cihazı internete bağlayın <i>(CMD_CONNECT_WIFI 0x04)</i>."),
                ("[🌐 MQTT Bağlantısı] -> [🔴 Yanıp Sönen Kırmızı LED]", "Cihazın IP aldığını, kalıcı olarak <b>Online Moda</b> geçtiğini, yerel kullanıcıları sildiğini ve <b>MQTT</b> sunucusuna bağlandığını gözlemleyin."),
            ],
            observe="Online modda da bekleme LED'i cihazın çalıştığını göstermek için KIRMIZI YANIP SÖNER (350ms açık / 350ms kapalı).",
            expect=[
                ("Bulut ayarları kaydedildi (0x06)", r"SET_CLOUD_CONFIG:|Cloud config reloaded:"),
                ("Wi-Fi bağlandı ve kalıcı Online moda geçildi", r"Device is now permanently in ONLINE mode|Switched permanently to Online Mode"),
                ("MQTT Bulut bağlantısı kuruldu", r"MQTT Connected! Active Cloud DeviceId:"),
                ("Zaman senkronizasyonu (0x33) / Firmware bilgisi (0x3A) gönderildi", r"Time synced!|Sent FIRMWARE_INFO:"),
            ],
            manual="Cihaz buluta bağlandı ve bekleme LED'i KIRMIZI yanıp sönüyor mu?",
            timeout=120,
        ),
    ]

    # E-02: Online modda yerel kullanıcı yönetimi kısıtlaması
    if model in ("OE0111", "OE0113"):
        steps.append(
            Step(
                id="E-02", section=sec, title="Online Modda Yerel Kullanıcı Menülerinin (Menü 3, 4, 5) Kilitlenmesi",
                rows=[
                    ("[*] [1] [2] [3] [4] [5] [6] [#]", "Programlama moduna girin <i>(D-07 fabrika reseti sonrası Master Şifre: 123456)</i>."),
                    ("[3]", "<b>Menü 3 (Kullanıcı Ekleme)</b> tuşuna basın → <b>KİLİTLİ OLMALI</b> <i>(3 hata bipi)</i>."),
                    ("[4]", "<b>Menü 4 (Sıra No ile Silme)</b> tuşuna basın → <b>KİLİTLİ OLMALI</b> <i>(3 hata bipi)</i>."),
                    ("[5]", "<b>Menü 5 (Veri ile Silme)</b> tuşuna basın → <b>KİLİTLİ OLMALI</b> <i>(3 hata bipi)</i>."),
                    ("[*]", "Programlama modundan çıkın."),
                ],
                observe="Online modda yerel kullanıcı ekleme/silme menüleri (3, 4, 5) kilitlidir; her tuşlamada 3 hata bipi verir.",
                expect=[
                    T_IDLE_PROG,
                    ("Menü 3 Online modda kilitli", r"Menu 3: Locked \(Online Mode\)"),
                    ("Menü 4 Online modda kilitli", r"Menu 4: Locked \(Online Mode\)"),
                    ("Menü 5 Online modda kilitli", r"Menu 5: Locked \(Online Mode\)"),
                    T_PROG_IDLE,
                ],
                fail_on=[
                    ("Online modda yerel kullanıcı menüsüne girildi!", r"Prog -> Add User|Prog -> Delete by Slot|Prog -> Delete by PIN"),
                ],
            )
        )
    else:
        # OE0112 (Sadece RFID): Online modda Master Kart okutulduğunda yerel Master Ekleme moduna GİRMEMELİ, sunucuya sormalı
        steps.append(
            Step(
                id="E-02", section=sec, title="Online Modda Yerel Master Ekleme Modunun Devre Dışı Olması",
                rows=[
                    ("[MASTER-KART-1]", "Bekleme modunda <b>MASTER-KART-1</b>'i okutun."),
                    ("[☁️ Bulut Sorgusu (0x13)]", "Cihazın yerel Master Ekleme moduna <b>GİRMEDİĞİNİ</b>, kartı doğrudan sunucuya (0x13) gönderdiğini doğrulayın."),
                ],
                observe="Online modda yerel Master Ekleme modu devre dışıdır; okutulan her kart sunucuya (0x13) sorulur.",
                expect=[
                    ("Kart Online doğrulama için sunucuya gönderildi", r"Online Mode Auth: RFID:"),
                    ("MQTT 0x13 paketi gönderildi", r"MQTT TX CMD: 0x13|Online Event Sent! Waiting for server"),
                ],
                fail_on=[
                    ("Online modda yerel Master Ekleme moduna girildi!", r"Entering Master Add Mode"),
                ],
            )
        )

    # E-03: Online geçiş doğrulama protokolü (0x12 PIN / 0x13 RFID / 0x14 QR)
    if model == "OE0111":
        auth_rows = [
            ("[KART-1] veya [2] [5] [8] [0] [#]", "Bekleme modunda <b>KART-1</b> okutun <i>(0x13)</i> veya tuş takımından 4 haneli şifre girip <b>#</b> tuşuna basın <i>(0x12)</i>."),
            ("[☁️ Sunucu Yanıtı]", "Cihazın geçiş isteğini MQTT üzerinden sunucuya ilettiğini ve sunucu yanıtına <i>(Onay veya Red)</i> göre işlem yaptığını doğrulayın."),
        ]
    elif model == "OE0113":
        auth_rows = [
            ("[QR-1] veya [3] [6] [9] [0] [#]", "Bekleme modunda <b>QR-1</b> okutun <i>(0x14)</i> veya tuş takımından 4 haneli şifre girip <b>#</b> tuşuna basın <i>(0x12)</i>."),
            ("[☁️ Sunucu Yanıtı]", "Cihazın geçiş isteğini MQTT üzerinden sunucuya ilettiğini ve sunucu yanıtına <i>(Onay veya Red)</i> göre işlem yaptığını doğrulayın."),
        ]
    else:
        auth_rows = [
            ("[KART-1]", "Bekleme modunda <b>KART-1</b>'i okutun <i>(0x13)</i>."),
            ("[☁️ Sunucu Yanıtı]", "Cihazın geçiş isteğini MQTT üzerinden sunucuya ilettiğini ve sunucu yanıtına <i>(Onay veya Red)</i> göre işlem yaptığını doğrulayın."),
        ]

    steps.append(
        Step(
            id="E-03", section=sec, title="Online Geçiş Doğrulama Protokolü (0x12 PIN / 0x13 RFID / 0x14 QR)",
            rows=auth_rows,
            observe="Okuma sonrası 1 kısa bip duyulur, sunucu onaylarsa kapı açılır (Yeşil LED), reddederse 3 hata bipi (Kırmızı LED) verir.",
            expect=[
                ("Online geçiş verisi yakalandı", r"Online Mode Auth: (RFID|QR|KEYPAD):"),
                ("MQTT geçiş paketi (0x12/0x13/0x14) gönderildi", r"MQTT TX CMD: 0x1[234]|Online Event Sent! Waiting for server"),
                ("Sunucu yanıtı işlendi (Onay veya Red)", r"Server (Approved|Denied) Access via CMD 0x1[234]|CMD=0x40|MQTT RX CMD: 0x40"),
            ],
        )
    )

    # E-04: Uzaktan Kapı Açma (CMD_REMOTE_OPEN 0x40)
    steps.append(
        Step(
            id="E-04", section=sec, title="Mobil Uygulama / Bulut Üzerinden Uzaktan Kapı Açma (0x40)",
            rows=[
                ("[📱 Uzaktan Kapı Aç (0x40)]", "Onloi mobil uygulamasından veya bulut yönetim panelinden <b>Kapıyı Aç</b> butonuna basın <i>(CMD_REMOTE_OPEN 0x40)</i>."),
                ("[⏳ 5 sn Bekle]", "Kapı rölesinin çektiğini, <b>Yeşil LED</b>'in yandığını, 0x10 (LOCK_OPENED) bildiriminin gönderildiğini ve ~5 sn sonra kapandığını gözlemleyin."),
            ],
            observe="Uzaktan komut geldiğinde röle 'TIK' sesiyle çeker, YEŞİL LED yanar ve ~5 sn sonra kapanır.",
            expect=[
                ("Uzaktan kapı açma komutu (0x40) alındı", r"CMD=0x40|MQTT TX CMD: 0x40|MQTT RX CMD: 0x40"),
                T_DOOR_OPEN,
                T_DOOR_CLOSE,
            ],
            manual="Mobil uygulamadan kapı açıldığında röle çekti ve YEŞİL LED yandı mı?",
        )
    )

    # E-05: RFID modellerde Online NFC Kart Tanıtma (0x44 -> 0x22 -> 0x23) / QR modelde Firmware Bilgisi Sorgulama (0x5C / 0x3A)
    if model in ("OE0111", "OE0112"):
        steps.append(
            Step(
                id="E-05", section=sec, title="Online Modda Uzaktan NFC Kart Tanıtma (CMD 0x44 → 0x22 → 0x23)",
                rows=[
                    ("[📱 Kart Tanıtma Başlat (0x44)]", "Mobil uygulamadan <b>Kart Ekle / NFC Tanıt</b> işlemini başlatın <i>(Sarı LED hızlı yanıp söner, cihaz 0x22 ENROLL_READY döner)</i>."),
                    ("[KART-1]", "Cihaza tanıtılacak <b>KART-1</b>'i okutun <i>(Kart UID değeri 0x23 NFC_ENROLLED ile sunucuya iletilir)</i>."),
                    ("[⏭️ Atla]", "Mobil uygulama üzerinden kart tanıtma testi yapılmayacaksa <b>Atla</b> butonuna tıklayabilirsiniz."),
                ],
                observe="0x44 komutu geldiğinde SARI LED hızlı yanıp söner; kart okutulunca YEŞİL LED + 2 bip ile sunucuya kaydedilir.",
                expect=[
                    ("Kart tanıtma modu (0x44) başlatıldı", r"CMD=0x44|MQTT RX CMD: 0x44|IDLE -> ONLINE_CARD_ENROLL"),
                    ("Kart okutuldu ve sunucuya bildirildi (0x23)", r"Online Card Enrolled! Card ID:|ONLINE_CARD_ENROLL -> IDLE"),
                ],
                timeout=90,
            )
        )
    else:
        steps.append(
            Step(
                id="E-05", section=sec, title="Bulut Üzerinden Cihaz ve Firmware Bilgisi Raporlama (0x5C / 0x3A)",
                rows=[
                    ("[🔍 Model ve Sürümü Sorgula]", "Aşağıdaki mavi sorgulama butonuna tıklayın <i>(veya sunucudan 0x5C GET_FIRMWARE_INFO gönderin)</i>."),
                    ("[☁️ 0x3A FIRMWARE_INFO]", "Cihazın mevcut sürümünü (<b><<FW>></b>) ve ürün kodunu (<b><<MODEL>></b>) MQTT üzerinden sunucuya raporladığını doğrulayın."),
                ],
                send=["onloi_ota_info"],
                expect=[
                    ("Firmware bilgisi (0x3A) buluta gönderildi", r"Sent FIRMWARE_INFO:.*Prod=0x0113"),
                ],
                timeout=30,
            )
        )

    # E-06: OTA ile Eski Versiyona Dönüş Testi (Downgrade)
    steps.append(
        Step(
            id="E-06", section=sec, title="OTA ile Eski Versiyona Dönüş Testi (Downgrade)",
            rows=[
                ("[☁️ OTA Hedef: Eski Sürüm]", "OTA sunucusunda / bulut panelinde cihaz için hedef sürümü <b>bir önceki (eski) versiyon</b> olarak seçip güncellemeyi başlatın <i>(0x53 / 0x60)</i> veya aşağıdaki <b>🔄 OTA Güncelleme İsteği Gönder (onloi_ota_req)</b> butonuna tıklayın."),
                ("[⏳ İndirme ve Doğrulama]", "Cihazın <b>OTA_UPDATING</b> moduna geçtiğini, yazılımı indirip <i>(0x02 DOWNLOADING)</i> SHA-256/CRC-32 doğrulamasını <i>(0x04 DOWNLOAD_VERIFIED)</i> tamamladığını izleyin."),
                ("[🚀 Açılış Kontrolü]", "Cihaz otomatik yeniden başladığında açılış logunda <b>Access Control Terminal Firmware</b> ve <b>OTA succeeded!</b> mesajının görüldüğünü doğrulayın."),
            ],
            observe="OTA sırasında durum paketleri (0x01..0x06) gönderilir, cihaz kapanıp indirilen sürümle yeniden açılır.",
            send=["onloi_ota_req"],
            expect=[
                ("OTA güncelleme işlemi başladı", r"Starting Onloi OTA Update|IDLE -> OTA_UPDATING"),
                ("Yazılım indirildi ve doğrulandı (0x04)", r"HTTP download finished|OTA State -> 0x04 \(DOWNLOAD_VERIFIED\)"),
                ("OTA tamamlandı, cihaz yeniden başlatılıyor", r"OTA UPDATE SUCCESSFUL! Rebooting"),
                ("Hedef versiyon ile açıldı", r"Access Control Terminal Firmware (?P<OTA_DOWN_VER>v\d+\.\d+\.\d+)"),
            ],
            fail_on=[
                ("OTA güncelleme başarısız oldu!", r"OTA State -> 0x07 \(FAILED\)|OTA Error:"),
            ],
            timeout=240,
        )
    )

    # E-07: OTA ile Yeni Versiyona Yükseltme Testi (Upgrade)
    steps.append(
        Step(
            id="E-07", section=sec, title="OTA ile Güncel Versiyona Yükseltme Testi (Upgrade)",
            rows=[
                ("[☁️ OTA Hedef: Güncel Sürüm (<<FW>>)]", "Şimdi OTA sunucusunda / bulut panelinde hedef sürümü tekrar <b>en son güncel versiyon (<<FW>>)</b> olarak seçip güncellemeyi başlatın <i>(0x53 / 0x60)</i> veya aşağıdaki <b>🔄 OTA Güncelleme İsteği Gönder (onloi_ota_req)</b> butonuna tıklayın."),
                ("[⏳ İndirme ve Doğrulama]", "Cihazın güncel sürümü indirip doğruladığını <i>(0x04 DOWNLOAD_VERIFIED)</i> ve yeniden başladığını izleyin."),
                ("[🚀 Güncel Sürüm Açılış Kontrolü]", "Cihaz yeniden başladığında açılış logunda <b>Access Control Terminal Firmware</b> ve <b>OTA succeeded!</b> mesajının görüldüğünü doğrulayın."),
            ],
            observe="Cihaz tekrar güncel sürüme yükseltilir ve buluta OTA başarısını bildirir.",
            send=["onloi_ota_req"],
            expect=[
                ("OTA güncelleme işlemi başladı", r"Starting Onloi OTA Update|IDLE -> OTA_UPDATING"),
                ("Yazılım indirildi ve doğrulandı (0x04)", r"HTTP download finished|OTA State -> 0x04 \(DOWNLOAD_VERIFIED\)"),
                ("OTA tamamlandı, cihaz yeniden başlatılıyor", r"OTA UPDATE SUCCESSFUL! Rebooting"),
                ("Yeni güncel versiyon ile açıldı", r"Access Control Terminal Firmware (?P<FW_VER>v\d+\.\d+\.\d+)"),
            ],
            fail_on=[
                ("OTA güncelleme başarısız oldu!", r"OTA State -> 0x07 \(FAILED\)|OTA Error:"),
            ],
            timeout=240,
        )
    )

    # E-08: Test Sonu Temizliği (Buluttan Ayırma / Fabrika Ayarlarına Dönüş)
    steps.append(
        Step(
            id="E-08", section=sec, title="Test Sonu: Buluttan Ayırma ve Fabrika Ayarlarına Dönüş (0x59 / 0x50)",
            rows=[
                ("[📱 Cihazı Uygulamadan Sil] veya [⚠️ Fabrika Sıfırlamayı Başlat]", "Mobil uygulamadan cihazı kaldırın <i>(CMD_DISCONNECTED 0x59 / CMD_FACTORY_RESET 0x50)</i> <b>veya</b> aşağıdaki turuncu <b>⚠️ Fabrika Sıfırlamayı Başlat</b> butonuna tıklayın."),
                ("[🔄 Temiz Açılış]", "Test Wi-Fi/MQTT ayarlarının silindiğini ve cihazın güncel sürümle temiz <b>Offline Fabrika Modunda</b> <i>(Yanıp sönen Kırmızı LED)</i> açıldığını doğrulayın."),
            ],
            observe="Tüm bulut ve Wi-Fi ayarları silinir; cihaz paketlemeye hazır fabrika çıkışı Offline moda döner.",
            send=["onloi_fab_reset"],
            expect=[
                ("Fabrika sıfırlama / buluttan ayırma tetiklendi", r"FACTORY RESET|Server Disconnected|Wiping NVS"),
                ("Hafıza temizlendi ve yeniden başlıyor", r"Wipe complete! Rebooting|Wipe complete\. Restarting system|Factory reset complete\. Rebooting"),
                ("Güncel sürümle temiz açıldı", r"Access Control Terminal Firmware (?P<FW_VER>v\d+\.\d+\.\d+)"),
            ],
            manual="Cihaz fabrika ayarlarına döndü ve bekleme LED'i KIRMIZI yanıp sönüyor mu?",
            timeout=60,
        )
    )

    return steps


def build_steps(model: str, fw: str = "v2.0.27") -> List[Step]:
    steps = prep_steps(model, fw)
    if model == "OE0111":
        steps += oe0111_steps()
    elif model == "OE0113":
        steps += oe0113_steps()
    elif model == "OE0112":
        steps += oe0112_steps()
    steps += common_hw_steps(model)
    steps += online_steps(model, new_fw=fw)
    return steps
