# Runtime and SDK packages

The runtime provider is **libgrotto-panels-0-0** on Debian and **grotto-panels**
on RPM systems. The SDK providers are **libgrotto-panels-0-dev** and
**grotto-panels-devel**. All start at the component's initial unreleased 0.1.0.
They own the public DSO/SONAME, headers, CMake config and `grotto-panels-0.pc`.
The runtime is not hidden inside Grotto's private-library directory.

Build the provider first from a repository archive containing this module and
`cmake/VersionHelpers.cmake`. For Debian, copy `packaging/debian` from this
module to the archive root's `debian/`, then run `dpkg-buildpackage -b -us -uc`.
For RPM, create a `grotto-panels-0.1.0.tar.gz` root with the same module/helper
paths and use `packaging/rpm/grotto-panels.spec`.

Both Grotto Debian recipes and both Grotto RPM recipes explicitly require the
provider at runtime and the SDK at build time. Their CMake route sets
`GROTTO_USE_SYSTEM_PANELS=ON`, which **requires** an installed CMake package and
never bundles an unclaimed DSO in the Grotto install subtree. Meson resolves the
same installed SDK with `dependency('grotto-panels-0', version: '>=0.1.0')`.
Nostr Settings' CMake route supports the same installed-package option. There
is no in-tree Nostr Settings native package recipe to edit; any future recipe
must depend on this runtime provider, not copy the DSO or grant daemon authority.

The pkg-config `Libs` field names `-lgrotto-panels-0`; `Requires` records the GTK
4.14/adwaita 1.5 floor. Its configured full lib/include paths work with both
relative Debian multiarch libdirs and an absolute RPM `%{_libdir}`.

## Explicit installed-consumer smoke

`tests/test-packages.sh` must run as root **only inside a disposable Linux
container**, with `GROTTO_PACKAGE_TEST_DISPOSABLE=1`. Missing tools fail visibly.
It builds the real Debian runtime/SDK, installs them with dpkg, checks ELF
`NEEDED`, and compiles/runs consumers through both pkg-config and CMake without
`LD_LIBRARY_PATH`. It then removes those packages and exercises the RPM spec,
metadata and installed payload with the same consumers.

For example, use the repository's Linux floor image, as a task-owned container:

```sh
docker run --rm --user 0 -e GROTTO_PACKAGE_TEST_DISPOSABLE=1 \
  -v "$PWD:/src:ro" --entrypoint sh nostrc-linux-ci:arm64 -ec '
    apt-get update -qq
    apt-get install -y --no-install-recommends debhelper rpm
    bash /src/gnome/grotto-panels/tests/test-packages.sh'
```

The RPM leg on Ubuntu uses `--nodeps` because the installed development
libraries belong to dpkg, not the RPM database; it overrides the native libdir
to Ubuntu's multiarch path. This proves the RPM build/payload/ELF/pkg-config
closure, **not** Fedora dependency-solver/native-distribution acceptance. The
Debian leg is native in that container. Neither is a full Grotto application
package build, production Settings mount, hosted CI or release qualification.
