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

bool ble_frame_parse_with_auth(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame,
                               const uint8_t **out_auth_tag, bool *out_has_auth_tag) {
    if (!data || !out_frame || data_len < 19) return false; // 17 header + 0 payload + 2 CRC
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

    uint32_t base_len = 17U + (uint32_t)out_frame->len;
    if (data_len < base_len + 2U) return false;

    bool has_auth = false;
    uint16_t expected_crc = 0;
    uint16_t actual_crc = 0;

    if (data_len >= base_len + 10U) {
        // Frame includes 8-byte AUTH_TAG before 2-byte CRC16
        has_auth = true;
        expected_crc = ((uint16_t)data[base_len + 8] << 8) | data[base_len + 9];
        actual_crc = calc_crc16_ccitt(data, (uint16_t)base_len);
        if (expected_crc != actual_crc) {
            // Fallback check in case CRC covered AUTH_TAG as well
            uint16_t alt_crc = calc_crc16_ccitt(data, (uint16_t)(base_len + 8U));
            if (expected_crc != alt_crc) return false;
            actual_crc = alt_crc;
        }
        if (out_auth_tag) *out_auth_tag = data + base_len;
    } else {
        expected_crc = ((uint16_t)data[base_len] << 8) | data[base_len + 1];
        actual_crc = calc_crc16_ccitt(data, (uint16_t)base_len);
        if (expected_crc != actual_crc) return false;
        if (out_auth_tag) *out_auth_tag = NULL;
    }

    if (out_has_auth_tag) *out_has_auth_tag = has_auth;
    out_frame->payload = (uint8_t *)(data + 17);
    out_frame->crc = actual_crc;
    return true;
}

bool ble_frame_parse(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame) {
    return ble_frame_parse_with_auth(data, data_len, out_frame, NULL, NULL);
}

static uint16_t ble_frame_build_internal(uint8_t msg_type, uint8_t cmd, uint16_t seq, uint8_t status,
                                         uint64_t timestamp, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    out_buf[0] = BLE_FRAME_SOF;
    out_buf[1] = 0x02; // ver
    out_buf[2] = msg_type;
    out_buf[3] = cmd;
    out_buf[4] = (seq >> 8) & 0xFF;
    out_buf[5] = seq & 0xFF;
    out_buf[6] = (payload_len >> 8) & 0xFF;
    out_buf[7] = payload_len & 0xFF;
    out_buf[8] = status;

    for (int i = 0; i < 8; i++) {
        out_buf[9 + i] = (uint8_t)((timestamp >> ((7 - i) * 8)) & 0xFF);
    }

    if (payload_len > 0 && payload != NULL) {
        memcpy(&out_buf[17], payload, payload_len);
    }

    uint16_t crc = calc_crc16_ccitt(out_buf, 17 + payload_len);
    out_buf[17 + payload_len] = (crc >> 8) & 0xFF;
    out_buf[17 + payload_len + 1] = crc & 0xFF;

    return 17 + payload_len + 2;
}

uint16_t ble_frame_build_with_type(uint8_t msg_type, uint8_t cmd, uint16_t seq, uint8_t status,
                                   uint64_t timestamp, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    return ble_frame_build_internal(msg_type, cmd, seq, status, timestamp, payload, payload_len, out_buf);
}

uint16_t ble_frame_build_resp(uint8_t cmd, uint16_t seq, uint8_t status, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    return ble_frame_build_internal(BLE_MSG_TYPE_RESP, cmd, seq, status, 0, payload, payload_len, out_buf);
}

uint16_t ble_frame_build_req(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    return ble_frame_build_internal(BLE_MSG_TYPE_REQ, cmd, seq, 0x00, 0, payload, payload_len, out_buf);
}

uint16_t ble_frame_build_event(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    return ble_frame_build_internal(BLE_MSG_TYPE_EVENT, cmd, seq, 0x00, 0, payload, payload_len, out_buf);
}

uint16_t ble_frame_build_cmd(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf) {
    return ble_frame_build_internal(BLE_MSG_TYPE_CMD, cmd, seq, 0x00, 0, payload, payload_len, out_buf);
}
