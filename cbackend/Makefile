# Compiler and flags
CC = gcc
CFLAGS = -O3 -I$(UI_INCLUDE_DIR) -I$(SWS_INCLUDE_DIR) -Iinclude -Ithird_party/cJSON -Ithird_party/hiredis -Wall
LDFLAGS = -lavformat -lavcodec -lavutil -lswscale -lcrypto -lssl -lpthread

# Directories
UI_SRC_DIR = src/ui-wrapper
SWS_SRC_DIR = $(SRC_DIR)/simple_ws
UI_INCLUDE_DIR = include/ui-wrapper
SWS_INCLUDE_DIR = include/simple_ws
SRC_DIR = src

# Source files: UI-wrapper files (main and our new modules) plus other source files
SRCS = $(SRC_DIR)/main.c \
       $(UI_SRC_DIR)/common_ws.c \
       $(UI_SRC_DIR)/rtsp2ws_video.c \
       $(UI_SRC_DIR)/video_ws.c \
       $(UI_SRC_DIR)/frontend_ws.c \
       $(UI_SRC_DIR)/remote_ws.c \
       $(SWS_SRC_DIR)/wshandshake.c \
       $(SWS_SRC_DIR)/websocket.c \
       $(SWS_SRC_DIR)/base64.c \
       $(SWS_SRC_DIR)/sha1.c \
       third_party/cJSON/cJSON.c \
       third_party/hiredis/hiredis.c \
       third_party/hiredis/sds.c \
       third_party/hiredis/async.c \
       third_party/hiredis/dict.c \
       third_party/hiredis/alloc.c \
       third_party/hiredis/net.c \
       third_party/hiredis/read.c \
       third_party/hiredis/sockcompat.c \
       third_party/hiredis/ssl.c

# Object files (each .c file compiled to .o)
OBJS = $(SRCS:.c=.o)

# Target executable name
TARGET = ground_station

# Build target
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Pattern rule for compiling C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET)
