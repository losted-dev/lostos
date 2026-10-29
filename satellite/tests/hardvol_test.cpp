// The volume table of Station 1's audio HAL (media/hardvol.*): g++ -std=c++17 -Isrc tests/hardvol_test.cpp src/media/hardvol.cpp -o /tmp/hvt && /tmp/hvt
// The table below has the numbers of /system/etc/hardvol_ti.xml (read on a speaker, 2026-10-29); the expected values are the
// controls a speaker showed at the stock volume 10 % (Subwoofer 484, Tweeters 492).
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "media/hardvol.h"

using namespace cs;

static int fails = 0;

static void check(const char *what, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) fails++;
}

static const char *kTable = R"(<hardvol>
  <init_value_pr>85</init_value_pr>
  <!-- a comment with <ctl name="ignored"> inside -->
  <ctl name="Tweeters Master Playback Volume" path="speaker">
    <per2db>  1, -58125</per2db>
    <per2db> 13, -37125</per2db>
    <per2db> 60,  -8625</per2db>
    <per2db>100,   8375</per2db>
    <db2val>-103750,    1</db2val>
    <db2val>  24000, 1023</db2val>
    <mute_value>0</mute_value>
  </ctl>
  <ctl name="Subwoofer Master Playback Volume" path="speaker">
    <per2db>  1, -59125</per2db>
    <per2db> 13, -38125</per2db>
    <per2db> 60,  -9625</per2db>
    <per2db>100,   7375</per2db>
    <db2val>-103750,    1</db2val>
    <db2val>  24000, 1023</db2val>
    <mute_value>0</mute_value>
  </ctl>
</hardvol>
)";

int main() {
  std::vector<HardvolCtl> t;
  const std::string err = parse_hardvol(kTable, t);
  check("the table parses", err.empty());
  check("two controls, the commented one skipped", t.size() == 2 && t[0].name == "Tweeters Master Playback Volume" &&
                                                     t[1].name == "Subwoofer Master Playback Volume");
  if (t.size() != 2) return 1;
  const HardvolCtl &tw = t[0], &sub = t[1];
  check("10 % gives what the stock left on a speaker (492 / 484)", tw.value(10) == 492 && sub.value(10) == 484);
  check("85 % (the HAL's start value): +2 dB = 847 on the tweeters", tw.value(85) == 847);
  check("100 % = +8.375 dB = 898, 1 % = -58.125 dB = 366", tw.value(100) == 898 && tw.value(1) == 366);
  check("0 % is the mute value", tw.value(0) == 0 && sub.value(-5) == 0);
  check("above 100 % stays at 100 %", tw.value(150) == tw.value(100));
  bool up = true;
  for (int p = 2; p <= 100; p++) up = up && tw.value(p) >= tw.value(p - 1) && sub.value(p) >= sub.value(p - 1);
  check("louder with every percent", up);
  check("back from the control value: 492 -> 10 %", std::fabs(tw.percent(492) - 10.0) < 0.01 && std::fabs(sub.percent(484) - 10.0) < 0.01);
  check("back from the mute value: 0 %", tw.percent(0) == 0);
  bool round = true;
  for (int p = 1; p <= 100; p++) round = round && std::lround(tw.percent(tw.value(p))) == p;
  check("percent -> value -> percent round trip", round);

  std::vector<HardvolCtl> bad;
  check("a table without <ctl> is refused", !parse_hardvol("<hardvol></hardvol>", bad).empty());
  check("per2db must start at 1 %", !parse_hardvol(R"(<ctl name="x"><per2db>0, -60000</per2db><per2db>100, 0</per2db>
      <db2val>-100000, 1</db2val><db2val>0, 100</db2val></ctl>)", bad).empty());
  check("db2val must ascend", !parse_hardvol(R"(<ctl name="x"><per2db>1, -60000</per2db><per2db>100, 0</per2db>
      <db2val>0, 100</db2val><db2val>-100000, 1</db2val></ctl>)", bad).empty());
  check("a point must be two numbers", !parse_hardvol(R"(<ctl name="x"><per2db>1 -60000</per2db><per2db>100, 0</per2db>
      <db2val>-100000, 1</db2val><db2val>0, 100</db2val></ctl>)", bad).empty());
  printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
  return fails ? 1 : 0;
}
