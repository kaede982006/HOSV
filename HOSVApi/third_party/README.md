# Bundled Dependencies

This directory is intentionally part of the project so the Seedance 2 C++
library can be built on another machine without installing libcurl, OpenSSL, or
mbedTLS development packages.

- `curl-8.11.1`: built as a static libcurl dependency.
- `mbedtls`: built as static TLS/crypto libraries for libcurl. Its `framework`
  submodule contents are vendored under `mbedtls/framework`.
- `cacert.pem`: CA root bundle used by bundled libcurl for HTTPS verification.

Keep the original license files in each vendored source directory when copying
or redistributing this project.
