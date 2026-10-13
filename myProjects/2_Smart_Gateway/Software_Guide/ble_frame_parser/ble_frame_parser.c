#include "ble_frame_parser.h"
#include <string.h>

uint16_t calc_crc16_ccitt(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
            } else {
                crc = (crc << 1) & 0xFFFF;
            }
        }
    }
    return crc;
}

bool ble_frame_parse(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame) {
    if (data_len < 19) return false; // 17 header + 0 payload + 2 CRC
    if (data[0] != BLE_FRAME_SOF) return false;

    out_frame->sof = data[0];
    out_frame->ver = data[1];
    out_frame->type = data[2];
    out_frame->cmd = data[3];
    out_frame->seq = ((uint16_t)data[4] << 8) | data[5];
    out_frame->len = ((uint16_t)data[6] << 8) | data[7];
    out_frame->status = data[8];

    uint64_t ts = 0;
    for (int i = 0; i < 8; i++) {
        ts = (ts << 8) | data[9 + i];
    }
    out_frame->timestamp = ts;

    if (data_len < 17 + out_frame->len + 2) return false;

    uint16_t expected_crc = ((uint16_t)data[17 + out_frame->len] << 8) | data[17 + out_frame->len + 1];
    uint16_t actual_crc = calc_crc16_ccitt(data, 17 + out_frame->len);
    if (expected_crc != actual_crc) return false;

    out_frame->payload = (uint8_t *)(data + 17);
    out_frame->crc = actual_crc;

    return true;
}

uint16_t ble_frame_build_resp(uint8_t cmd, uint16_t seq, uint8_t status, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    out_buf[0] = BLE_FRAME_SOF;
    out_buf[1] = 0x02; // ver
    out_buf[2] = 0x02; // RESPONSE
    out_buf[3] = cmd;
    out_buf[4] = (seq >> 8) & 0xFF;
    out_buf[5] = seq & 0xFF;
    out_buf[6] = (payload_len >> 8) & 0xFF;
    out_buf[7] = payload_len & 0xFF;
    out_buf[8] = status;

    // 8 bytes timestamp (0 ms for device/gw response)
    memset(&out_buf[9], 0, 8);

    if (payload_len > 0 && payload != NULL) {
        memcpy(&out_buf[17], payload, payload_len);
    }

    uint16_t crc = calc_crc16_ccitt(out_buf, 17 + payload_len);
    out_buf[17 + payload_len] = (crc >> 8) & 0xFF;
    out_buf[17 + payload_len + 1] = crc & 0xFF;

    return 17 + payload_len + 2;
}

uint16_t ble_frame_build_req(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    out_buf[0] = BLE_FRAME_SOF;
    out_buf[1] = 0x02; // ver
    out_buf[2] = 0x01; // REQUEST
    out_buf[3] = cmd;
    out_buf[4] = (seq >> 8) & 0xFF;
    out_buf[5] = seq & 0xFF;
    out_buf[6] = (payload_len >> 8) & 0xFF;
    out_buf[7] = payload_len & 0xFF;
    out_buf[8] = 0x00; // status is 0 for requests

    memset(&out_buf[9], 0, 8);

    if (payload_len > 0 && payload != NULL) {
        memcpy(&out_buf[17], payload, payload_len);
    }

    uint16_t crc = calc_crc16_ccitt(out_buf, 17 + payload_len);
    out_buf[17 + payload_len] = (crc >> 8) & 0xFF;
    out_buf[17 + payload_len + 1] = crc & 0xFF;

    return 17 + payload_len + 2;
}
