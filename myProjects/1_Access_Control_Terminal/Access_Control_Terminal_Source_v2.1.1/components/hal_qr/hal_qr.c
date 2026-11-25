#include "hal_qr.h"

#ifndef HW_HAS_QR
#define HW_HAS_QR 1
#endif

#if HW_HAS_QR
#include "xr1300.h"

bool hal_qr_init(void) {
  return xr1300_init();
}

bool hal_qr_read(char *qr_out, uint16_t max_len) {
  return xr1300_read_qr(qr_out, max_len);
}
#else
bool hal_qr_init(void) {
  return false;
}

bool hal_qr_read(char *qr_out, uint16_t max_len) {
  (void)qr_out;
  (void)max_len;
  return false;
}
#endif
