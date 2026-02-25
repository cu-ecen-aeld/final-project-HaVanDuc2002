# Camera Frame Streamer (C++ / OpenCV)

A camera frame streaming system over TLS, written in modern C++ using OpenCV for camera capture.

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

### Install dependencies (Ubuntu/Debian)

```bash
sudo apt-get update
sudo apt-get install -y g++ libopencv-dev libssl-dev pkg-config
```

## Building

```bash
# Build both client and server
make

# Build client only
make client

# Build server only  
make server

```bash
```

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
    --ca certs/server.crt \
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