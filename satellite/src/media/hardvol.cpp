#include "media/hardvol.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace cs {

namespace {

using Points = std::vector<std::pair<double, double>>;

// Through the points (first column ascending), clamped at both ends.
double interp(const Points &pts, double x) {
  if (x <= pts.front().first) return pts.front().second;
  for (size_t i = 1; i < pts.size(); i++)
    if (x <= pts[i].first)
      return pts[i - 1].second +
             (x - pts[i - 1].first) * (pts[i].second - pts[i - 1].second) / (pts[i].first - pts[i - 1].first);
  return pts.back().second;
}

Points swapped(const Points &pts) {
  Points out;
  for (const auto &p : pts) out.push_back({p.second, p.first});
  return out;
}

bool ascending(const Points &pts) {
  for (size_t i = 1; i < pts.size(); i++)
    if (pts[i].first <= pts[i - 1].first || pts[i].second <= pts[i - 1].second) return false;
  return true;
}

// The text of the next <tag>...</tag> at or after pos, inside [pos, end); pos moves past it.
bool next_element(const std::string &s, const std::string &tag, size_t &pos, size_t end, std::string &text) {
  const size_t open = s.find("<" + tag, pos);
  if (open == std::string::npos || open >= end) return false;
  const size_t gt = s.find('>', open);
  const size_t close = s.find("</" + tag + ">", open);
  if (gt == std::string::npos || close == std::string::npos || close > end || gt > close) return false;
  text = s.substr(open, close - open);  // the start tag with its attributes, then the content after '>'
  pos = close + tag.size() + 3;
  return true;
}

// "<a, b>" content of a point element: two numbers separated by a comma.
bool point(const std::string &el, std::pair<double, double> &p) {
  const char *c = el.c_str() + el.find('>') + 1;
  char *e = nullptr;
  p.first = strtod(c, &e);
  if (e == c) return false;
  while (*e == ' ' || *e == '\t') e++;
  if (*e != ',') return false;
  c = e + 1;
  p.second = strtod(c, &e);
  return e != c;
}

std::string attribute(const std::string &start_tag, const std::string &name) {
  const size_t a = start_tag.find(name + "=\"");
  if (a == std::string::npos || a > start_tag.find('>')) return "";
  const size_t b = a + name.size() + 2, q = start_tag.find('"', b);
  return q == std::string::npos ? "" : start_tag.substr(b, q - b);
}

}  // namespace

int HardvolCtl::value(double pr) const {
  if (pr <= 0) return mute;
  const double v = interp(db2val, interp(per2db, std::min(100.0, pr)));
  return int(std::lround(std::max(db2val.front().second, std::min(db2val.back().second, v))));
}

double HardvolCtl::percent(int v) const {
  if (v <= mute && mute < db2val.front().second) return 0;
  return interp(swapped(per2db), interp(swapped(db2val), v));
}

std::string parse_hardvol(const std::string &xml_in, std::vector<HardvolCtl> &out) {
  std::string xml = xml_in;
  for (size_t c; (c = xml.find("<!--")) != std::string::npos;) {  // comments may hold anything
    const size_t e = xml.find("-->", c);
    xml.erase(c, e == std::string::npos ? std::string::npos : e + 3 - c);
  }
  out.clear();
  size_t pos = 0;
  std::string ctl;
  while (next_element(xml, "ctl", pos, xml.size(), ctl)) {
    HardvolCtl h;
    h.name = attribute(ctl, "name");
    if (h.name.empty()) return "a <ctl> without a name";
    std::string el;
    for (size_t p = 0; next_element(ctl, "per2db", p, ctl.size(), el);) {
      std::pair<double, double> pt;
      if (!point(el, pt)) return h.name + ": a per2db point is not '<percent>, <mdB>'";
      h.per2db.push_back(pt);
    }
    for (size_t p = 0; next_element(ctl, "db2val", p, ctl.size(), el);) {
      std::pair<double, double> pt;
      if (!point(el, pt)) return h.name + ": a db2val point is not '<mdB>, <value>'";
      h.db2val.push_back(pt);
    }
    size_t p = 0;
    if (next_element(ctl, "mute_value", p, ctl.size(), el)) h.mute = atoi(el.c_str() + el.find('>') + 1);
    if (h.per2db.size() < 2 || h.per2db.front().first != 1 || h.per2db.back().first != 100 || !ascending(h.per2db))
      return h.name + ": per2db must go from 1 to 100 % with both columns ascending";
    if (h.db2val.size() < 2 || !ascending(h.db2val)) return h.name + ": db2val needs two or more ascending points";
    out.push_back(std::move(h));
  }
  return out.empty() ? "no <ctl> in the table" : "";
}

}  // namespace cs
