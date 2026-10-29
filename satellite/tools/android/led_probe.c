// Lights the 24 ring LEDs of Station 1 one by one so their order and colour wiring can be seen.
// Hardware (from /system/etc/rc.local and the vendor i2c_24led_* tools): two SN3236 drivers on /dev/i2c-0,
// LEDs 1..12 at 0x3F and 13..24 at 0x3C, three PWM registers per LED from 0x01 in the order G, R, B;
// 0x00 = shutdown (1 runs), 0x25 = update, 0x26.. = LED control (1 = on at full current).
// Usage: led_probe walk [ms]   - each LED red, then the first LED green and blue
//        led_probe all R G B   - every LED one colour (0 0 0 switches off)
//        led_probe set N:R,G,B ... - the named LEDs (1..24) in their colours, the others off
//        led_probe als         - reads the light sensor at 0x53 (rc.local enables it with reg 0x00 = 2; LTR-390 layout assumed:
//                                0x06 part id, 0x0D..0x0F ALS data), to see the ring light up with nobody at the speaker
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const int kChip[2] = {0x3F, 0x3C};
static int fd;

static int wr(int chip, int reg, const unsigned char *data, int n) {
  unsigned char b[64];
  b[0] = reg;
  memcpy(b + 1, data, n);
  if (ioctl(fd, I2C_SLAVE_FORCE, chip) < 0 || write(fd, b, n + 1) != n + 1) {
    printf("i2c 0x%02x reg 0x%02x: write failed\n", chip, reg);
    return -1;
  }
  return 0;
}

// rgb: 24 * 3 bytes R,G,B
static int show(const unsigned char *rgb) {
  for (int c = 0; c < 2; c++) {
    unsigned char pwm[36], on[36], one = 1, zero = 0;
    for (int i = 0; i < 12; i++) {
      const unsigned char *p = rgb + (c * 12 + i) * 3;
      pwm[i * 3] = p[1], pwm[i * 3 + 1] = p[0], pwm[i * 3 + 2] = p[2];
    }
    memset(on, 1, sizeof on);
    if (wr(kChip[c], 0x00, &one, 1) || wr(kChip[c], 0x26, on, 36) || wr(kChip[c], 0x01, pwm, 36) || wr(kChip[c], 0x25, &zero, 1)) return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s walk [ms] | all R G B\n", argv[0]); return 1; }
  fd = open("/dev/i2c-0", O_RDWR);
  if (fd < 0) { perror("/dev/i2c-0"); return 1; }
  unsigned char rgb[72];
  memset(rgb, 0, sizeof rgb);
  if (!strcmp(argv[1], "als")) {
    unsigned char reg = 0x06, id = 0, d[3] = {0};
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x53) < 0 || write(fd, &reg, 1) != 1 || read(fd, &id, 1) != 1) { printf("als: no answer\n"); return 2; }
    reg = 0x0D;
    if (write(fd, &reg, 1) != 1 || read(fd, d, 3) != 3) { printf("als: read failed\n"); return 2; }
    printf("als: part id 0x%02x, data %u\n", id, d[0] | d[1] << 8 | (d[2] & 0x0F) << 16);
    return 0;
  }
  if (!strcmp(argv[1], "set")) {  // set N:R,G,B ... (the others off)
    for (int a = 2; a < argc; a++) {
      int n, r, g, b;
      if (sscanf(argv[a], "%d:%d,%d,%d", &n, &r, &g, &b) != 4 || n < 1 || n > 24) { fprintf(stderr, "bad %s\n", argv[a]); return 1; }
      rgb[(n - 1) * 3] = r, rgb[(n - 1) * 3 + 1] = g, rgb[(n - 1) * 3 + 2] = b;
    }
    return show(rgb) ? 2 : 0;
  }
  if (!strcmp(argv[1], "all") && argc == 5) {
    for (int i = 0; i < 24; i++) rgb[i * 3] = atoi(argv[2]), rgb[i * 3 + 1] = atoi(argv[3]), rgb[i * 3 + 2] = atoi(argv[4]);
    return show(rgb) ? 2 : 0;
  }
  if (strcmp(argv[1], "walk")) return 1;
  int ms = argc > 2 ? atoi(argv[2]) : 700;
  for (int i = 0; i < 24; i++) {
    memset(rgb, 0, sizeof rgb);
    rgb[i * 3] = 255;
    printf("LED %d red\n", i + 1);
    fflush(stdout);
    if (show(rgb)) return 2;
    usleep(ms * 1000);
  }
  const char *name[3] = {"red", "green", "blue"};
  for (int k = 0; k < 3; k++) {
    memset(rgb, 0, sizeof rgb);
    rgb[k] = 255;
    printf("LED 1 %s\n", name[k]);
    fflush(stdout);
    if (show(rgb)) return 2;
    usleep(1500 * 1000);
  }
  memset(rgb, 0, sizeof rgb);
  return show(rgb) ? 2 : 0;
}
