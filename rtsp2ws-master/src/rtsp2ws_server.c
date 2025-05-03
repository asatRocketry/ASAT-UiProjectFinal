/* rtsp2ws_server.c - RTSP to WebSocket stream relay (Server)
 *
 * This program opens an RTSP stream, then acts as a WebSocket server
 * accepting multiple client connections. It uses epoll (with edge-triggered
 * notifications) and pthreads for scalable event notification. It performs the
 * WebSocket handshake on each new connection and broadcasts raw H264 frames
 * (wrapped in WebSocket binary frames) to all connected clients.
 *
 * Compile with:
 *   gcc rtsp2ws_server.c -o rtsp2ws_server -lavformat -lavcodec -lavutil -lpthread -lssl -lcrypto
 *
 * Usage: ./rtsp2ws_server <rtsp_url> <listen_port>
 * Example: ./rtsp2ws_server rtsp://192.168.1.100/stream 8765
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

#include "simple_ws/websocket.h"
#include "simple_ws/wshandshake.h"
#include "simple_ws/base64.h"

/* Constants */
#define MAX_PKT 2000000
#define HANDSHAKE_BUF 4096
#define MAX_CLIENTS 1024

/* Client structure for tracking connection state */
typedef struct
{
    int fd;                        // Socket file descriptor, or -1 if unused
    bool handshake_done;           // Has the WebSocket handshake been completed?
    uint8_t buffer[HANDSHAKE_BUF]; // Buffer for handshake data
    size_t buffer_len;             // Current length of data in buffer
} client_t;

/* Global variables for RTSP stream and server configuration */
static const char *rtsp_url;
static int listen_port;

/* Global variables for the WebSocket server */
static int g_server_fd = -1;
static int g_epoll_fd = -1;
static client_t *g_clients = NULL;
static pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t shutdown_flag = 0;

/* FFmpeg globals */
AVFormatContext *fmt = NULL;
AVCodecContext *ctx = NULL;
AVPacket *pkt = NULL;
AVDictionary *opts = NULL;

/* New globals for storing SPS/PPS configuration frame */
static uint8_t *g_config_data = NULL;
static size_t g_config_size = 0;

/* Function prototypes */

static void Shutdown(void);
static void sigint_handler(int signum);
static void set_nonblocking(int fd);
static int init_server_socket(int port);
static void handle_new_connection(int epoll_fd);
static void handle_client_read(client_t *client, int epoll_fd);
static void close_client(client_t *client, const char *reason, int epoll_fd);
static void broadcast_frame(uint8_t *wsbuf, size_t wslen);
static void *server_thread_func(void *arg);
static void stream_loop(void);

#ifdef DEBUG
FILE* fp = NULL;
#endif
/*------------------------------------------------------------------------------
 * main: Entry point of the RTSP to WebSocket relay server.
 * Processes command-line arguments, initializes the server and RTSP stream, starts
 * the server thread, and enters the streaming loop.
 *------------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <rtsp_url> <listen_port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    rtsp_url = argv[1];
    listen_port = atoi(argv[2]);

    signal(SIGINT, sigint_handler);

    avformat_network_init();

    /* Initialize server socket */
    g_server_fd = init_server_socket(listen_port);
    if (g_server_fd < 0)
    {
        fprintf(stderr, "Failed to initialize server socket\n");
        exit(EXIT_FAILURE);
    }

    /* Create epoll instance and add the server socket */
    g_epoll_fd = epoll_create1(0);
    if (g_epoll_fd < 0)
    {
        perror("epoll_create1()");
        exit(EXIT_FAILURE);
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL; // NULL indicates the server socket
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_server_fd, &ev) < 0)
    {
        perror("epoll_ctl() server_fd");
        exit(EXIT_FAILURE);
    }

    /* Allocate client slots */
    g_clients = calloc(MAX_CLIENTS, sizeof(client_t));
    if (!g_clients)
    {
        fprintf(stderr, "Failed to allocate client slots\n");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        g_clients[i].fd = -1;
        g_clients[i].handshake_done = false;
        g_clients[i].buffer_len = 0;
    }

    /* Start the server thread to handle incoming connections and client I/O */
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_thread_func, NULL) != 0)
    {
        perror("pthread_create()");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "WebSocket server started on port %d\nRTSP stream: %s\n", listen_port, rtsp_url);

    /* Main thread: start streaming RTSP and broadcasting frames */
    stream_loop();

    pthread_join(server_thread, NULL);
    Shutdown();
    return 0;
}

