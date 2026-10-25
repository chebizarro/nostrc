# Development Guide

This guide provides information for developers working on the Grotto project.

## Prerequisites

- C compiler (GCC/Clang)
- CMake 3.16+
- GTK 4.0+ development files
- libadwaita development files
- D-Bus development files
- pkg-config
- Git

## Getting Started

### Clone the Repository

```bash
git clone https://github.com/chebizarro/grotto.git
cd grotto
```

### Build Dependencies

#### Debian/Ubuntu

```bash
sudo apt-get install -y build-essential cmake pkg-config \
    libgtk-4-dev libadwaita-1-dev libglib2.0-dev \
    libdbus-1-dev git
```

#### Fedora

```bash
sudo dnf install -y gcc gcc-c++ cmake pkgconfig \
    gtk4-devel libadwaita-devel glib2-devel \
    dbus-devel git
```

### Building the Project

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Running Tests

From the repository root, use the affected commands in [Testing](#testing).
Do not run whole-repository CTest for an ordinary Grotto change.

## Code Organization

- `src/`: Main application source code
  - `accounts_store.[ch]`: Account management
  - `signer_grants.[ch]`: Remembered decisions, listed/revoked through the daemon
  - `ui/`: GTK UI components
    - `*.ui`: UI definitions in XML format
    - `*.c`: UI controller code
- `daemon/`: Signer daemon implementation
- `tests/`: Unit and integration tests
- `data/`: Application data and resources

## Coding Style

- Follow the [GNOME Coding Style](https://developer.gnome.org/programming-guidelines/stable/c-coding-style.html.en)
- Use 2 spaces for indentation
- Maximum line length of 100 characters
- Document all public APIs with GObject-style comments
- Write unit tests for new functionality

## Debugging

### Environment Variables

- `G_MESSAGES_DEBUG=all`: Enable debug messages
- `NOSTR_DEBUG=1`: Enable application-specific debug output
- `G_DEBUG=fatal_warnings`: Make warnings fatal

### Using GDB

```bash
gdb --args ./grotto
```

### Logging

The application uses GLib's logging facilities. To see debug output:

```bash
G_MESSAGES_DEBUG=all ./grotto
```

## Testing

### Running Tests

Run from the repository root against the commits being reviewed:

```sh
BASE=$(git merge-base origin/master HEAD)
HEAD_SHA=$(git rev-parse HEAD)
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"
```

For uncommitted work, use `scripts/test-gate.sh --explain` then
`scripts/test-gate.sh`; committed `--head` execution never tests dirty files.
Inspect inclusions, downstream compatibility, unrelated omissions and any broad
fallback before executing. The native Linux profile is `grotto.linux`; explicit
ASan/UBSan verification uses `--profile grotto.linux.asan_ubsan` on Linux x86_64
(append the same option to both committed commands). A profile restricts coverage;
it does not replace the other selected consumer families.

Coverage labels `nip55l`, `core.macos` and `grotto.tests.*` refer to signer,
macOS and Grotto test coverage, not three literal `--profile` IDs. The current
macOS profile is `core.macos.asan`; NIP-55L and Grotto checks span the profiles
printed by `--explain`. Do not pass globs as profile IDs or run a Linux profile
on macOS. Missing required capabilities fail visibly; use the matching CI/native
runner rather than dropping selected checks. See
[dependency-aware commands](../../docs/TESTING.md#dependency-aware-commands).

Use `scripts/test-gate.sh --broad` only for selector-code changes,
master/release/manual validation, or a recorded named reason. Broad fallback for
unknown inputs must be explained, not disguised as bounded selection.
`--broad --profile <id>` is broad only within that profile, not whole-repo proof.
The push hook remains static-only; `scripts/linux-gate.sh` is a legacy adapter,
not an ordinary Grotto prerequisite.

### Agent brief template

“Implement the scoped Grotto/nip55l change. From the repository root run
`BASE=$(git merge-base origin/master HEAD)` and `HEAD_SHA=$(git rev-parse HEAD)`,
then `scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" --explain` and
`scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA"`. Inspect `grotto.linux`,
`grotto.linux.asan_ubsan` and the `nip55l`, `core.macos`, `grotto.tests.*` coverage
above; record selected consumers/compatibility, unrelated omissions and actual
results. For explicit Linux sanitizer verification append
`--profile grotto.linux.asan_ubsan` to both commands. For CI/selector/runner changes
also run `python3 scripts/test-selection-operational.py`. Use `--broad` only for
selector-code changes, master/release/manual runs or a stated named reason.
Do not substitute full CTest or linux-gate, or claim skipped/unavailable checks
as passing.”

### Writing Tests

- Tests should be written using the GLib testing framework
- Each source file should have a corresponding test file in `tests/`
- Test files should be named `test_<module>.c`

## Documentation

### Building Documentation

```bash
cd docs
make html
```

### Documenting Code

- Use GObject-style documentation comments
- Document all public APIs
- Include examples for complex functions
- Document parameters and return values

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## Release Process

1. Update version in `meson.build`
2. Update `CHANGELOG.md`
3. Create a signed tag (`git tag -s vX.Y.Z`)
4. Push the tag (`git push --tags`)
5. Create a GitHub release

## Troubleshooting

### Common Build Issues

- **Missing Dependencies**: Ensure all build dependencies are installed
- **CMake Errors**: Try removing the `build` directory and reconfigure
- **Linker Errors**: Check that all required libraries are installed

### Runtime Issues

- **D-Bus Errors**: Ensure the session bus is running
- **Permission Issues**: Check file permissions in `~/.local/share/grotto`

## Getting Help

- Check the [issue tracker](https://github.com/your-org/grotto/issues)
- Join our [Matrix channel](#) for real-time discussion
- Read the [API documentation](https://docs.gnome.org/grotto/)
