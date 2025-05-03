#ifndef __UCWEBSOCKET_H
#define __UCWEBSOCKET_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>
#include <stdint.h>

#include "base64.h"
#include "sha1.h"

#define WS_VERSION 13
#define WS_WEBSOCK "websocket"
#define WS_MAGIC "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* WebSocket Frame Types */
typedef enum
{
    WS_EMPTY_FRAME = 0xF0,
    WS_ERROR_FRAME = 0xF1,
    WS_INCOMPLETE_FRAME = 0xF2,
    WS_TEXT_FRAME = 0x01,
    WS_BINARY_FRAME = 0x02,
    WS_PING_FRAME = 0x09,
    WS_PONG_FRAME = 0x0A,
    WS_OPENING_FRAME = 0xF3,
    WS_CLOSING_FRAME = 0x08
} wsFrameType;

/* WebSocket Connection States */
typedef enum
{
    CONNECTING = 0, /* Connection is not yet open */
    OPEN = 1,       /* Connection is open and ready to communicate */
    CLOSING = 2,    /* Connection is in the process of closing */
    CLOSED = 3      /* Connection is closed or couldn't be opened */
} wsState;

/* WebSocket Frame Structure */
typedef struct
{
    uint8_t fin;
    uint8_t rsv1;
    uint8_t rsv2;
    uint8_t rsv3;
    uint8_t opcode;
    uint8_t *payload;
    size_t payload_length;
    wsFrameType type;
} ws_frame;

/* WebSocket Functions */
void ws_parse_frame(ws_frame *frame, uint8_t *data, size_t len);
void ws_create_frame(ws_frame *frame, uint8_t *out_data, size_t *out_len);
void ws_create_closing_frame(uint8_t *out_data, size_t *out_len);
void ws_create_text_frame(const char *text, uint8_t *out_data, size_t *out_len);
void ws_create_binary_frame(const uint8_t *data, size_t datalen, uint8_t *out_data, size_t *out_len);
void ws_create_control_frame(wsFrameType type, const uint8_t *data, size_t data_len, uint8_t *out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* __UCWEBSOCKET_H */