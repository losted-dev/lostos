#include "plugins/trust.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

#include "update/https.h"
#include "update/tls.h"

namespace cs {

namespace {

// The keys that this firmware trusts, public parts (base64 of 32 bytes). They are built in on purpose: the server that talks to the speaker has no say in who is trusted.
// Changing them needs a new firmware (the keys of the lostOS team do not change often; see tools/ and docs/plugins.md of the server for how they are made).
const char *const kTeamKeys[] = {"Kn7QcpJmGVBEwX3DAhFlFQzmA8AcRVrPKKgOqtlth08="};            // sign packages, and the list of developers too
const char *const kListKeys[] = {"0BfdLXVvjXbwewfoIz0PNDv0QDM+QkbXOMF5oGXHEMs=",            // the key of plugapi.losted.dev (scope "developers": it signs the list, never a package)
                                 "Kn7QcpJmGVBEwX3DAhFlFQzmA8AcRVrPKKgOqtlth08="};
constexpr int64_t kSane = 1735689600;                                                       // 2025-01-01: an earlier clock is a clock that has not been set
constexpr size_t kMaxFile = 256 * 1024;

std::string slurp(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return s.size() > kMaxFile ? std::string() : s;
}

std::string b64_decode(const std::string &in) {
  static const std::string tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const size_t v = tab.find(c);
    if (v == std::string::npos) return "";
    acc = (acc << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(char((acc >> bits) & 0xff));
    }
  }
  return out;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// "key": "value" : the first one in the text
std::string str_field(const std::string &j, const std::string &key) {
  size_t p = j.find("\"" + key + "\"");
  if (p == std::string::npos) return "";
  p = j.find(':', p);
  if (p == std::string::npos) return "";
  p++;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\n')) p++;
  if (p >= j.size() || j[p] != '"') return "";
  const size_t e = j.find('"', p + 1);
  return e == std::string::npos ? "" : j.substr(p + 1, e - p - 1);
}

// the strings of the array that follows "key": ["a", "b"]  (from `from` on)
std::vector<std::string> str_array(const std::string &j, const std::string &key, size_t from = 0) {
  std::vector<std::string> out;
  size_t p = j.find("\"" + key + "\"", from);
  if (p == std::string::npos) return out;
  p = j.find('[', p);
  const size_t end = p == std::string::npos ? p : j.find(']', p);
  if (end == std::string::npos) return out;
  for (size_t i = p + 1; i < end;) {
    const size_t a = j.find('"', i);
    if (a == std::string::npos || a >= end) break;
    const size_t b = j.find('"', a + 1);
    if (b == std::string::npos || b > end) break;
    out.push_back(j.substr(a + 1, b - a - 1));
    i = b + 1;
  }
  return out;
}

// the table "files": {"path": "sha256", ...} of the signature file (it is the last thing in it; a path with an escape is refused)
bool parse_files(const std::string &sig, std::map<std::string, std::string> &out) {
  size_t p = sig.find("\"files\"");
  if (p == std::string::npos) return false;
  p = sig.find('{', p);
  const size_t end = p == std::string::npos ? p : sig.find('}', p);
  if (end == std::string::npos) return false;
  const std::string body = sig.substr(p + 1, end - p - 1);
  if (body.find('\\') != std::string::npos) return false;
  for (size_t i = 0; i < body.size();) {
    const size_t a = body.find('"', i);
    if (a == std::string::npos) break;
    const size_t b = body.find('"', a + 1);
    const size_t c = b == std::string::npos ? b : body.find('"', b + 1);
    const size_t d = c == std::string::npos ? c : body.find('"', c + 1);
    if (d == std::string::npos) return false;
    out[body.substr(a + 1, b - a - 1)] = body.substr(c + 1, d - c - 1);
    i = d + 1;
  }
  return !out.empty();
}

bool ends_with(const std::string &s, const std::string &t) { return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }

bool same_key(const std::string &a_b64, const std::string &raw) { return b64_decode(a_b64) == raw; }

// Is the list of developers (as plugapi signed it) genuine, in date, and does it vouch for this key, and has it not revoked it? why: set when not.
bool on_verified_list(const std::string &dir, const std::string &pub, int64_t now, std::string &why) {
  const std::string raw = slurp(dir + "/trust/developers.json"), sig = trim(slurp(dir + "/trust/developers.json.sig"));
  if (raw.empty() || sig.empty()) {
    why = "no list of verified developers";
    return false;
  }
  const std::string sigb = b64_decode(sig);
  bool genuine = false;
  if (sigb.size() == 64) {
    const std::string msg = std::string("lostos-developers-v1\n") + raw;
    for (const char *k : kListKeys) {
      const std::string key = b64_decode(k);
      if (key.size() == 32 && ed25519_verify(reinterpret_cast<const uint8_t *>(key.data()), msg, reinterpret_cast<const uint8_t *>(sigb.data()))) {
        genuine = true;
        break;
      }
    }
  }
  if (!genuine) {
    why = "the list of verified developers is not signed by plugapi";
    return false;
  }
  struct tm tm {};
  const std::string exp = str_field(raw, "expires");
  if (sscanf(exp.c_str(), "%d-%d-%dT%d:%d:%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
    why = "the list of verified developers has no end date";
    return false;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  const int64_t expires = int64_t(timegm(&tm));
  if (now < kSane || now > expires) {
    why = now < kSane ? "the clock of the speaker is not set" : "the list of verified developers is out of date";
    return false;
  }
  const std::string fp = sha256_hex(pub).substr(0, 16);
  for (const std::string &r : str_array(raw, "revoked"))
    if (r == fp) {
      why = "the key is revoked";
      return false;
    }
  size_t from = raw.find("\"developers\"");
  if (from == std::string::npos) {
    why = "the key is not on the list of verified developers";
    return false;
  }
  for (size_t p = raw.find("\"key\"", from); p != std::string::npos; p = raw.find("\"key\"", p + 5)) {
    const std::string k = str_field(raw.substr(p), "key");
    if (!k.empty() && same_key(k, pub)) return true;
  }
  why = "the key is not on the list of verified developers";
  return false;
}

}  // namespace

bool dangerous_permission(const std::string &perm) {
  return perm == "network.connect" || perm == "network.listen" || perm == "audio.play" || perm == "assistant.intercept" || perm == "events.publish";
}

std::vector<std::string> permissions_to_grant(const std::vector<std::string> &asked, const PluginTrust &t) {
  std::vector<std::string> out;
  for (const std::string &g : asked) {
    if (std::find(out.begin(), out.end(), g) != out.end()) continue;
    const bool declared = std::find(t.declared.begin(), t.declared.end(), g) != t.declared.end();
    if (t.trusted ? declared : !dangerous_permission(g)) out.push_back(g);
  }
  return out;
}

PluginTrust verify_plugin(const std::string &dir, const std::string &program_sha, int64_t now) {
  PluginTrust t;
  auto fail = [&t](const std::string &why) {
    t.trusted = false;
    t.why = why;
    return t;
  };
  const std::string sig = slurp(dir + "/trust/lostos-signature.json");
  if (sig.empty()) return fail("the package carries no signature");
  const size_t files_at = sig.find("\"files\"");
  if (files_at == std::string::npos) return fail("the signature has no table of files (signed by an old tool)");
  const std::string head = sig.substr(0, files_at);
  const std::string key_b64 = str_field(head, "key"), digest = str_field(head, "digest"), sig_b64 = str_field(head, "sig");
  t.who = str_field(head, "publisher");
  const std::string pub = b64_decode(key_b64), sigb = b64_decode(sig_b64);
  if (pub.size() != 32 || sigb.size() != 64 || digest.size() != 64) return fail("the signature is malformed");
  std::map<std::string, std::string> files;
  if (!parse_files(sig, files)) return fail("the table of files is malformed");

  // what this speaker holds must be what was signed: the program, and the manifest whose permissions are going to be believed
  bool program_listed = false;
  for (const auto &kv : files)
    if (kv.second == program_sha) program_listed = true;
  if (!program_listed) return fail("the program is not one of the signed files");
  std::string manifest_sha;
  for (const auto &kv : files)
    if (kv.first == "manifest.json" || ends_with(kv.first, "/manifest.json")) manifest_sha = kv.second;
  const std::string manifest = slurp(dir + "/trust/manifest.json");
  if (manifest.empty() || manifest_sha.empty() || sha256_hex(manifest) != manifest_sha) return fail("the manifest is not the signed one");

  // the digest that the signer made, recomputed from the table, and the signature over it
  std::string lines;
  for (const auto &kv : files) {
    lines += kv.first;
    lines.push_back('\0');
    lines += kv.second;
    lines.push_back('\n');
  }
  if (sha256_hex(lines) != digest) return fail("the table of files does not match the signed digest");
  if (!ed25519_verify(reinterpret_cast<const uint8_t *>(pub.data()), std::string("lostos-plugin-v1\n") + digest, reinterpret_cast<const uint8_t *>(sigb.data())))
    return fail("the signature is not valid");

  // whose key is it
  bool team = false;
  for (const char *k : kTeamKeys)
    if (same_key(k, pub)) team = true;
  std::string why;
  if (!team && !on_verified_list(dir, pub, now, why)) return fail(why);

  // the permissions that the signed manifest declares for the speaker part
  const size_t dev = manifest.find("\"device\"");
  if (dev != std::string::npos) t.declared = str_array(manifest, "permissions", dev);
  t.trusted = true;
  return t;
}

}  // namespace cs
