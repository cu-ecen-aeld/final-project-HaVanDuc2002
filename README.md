# Camera Frame Streamer (C++ / OpenCV)

A camera frame streaming system over TLS, rewritten in modern C++ using OpenCV for camera capture.

## Features

- **OpenCV-based capture**: Works with any camera supported by OpenCV (USB, IP cameras, etc.)
- **JPEG compression**: Efficient frame encoding for network transmission
- **TLS encryption**: Secure frame streaming with certificate verification
- **Thread-safe queue**: Lock-free producer-consumer pattern with backpressure handling
- **Reconnection**: Automatic reconnect with exponential backoff
- **Modern C++17**: RAII, smart pointers, standard library threads

## Architecture

```
┌─────────────────┐     ┌─────────────┐     ┌─────────────────┐
│  Camera Thread  │────>│ Ring Queue  │────>│ Network Thread  │
│  (OpenCV)       │     │ (SPSC)      │     │ (TLS)           │
└─────────────────┘     └─────────────┘     └─────────────────┘
         │                                           │
    cv::VideoCapture                          SSL_write
         │                                           │
         v                                           v
    ┌─────────┐                               ┌───────────┐
    │ Camera  │                               │  Server   │
    └─────────┘                               └───────────┘
```

## Dependencies

- **OpenCV** >= 4.0
- **OpenSSL** >= 1.1.0
- **C++17** compatible compiler (GCC 8+, Clang 7+)
- **pkg-config**

### Install dependencies (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y g++ libopencv-dev libssl-dev pkg-config
```

### Install dependencies (Fedora/RHEL)

```bash
sudo dnf install gcc-c++ opencv-devel openssl-devel pkgconfig
```

### Install dependencies (Yocto/Embedded Linux)

Add to your image recipe:
```
IMAGE_INSTALL += "opencv libssl pkgconfig"
```

## Building

```bash
# Build both client and server
make

# Build client only
make client

# Build server only  
make server

# Debug build (with AddressSanitizer)
make debug

# Release build (optimized + stripped)
make release

# Show build configuration
make info

# Show all targets
make help
```

### Cross-compilation for Embedded Linux

```bash
# ARM 32-bit (e.g., Raspberry Pi)
make CROSS_COMPILE=arm-linux-gnueabihf-

# ARM 64-bit (e.g., Raspberry Pi 4, Jetson)
make CROSS_COMPILE=aarch64-linux-gnu-
```

For cross-compilation, ensure the target sysroot has OpenCV and OpenSSL installed,
and set `PKG_CONFIG_PATH` and `PKG_CONFIG_SYSROOT_DIR` appropriately.

## Usage

### Generate TLS certificates (for testing)

```bash
cd build
make certs
```

Or manually:

```bash
openssl req -x509 -newkey rsa:4096 \
    -keyout server.key -out server.crt \
    -days 365 -nodes -subj '/CN=localhost'
```

### Start the server

```bash
./build/frame_server \
    --cert certs/server.crt \
    --key certs/server.key \
    --port 4433 \
    --output /tmp/frames \
    --verbose
```

### Start the client

```bash
./build/camera_streamer \
    --device 0 \
    --width 1280 \
    --height 720 \
    --fps 30 \
    --host localhost \
    --port 4433 \
    --verbose
```

## Command Line Options

### Client (camera_streamer)

| Option | Description | Default |
|--------|-------------|---------|
| `--device PATH/INDEX` | Camera device path or index | `0` |
| `--width N` | Frame width | `1280` |
| `--height N` | Frame height | `720` |
| `--fps N` | Frames per second | `30` |
| `--no-jpeg` | Send raw BGR instead of JPEG | disabled |
| `--jpeg-quality N` | JPEG quality (1-100) | `85` |
| `--host HOSTNAME` | Server hostname | (required) |
| `--port N` | Server port | `4433` |
| `--ca PATH` | CA certificate bundle | system default |
| `--queue-size N` | Frame queue size | `8` |
| `--verbose, -v` | Enable debug logging | disabled |

### Server (frame_server)

| Option | Description | Default |
|--------|-------------|---------|
| `--port N` | Listen port | `4433` |
| `--cert FILE` | Server certificate (PEM) | (required) |
| `--key FILE` | Server private key (PEM) | (required) |
| `--output DIR` | Save frames to directory | disabled |
| `--verbose, -v` | Verbose output | disabled |

## Protocol

Each frame is transmitted with a fixed-size header followed by the payload:

```
┌────────────────────────────────────────────────────────────┐
│ Header (40 bytes)                                          │
├──────────────┬──────────────┬──────────────────────────────┤
│ magic[4]     │ version[2]   │ header_len[2]                │
│ "FRAM"       │ 1            │ 40                           │
├──────────────┴──────────────┴──────────────────────────────┤
│ seq[8]       │ timestamp_ns[8]                             │
├──────────────┴─────────────────────────────────────────────┤
│ payload_len[4] │ pixel_format[4] │ width[2] │ height[2]    │
├────────────────┴─────────────────┴─────────┴───────────────┤
│ Payload (payload_len bytes)                                │
│ [JPEG or raw BGR frame data]                               │
└────────────────────────────────────────────────────────────┘
```

All multi-byte integers are in network byte order (big-endian).

## Project Structure

```
camera_streamer/
├── CMakeLists.txt          # CMake build configuration
├── README.md               # This file
├── include/
│   ├── log.hpp             # Logging utilities
│   ├── protocol.hpp        # Wire protocol definitions
│   ├── ring_queue.hpp      # Thread-safe queue
│   ├── camera_capture.hpp  # OpenCV camera capture
│   └── tls_client.hpp      # TLS client
├── src/
│   ├── main.cpp            # Client entry point
│   ├── ring_queue.cpp      # Queue implementation
│   ├── camera_capture.cpp  # Camera implementation
│   └── tls_client.cpp      # TLS implementation
└── server/
    └── server.cpp          # Server implementation
```

## Changes from C Version

1. **OpenCV instead of V4L2**: More portable, supports more camera types
2. **JPEG encoding**: Built-in JPEG compression for efficient streaming
3. **Modern C++17**: 
   - `std::unique_ptr` instead of manual memory management
   - `std::thread` instead of pthreads
   - `std::atomic` for thread-safe flags
   - `std::chrono` for time handling
   - `std::mutex`/`std::condition_variable` for synchronization
4. **RAII**: Automatic resource cleanup via destructors
5. **CMake**: Cross-platform build system instead of Makefile

## License

See the original license file for terms.
