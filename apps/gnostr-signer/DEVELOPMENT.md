# Development Guide

This guide provides information for developers working on the Gnostr Signer project.

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
git clone https://github.com/chebizarro/gnostr-signer.git
cd gnostr-signer
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

```bash
cd build
ctest --output-on-failure
```

## Code Organization

- `src/`: Main application source code
  - `accounts_store.[ch]`: Account management
  - `policy_store.[ch]`: Permission management
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
gdb --args ./gnostr-signer
```

### Logging

The application uses GLib's logging facilities. To see debug output:

```bash
G_MESSAGES_DEBUG=all ./gnostr-signer
```

## Testing

### Running Tests

```bash
cd build
ctest -V
```

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
- **Permission Issues**: Check file permissions in `~/.local/share/gnostr-signer`

## Getting Help

- Check the [issue tracker](https://github.com/your-org/gnostr-signer/issues)
- Join our [Matrix channel](#) for real-time discussion
- Read the [API documentation](https://docs.gnome.org/gnostr-signer/)
