#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "websocket.h"

#define MASK_LEN 4

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define HTONS(v) ((v >> 8) | (v << 8))
#else
#define HTONS(v) (v)
#endif

/* Function to create a WebSocket frame */
void ws_create_frame(ws_frame *frame, uint8_t *out_data, size_t *out_len)
{
    assert(frame->type);

    out_data[0] = 0x80 | frame->type;

    if (frame->payload_length <= 0x7D)
    {
        out_data[1] = frame->payload_length;
        *out_len = 2;
    }
    else if (frame->payload_length <= 0xFFFF)
    {
        out_data[1] = 0x7E;
        out_data[2] = (frame->payload_length >> 8) & 0xFF;
        out_data[3] = frame->payload_length & 0xFF;
        *out_len = 4;
    }
    else
    {
        out_data[1] = 0x7F;
        for (int i = 0; i < 8; i++)
        {
            out_data[2 + i] = (frame->payload_length >> (56 - i * 8)) & 0xFF;
        }
        *out_len = 10;
    }

    memcpy(&out_data[*out_len], frame->payload, frame->payload_length);
    *out_len += frame->payload_length;
}

/* Parses the opcode and assigns frame type */
static wsFrameType ws_parse_opcode(ws_frame *frame)
{
    switch (frame->opcode)
    {
    case WS_TEXT_FRAME:
    case WS_BINARY_FRAME:
    case WS_CLOSING_FRAME:
    case WS_PING_FRAME:
    case WS_PONG_FRAME:
        frame->type = frame->opcode;
        break;
    default:
        frame->type = WS_ERROR_FRAME; /* Reserved frames treated as errors */
        break;
    }
    return frame->type;
}

/* Parses a WebSocket frame */
void ws_parse_frame(ws_frame *frame, uint8_t *data, size_t len)
{
    if (len < 2)
    {
        frame->type = WS_INCOMPLETE_FRAME;
        return;
    }

    frame->fin = (data[0] & 0x80) != 0;
    frame->opcode = data[0] & 0x0F;

    if (ws_parse_opcode(frame) == WS_ERROR_FRAME)
    {
        return;
    }

    int masked = (data[1] & 0x80) != 0;
    size_t payloadLength = data[1] & 0x7F;
    size_t headerSize = 2;

    if (payloadLength == 0x7E)
    {
        payloadLength = (data[2] << 8) | data[3];
        headerSize += 2;
    }
    else if (payloadLength == 0x7F)
    {
        payloadLength = 0;
        for (int i = 0; i < 8; i++)
        {
            payloadLength |= ((size_t)data[2 + i]) << (8 * (7 - i));
        }
        headerSize += 8;
    }

    if (masked)
    {
        headerSize += MASK_LEN;
    }

    if (payloadLength + headerSize > len)
    {
        frame->type = WS_INCOMPLETE_FRAME;
        return;
    }

    frame->payload = &data[headerSize];
    frame->payload_length = payloadLength;

    if (masked)
    {
        uint8_t maskingKey[MASK_LEN];
        memcpy(maskingKey, &data[headerSize - MASK_LEN], MASK_LEN);

        for (size_t i = 0; i < frame->payload_length; i++)
        {
            frame->payload[i] ^= maskingKey[i % MASK_LEN];
        }
    }
}

/* Functions to create different types of frames */
void ws_create_closing_frame(uint8_t *out_data, size_t *out_len)
{
    ws_frame frame = {.payload_length = 0, .payload = NULL, .type = WS_CLOSING_FRAME};
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_text_frame(const char *text, uint8_t *out_data, size_t *out_len)
{
    ws_frame frame = {.payload_length = strlen(text), .payload = (uint8_t *)text, .type = WS_TEXT_FRAME};
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_binary_frame(const uint8_t *data, size_t datalen, uint8_t *out_data, size_t *out_len)
{
    ws_frame frame = {.payload_length = datalen, .payload = (uint8_t *)data, .type = WS_BINARY_FRAME};
    ws_create_frame(&frame, out_data, out_len);
}

void ws_create_control_frame(wsFrameType type, const uint8_t *data, size_t data_len, uint8_t *out_data, size_t *out_len)
{
    ws_frame frame = {.payload_length = data_len, .payload = (uint8_t *)data, .type = type};
    ws_create_frame(&frame, out_data, out_len);
}