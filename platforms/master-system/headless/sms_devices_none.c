/* Device wiring of the T003 baseline: no VDP, PSG or controller device is attached yet, so every device port class fails
 * closed with SMS_ERROR_PORT_UNIMPLEMENTED. T004/T006/T007 replace this unit with the real device wiring (the headless
 * driver calls `sms_install_devices` between machine init and the device-reset pass). */
#include "sms_machine.h"

void sms_install_devices(SmsMachine *machine) { (void)machine; }
