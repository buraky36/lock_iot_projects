# Cihaz Yönetimi ve Sıfırlama Protokolü Spesifikasyonu (`WIFI_RESET`, `SERVER_DISCONNECT`, `FACTORY_RESET`, `LOCAL_FACTORY_RESET`)

## 1. Genel Frame Yapısı (Protokol v2)
- **Header (17 Byte):**
  - `SOF` (1B): `0x4F` (`"O"` - Frame başlangıç işareti)
  - `VER` (1B): `0x02` (Protokol versiyonu 2)
  - `MSG_TYPE` (1B): Mesaj tipi:
    - `0x03` = `EVENT` (Cihaz -> Sunucu olay bildirimi)
    - `0x04` = `COMMAND` (Sunucu -> Cihaz komut mesajı)
    - `0x05` = `ACK` (Cihaz -> Sunucu onay mesajı)
  - `CMD` (1B): Komut / Olay kodu (`0x58` = `WIFI_RESET`, `0x59` = `SERVER_DISCONNECT`, `0x50` = `FACTORY_RESET`, `0x61` = `LOCAL_FACTORY_RESET`, `0x05` = `ACK`)
  - `SEQ` (2B): Sıra numarası (Reply/ACK mesajlarında aynı değer döner. Örn: `12` -> `00 0C`, `1` -> `00 01`, `256` -> `01 00`)
  - `PAYLOAD_LEN` (2B, `uint16 BE`): Payload uzunluğu (AUTH_TAG hariç sadece payload byte sayısı)
  - `STATUS` (1B): Durum kodu (`0x00` = OK)
  - `TIMESTAMP` (8B, `uint64 BE`): Unix ms zaman damgası (Cihaz gönderirken `0` gönderebilir)
- **Payload (Değişken):** Komuta, olaya veya ACK mesajına özel veri.
- **AUTH_TAG (8 Byte - Sadece Sunucu -> Cihaz komutlarında):**
  - `HMAC-SHA256(deviceSecret, header || payload)` çıktısının **ilk 8 byte'ı**.
  - Frame sonunda, **CRC16'dan hemen önce** gelir. Eksikse veya hatalıysa cihaz komutu reddeder.
