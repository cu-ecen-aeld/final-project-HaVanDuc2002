# Camera Frame Streamer (C++ / OpenCV)

A camera frame streaming system over TLS, written in C++17 using OpenCV for camera capture and OpenSSL for secure transmission. Built for embedded Linux (Raspberry Pi 4B) via Yocto, as a personal project from the course "Advanced Embedded Linux Development".

Yocto repository: https://github.com/cu-ecen-aeld/assignment-6-HaVanDuc2002

## Features

- **OpenCV-based capture**: Works with any V4L2-compatible camera (USB, CSI, etc.)
- **JPEG compression**: Efficient frame encoding for network transmission (raw BGR also supported)
- **TLS 1.2+ encryption**: Secure streaming with certificate verification, SNI, and hostname checking
- **Thread-safe ring queue**: Bounded POSIX-mutex-protected queue with DropOldest backpressure — capture thread never blocks
- **Automatic reconnection**: Exponential backoff (1 s → 30 s) on connection failure
- **POSIX threads**: Two pthreads — one for capture, one for network I/O
- **Dual logging**: Writes to `/var/tmp/camera_log` and stderr simultaneously

## Architecture

```
 RPi (Client)                                    PC (Server)
┌──────────────────────────────────────┐    ┌─────────────────────┐
│  Capture Thread ──> RingQueue ──> Network Thread ──TLS──> frame_server │
│  (OpenCV / V4L2)   (POSIX mutex)  (OpenSSL)      │    │  (saves frames) │
└──────────────────────────────────────┘    └─────────────────────┘
```

- **Capture thread**: reads frames from the camera via `cv::VideoCapture`, JPEG-encodes them, and pushes into the ring queue
- **Ring queue**: bounded circular buffer (default 8 slots, max 4 MB/frame); when full, the oldest frame is silently dropped to keep capture running
- **Network thread**: pops frames from the queue, connects to the server over TLS with automatic reconnect, and streams frames using the binary wire protocol

## Dependencies

| Library | Purpose |
|---------|---------|
| `libopencv` | Camera capture and JPEG encoding |
| `libopenssl` | TLS connection |
| `libpthread` | POSIX threads and mutex |

### Install on Ubuntu/Debian (for host development)

```bash
sudo apt-get install -y g++ libopencv-dev libssl-dev pkg-config
```

## Building

```bash
# Build both client and server
make

# Build client only (deployed to RPi)
make client

# Build server only (runs on PC)
make server
```

Output binaries: `build/camera_streamer` (client), `build/frame_server` (server).

## Usage

### Step 1 — Generate TLS certificates (on your PC)

```bash
mkdir -p certs
openssl req -x509 -newkey rsa:4096 \
    -keyout certs/server.key -out certs/server.crt \
    -days 365 -nodes -subj '/CN=<YOUR_PC_IP>'
```

Or use the Makefile target:

```bash
make certs
```

### Step 2 — Run the server (on your PC)

```bash
./build/frame_server \
    --cert certs/server.crt \
    --key  certs/server.key \
    --port 4433 \
    --output /tmp/frames \
    --verbose
```

### Step 3 — Copy the CA cert to the RPi

```bash
scp certs/server.crt root@<RPI_IP>:/etc/camera-streamer-ca.crt
```

### Step 4 — Run the client (on the RPi)

```bash
camera_streamer \
    --device 0 \
    --width 1280 --height 720 --fps 30 \
    --host <YOUR_PC_IP> --port 4433 \
    --ca /etc/camera-streamer-ca.crt \
    --verbose
```

When deployed via Yocto, configure `/etc/camera-streamer.conf` and manage via the init script:

```bash
/etc/init.d/camera-streamer start
```

## Command Line Options

### Client (`camera_streamer`)

| Option | Description | Default |
|--------|-------------|---------|
| `--device PATH/INDEX` | Camera device path or index | `0` |
| `--width N` | Frame width | `1280` |
| `--height N` | Frame height | `720` |
| `--fps N` | Frames per second | `30` |
| `--no-jpeg` | Send raw BGR instead of JPEG | disabled |
| `--jpeg-quality N` | JPEG quality (1-100) | `85` |
| `--host HOSTNAME` | Server hostname (required) | — |
| `--port N` | Server port | `4433` |
| `--ca PATH` | CA certificate bundle path | system default |
| `--queue-size N` | Ring queue capacity (frames) | `8` |
| `--verbose, -v` | Enable debug logging | disabled |

### Server (`frame_server`)

| Option | Description | Default |
|--------|-------------|---------|
| `--port N` | Listen port | `4433` |
| `--cert FILE` | Server certificate PEM (required) | — |
| `--key FILE` | Server private key PEM (required) | — |
| `--output DIR` | Save received frames to directory | disabled |
| `--verbose, -v` | Verbose output | disabled |

## Wire Protocol

Each frame is sent as a fixed 40-byte header followed by the payload. All multi-byte integers are big-endian (network byte order).

```
┌────────────────────────────────────────────────────────────┐
│ Header (40 bytes)                                          │
├──────────────┬──────────────┬──────────────────────────────┤
│ magic[4]     │ version[2]   │ header_len[2]                │
│ "FRAM"       │ 1            │ 40                           │
├──────────────┴──────────────┴──────────────────────────────┤
│ seq[8]            │ timestamp_ns[8]                        │
├───────────────────┴────────────────────────────────────────┤
│ payload_len[4] │ pixel_format[4] │ width[2] │ height[2]    │
├────────────────┴─────────────────┴─────────┴───────────────┤
│ Payload (payload_len bytes)                                │
│ [JPEG or raw BGR frame data]                               │
└────────────────────────────────────────────────────────────┘
```

Pixel format values (FourCC): `MJPG` (JPEG), `BGR3` (raw BGR24).

## Logging

Log messages are written simultaneously to:
- **`/var/tmp/camera_log`** — persistent log file on the target
- **stderr** — visible in the terminal when run interactively

Format: `[LEVEL] [YYYY-MM-DD HH:MM:SS.mmm] [file:line] message`

Log level is controlled at runtime: default is `INFO`; pass `--verbose` for `DEBUG`.

## Project Structure

```
camera_steamer/
├── Makefile                # Build system
├── README.md               # This file
├── certs/                  # TLS certificates (not committed)
├── include/
│   ├── log.hpp             # POSIX-based dual-output logger
│   ├── protocol.hpp        # Wire protocol definitions
│   ├── ring_queue.hpp      # Thread-safe ring buffer (POSIX mutex)
│   ├── camera_capture.hpp  # OpenCV camera capture interface
│   └── tls_client.hpp      # TLS client with reconnect
├── src/
│   ├── main.cpp            # Client entry point and thread management
│   ├── ring_queue.cpp      # Ring queue implementation
│   ├── camera_capture.cpp  # Camera capture implementation
│   └── tls_client.cpp      # TLS client implementation
└── server/
    └── server.cpp          # PC-side frame receiver
```