#include "update/tls.h"
#include "update/https.h"
#include <iostream>

int main(int argc, char **argv) {
  if (argc == 5) {
    auto response = cs::https_get(argv[4], {}, 1024, {}, nullptr, 0, argv[3]);
    return response.status == 200 && response.body == "secure" ? 0 : 1;
  }
  if (argc != 4) return 2;
  cs::TlsSock socket;
  std::string error;
  const bool ok = cs::tls_connect(argv[1], argv[2], socket, error, argv[3]);
  cs::tls_close(socket);
  if (!ok) std::cerr << error << '\n';
  return ok ? 0 : 1;
}