- **CRC16 (2 Byte, `uint16 BE`):**
  - `CRC16-CCITT` (`SOF` byte'ından başlayıp `PAYLOAD` son byte'ına / varsa `AUTH_TAG`'a kadar olan kısım üzerinden hesaplanır).

---

## 2. Sıfırlama İşlemleri Karşılaştırma Tablosu (AI Agent Hızlı Referans)

| İşlem Adı | Kodu (`CMD`) | Yön & Tip | MQTT Topic | Silinen Veriler | Korunan (Silinmeyen) Veriler | İşlem Sonrası Durum |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`WIFI_RESET`** | `0x58` | Sunucu -> Cihaz `COMMAND (0x04)` | `ONLOI/V1/D/{id}/CMD` | Sadece Wi-Fi ağ ayarları | `SET_CLOUD_CONFIG` NVS (MQTT credential, `deviceSecret`), NVC credential'ları (`PIN`/`NFC`/`FP`/`QR`), `deviceCode` | Mevcut ağdan çıkar, BLE bağlanma moduna geçer (Yeni Wi-Fi yapılandırılabilir). |
| **`SERVER_DISCONNECT`** | `0x59` | Sunucu -> Cihaz `COMMAND (0x04)` | `ONLOI/V1/D/{id}/CMD` | Wi-Fi ayarları + `SET_CLOUD_CONFIG` NVS (`host`/`port`/`user`/`pass`/`deviceSecret`/`sessionEpoch`) | NVC credential'ları (`PIN`/`NFC`/`FP`/`QR`) ve `deviceCode` | Bağlanma moduna geçer; yeniden eklemek için BLE `SET_CLOUD_CONFIG` + `CONNECT_WIFI` gerekir. |
| **`FACTORY_RESET`** | `0x50` | Sunucu -> Cihaz `COMMAND (0x04)` | `ONLOI/V1/D/{id}/CMD` | **Tüm veriler:** Kimlik bilgileri, Wi-Fi ayarları, `SET_CLOUD_CONFIG` NVS ve NVC credential'ları (`PIN`/`NFC`/`FP`/`QR`) | *(Yok - Uzaktan fabrika ayarlarına döner)* | Anında sıfırlanır ve MQTT'yi keser. Yeniden `SET_CLOUD_CONFIG` + `CONNECT_WIFI` gerekir. |
| **`LOCAL_FACTORY_RESET`** | `0x61` | Cihaz -> Sunucu `EVENT (0x03)` | `ONLOI/V1/D/{id}/LIFECYCLE` | **Tüm veriler:** Eşleşme bilgileri + NVC credential'ları | *(Yok - Cihaz üzerinden tuş kombinasyonu ile fabrika ayarlarına döner)* | Cihaz tüm veriyi siler ve bağlanma moduna geçer. Sunucu cihazı app/hesap eşleşmesinden düşürür (`unbind`). |

---

## 37. `WIFI_RESET` — Wi-Fi Sıfırlama (`CMD: 0x58`)

### 37.1. Tanım ve Kurallar
- Sunucu, cihazı bağlı olduğu Wi-Fi ağından koparır. Cihaz farklı bir ağa bağlanmak için bağlanma moduna (BLE) geçer.
- `SET_CLOUD_CONFIG` ile yazılan broker bilgisi (MQTT credential, `deviceSecret`) ve NVC credential'ları **korunur**.
- `AUTH_TAG` **zorunludur**.

### 37.2. REST API Tetiklemesi
- **Endpoint:** `POST https://api.onloi.com/v1/devices/78356203936/wifi-reset`
- **Response (`202 ACCEPTED`):**
```json
{
  "requestId": 4660,
  "state": "QUEUED",
  "cmd": "WIFI_RESET",
  "message": "Cihaza ulaşmak için butona basın"
}
```

### 37.3. Sunucu -> Cihaz: `WIFI_RESET` Komutu
- **MQTT Topic:** `ONLOI/V1/D/78356203936/CMD` (`PUBLISH`, `QoS 1`)
- **Header:** `MSG_TYPE: 0x04` (COMMAND), `CMD: 0x58` (WIFI_RESET), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 5` (`00 05`)

#### Payload Tablosu (5 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut takip ID |
| `4` | `reason` | `uint8` | 1 | `0x01` | `0x01` = Mobil app |

- **Örnek Raw Frame:**
  `4F 02 04 58 01 00 00 05 00 00 00 01 95 4C D9 9E A9 00 00 12 34 01 DE AD BE EF CA FE BA BE 00 61`
  - Payload (5B): `00 00 12 34 01`
  - AUTH_TAG (8B): `DE AD BE EF CA FE BA BE`
  - CRC16 (2B): `00 61`

### 37.4. Cihaz -> Sunucu: `ACK RECEIVED`
- **Header:** `MSG_TYPE: 0x05` (ACK), `CMD: 0x58` (WIFI_RESET), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 7` (`00 07`)

#### Payload Tablosu (7 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut `requestId` |
| `4` | `ackPhase` | `uint8` | 1 | `0x01` | `0x01` = `RECEIVED` |
| `5` | `cmdEcho` | `uint8` | 1 | `0x58` | `WIFI_RESET` |
| `6` | `detailCode` | `uint8` | 1 | `0x00` | `0x00` = Başarılı |

- **Örnek Raw Frame:**
  `4F 02 05 58 01 00 00 07 00 00 00 01 95 4C D9 9E A9 00 00 12 34 01 58 00 DE 2D`
  - Payload (7B): `00 00 12 34 01 58 00`
  - CRC16 (2B): `DE 2D`

### 37.5. Cihaz -> Sunucu: `ACK COMPLETED`
- **Header:** `MSG_TYPE: 0x05` (ACK), `CMD: 0x58` (WIFI_RESET), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 7` (`00 07`)

#### Payload Tablosu (7 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut `requestId` |
| `4` | `ackPhase` | `uint8` | 1 | `0x03` | `0x03` = `COMPLETED` |
| `5` | `cmdEcho` | `uint8` | 1 | `0x58` | `WIFI_RESET` |
| `6` | `detailCode` | `uint8` | 1 | `0x00` | `0x00` = Başarılı |

- **Örnek Raw Frame:**
  `4F 02 05 58 01 00 00 07 00 00 00 01 95 4C D9 9E A9 00 00 12 34 03 58 00 B0 4D`
  - Payload (7B): `00 00 12 34 03 58 00`
  - CRC16 (2B): `B0 4D`

- **Kritik İşlem Sırası:**
  1. `ACK COMPLETED`, Wi-Fi kesilmeden **önce** gönderilir.
  2. Ardından cihaz mevcut ağdan çıkar ve bağlanma moduna geçer (BLE ile yeni Wi-Fi yapılandırılabilir).
  3. Cloud config (`SET_CLOUD_CONFIG` NVS) ve NVC credential'ları silinmez.

---

## 38. `SERVER_DISCONNECT` — Eşleşme Sıfırlama (`CMD: 0x59`)

### 38.1. Tanım ve Kurallar
- Cihaz App'ten silindiğinde gönderilir.
- Ağ ve bulut eşleşmeleri sıfırlanır (Wi-Fi, `SET_CLOUD_CONFIG` NVS: MQTT credential + `deviceSecret`).
- NVC'de kayıtlı `PIN`/`NFC`/`parmak izi`/`QR` credential'ları ve `deviceCode` **silinmez**.
- `FACTORY_RESET`'ten farklıdır — yalnızca eşleştirme bilgileri temizlenir.
- `AUTH_TAG` **zorunludur**.

### 38.2. REST API Tetiklemesi
- **Endpoint:** `DELETE https://api.onloi.com/v1/devices/78356203936`
- **Response (`202 ACCEPTED`):**
```json
{
  "requestId": 4660,
  "state": "QUEUED",
  "cmd": "SERVER_DISCONNECT",
  "message": "Cihaza ulaşmak için butona basın"
}
```

### 38.3. Sunucu -> Cihaz: `SERVER_DISCONNECT` Komutu
- **MQTT Topic:** `ONLOI/V1/D/78356203936/CMD` (`PUBLISH`, `QoS 1`)
- **Header:** `MSG_TYPE: 0x04` (COMMAND), `CMD: 0x59` (SERVER_DISCONNECT), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 5` (`00 05`)

#### Payload Tablosu (5 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut takip ID |
| `4` | `reason` | `uint8` | 1 | `0x01` | `0x01` = Mobil app (cihaz silindi) |

- **Örnek Raw Frame:**
  `4F 02 04 59 01 00 00 05 00 00 00 01 95 4C D9 9E A9 00 00 12 34 01 DE AD BE EF CA FE BA BE 15 07`
  - Payload (5B): `00 00 12 34 01`
  - AUTH_TAG (8B): `DE AD BE EF CA FE BA BE`
  - CRC16 (2B): `15 07`

### 38.4. Cihaz -> Sunucu: `ACK RECEIVED`
- **Header:** `MSG_TYPE: 0x05` (ACK), `CMD: 0x59` (SERVER_DISCONNECT), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 7` (`00 07`)

#### Payload Tablosu (7 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut `requestId` |
| `4` | `ackPhase` | `uint8` | 1 | `0x01` | `0x01` = `RECEIVED` |
| `5` | `cmdEcho` | `uint8` | 1 | `0x59` | `SERVER_DISCONNECT` |
| `6` | `detailCode` | `uint8` | 1 | `0x00` | `0x00` = Başarılı |

- **Örnek Raw Frame:**
  `4F 02 05 59 01 00 00 07 00 00 00 01 95 4C D9 9E A9 00 00 12 34 01 59 00 38 EA`
  - Payload (7B): `00 00 12 34 01 59 00`
  - CRC16 (2B): `38 EA`

### 38.5. Cihaz -> Sunucu: `ACK COMPLETED`
- **Header:** `MSG_TYPE: 0x05` (ACK), `CMD: 0x59` (SERVER_DISCONNECT), `SEQ: 256` (`01 00`), `PAYLOAD_LEN: 7` (`00 07`)

#### Payload Tablosu (7 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00001234` | Komut `requestId` |
| `4` | `ackPhase` | `uint8` | 1 | `0x03` | `0x03` = `COMPLETED` |
| `5` | `cmdEcho` | `uint8` | 1 | `0x59` | `SERVER_DISCONNECT` |
| `6` | `detailCode` | `uint8` | 1 | `0x00` | `0x00` = Başarılı |

- **Örnek Raw Frame:**
  `4F 02 05 59 01 00 00 07 00 00 00 01 95 4C D9 9E A9 00 00 12 34 03 59 00 56 8A`
  - Payload (7B): `00 00 12 34 03 59 00`
  - CRC16 (2B): `56 8A`

- **Kritik İşlem Sırası:**
  1. `ACK COMPLETED`, eşleşme silinmeden **önce** gönderilir.
  2. **Silinen:** Wi-Fi ayarları, `SET_CLOUD_CONFIG` NVS (`host`/`port`/`user`/`pass`/`deviceSecret`/`sessionEpoch`).
  3. **Korunan:** NVC credential'ları (`PIN`/`NFC`/`FP`/`QR`) ve `deviceCode`.
  4. Cihaz bağlanma moduna geçer; yeniden eklemek için BLE `SET_CLOUD_CONFIG` + `CONNECT_WIFI` gerekir.

---

## 39. `FACTORY_RESET` — Fabrika Sıfırlama (Sunucu Komutu) (`CMD: 0x50`)

### 39.1. Tanım ve Kurallar
- Sunucu, cihazı uzaktan fabrika ayarlarına döndürür.
- Tüm kimlik bilgileri, Wi-Fi ayarları, kayıtlı şifre/NFC/FP verileri **silinir**.
- `SERVER_DISCONNECT`'ten farklı olarak **NVC credential'ları da silinir**.
- Yerel tuş kombinasyonu ile yapılan sıfırlama için `LOCAL_FACTORY_RESET (0x61)` kullanılır — o cihaz->sunucu `EVENT`'tir.
- Güvenlik için `AUTH_TAG` **zorunludur** (eksikse cihaz komutu reddeder).

### 39.2. Sunucu -> Cihaz: `FACTORY_RESET` Komutu
- **MQTT Topic:** `ONLOI/V1/D/78356203936/CMD` (`PUBLISH`, `QoS 1`)
- **Header:** `MSG_TYPE: 0x04` (COMMAND), `CMD: 0x50` (FACTORY_RESET), `SEQ: 1` (`00 01`), `PAYLOAD_LEN: 4` (`00 04`)

#### Payload ve İmza Tablosu
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00000001` | Komut takip ID (Payload uzunluğu = 4 byte) |
| `4` | `AUTH_TAG` | `bytes` | 8 | `AA BB CC ...` | `HMAC-SHA256(deviceSecret, header\|\|payload)` ilk 8 byte — eksikse cihaz komutu reddeder |

- **Örnek Raw Frame:**
  `4F 02 04 50 00 01 00 04 00 00 00 00 00 00 00 00 00 00 00 00 01 AA BB CC DD EE FF 11 22 7D E8`
  - Header (17B): `4F 02 04 50 00 01 00 04 00 00 00 00 00 00 00 00 00`
  - Payload (4B): `00 00 00 01` (`requestId` = 1)
  - AUTH_TAG (8B): `AA BB CC DD EE FF 11 22`
  - CRC16 (2B): `7D E8`

### 39.3. Cihaz -> Sunucu: `ACK COMPLETED`
- **Header:** `MSG_TYPE: 0x05` (ACK), `CMD: 0x05` (ACK), `SEQ: 1` (`00 01`), `PAYLOAD_LEN: 7` (`00 07`)

#### Payload Tablosu (7 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `requestId` | `uint32 BE` | 4 | `0x00000001` | Komut `requestId` |
| `4` | `ackPhase` | `uint8` | 1 | `0x03` | `0x03` = `COMPLETED` — sıfırlama tamamlandı |
| `5` | `cmdEcho` | `uint8` | 1 | `0x50` | `FACTORY_RESET` |
| `6` | `detailCode` | `uint8` | 1 | `0x00` | `0x00` = Başarılı |

- **Örnek Raw Frame:**
  `4F 02 05 05 00 01 00 07 00 00 00 00 00 00 00 00 00 00 00 00 01 03 50 00 EC FF`
  - Payload (7B): `00 00 00 01 03 50 00`
  - CRC16 (2B): `EC FF`

- **Kritik İşlem Sırası:**
  1. `ACK` gönderildikten sonra cihaz **anında sıfırlanır** ve MQTT bağlantısını keser.
  2. Yeniden `SET_CLOUD_CONFIG` + `CONNECT_WIFI` gerekir.

---

## 40. `LOCAL_FACTORY_RESET` — Yerel Fabrika Sıfırlama Bildirimi (`CMD: 0x61`)

### 40.1. Tanım ve Kurallar
- Kullanıcı cihaz üzerinde tuş kombinasyonu ile fabrika ayarlarına döndürdüğünde cihaz, sunucuya **"fabrika ayarlarına döndürüldüm"** bilgisini gönderir.
- Sunucu hiçbir komut göndermez — işlem tamamen kullanıcı müdahalesiyle cihazda başlar.
- `FACTORY_RESET (0x50)` sunucu->cihaz `COMMAND`'ından farklıdır; bu **cihaz -> sunucu `EVENT` (`0x03`)** mesajıdır.

### 40.2. Cihaz -> Sunucu: `LOCAL_FACTORY_RESET` Event Frame'i
- **MQTT Topic:** `ONLOI/V1/D/78356203936/LIFECYCLE` (`PUBLISH`, `QoS 1`)
- **Header:** `MSG_TYPE: 0x03` (EVENT), `CMD: 0x61` (LOCAL_FACTORY_RESET), `SEQ: 12` (`00 0C`), `PAYLOAD_LEN: 1` (`00 01`), `STATUS: 0x00` (OK)

#### Payload Tablosu (1 Byte)
| Offset | Alan | Tip | Boyut | Örnek | Açıklama |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0` | `reason` | `uint8` | 1 | `0x01` | `0x01` = Tuş kombinasyonu (kullanıcı müdahalesi) |

- **Örnek Raw Frame:**
  `4F 02 03 61 00 0C 00 01 00 00 00 01 95 4C D9 9E A9 01 99 54`
  - `4F`: SOF (`"O"`)
  - `02`: VER (`2`)
  - `03`: MSG_TYPE (`EVENT`)
  - `61`: CMD (`LOCAL_FACTORY_RESET`)
  - `00 0C`: SEQ (`12`)
  - `00 01`: PAYLOAD_LEN (`1` byte)
  - `00`: STATUS (`0x00 = OK`)
  - `00 00 01 95 4C D9 9E A9`: TIMESTAMP (8 byte Unix ms)
  - `01`: PAYLOAD (`reason = 0x01`)
  - `99 54`: CRC16 (`0x9954` - CRC16-CCITT)

- **Kritik İşlem Sırası ve Sunucu Davranışı:**
  1. Event, **MQTT bağlantısı hâlâ açıkken ve veri silinmeden hemen önce** gönderilir.
  2. Ardından cihaz **tüm veriyi (eşleşme + NVC credential) siler** ve bağlanma moduna geçer.
  3. Sunucu bu event'i alınca cihazı **app/hesap eşleşmesinden düşürmelidir (`unbind`)**.
  4. `FACTORY_RESET (0x50)` ile karıştırılmamalıdır: `0x50` sunucunun uzaktan sıfırlama komutudur, `0x61` ise cihazdaki yerel sıfırlamanın bildirimidir.
