# Camera Frame Streamer - Makefile
# C++ with OpenCV and OpenSSL for embedded Linux

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2 -g
CXXFLAGS += -D_GNU_SOURCE
CXXFLAGS += -fstack-protector-strong -fPIE
CXXFLAGS += -I./include

# OpenCV flags (use pkg-config)
OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv 2>/dev/null)
OPENCV_LIBS := $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv 2>/dev/null)

# OpenSSL flags
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS := $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")

CXXFLAGS += $(OPENCV_CFLAGS) $(OPENSSL_CFLAGS)

LDFLAGS = -pie -Wl,-z,relro,-z,now
LIBS = $(OPENCV_LIBS) $(OPENSSL_LIBS) -lpthread

# Directories
SRC_DIR = src
INC_DIR = include
SVR_DIR = server
OUT_DIR = build
CERT_DIR = certs

# Client source files
CLIENT_SRCS = $(SRC_DIR)/main.cpp \
              $(SRC_DIR)/ring_queue.cpp \
              $(SRC_DIR)/camera_capture.cpp \
              $(SRC_DIR)/tls_client.cpp
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.cpp=$(OUT_DIR)/%.o)
CLIENT_TARGET = $(OUT_DIR)/camera_streamer

# Server source files
SERVER_SRCS = $(SVR_DIR)/server.cpp
SERVER_OBJS = $(SERVER_SRCS:$(SVR_DIR)/%.cpp=$(OUT_DIR)/%.o)
SERVER_TARGET = $(OUT_DIR)/frame_server

.PHONY: all clean client server certs help debug release check-deps

all: check-deps $(OUT_DIR) client server

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Check dependencies
check-deps:
	@echo "Checking dependencies..."
	@pkg-config --exists opencv4 2>/dev/null || pkg-config --exists opencv 2>/dev/null || \
		(echo "ERROR: OpenCV not found. Install with: sudo apt-get install libopencv-dev" && exit 1)
	@pkg-config --exists openssl 2>/dev/null || \
		(echo "WARNING: OpenSSL pkg-config not found, using defaults")
	@echo "Dependencies OK"

# Client build
client: $(OUT_DIR) $(CLIENT_TARGET)
	@echo "Client built: $(CLIENT_TARGET)"

$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

$(OUT_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Server build
server: $(OUT_DIR) $(SERVER_TARGET)
	@echo "Server built: $(SERVER_TARGET)"

$(SERVER_TARGET): $(SERVER_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(OPENSSL_LIBS) -lpthread

$(OUT_DIR)/server.o: $(SVR_DIR)/server.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Generate self-signed certificates for testing
certs:
	@mkdir -p $(CERT_DIR)
	@echo "Generating self-signed certificate..."
	openssl req -x509 -newkey rsa:4096 \
		-keyout $(CERT_DIR)/server.key \
		-out $(CERT_DIR)/server.crt \
		-days 365 -nodes \
		-subj '/CN=localhost'
	@echo "Certificates created in $(CERT_DIR)/"

# Debug build
debug: CXXFLAGS += -DDEBUG -O0 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean all
	@echo "Debug build complete"

# Release build
release: CXXFLAGS += -DNDEBUG -O3
release: clean all
	strip $(CLIENT_TARGET) $(SERVER_TARGET)
	@echo "Release build complete"

# Cross-compilation support for embedded Linux
# Usage: make CROSS_COMPILE=arm-linux-gnueabihf-
ifdef CROSS_COMPILE
CXX = $(CROSS_COMPILE)g++
# For cross-compilation, you may need to specify sysroot
# CXXFLAGS += --sysroot=$(SYSROOT)
# PKG_CONFIG_PATH and PKG_CONFIG_SYSROOT_DIR should be set
endif

clean:
	rm -rf $(OUT_DIR)
	@echo "Cleaned"

# Help
help:
	@echo "Camera Frame Streamer - Build System (C++ / OpenCV)"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build both client and server (default)"
	@echo "  client   - Build camera_streamer client only"
	@echo "  server   - Build frame_server only"
	@echo "  certs    - Generate self-signed TLS certificates"
	@echo "  debug    - Build with debug flags and sanitizers"
	@echo "  release  - Build optimized release version"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Cross-compilation (embedded Linux):"
	@echo "  make CROSS_COMPILE=arm-linux-gnueabihf-"
	@echo "  make CROSS_COMPILE=aarch64-linux-gnu-"
	@echo ""
	@echo "Dependencies:"
	@echo "  - OpenCV (libopencv-dev)"
	@echo "  - OpenSSL (libssl-dev)"
	@echo "  - pkg-config"

# Print configuration (useful for debugging build issues)
info:
	@echo "CXX: $(CXX)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "LIBS: $(LIBS)"
	@echo "OPENCV_CFLAGS: $(OPENCV_CFLAGS)"
	@echo "OPENCV_LIBS: $(OPENCV_LIBS)"
	@echo "OPENSSL_CFLAGS: $(OPENSSL_CFLAGS)"
	@echo "OPENSSL_LIBS: $(OPENSSL_LIBS)"
