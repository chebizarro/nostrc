# systemd units

The units are templates: CMake generates them from `*.service.in` with
the install prefix (`@CMAKE_INSTALL_FULL_BINDIR@`, `@CMAKE_INSTALL_FULL_SYSCONFDIR@`,
`@FIPSD_DNS_HELPER_DIR@`) and `cmake --install <build>/fipsd` places them in
`<prefix>/lib/systemd/system` (`FIPSD_SYSTEMD_UNIT_DIR` overrides the
directory). The Debian, RPM and Arch packages ship the generated files
unchanged, so there is one copy to keep in step.

| Unit | Enabled by the packages | Role |
| ---- | ----------------------- | ---- |
| `fipsd.service` | yes | The daemon: `fipsd --config /etc/fips/fips.yaml`, `RuntimeDirectory=fips` for the control socket, journal logging. |
| `fips-dns.service` | yes (skipped when the helpers are absent) | `Type=oneshot` wrapper for `fips-dns-setup`/`fips-dns-teardown` (`packaging/common/`), routing `.fips` to the daemon's resolver. |
| `fips-firewall.service` | no, opt in | Loads `/etc/fips/fips.nft`, the default-deny baseline for the mesh interface. |
| `fips-gateway.service` | not shipped yet | `fips-gateway --config /etc/fips/fips.yaml`, requiring `fipsd.service`; runs as root (nftables, proxy NDP and the pool route need `CAP_NET_ADMIN`), pool state in `/var/run/fips-gateway`. Generated and installed only when `fips-gateway` is built (Linux, libnftables); `common/cmake-args.sh` turns the gateway off, so the packages carry neither until they add it with its libnftables dependency. See `fipsd/docs/fips-gateway.md`. |

Manual install from a build tree (Linux, prefix `/usr/local`):

```sh
cmake -S . -B build -G Ninja $(sh fipsd/packaging/common/cmake-args.sh) -DCMAKE_BUILD_TYPE=Release
cmake --build build --target fipsd/all
sudo cmake --install build/fipsd
sudo groupadd --system fips
sudo install -m 600 /usr/local/share/fips/fips.yaml.example /usr/local/etc/fips/fips.yaml
sudo systemd-tmpfiles --create /usr/local/lib/tmpfiles.d/fipsd.conf
sudo systemctl daemon-reload && sudo systemctl enable --now fipsd
```

The gateway from a build tree: configure without `-DFIPSD_BUILD_GATEWAY=OFF`
(drop that line of `cmake-args.sh`'s output, with `libnftables-dev`
installed), install as above, set the `gateway:` block in `fips.yaml`, then
`sudo systemctl enable --now fips-gateway`.
