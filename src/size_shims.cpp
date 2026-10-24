// Link-time size shims (enabled with -Wl,--wrap=... in platformio.ini).
//
// Each shim replaces a library piece that is linked only for convenience.

#include <cstdarg>
#include <cstddef>
#include <cstdio>

extern "C" {

// mbedtls_strerror() drags in mbedTLS's full error-string table (~15 KB).
// Its only users are WiFiClientSecure's log lines, so print the numeric code
// instead ("mbedTLS -0x7280"): the same thing esp-idf docs index by.
void __wrap_mbedtls_strerror(int ret, char* buf, size_t buflen) {
  if (buf == nullptr || buflen == 0) return;
  snprintf(buf, buflen, "mbedTLS %s0x%04x", ret < 0 ? "-" : "",
           static_cast<unsigned>(ret < 0 ? -ret : ret));
}

}  // extern "C"
