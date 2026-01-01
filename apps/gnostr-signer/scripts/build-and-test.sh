#!/bin/bash
# Gnostr Signer - Build and Test Script
# This script builds the daemon and runs basic validation tests

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BUILD_TYPE="${BUILD_TYPE:-Release}"
ENABLE_TCP="${ENABLE_TCP:-ON}"
BUILD_DIR="build"
INSTALL_PREFIX="${INSTALL_PREFIX:-/usr}"

# Functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_dependencies() {
    log_info "Checking build dependencies..."
    
    local missing_deps=()
    
    # Check for required tools
    command -v cmake >/dev/null 2>&1 || missing_deps+=("cmake")
    command -v make >/dev/null 2>&1 || missing_deps+=("make")
    command -v pkg-config >/dev/null 2>&1 || missing_deps+=("pkg-config")
    
    # Check for required libraries
    pkg-config --exists glib-2.0 || missing_deps+=("glib-2.0")
    pkg-config --exists gio-2.0 || missing_deps+=("gio-2.0")
    pkg-config --exists gtk4 || missing_deps+=("gtk4")
    pkg-config --exists libadwaita-1 || missing_deps+=("libadwaita-1")
    
    if [ ${#missing_deps[@]} -ne 0 ]; then
        log_error "Missing dependencies: ${missing_deps[*]}"
        log_info "Install them with:"
        log_info "  Debian/Ubuntu: sudo apt-get install cmake build-essential pkg-config libglib2.0-dev libgtk-4-dev libadwaita-1-dev"
        log_info "  Fedora: sudo dnf install cmake gcc gcc-c++ pkgconfig glib2-devel gtk4-devel libadwaita-devel"
        exit 1
    fi
    
    log_info "All dependencies found"
}

clean_build() {
    if [ -d "$BUILD_DIR" ]; then
        log_info "Cleaning existing build directory..."
        rm -rf "$BUILD_DIR"
    fi
}

configure_build() {
    log_info "Configuring build (type: $BUILD_TYPE, TCP: $ENABLE_TCP)..."
    
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
        -DENABLE_TCP_IPC="$ENABLE_TCP" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    
    cd ..
}

build_project() {
    log_info "Building project..."
    
    cd "$BUILD_DIR"
    make -j$(nproc)
    cd ..
    
    log_info "Build completed successfully"
}

run_tests() {
    log_info "Running tests..."
    
    cd "$BUILD_DIR"
    
    if [ -f "CTestTestfile.cmake" ]; then
        ctest --output-on-failure
        log_info "Tests passed"
    else
        log_warn "No tests configured"
    fi
    
    cd ..
}

validate_binaries() {
    log_info "Validating binaries..."
    
    local daemon_bin="$BUILD_DIR/gnostr-signer-daemon"
    local app_bin="$BUILD_DIR/gnostr-signer"
    
    if [ ! -f "$daemon_bin" ]; then
        log_error "Daemon binary not found: $daemon_bin"
        exit 1
    fi
    
    if [ ! -x "$daemon_bin" ]; then
        log_error "Daemon binary is not executable: $daemon_bin"
        exit 1
    fi
    
    # Test daemon help
    if ! "$daemon_bin" --help >/dev/null 2>&1; then
        log_error "Daemon --help failed"
        exit 1
    fi
    
    # Test daemon version
    if ! "$daemon_bin" --version >/dev/null 2>&1; then
        log_error "Daemon --version failed"
        exit 1
    fi
    
    log_info "Binary validation passed"
}

test_daemon_startup() {
    log_info "Testing daemon startup..."
    
    local daemon_bin="$BUILD_DIR/gnostr-signer-daemon"
    local test_socket="/tmp/gnostr-test-$$.sock"
    
    # Start daemon in background
    NOSTR_SIGNER_ENDPOINT="unix:$test_socket" "$daemon_bin" &
    local daemon_pid=$!
    
    # Wait for socket to be created
    local timeout=5
    local elapsed=0
    while [ ! -S "$test_socket" ] && [ $elapsed -lt $timeout ]; do
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    
    if [ ! -S "$test_socket" ]; then
        log_error "Daemon failed to create socket within ${timeout}s"
        kill $daemon_pid 2>/dev/null || true
        exit 1
    fi
    
    log_info "Daemon started successfully (PID: $daemon_pid)"
    
    # Test graceful shutdown
    log_info "Testing graceful shutdown..."
    kill -TERM $daemon_pid
    
    # Wait for daemon to exit
    timeout=5
    elapsed=0
    while kill -0 $daemon_pid 2>/dev/null && [ $elapsed -lt $timeout ]; do
        sleep 0.5
        elapsed=$((elapsed + 1))
    done
    
    if kill -0 $daemon_pid 2>/dev/null; then
        log_error "Daemon did not shutdown gracefully"
        kill -9 $daemon_pid 2>/dev/null || true
        exit 1
    fi
    
    # Cleanup
    rm -f "$test_socket"
    
    log_info "Daemon startup and shutdown test passed"
}

install_project() {
    log_info "Installing project..."
    
    cd "$BUILD_DIR"
    
    if [ "$EUID" -eq 0 ]; then
        make install
    else
        log_info "Running with sudo for installation..."
        sudo make install
    fi
    
    cd ..
    
    log_info "Installation completed"
}

setup_systemd() {
    log_info "Setting up systemd service..."
    
    # Reload systemd
    systemctl --user daemon-reload
    
    log_info "Systemd service configured"
    log_info "To enable and start the service, run:"
    log_info "  systemctl --user enable --now gnostr-signer-daemon.service"
}

print_summary() {
    echo ""
    log_info "========================================="
    log_info "Build and Test Summary"
    log_info "========================================="
    log_info "Build Type: $BUILD_TYPE"
    log_info "TCP Support: $ENABLE_TCP"
    log_info "Install Prefix: $INSTALL_PREFIX"
    log_info "Build Directory: $BUILD_DIR"
    echo ""
    log_info "Binaries:"
    log_info "  Daemon: $BUILD_DIR/gnostr-signer-daemon"
    log_info "  App: $BUILD_DIR/gnostr-signer"
    echo ""
    log_info "Next Steps:"
    log_info "  1. Install: cd $BUILD_DIR && sudo make install"
    log_info "  2. Enable: systemctl --user enable gnostr-signer-daemon.service"
    log_info "  3. Start: systemctl --user start gnostr-signer-daemon.service"
    log_info "  4. Check: systemctl --user status gnostr-signer-daemon.service"
    echo ""
    log_info "Documentation:"
    log_info "  Quick Start: DAEMON_QUICKSTART.md"
    log_info "  Deployment: DAEMON_DEPLOYMENT.md"
    log_info "  Development: DEVELOPMENT.md"
    echo ""
}

# Main script
main() {
    log_info "Gnostr Signer - Build and Test Script"
    log_info "======================================"
    echo ""
    
    # Parse arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --clean)
                CLEAN_BUILD=1
                shift
                ;;
            --debug)
                BUILD_TYPE="Debug"
                shift
                ;;
            --no-tcp)
                ENABLE_TCP="OFF"
                shift
                ;;
            --install)
                DO_INSTALL=1
                shift
                ;;
            --skip-tests)
                SKIP_TESTS=1
                shift
                ;;
            --help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --clean       Clean build directory before building"
                echo "  --debug       Build in Debug mode (default: Release)"
                echo "  --no-tcp      Disable TCP IPC support"
                echo "  --install     Install after building"
                echo "  --skip-tests  Skip running tests"
                echo "  --help        Show this help message"
                echo ""
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                log_info "Use --help for usage information"
                exit 1
                ;;
        esac
    done
    
    # Run build steps
    check_dependencies
    
    if [ -n "$CLEAN_BUILD" ]; then
        clean_build
    fi
    
    configure_build
    build_project
    
    if [ -z "$SKIP_TESTS" ]; then
        run_tests
        validate_binaries
        test_daemon_startup
    fi
    
    if [ -n "$DO_INSTALL" ]; then
        install_project
        setup_systemd
    fi
    
    print_summary
    
    log_info "Build and test completed successfully!"
}

# Run main function
main "$@"
