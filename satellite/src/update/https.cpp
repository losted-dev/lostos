#include "update/https.h"
#include "update/tls.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "core/log.h"

namespace cs {
namespace {

constexpr const char *kCaBundle = "/etc/ssl/certs/ca-certificates.crt";
constexpr int kConnectTimeoutMs = 10000;
constexpr int kIoTimeoutS = 20;
// OpenSSL constants, identical in 1.1 and 3.
constexpr int SSL_VERIFY_PEER = 1;
constexpr int SSL_CTRL_SET_TLSEXT_HOSTNAME = 55;
constexpr int TLSEXT_NAMETYPE_host_name = 0;
constexpr int SSL_CTRL_SET_MIN_PROTO_VERSION = 123;
constexpr long TLS1_2_VERSION = 0x0303;
constexpr int SSL_ERROR_ZERO_RETURN = 6;
constexpr int SSL_ERROR_SYSCALL = 5;

struct Ssl {
  bool ok = false;
  std::string error;
  int (*OPENSSL_init_ssl)(uint64_t, const void *);
  const void *(*TLS_client_method)();
  void *(*SSL_CTX_new)(const void *);
  void (*SSL_CTX_free)(void *);
  int (*SSL_CTX_load_verify_locations)(void *, const char *, const char *);
  void (*SSL_CTX_set_verify)(void *, int, void *);
  long (*SSL_CTX_ctrl)(void *, int, long, void *);
  void *(*SSL_new)(void *);
  void (*SSL_free)(void *);
  int (*SSL_set_fd)(void *, int);
  long (*SSL_ctrl)(void *, int, long, void *);
  int (*SSL_set1_host)(void *, const char *);
  int (*SSL_connect)(void *);
  int (*SSL_read)(void *, void *, int);
  int (*SSL_write)(void *, const void *, int);
  int (*SSL_shutdown)(void *);
  int (*SSL_get_error)(const void *, int);
  long (*SSL_get_verify_result)(const void *);
  unsigned long (*ERR_get_error)();
  void (*ERR_error_string_n)(unsigned long, char *, size_t);
  void (*ERR_clear_error)();
  const char *(*X509_verify_cert_error_string)(long);
  unsigned char *(*SHA256)(const unsigned char *, size_t, unsigned char *);
  int (*SSL_pending)(const void *);
  // Ed25519 through EVP (optional: OpenSSL 1.1.1+)
  void *(*EVP_PKEY_new_raw_private_key)(int, void *, const unsigned char *, size_t) = nullptr;
  int (*EVP_PKEY_get_raw_public_key)(const void *, unsigned char *, size_t *) = nullptr;
  void (*EVP_PKEY_free)(void *) = nullptr;
  void *(*EVP_MD_CTX_new)() = nullptr;
  void (*EVP_MD_CTX_free)(void *) = nullptr;
  int (*EVP_DigestSignInit)(void *, void **, const void *, void *, void *) = nullptr;
  int (*EVP_DigestSign)(void *, unsigned char *, size_t *, const unsigned char *, size_t) = nullptr;
  void *(*EVP_PKEY_new_raw_public_key)(int, void *, const unsigned char *, size_t) = nullptr;
  int (*EVP_DigestVerifyInit)(void *, void **, const void *, void *, void *) = nullptr;
  int (*EVP_DigestVerify)(void *, const unsigned char *, size_t, const unsigned char *, size_t) = nullptr;
  bool ed = false;
  bool ed_verify = false;
  void *ctx = nullptr;  // shared, configured once (SSL_CTX is thread-safe once set up)
};

template <typename T>
bool sym(void *h, const char *name, T &out, std::string &missing) {
  out = reinterpret_cast<T>(dlsym(h, name));
  if (!out) missing = name;
  return out != nullptr;
}

const Ssl &ssl() {
  static Ssl s;
  static std::once_flag once;
  std::call_once(once, [] {
    void *h = nullptr;
    for (const char *lib : {"libssl.so.3", "libssl.so.1.1", "libssl.so"})
      if ((h = dlopen(lib, RTLD_NOW | RTLD_LOCAL))) break;
    if (!h) {
      s.error = std::string("no libssl: ") + dlerror();
      return;
    }
    std::string m;
    // dlsym on the libssl handle also searches its dependency libcrypto.
    if (!(sym(h, "OPENSSL_init_ssl", s.OPENSSL_init_ssl, m) && sym(h, "TLS_client_method", s.TLS_client_method, m) &&
          sym(h, "SSL_CTX_new", s.SSL_CTX_new, m) && sym(h, "SSL_CTX_free", s.SSL_CTX_free, m) &&
          sym(h, "SSL_CTX_load_verify_locations", s.SSL_CTX_load_verify_locations, m) &&
          sym(h, "SSL_CTX_set_verify", s.SSL_CTX_set_verify, m) && sym(h, "SSL_CTX_ctrl", s.SSL_CTX_ctrl, m) &&
          sym(h, "SSL_new", s.SSL_new, m) && sym(h, "SSL_free", s.SSL_free, m) &&
          sym(h, "SSL_set_fd", s.SSL_set_fd, m) && sym(h, "SSL_ctrl", s.SSL_ctrl, m) &&
          sym(h, "SSL_set1_host", s.SSL_set1_host, m) && sym(h, "SSL_connect", s.SSL_connect, m) &&
          sym(h, "SSL_read", s.SSL_read, m) && sym(h, "SSL_write", s.SSL_write, m) &&
          sym(h, "SSL_shutdown", s.SSL_shutdown, m) && sym(h, "SSL_get_error", s.SSL_get_error, m) &&
          sym(h, "SSL_get_verify_result", s.SSL_get_verify_result, m) &&
          sym(h, "ERR_get_error", s.ERR_get_error, m) && sym(h, "ERR_error_string_n", s.ERR_error_string_n, m) &&
          sym(h, "ERR_clear_error", s.ERR_clear_error, m) &&
          sym(h, "X509_verify_cert_error_string", s.X509_verify_cert_error_string, m) &&
          sym(h, "SHA256", s.SHA256, m) && sym(h, "SSL_pending", s.SSL_pending, m))) {
      s.error = "libssl: missing " + m;
      return;
    }
    {
      std::string ignored;
      s.ed = sym(h, "EVP_PKEY_new_raw_private_key", s.EVP_PKEY_new_raw_private_key, ignored) &&
             sym(h, "EVP_PKEY_get_raw_public_key", s.EVP_PKEY_get_raw_public_key, ignored) && sym(h, "EVP_PKEY_free", s.EVP_PKEY_free, ignored) &&
             sym(h, "EVP_MD_CTX_new", s.EVP_MD_CTX_new, ignored) && sym(h, "EVP_MD_CTX_free", s.EVP_MD_CTX_free, ignored) &&
             sym(h, "EVP_DigestSignInit", s.EVP_DigestSignInit, ignored) && sym(h, "EVP_DigestSign", s.EVP_DigestSign, ignored);
      s.ed_verify = sym(h, "EVP_PKEY_new_raw_public_key", s.EVP_PKEY_new_raw_public_key, ignored) && sym(h, "EVP_PKEY_free", s.EVP_PKEY_free, ignored) &&
                    sym(h, "EVP_MD_CTX_new", s.EVP_MD_CTX_new, ignored) && sym(h, "EVP_MD_CTX_free", s.EVP_MD_CTX_free, ignored) &&
                    sym(h, "EVP_DigestVerifyInit", s.EVP_DigestVerifyInit, ignored) && sym(h, "EVP_DigestVerify", s.EVP_DigestVerify, ignored);
    }
    s.OPENSSL_init_ssl(0, nullptr);
    s.ctx = s.SSL_CTX_new(s.TLS_client_method());
    if (!s.ctx) {
      s.error = "SSL_CTX_new failed";
      return;
    }
    s.SSL_CTX_ctrl(s.ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, TLS1_2_VERSION, nullptr);
    s.SSL_CTX_set_verify(s.ctx, SSL_VERIFY_PEER, nullptr);
    if (s.SSL_CTX_load_verify_locations(s.ctx, kCaBundle, nullptr) != 1) {
      s.error = std::string("cannot load ") + kCaBundle;
      return;
    }
    s.ok = true;
  });
  return s;
}

std::string ssl_error(const Ssl &s, const char *what) {
  char buf[256] = "";
  unsigned long e = s.ERR_get_error();
  if (e) s.ERR_error_string_n(e, buf, sizeof buf);
  return std::string(what) + (e ? std::string(": ") + buf : std::string());
}

int tcp_connect(const std::string &host, const std::string &port, std::string &err) {
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
    err = "resolve " + host + ": " + gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  for (addrinfo *a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
    if (fd < 0) continue;
    int rc = connect(fd, a->ai_addr, a->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      int soerr = ETIMEDOUT;
      socklen_t len = sizeof soerr;
      if (poll(&p, 1, kConnectTimeoutMs) == 1) getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
      rc = soerr ? -1 : 0;
      errno = soerr;
    }
    if (rc == 0) break;
    err = "connect " + host + ": " + strerror(errno);
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  timeval tv{kIoTimeoutS, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

struct Url {
  std::string host, port = "443", path = "/";
};

bool parse_url(const std::string &url, Url &u) {
  const std::string scheme = "https://";
  if (url.compare(0, scheme.size(), scheme) != 0) return false;
  std::string rest = url.substr(scheme.size());
  size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash);
  if (slash != std::string::npos) u.path = rest.substr(slash);
  size_t colon = hostport.find(':');
  u.host = hostport.substr(0, colon);
  if (colon != std::string::npos) u.port = hostport.substr(colon + 1);
  return !u.host.empty();
}

// One TLS connection; buffered reads.
class Conn {
 public:
  Conn(const Ssl &s) : s_(s) {}
  ~Conn() {
    if (ssl_) {
      s_.SSL_shutdown(ssl_);
      s_.SSL_free(ssl_);
    }
    if (fd_ >= 0) close(fd_);
  }
  bool open(const Url &u, std::string &err) {
    fd_ = tcp_connect(u.host, u.port, err);
    if (fd_ < 0) return false;
    s_.ERR_clear_error();
    ssl_ = s_.SSL_new(s_.ctx);
    if (!ssl_) return err = ssl_error(s_, "SSL_new"), false;
    s_.SSL_set_fd(ssl_, fd_);
    s_.SSL_ctrl(ssl_, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, const_cast<char *>(u.host.c_str()));
    s_.SSL_set1_host(ssl_, u.host.c_str());
    if (s_.SSL_connect(ssl_) != 1) {
      long v = s_.SSL_get_verify_result(ssl_);
      err = v != 0 ? "TLS " + u.host + ": certificate: " + s_.X509_verify_cert_error_string(v)
                   : ssl_error(s_, ("TLS " + u.host).c_str());
      return false;
    }
    return true;
  }
  bool write_all(const std::string &d) {
    size_t off = 0;
    while (off < d.size()) {
      int n = s_.SSL_write(ssl_, d.data() + off, int(d.size() - off));
      if (n <= 0) return false;
      off += size_t(n);
    }
    return true;
  }
  // Reads more into buf_; false at EOF or error (err set on error).
  bool fill(std::string &err) {
    char tmp[16384];
    errno = 0;
    int n = s_.SSL_read(ssl_, tmp, sizeof tmp);
    if (n > 0) {
      buf_.append(tmp, size_t(n));
      return true;
    }
    int e = s_.SSL_get_error(ssl_, n);
    if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0)) return false;  // closed
    err = e == SSL_ERROR_SYSCALL ? std::string("read: ") + strerror(errno) : ssl_error(s_, "read");
    return false;
  }
  std::string buf_;

 private:
  const Ssl &s_;
  int fd_ = -1;
  void *ssl_ = nullptr;
};

std::string lower(std::string s) {
  for (auto &c : s) c = char(tolower(static_cast<unsigned char>(c)));
  return s;
}

// Reads the body (Content-Length, chunked or up to the close) into out; "" or an error.
std::string read_body(Conn &c, long long length, bool chunked, size_t max_bytes, std::string &out,
                      const std::function<void(size_t, size_t)> &progress, const std::atomic<bool> *cancel) {
  std::string err;
  auto more = [&] {
    if (cancel && cancel->load()) {
      err = "cancelled";
      return false;
    }
    if (c.fill(err)) return true;
    if (err.empty()) err = "connection closed in the body";
    return false;
  };
  if (length > 0 && size_t(length) > max_bytes) return "response too large";
  const size_t total = length > 0 ? size_t(length) : 0;
  if (chunked) {
    for (;;) {
      size_t eol;
      while ((eol = c.buf_.find("\r\n")) == std::string::npos)
        if (!more()) return err;
      size_t n = strtoul(c.buf_.c_str(), nullptr, 16);
      c.buf_.erase(0, eol + 2);
      if (n == 0) return "";
      if (out.size() + n > max_bytes) return "response too large";
      while (c.buf_.size() < n + 2)
        if (!more()) return err;
      out.append(c.buf_, 0, n);
      c.buf_.erase(0, n + 2);
      if (progress) progress(out.size(), 0);
    }
  }
  for (;;) {
    out += c.buf_;
    c.buf_.clear();
    if (out.size() > max_bytes) return "response too large";
    if (progress) progress(out.size(), total);
    if (length >= 0 && out.size() >= size_t(length)) {
      out.resize(size_t(length));
      return "";
    }
    if (!more()) return length < 0 && err == "connection closed in the body" ? "" : err;
  }
}

}  // namespace

// ---- TLS socket for the station link
constexpr int kEd25519 = 1087;  // EVP_PKEY_ED25519 (NID_ED25519), the same in 1.1.1 and 3
constexpr int SSL_CTRL_MODE = 33;
constexpr long SSL_MODE_ENABLE_PARTIAL_WRITE = 1, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER = 2;

bool tls_connect(const std::string &host, const std::string &port, TlsSock &t, std::string &err) {
  const Ssl &s = ssl();
  if (!s.ok) return err = s.error, false;
  t.fd = tcp_connect(host, port, err);
  if (t.fd < 0) return false;
  s.ERR_clear_error();
  t.ssl = s.SSL_new(s.ctx);
  if (!t.ssl) return err = ssl_error(s, "SSL_new"), tls_close(t), false;
  s.SSL_set_fd(t.ssl, t.fd);
  s.SSL_ctrl(t.ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, const_cast<char *>(host.c_str()));
  s.SSL_set1_host(t.ssl, host.c_str());
  if (s.SSL_connect(t.ssl) != 1) {
    long v = s.SSL_get_verify_result(t.ssl);
    err = v != 0 ? "TLS " + host + ": certificate: " + s.X509_verify_cert_error_string(v) : ssl_error(s, ("TLS " + host).c_str());
    tls_close(t);
    return false;
  }
  s.SSL_ctrl(t.ssl, SSL_CTRL_MODE, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER, nullptr);
  timeval none{0, 0};
  setsockopt(t.fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
  setsockopt(t.fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
  fcntl(t.fd, F_SETFL, fcntl(t.fd, F_GETFL) | O_NONBLOCK);
  return true;
}

bool plain_connect(const std::string &host, const std::string &port, TlsSock &t, std::string &err) {
  t.fd = tcp_connect(host, port, err);
  if (t.fd < 0) return false;
  t.ssl = nullptr;
  timeval none{0, 0};
  setsockopt(t.fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
  setsockopt(t.fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
  fcntl(t.fd, F_SETFL, fcntl(t.fd, F_GETFL) | O_NONBLOCK);
  return true;
}

static TlsIo tls_result(const Ssl &s, TlsSock &t, int r) {
  int e = s.SSL_get_error(t.ssl, r);
  if (e == 2) return TlsIo::WANT_READ;
  if (e == 3) return TlsIo::WANT_WRITE;
  if (e == SSL_ERROR_ZERO_RETURN || (e == SSL_ERROR_SYSCALL && errno == 0)) return TlsIo::CLOSED;
  return TlsIo::ERROR;
}

TlsIo tls_read(TlsSock &t, char *buf, int cap, int &n) {
  if (!t.ssl) {                                              // a plain socket (plain_connect)
    ssize_t r = recv(t.fd, buf, size_t(cap), 0);
    if (r > 0) return n = int(r), TlsIo::OK;
    n = 0;
    if (r == 0) return TlsIo::CLOSED;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? TlsIo::WANT_READ : TlsIo::ERROR;
  }
  const Ssl &s = ssl();
  errno = 0;
  int r = s.SSL_read(t.ssl, buf, cap);
  if (r > 0) return n = r, TlsIo::OK;
  return n = 0, tls_result(s, t, r);
}

TlsIo tls_write(TlsSock &t, const char *buf, int len, int &n) {
  if (!t.ssl) {
    ssize_t r = send(t.fd, buf, size_t(len), MSG_NOSIGNAL);
    if (r > 0) return n = int(r), TlsIo::OK;
    n = 0;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? TlsIo::WANT_WRITE : TlsIo::ERROR;
  }
  const Ssl &s = ssl();
  errno = 0;
  int r = s.SSL_write(t.ssl, buf, len);
  if (r > 0) return n = r, TlsIo::OK;
  return n = 0, tls_result(s, t, r);
}

bool tls_pending(TlsSock &t) { return t.ssl && ssl().SSL_pending(t.ssl) > 0; }

void tls_close(TlsSock &t) {
  if (t.fd >= 0 && !t.ssl) { close(t.fd); t.fd = -1; return; }
  const Ssl &s = ssl();
  if (t.ssl) {
    s.SSL_shutdown(t.ssl);
    s.SSL_free(t.ssl);
    t.ssl = nullptr;
  }
  if (t.fd >= 0) close(t.fd);
  t.fd = -1;
}

bool ed25519_available() { return ssl().ok && ssl().ed; }

bool ed25519_pub(const uint8_t priv[32], uint8_t pub[32]) {
  const Ssl &s = ssl();
  if (!ed25519_available()) return false;
  void *k = s.EVP_PKEY_new_raw_private_key(kEd25519, nullptr, priv, 32);
  if (!k) return false;
  size_t len = 32;
  bool ok = s.EVP_PKEY_get_raw_public_key(k, pub, &len) == 1 && len == 32;
  s.EVP_PKEY_free(k);
  return ok;
}

bool ed25519_sign(const uint8_t priv[32], const std::string &msg, uint8_t sig[64]) {
  const Ssl &s = ssl();
  if (!ed25519_available()) return false;
  void *k = s.EVP_PKEY_new_raw_private_key(kEd25519, nullptr, priv, 32);
  if (!k) return false;
  void *ctx = s.EVP_MD_CTX_new();
  size_t len = 64;
  bool ok = ctx && s.EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
            s.EVP_DigestSign(ctx, sig, &len, reinterpret_cast<const unsigned char *>(msg.data()), msg.size()) == 1 && len == 64;
  if (ctx) s.EVP_MD_CTX_free(ctx);
  s.EVP_PKEY_free(k);
  return ok;
}

bool ed25519_verify(const uint8_t pub[32], const std::string &msg, const uint8_t sig[64]) {
  const Ssl &s = ssl();
  if (!s.ed_verify) return false;
  void *k = s.EVP_PKEY_new_raw_public_key(kEd25519, nullptr, pub, 32);
  if (!k) return false;
  void *ctx = s.EVP_MD_CTX_new();
  const bool ok = ctx && s.EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, k) == 1 &&
                  s.EVP_DigestVerify(ctx, sig, 64, reinterpret_cast<const unsigned char *>(msg.data()), msg.size()) == 1;
  if (ctx) s.EVP_MD_CTX_free(ctx);
  s.EVP_PKEY_free(k);
  return ok;
}

std::string sha256_hex(const std::string &data) {
  const Ssl &s = ssl();
  if (!s.SHA256) return "";
  unsigned char md[32];
  s.SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), md);
  static const char *hex = "0123456789abcdef";
  std::string out;
  for (unsigned char b : md) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}

HttpsResponse https_get(const std::string &url_in, const std::vector<std::string> &headers, size_t max_bytes,
                        const std::function<void(size_t, size_t)> &progress, const std::atomic<bool> *cancel,
                        int max_redirects) {
  HttpsResponse r;
  const Ssl &s = ssl();
  if (!s.ok) {
    r.error = s.error;
    return r;
  }
  std::string url = url_in;
  for (int hop = 0;; hop++) {
    Url u;
    if (!parse_url(url, u)) {
      r.error = "not an https URL: " + url;
      return r;
    }
    errno = 0;
    Conn c(s);
    if (!c.open(u, r.error)) return r;
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    for (const auto &h : headers) req += h + "\r\n";
    req += "\r\n";
    if (!c.write_all(req)) {
      r.error = ssl_error(s, "write");
      return r;
    }
    size_t hdr_end;
    while ((hdr_end = c.buf_.find("\r\n\r\n")) == std::string::npos) {
      if (c.buf_.size() > 65536 || !c.fill(r.error)) {
        if (r.error.empty()) r.error = "connection closed in the response header";
        return r;
      }
    }
    std::string head = c.buf_.substr(0, hdr_end);
    c.buf_.erase(0, hdr_end + 4);
    int status = 0;
    if (sscanf(head.c_str(), "HTTP/%*s %d", &status) != 1) {
      r.error = "bad status line";
      return r;
    }
    std::string location;
    long long length = -1;
    bool chunked = false;
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos) {
      size_t next = head.find("\r\n", pos + 2);
      std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
      pos = next;
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = lower(line.substr(0, colon)), value = line.substr(colon + 1);
      value.erase(0, value.find_first_not_of(" \t"));
      if (name == "location") location = value;
      else if (name == "content-length") length = atoll(value.c_str());
      else if (name == "transfer-encoding") chunked = lower(value).find("chunked") != std::string::npos;
    }
    if (status >= 300 && status < 400 && !location.empty()) {
      if (location[0] == '/') location = "https://" + u.host + (u.port == "443" ? "" : ":" + u.port) + location;
      if (hop < max_redirects) {
        url = location;
        continue;
      }
      r.location = location;
    }
    r.status = status;
    if (std::string err = read_body(c, length, chunked, max_bytes, r.body, progress, cancel); !err.empty()) {
      r.status = 0;
      r.error = err;
    }
    return r;
  }
}

}  // namespace cs
