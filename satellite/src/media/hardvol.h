// The volume table of the stock audio HAL of Station 1 (/system/etc/hardvol_ti.xml, read by
// audio.primary.tulip.so for its parameter hw_master_volume, 0..100 %). For each codec control:
// percent -> millidB piecewise linear through the <per2db> points (1 % .. 100 %), millidB -> control
// value linear through the <db2val> points, <mute_value> for silence. Mixer "hardvol" (player.h)
// writes these controls itself, as the HAL would for maind.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cs {

struct HardvolCtl {
  std::string name;
  std::vector<std::pair<double, double>> per2db, db2val;  // ascending in both columns
  int mute = 0;

  int value(double percent) const;  // percent <= 0: the mute value
  double percent(int value) const;  // 0 for the mute value or below the table
};

// Reads the <ctl> elements of the HAL's XML: "" when there is at least one valid control, else why not
// (the HAL's own checks: per2db from 1 to 100 %, both columns ascending, at least two db2val points).
std::string parse_hardvol(const std::string &xml, std::vector<HardvolCtl> &out);

}  // namespace cs
