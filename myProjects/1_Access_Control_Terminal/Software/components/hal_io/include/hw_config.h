#ifndef HW_CONFIG_H
#define HW_CONFIG_H

#include <stdint.h>

// Hardware Versions (Compile / internal index)
#define HW_VERSION_PIN_QR     1  // OE0110
#define HW_VERSION_PIN_RFID   2  // OE0111
#define HW_VERSION_RFID_ONLY  3  // OE0112

// Hardware Product Codes
#define PRODUCT_ID_PIN_QR     0x0110
#define PRODUCT_ID_PIN_RFID   0x0111
#define PRODUCT_ID_RFID_ONLY  0x0112

// Capability flags derived from compilation version
#if COMPILE_HW_VERSION == HW_VERSION_PIN_QR
    #define HW_HAS_KEYPAD         1
    #define HW_HAS_QR             1
    #define HW_HAS_RFID           0
    #define HW_HAS_MASTER_CODE    1
    #define HW_BOOT_MASTER_ENROLL 0
#elif COMPILE_HW_VERSION == HW_VERSION_PIN_RFID
    #define HW_HAS_KEYPAD         1
    #define HW_HAS_QR             0
    #define HW_HAS_RFID           1
    #define HW_HAS_MASTER_CODE    1
    #define HW_BOOT_MASTER_ENROLL 0
#elif COMPILE_HW_VERSION == HW_VERSION_RFID_ONLY
    #define HW_HAS_KEYPAD         0
    #define HW_HAS_QR             0
    #define HW_HAS_RFID           1
    #define HW_HAS_MASTER_CODE    0
    #define HW_BOOT_MASTER_ENROLL 1
#endif

extern char device_code_str[32];
extern char ble_name_str[64];

// The active hardware version is determined dynamically at boot
extern uint8_t active_hw_version;

// Firmware / Hardware information getters
uint16_t app_get_product_id(void);
uint8_t app_get_hardware_revision(void);
uint32_t app_get_firmware_version(void);

// PIN DEFINITIONS

// RFID (MFRC522)
#define PIN_RFID_MOSI   5
#define PIN_RFID_MISO   36
#define PIN_RFID_SCK    15
#define PIN_RFID_CS     0
#define PIN_RFID_IRQ    35

// QR Scanner (XR1300)
#define PIN_QR_TX       25
#define PIN_QR_RX       39

// RS485
#define PIN_RS485_RX    34
#define PIN_RS485_TX    26
#define PIN_RS485_DE    18

// Touch Keypad (TSM12)
#define PIN_TSM_SDA     32
#define PIN_TSM_SCL     33
#define PIN_TSM_I2C_EN  27
#define PIN_TSM_INT     14

// Shift Registers (74HC595 U1 & U2)
#define PIN_SHIFT_LATCH1 17
#define PIN_SHIFT_LATCH2 13
#define PIN_SHIFT_DATA   16
#define PIN_SHIFT_CLK    4

// Wiegand
#define PIN_WIEGAND_D0   22
#define PIN_WIEGAND_D1   23

// Peripherals
#define PIN_WS2812B_RGB  12
#define PIN_BUZZER_PWM   2

// Digital Inputs
#define PIN_DIGITAL_IN1  19
#define PIN_DIGITAL_IN2  21

#endif // HW_CONFIG_H
