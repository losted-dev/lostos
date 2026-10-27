/* Example plugin for the speaker: when the centre button is pressed the ring flashes violet and the clock says "HI".
 * Permissions in manifest.json: events.subscribe, device.led, device.display. Build and pack: ref/build-example-plugin.sh */
#include <string.h>

#include "../../sdk/lostos_plugin.h"

int main(void) {
  char ev[128], res[64];
  lp_call("log", "button-flash started", res, sizeof res);
  if (lp_call("subscribe", "button", res, sizeof res) != 0) {
    lp_call("log", "no permission to subscribe", NULL, 0);
    return 1;
  }
  for (;;) {
    int n = lp_wait_event(ev, sizeof ev, 10000);
    if (n < 0) return 0;                                   /* the firmware closed the line: we are being stopped */
    if (n > 0 && !strncmp(ev, "button action", 13)) {
      lp_call("led", "139 92 246 2", res, sizeof res);
      lp_call("display", "2 HI", res, sizeof res);
    }
  }
}
