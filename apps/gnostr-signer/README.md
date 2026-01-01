# Gnostr Signer

A secure, cross-platform application for managing Nostr identities and signing events. The application provides a user-friendly interface for managing accounts, permissions, and signing requests from Nostr clients.

## Features

- **Secure Identity Management**: Securely store and manage Nostr identities
- **Permission Control**: Granular control over which applications can access your keys
- **DBus Integration**: Provides a D-Bus interface for system integration
- **Cross-Platform**: Works on Linux, Windows, and other platforms
- **User Approval Flow**: Interactive approval dialogs for signing requests
- **Backup & Recovery**: Secure backup and recovery options for identities

## Architecture

The application consists of two main components:

1. **GTK-based GUI Application**: Provides the user interface for managing identities and permissions
2. **Background Daemon**: Handles secure key storage and cryptographic operations

## Building from Source

### Dependencies

- GTK 4.0+
- libadwaita
- GLib 2.68+
- CMake 3.16+
- pkg-config
- Compiler with C11 support

### Build Instructions

```bash
# Clone the repository
# Navigate to the project directory
mkdir build && cd build
cmake ..
make
```

### Installation

```bash
sudo make install
```

## Usage

### Running the Application

```bash
gnostr-signer
```

### Environment Variables

- `NOSTR_SIGNER_ENDPOINT`: Custom IPC endpoint (e.g., `unix:/run/user/1000/gnostr/signer.sock`)
- `NOSTR_DEBUG`: Enable debug output when set

## Security Considerations

- Private keys are stored encrypted using the system keyring
- All signing operations require explicit user approval
- Communication between components is secured using D-Bus authentication

## Contributing

Contributions are welcome! Please read our [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and the process for submitting pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
