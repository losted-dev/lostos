// A TLS client socket on the firmware's own OpenSSL (the same dlopen'ed libssl as https.h), non-blocking after the handshake, for code that
// multiplexes it with other descriptors (the station link). Plus Ed25519 from the same library.
#pragma once

#include <cstdint>
#include <string>

namespace cs {

struct TlsSock {
  int fd = -1;
  void *ssl = nullptr;
};

enum class TlsIo { OK, WANT_READ, WANT_WRITE, CLOSED, ERROR };

// Resolves, connects and does the handshake (blocking, the certificate and the host name are checked); the socket is non-blocking afterwards.
bool tls_connect(const std::string &host, const std::string &port, TlsSock &s, std::string &err);
// The same socket WITHOUT encryption, for a server in the owner's own home network (http://192.168.x.y): nothing is checked and anybody on that network can read it. The caller must refuse any address that is not private.
bool plain_connect(const std::string &host, const std::string &port, TlsSock &s, std::string &err);
TlsIo tls_read(TlsSock &s, char *buf, int cap, int &n);
TlsIo tls_write(TlsSock &s, const char *buf, int len, int &n);
bool tls_pending(TlsSock &s);  // decrypted bytes are already waiting inside the library (poll() would not show them)
void tls_close(TlsSock &s);

// The firmware's CA bundle (the first of the known places that exists).
const char *ca_bundle();

// Ed25519: the firmware's OpenSSL when it has it (1.1.1+), else the built-in Monocypher; always available.
bool ed25519_available();
bool ed25519_pub(const uint8_t priv[32], uint8_t pub[32]);
bool ed25519_sign(const uint8_t priv[32], const std::string &msg, uint8_t sig[64]);
bool ed25519_verify(const uint8_t pub[32], const std::string &msg, const uint8_t sig[64]);

}  // namespace cs
