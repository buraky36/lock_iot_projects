
#ifndef BLE_FRAME_PARSER_H
#define BLE_FRAME_PARSER_H

#include <stdint.h>
#include <stdbool.h>

#define BLE_FRAME_SOF 0x4F

typedef struct {
    uint8_t sof;
    uint8_t ver;
    uint8_t type;
    uint8_t cmd;
    uint16_t seq;
    uint16_t len;
    uint8_t status;
    uint64_t timestamp;
    uint8_t *payload;
    uint16_t crc;
} ble_frame_t;

uint16_t calc_crc16_ccitt(const uint8_t *data, uint16_t len);
bool ble_frame_parse(const uint8_t *data, uint16_t data_len, ble_frame_t *out_frame);
uint16_t ble_frame_build_req(uint8_t cmd, uint16_t seq, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);
uint16_t ble_frame_build_resp(uint8_t cmd, uint16_t seq, uint8_t status, const uint8_t *payload, uint16_t payload_len, uint8_t *out_buf);

#endif