/*------------------------------------------------------------------------------
 * set_nonblocking: Set file descriptor to non-blocking mode.
 *------------------------------------------------------------------------------*/
static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*------------------------------------------------------------------------------
 * init_server_socket: Initialize a TCP server socket.
 *------------------------------------------------------------------------------*/
static int init_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket()");
        return -1;
    }
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt()");
        close(fd);
        return -1;
    }
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("0.0.0.0");
    serv.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&serv, sizeof(serv)) < 0)
    {
        perror("bind()");
        close(fd);
        return -1;
    }
    if (listen(fd, 10) < 0)
    {
        perror("listen()");
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

/*------------------------------------------------------------------------------
 * handle_new_connection: Accept and set up a new client connection.
 *------------------------------------------------------------------------------*/
static void handle_new_connection(int epoll_fd)
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int new_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (new_fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept()");
        return;
    }
    set_nonblocking(new_fd);

    /* Find a free slot for the new client */
    pthread_mutex_lock(&g_clients_mutex);
    int found = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].fd == -1)
        {
            g_clients[i].fd = new_fd;
            g_clients[i].handshake_done = false;
            g_clients[i].buffer_len = 0;
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.ptr = &g_clients[i];
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &ev) < 0)
            {
                perror("epoll_ctl() new client");
                close(new_fd);
                g_clients[i].fd = -1;
            }
            else
            {
                printf("New client connected. FD = %d\n", new_fd);
            }
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
    if (!found)
    {
        printf("Too many clients, rejecting connection.\n");
        close(new_fd);
    }
}

/*------------------------------------------------------------------------------
 * handle_client_read: Process incoming data from a client.
 *------------------------------------------------------------------------------*/
static void handle_client_read(client_t *client, int epoll_fd)
{
    while (1)
    {
        uint8_t recv_buf[HANDSHAKE_BUF];
        ssize_t n = recv(client->fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0)
        {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break; // No more data available
            close_client(client, "Client disconnected or read error", epoll_fd);
            return;
        }

        // If handshake is not complete, accumulate data and process handshake.
        if (!client->handshake_done)
        {
            if (client->buffer_len + n > sizeof(client->buffer))
            {
                close_client(client, "Handshake buffer overflow", epoll_fd);
                return;
            }
            memcpy(client->buffer + client->buffer_len, recv_buf, (size_t)n);
            client->buffer_len += n;

            http_header header;
            memset(&header, 0, sizeof(header));
            size_t out_len = sizeof(client->buffer);
            ws_handshake(&header, client->buffer, client->buffer_len, &out_len);
            if (header.type == WS_OPENING_FRAME)
            {
                /* Handshake complete: send handshake response */
                send(client->fd, client->buffer, out_len, 0);
                client->handshake_done = true;
                printf("Client FD %d handshake done (Key=%s)\n", client->fd, header.key);
                client->buffer_len = 0;
                /* Send stored SPS/PPS configuration, if available */
                if (g_config_data != NULL && g_config_size > 0)
                {
                    send(client->fd, g_config_data, g_config_size, 0);
                    printf("Sent configuration to new client FD %d\n", client->fd);
                }
            }
            else if (out_len > 0)
            {
                /* If a handshake response was generated but not valid, send it and close */
                send(client->fd, client->buffer, out_len, 0);
                close_client(client, "Invalid handshake", epoll_fd);
                return;
            }
            continue; // Check for more data
        }

        // Handshake complete – process incoming frames.
        ws_frame frame;
        memset(&frame, 0, sizeof(frame));
        ws_parse_frame(&frame, recv_buf, (size_t)n);
        if (frame.type == WS_CLOSING_FRAME)
        {
            printf("Client FD %d sent CLOSE, closing connection.\n", client->fd);
            uint8_t out_buf[128];
            size_t out_size = sizeof(out_buf);
            ws_create_closing_frame(out_buf, &out_size);
            send(client->fd, out_buf, out_size, 0);
            close_client(client, "Close frame", epoll_fd);
            return;
        }
    }
}

