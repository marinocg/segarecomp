/* Device wiring of the T003 baseline: no VDP, PSG or controller device is attached, so every device port class fails
 * closed with SMS_ERROR_PORT_UNIMPLEMENTED. `sms_devices_vdp.c` is the variant with the T004 VDP; the headless driver
 * calls `sms_install_devices` between machine init and the device-reset pass and `sms_write_device_artifacts` when it
 * writes `--artifacts`. */
#include "sms_machine.h"

void sms_install_devices(SmsMachine *machine) { (void)machine; }
int sms_write_device_artifacts(SmsMachine *machine, const char *dir) {
  (void)machine;
  (void)dir;
  return 1;
}
