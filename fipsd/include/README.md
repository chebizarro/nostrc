# fipsd host headers

`fips-native-client.h` is the public header of **libfipsnative**, the
plain-POSIX C client for the native datagram API (installed with the library
and `libfipsnative.pc`; see `fipsd/README.md`, "Native datagram API and
libfipsnative"). Every other daemon interface stays private under `src/`; no
host API or `fips/host.h` vtables are introduced here.
