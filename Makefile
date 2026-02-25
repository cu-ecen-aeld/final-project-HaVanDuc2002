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

clean:
	rm -rf $(OUT_DIR)
	@echo "Cleaned"
