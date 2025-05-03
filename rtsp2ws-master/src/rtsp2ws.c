/* rtsp2ws.c - RTSP to WebSocket stream relay
 *
 * Copyright (C) 2025 Athanasios Papadopoulos
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or (at
 * your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <openssl/rand.h>
#include <signal.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

#include "simple_ws/websocket.h"
#include "simple_ws/wshandshake.h"
#include "simple_ws/base64.h"

#define MAX_PKT 200000
#define HANDSHAKE_BUF 2048

/* Global variables */
AVFormatContext *fmt = NULL;
AVCodecContext *ctx = NULL;
AVPacket *pkt = NULL;

static const char *rtsp_url;
static const char *ws_host;
static int ws_port;
static const char *ws_path;

static int ws_fd = -1;

static void Shutdown(bool signit);
static void SIGINT_handler(int signum);
static int connect_ws(void);
static void stream_loop(int ws_fd);

int main(int argc, char **argv)
{
    signal(SIGINT, SIGINT_handler);

    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <rtsp_url> <ws_host> <ws_port> <ws_path>\n", argv[0]);
        return EXIT_FAILURE;
    }
    rtsp_url = argv[1];
    ws_host = argv[2];
    ws_port = atoi(argv[3]);
    ws_path = argv[4];

    ws_fd = connect_ws();
    if (ws_fd >= 0)
    {
        fprintf(stderr, "WebSocket connected → streaming...\n");
        stream_loop(ws_fd);
    }
    else
    {
        fprintf(stderr, "Failed to connect to WebSocket server\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

/* Frees up resources, sends a WebSocket close frame, and exits the program */
static void Shutdown(bool signit)
{
    if (!signit)
        fprintf(stderr, "[ERROR] Shutting down...\n");

    if (pkt)
        av_packet_free(&pkt);
    if (ctx)
        avcodec_free_context(&ctx);
    if (fmt)
        avformat_close_input(&fmt);

    /* Send WebSocket closing frame */
    uint8_t wsbuf[16];
    size_t wslen;
    ws_create_closing_frame(wsbuf, &wslen);
    if (send(ws_fd, wsbuf, wslen, 0) < 0)
    {
        perror("[ERROR] send close frame");
    }

    close(ws_fd);
    exit(EXIT_FAILURE);
}

/* Handles SIGINT signal and calls Shutdown for graceful termination */
static void SIGINT_handler(int signum)
{
    fprintf(stderr, "[INFO] Received signal %d. Shutting down...\n", signum);
    Shutdown(true);
}

/* Establishes a TCP connection to the WebSocket server and performs the handshake */
static int connect_ws(void)
{
    int sock;
    struct sockaddr_in serv;
    char req[512], resp[HANDSHAKE_BUF];
    unsigned char rnd[16];
    char client_key[32], server_accept[64], expected_accept[64];
    size_t key_len = sizeof(client_key), accept_len = sizeof(expected_accept);
    ssize_t n;

    /* TCP connect */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    serv.sin_family = AF_INET;
    serv.sin_port = htons(ws_port);
    inet_pton(AF_INET, ws_host, &serv.sin_addr);
    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        perror("ERROR connecting to WebSocket server");
        close(sock);
        return -1;
    }

    /* Generate & encode Sec-WebSocket-Key */
    RAND_bytes(rnd, sizeof(rnd));
    base64_encode(rnd, sizeof(rnd), client_key, &key_len);
    client_key[key_len] = '\0';

    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n",
             ws_path, ws_host, ws_port, client_key);
    send(sock, req, strlen(req), 0);

    n = recv(sock, resp, sizeof(resp) - 1, 0);
    resp[n] = '\0';
    if (!strstr(resp, "101 Switching Protocols"))
    {
        close(sock);
        return -1;
    }

    /* Extract Sec‑WebSocket-Accept from the server response */
    char *p = strstr(resp, "Sec-WebSocket-Accept:");
    if (!p)
    {
        close(sock);
        return -1;
    }
    p += strlen("Sec-WebSocket-Accept:");
    while (*p == ' ')
        p++;
    sscanf(p, "%63s", server_accept);

    /* Validate the server’s accept key */
    ws_make_accept_key(client_key, expected_accept, &accept_len);
    expected_accept[accept_len] = '\0';
    if (strcmp(server_accept, expected_accept) != 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

/* Opens the RTSP stream, reads frames, and relays them via WebSocket continuously */
static void stream_loop(int ws_fd)
{
    for (;;)
    {
        AVFormatContext *fmt = NULL;
        AVCodecContext *ctx = NULL;
        AVPacket *pkt = NULL;
        int vid_idx, ret, ok = 0;
        int sent_sps = 0;
        uint8_t wsbuf[10 + MAX_PKT];
        size_t wslen;

        fprintf(stderr, "[DEBUG] Opening RTSP stream: %s\n", rtsp_url);

        /* Open RTSP input */
        if ((ret = avformat_open_input(&fmt, rtsp_url, NULL, NULL)) < 0)
        {
            fprintf(stderr, "[ERROR] avformat_open_input failed (%d). Retrying in 1s…\n", ret);
        }
        else if ((ret = avformat_find_stream_info(fmt, NULL)) < 0)
        {
            fprintf(stderr, "[ERROR] avformat_find_stream_info failed (%d). Retrying in 1s…\n", ret);
        }
        else if ((vid_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) < 0)
        {
            fprintf(stderr, "[ERROR] No video stream found. Retrying in 1s…\n");
        }
        else
        {
            AVCodecParameters *cp = fmt->streams[vid_idx]->codecpar;
            const AVCodec *dec = avcodec_find_decoder(cp->codec_id);
            if (!dec)
            {
                fprintf(stderr, "[ERROR] Unsupported codec (%d). Retrying in 1s…\n", cp->codec_id);
            }
            else
            {
                ctx = avcodec_alloc_context3(dec);
                if (!ctx ||
                    avcodec_parameters_to_context(ctx, cp) < 0 ||
                    avcodec_open2(ctx, dec, NULL) < 0)
                {
                    fprintf(stderr, "[ERROR] Decoder init failed. Retrying in 1s…\n");
                }
                else if (!(pkt = av_packet_alloc()))
                {
                    fprintf(stderr, "[ERROR] av_packet_alloc failed. Retrying in 1s…\n");
                }
                else
                {
                    ok = 1;
                }
            }
        }

        if (ok)
        {
            while ((ret = av_read_frame(fmt, pkt)) >= 0)
            {
                if (pkt->stream_index == vid_idx)
                {
                    if (!sent_sps && (pkt->flags & AV_PKT_FLAG_KEY) && ctx->extradata)
                    {
                        ws_create_binary_frame(ctx->extradata, ctx->extradata_size, wsbuf, &wslen);
                        if (send(ws_fd, wsbuf, wslen, 0) < 0)
                        {
                            perror("[ERROR] send SPS/PPS");
                            Shutdown(false);
                        }
                        sent_sps = 1;
                    }
                    ws_create_binary_frame(pkt->data, pkt->size, wsbuf, &wslen);
                    if (send(ws_fd, wsbuf, wslen, 0) < 0)
                    {
                        perror("[ERROR] send frame");
                        Shutdown(false);
                    }
                }
                av_packet_unref(pkt);
            }
            if (ret != AVERROR_EOF)
            {
                fprintf(stderr, "[ERROR] av_read_frame failed (%d). Retrying in 1s…\n", ret);
            }
        }

        /* Cleanup before retrying connection */
        if (pkt)
            av_packet_free(&pkt);
        if (ctx)
            avcodec_free_context(&ctx);
        if (fmt)
            avformat_close_input(&fmt);

        fprintf(stderr, "[DEBUG] Retry RTSP connection in 1 second\n");
        sleep(1);
    }
}
