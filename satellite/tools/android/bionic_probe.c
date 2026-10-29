// Does a Bionic program of ours load the Yandex libraries of Station 1 and see the same C API that the daemon uses on the other stations? Read-only: no sound device is opened.
#include <dlfcn.h>
#include <stdio.h>
typedef void *P;
int main(void) {
  const char *dir = "/system/vendor/quasar/libs/";
  char path[256];
  snprintf(path, sizeof path, "%slibtflite.so", dir);
  void *t = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  printf("libtflite.so: %s\n", t ? "loaded" : dlerror());
  snprintf(path, sizeof path, "%slibyandex_evil.so", dir);
  void *l = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  printf("libyandex_evil.so: %s\n", l ? "loaded" : dlerror());
  if (!l) return 1;
  static const char *names[] = {"YandexVqeC_Config_CreateFromPreset", "YandexVqeC_Config_Free", "YandexVqeC_Config_GetInputChunkSize", "YandexVqeC_Config_GetProcessingFrame", "YandexVqeC_Create",
    "YandexVqeC_Free", "YandexVqeC_Process", "YandexVqeC_GetOutputChannelData", "YandexVqeC_Error_GetMessage", "YandexVqeC_Error_Free", "spotter_conf_read", "spotter_conf_free",
    "spotter_conf_native_sample_rate", "spotter_conf_get_required_channel_count", "spotter_create_multichannel", "spotter_destroy", "spotter_submit_raw_data", "spotter_get_next_event",
    "spotter_get_current_phrase", "spotter_error_get_message", "spotter_error_free"};
  int missing = 0;
  for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
    void *s = dlsym(l, names[i]);
    if (!s) { printf("MISSING %s\n", names[i]); missing++; }
  }
  printf("symbols: %d of %d found\n", (int)(sizeof names / sizeof *names) - missing, (int)(sizeof names / sizeof *names));
  // the VQE configuration of the stock preset: pure computation
  P (*create)(const char *, P *) = (P (*)(const char *, P *))dlsym(l, "YandexVqeC_Config_CreateFromPreset");
  int (*chunk)(P) = (int (*)(P))dlsym(l, "YandexVqeC_Config_GetInputChunkSize");
  int (*frame)(P) = (int (*)(P))dlsym(l, "YandexVqeC_Config_GetProcessingFrame");
  if (create && chunk && frame) {
    P cfg = 0;
    P err = create("yandexstation", &cfg);
    if (err) printf("preset yandexstation: error object returned\n");
    else printf("preset yandexstation: input chunk %d, processing frame %d\n", chunk(cfg), frame(cfg));
  }
  return missing ? 2 : 0;
}