/*------------------------------------------------------------------------------
 * close_client: Safely close a client connection.
 *------------------------------------------------------------------------------*/
static void close_client(client_t *client, const char *reason, int epoll_fd)
{
    printf("Closing client FD %d: %s\n", client->fd, reason);
    if (client->fd != -1)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
    }
    client->fd = -1;
    client->handshake_done = false;
    client->buffer_len = 0;
}

/*------------------------------------------------------------------------------
 * broadcast_frame: Broadcast a WebSocket binary frame to all connected clients.
 *------------------------------------------------------------------------------*/
static void broadcast_frame(uint8_t *wsbuf, size_t wslen)
{
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].fd != -1 && g_clients[i].handshake_done)
        {
            if (send(g_clients[i].fd, wsbuf, wslen, 0) < 0)
            {
                perror("send() broadcast");
                // Optionally, you can close the client on error
            }
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);
}

/*------------------------------------------------------------------------------
 * server_thread_func: Main loop for the server thread handling I/O events.
 *------------------------------------------------------------------------------*/
static void *server_thread_func(void *arg)
{
    (void)arg;
    struct epoll_event events[64];
    while (!shutdown_flag)
    {
        int nfds = epoll_wait(g_epoll_fd, events, 64, 1000);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            perror("epoll_wait()");
            break;
        }
        for (int i = 0; i < nfds; i++)
        {
            // If data.ptr is NULL, this is the server listening socket.
            if (events[i].data.ptr == NULL)
            {
                handle_new_connection(g_epoll_fd);
            }
            else
            {
                client_t *client = (client_t *)events[i].data.ptr;
                if (client)
                    handle_client_read(client, g_epoll_fd);
            }
        }
    }
    return NULL;
}

/*------------------------------------------------------------------------------
 * stream_loop: Process the RTSP stream and broadcast video frames.
 *------------------------------------------------------------------------------*/
