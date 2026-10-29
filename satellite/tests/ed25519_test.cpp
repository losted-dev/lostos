// Ed25519 of the daemon (update/https.cpp: the firmware's OpenSSL, else Monocypher) against the RFC 8032 test vectors.
// Build: compile monocypher.c and optional/monocypher-ed25519.c with gcc -std=c99, then
//   g++ -std=c++20 -Isrc -I<monocypher>/src -I<monocypher>/src/optional tests/ed25519_test.cpp src/update/https.cpp src/core/log.cpp <the two .o> -ldl
// Run once as is (the host's OpenSSL) and once with libssl hidden (the Monocypher path, as on Station 1).
#include <cstdio>
#include <cstring>
#include <string>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "update/tls.h"

static int fails = 0;
static void check(const char *what, bool ok) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) fails++;
}

static void unhex(const char *h, uint8_t *out) {
  for (size_t i = 0; h[2 * i]; i++) sscanf(h + 2 * i, "%2hhx", &out[i]);
}

struct Vec {
  const char *name, *seed, *pub, *msg, *sig;
};
static const Vec kVecs[] = {
    {"RFC 8032 test 1 (empty message)", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"RFC 8032 test 2 (one byte)", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
};

int main() {
  for (const Vec &v : kVecs) {
    uint8_t seed[32], pub[32], want_sig[64], msg[64] = {};
    unhex(v.seed, seed);
    unhex(v.pub, pub);
    unhex(v.sig, want_sig);
    const size_t mlen = strlen(v.msg) / 2;
    unhex(v.msg, msg);
    const std::string m(reinterpret_cast<char *>(msg), mlen);

    // Monocypher directly, as the fallback in https.cpp calls it
    uint8_t s2[32], secret[64], p[32], sig[64];
    memcpy(s2, seed, 32);
    crypto_ed25519_key_pair(secret, p, s2);
    crypto_ed25519_sign(sig, secret, msg, mlen);
    std::string n = std::string(v.name) + ": Monocypher public key, signature, check";
    check(n.c_str(), !memcmp(p, pub, 32) && !memcmp(sig, want_sig, 64) && crypto_ed25519_check(sig, pub, msg, mlen) == 0);

    // the daemon's functions (on this host: whichever path ssl() takes)
    uint8_t dp[32], ds[64];
    n = std::string(v.name) + ": cs::ed25519_pub / _sign / _verify";
    check(n.c_str(), cs::ed25519_pub(seed, dp) && !memcmp(dp, pub, 32) && cs::ed25519_sign(seed, m, ds) && !memcmp(ds, want_sig, 64) &&
                         cs::ed25519_verify(pub, m, ds));
    ds[10] ^= 1;
    n = std::string(v.name) + ": a changed signature is refused";
    check(n.c_str(), !cs::ed25519_verify(pub, m, ds) && crypto_ed25519_check(ds, pub, msg, mlen) != 0);
  }
  check("ed25519_available() is always true now", cs::ed25519_available());
  printf("ca_bundle(): %s\n", cs::ca_bundle());
  printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
  return fails ? 1 : 0;
}
