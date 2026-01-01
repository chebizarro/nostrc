# Gnostr Signer Architecture

## Overview

Gnostr Signer is designed with a client-daemon architecture to ensure secure key management and a responsive user interface. The system is built using GTK for the user interface and D-Bus for inter-process communication.

## System Components

### 1. Main Application (`gnostr-signer`)

The main GTK application that provides the user interface for managing Nostr identities and permissions.

- **UI Layer**:
  - Built with GTK 4 and libadwaita
  - Implements responsive design for different screen sizes
  - Provides visual feedback for all operations

- **Core Components**:
  - `AccountsStore`: Manages Nostr identities and their metadata
  - `PolicyStore`: Handles application permissions and policies
  - UI Controllers: Handle user interactions and update the UI accordingly

### 2. Signer Daemon (`gnostr-signer-daemon`)

A background service that handles secure operations and exposes functionality via D-Bus.

- **D-Bus Interface**:
  - Exposes methods for key management and signing
  - Implements the `org.nostr.Signer` D-Bus interface
  - Handles authentication and authorization of requests

- **Security Layer**:
  - Manages secure storage of private keys
  - Implements rate limiting and request validation
  - Provides audit logging for security-sensitive operations

### 3. IPC (Inter-Process Communication)

- **D-Bus**:
  - Primary IPC mechanism for local communication
  - Uses system or session bus based on configuration
  - Implements a request/response pattern for operations

- **Unix Domain Sockets**:
  - Used for local communication with the daemon
  - Provides better performance for high-frequency operations
  - Implements a simple protocol for message passing

## Data Flow

1. **User Interaction**:
   - User performs actions in the GTK interface
   - UI components validate input and prepare requests

2. **Request Processing**:
   - Requests are sent to the daemon via D-Bus
   - Daemon validates the request and checks permissions
   - For signing operations, user approval may be required

3. **Secure Operations**:
   - Private keys are never exposed to the UI process
   - All cryptographic operations happen in the daemon
   - Results are returned to the UI via D-Bus

4. **UI Updates**:
   - The UI updates to reflect the result of operations
   - User is notified of success or failure

## Security Model

- **Process Isolation**: Critical operations run in a separate process
- **Least Privilege**: Each component has only the permissions it needs
- **Secure Storage**: Private keys are stored encrypted
- **User Consent**: All sensitive operations require explicit user approval

## Dependencies

- **Core**:
  - GLib 2.68+
  - GTK 4.0+
  - libadwaita
  - D-Bus

- **Build**:
  - CMake 3.16+
  - pkg-config
  - C11-compatible compiler

## Directory Structure

```
gnostr-signer/
├── daemon/           # Daemon implementation
│   ├── ipc.c        # IPC implementation
│   ├── ipc.h        # IPC interface
│   └── main_daemon.c # Daemon entry point
├── data/            # Application data
├── flatpak/         # Flatpak packaging
├── packaging/       # Distribution packaging
├── src/             # Main application source
│   ├── accounts_store.c  # Account management
│   ├── policy_store.c    # Permission management
│   └── ui/          # User interface components
└── tests/           # Unit and integration tests
```
