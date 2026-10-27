/* Test plugin for the Alice button (needs assistant.intercept, events.subscribe, device.led):
 * it claims the centre button; a tap then flashes the ring green and the assistant does NOT start.
 * When the plugin is switched off or removed, the firmware gives the button back. */
#include <string.h>

#include "../sdk/lostos_plugin.h"

int main(void) {
  char res[96];
  lp_call("subscribe", "button", NULL, 0);
  if (lp_call("button", "claim", res, sizeof res) != 0) lp_call("log", res, NULL, 0);
  else lp_call("log", "the button is mine", NULL, 0);
  for (;;) {
    char ev[128];
    if (lp_wait_event(ev, sizeof ev, 5000) > 0 && !strncmp(ev, "button action", 13) && strncmp(ev, "button action_long", 18))
      lp_call("led", "52 199 89 2", NULL, 0);
  }
}
