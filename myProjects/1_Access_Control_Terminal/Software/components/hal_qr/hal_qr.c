#include "hal_qr.h"
#include "xr1300.h"

bool hal_qr_init(void) { return xr1300_init(); }

bool hal_qr_read(char *qr_out, uint16_t max_len) {
  return xr1300_read_qr(qr_out, max_len);
}