static void stream_loop(void)
{
    #ifdef DEBUG
    fp = fopen("output.h264", "wb");
    if (!fp) {
        perror("fopen()");
        exit(EXIT_FAILURE);
    }
    #endif
    int ret, vid_idx, sent_sps = 0;
    uint8_t wsbuf[10 + MAX_PKT];
    size_t wslen;

    for (;;)
    {
        printf("[DEBUG] Opening RTSP stream: %s\n", rtsp_url);
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "max_delay", "500000", 0); // 500ms delay
        if ((ret = avformat_open_input(&fmt, rtsp_url, NULL, &opts)) < 0)
        {
            fprintf(stderr, "[ERROR] avformat_open_input failed (%d). Retrying in 1s…\n", ret);
            av_dict_free(&opts);
            sleep(1);
            continue;
        }
        av_dict_free(&opts);
        if ((ret = avformat_find_stream_info(fmt, NULL)) < 0)
        {
            fprintf(stderr, "[ERROR] avformat_find_stream_info failed (%d). Retrying in 1s…\n", ret);
            avformat_close_input(&fmt);
            sleep(1);
            continue;
        }
        if ((vid_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) < 0)
        {
            fprintf(stderr, "[ERROR] No video stream found. Retrying in 1s…\n");
            avformat_close_input(&fmt);
            sleep(1);
            continue;
        }
        AVCodecParameters *cp = fmt->streams[vid_idx]->codecpar;
        const AVCodec *dec = avcodec_find_decoder(cp->codec_id);
        if (!dec)
        {
            fprintf(stderr, "[ERROR] Unsupported codec (%d). Retrying in 1s…\n", cp->codec_id);
            avformat_close_input(&fmt);
            sleep(1);
            continue;
        }
        ctx = avcodec_alloc_context3(dec);
        if (!ctx ||
            avcodec_parameters_to_context(ctx, cp) < 0 ||
            avcodec_open2(ctx, dec, NULL) < 0)
        {
            fprintf(stderr, "[ERROR] Decoder init failed. Retrying in 1s…\n");
            if (ctx)
                avcodec_free_context(&ctx);
            avformat_close_input(&fmt);
            sleep(1);
            continue;
        }
        pkt = av_packet_alloc();
        if (!pkt)
        {
            fprintf(stderr, "[ERROR] av_packet_alloc failed. Retrying in 1s…\n");
            avcodec_free_context(&ctx);
            avformat_close_input(&fmt);
            sleep(1);
            continue;
        }
        sent_sps = 0;
        while ((ret = av_read_frame(fmt, pkt)) >= 0 && !shutdown_flag)
        {
            #ifdef DEBUG
            printf("Read frame, stream_index: %d, size: %d, key: %s\n",
                pkt->stream_index, pkt->size, (pkt->flags & AV_PKT_FLAG_KEY) ? "yes" : "no");
            #endif

            if (pkt->stream_index == vid_idx)
            {
                /* Before sending the first key frame, send SPS/PPS if available */
                if (!sent_sps && (pkt->flags & AV_PKT_FLAG_KEY) && ctx->extradata)
                {
                    ws_create_binary_frame(ctx->extradata, ctx->extradata_size, wsbuf, &wslen);
                    broadcast_frame(wsbuf, wslen);
                    /* Store configuration for new clients if not already stored */
                    if (g_config_data == NULL)
                    {
                        g_config_data = malloc(wslen);
                        if (g_config_data)
                        {
                            memcpy(g_config_data, wsbuf, wslen);
                            g_config_size = wslen;
                        }
                    }
                    sent_sps = 1;
                }
                ws_create_binary_frame(pkt->data, pkt->size, wsbuf, &wslen);
                #ifdef DEBUG
                {
                    ws_frame test_frame;
                    memset(&test_frame, 0, sizeof(test_frame));
                    ws_parse_frame(&test_frame, wsbuf, wslen);
                    if (fp) {
                        fwrite(test_frame.payload, 1, test_frame.payload_length, fp);
                        fflush(fp);
                        printf("Parsed frame: payload length = %zu\n", test_frame.payload_length);
                    }
                }
                #endif
                broadcast_frame(wsbuf, wslen);
            }
            av_packet_unref(pkt);
        }
        if (ret != AVERROR_EOF)
        {
            fprintf(stderr, "[ERROR] av_read_frame failed (%d). Retrying in 1s…\n", ret);
        }
        av_packet_free(&pkt);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        sleep(1);
    }
}

/*------------------------------------------------------------------------------
 * Shutdown: Cleanup resources and shut down the server gracefully.
 *------------------------------------------------------------------------------*/
static void Shutdown(void)
{
    shutdown_flag = 1;
    uint8_t wsbuf[128];
    size_t wslen = sizeof(wsbuf);
    ws_create_closing_frame(wsbuf, &wslen);

    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (g_clients[i].fd != -1)
        {
            send(g_clients[i].fd, wsbuf, wslen, 0);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);

    if (g_config_data)
    {
        free(g_config_data);
        g_config_data = NULL;
        g_config_size = 0;
    }
    if (pkt)
        av_packet_free(&pkt);
    if (ctx)
        avcodec_free_context(&ctx);
    if (fmt)
        avformat_close_input(&fmt);
    if (opts)
        av_dict_free(&opts);

    if (g_server_fd >= 0)
        close(g_server_fd);
    if (g_epoll_fd >= 0)
        close(g_epoll_fd);
    if (g_clients)
        free(g_clients);

    #ifdef DEBUG
    fclose(fp);
    #endif
    exit(EXIT_SUCCESS);
}

/*------------------------------------------------------------------------------
 * sigint_handler: Handle SIGINT for graceful shutdown.
 *------------------------------------------------------------------------------*/
static void sigint_handler(int signum)
{
    (void)signum;
    printf("[INFO] SIGINT received, shutting down...\n");
    Shutdown();
}
