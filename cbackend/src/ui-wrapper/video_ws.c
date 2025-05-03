#include "video_ws.h"
#include "websocket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "wshandshake.h"

int g_video_server_fd = -1;
client_t *g_video_clients = NULL; // Separate array for video clients
pthread_mutex_t g_video_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_video_epoll_fd = -1;



void handle_video_client_read(client_t *client) {
    while (1) {
        uint8_t recv_buf[BUFFER_SIZE];
        ssize_t n = recv(client->fd, recv_buf, sizeof(recv_buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            printf("Video client FD %d disconnected.\n", client->fd);
            close(client->fd);
            client->fd = -1;
            break;
        }

        if (!client->handshake_done) {
            if (client->buffer_len + n > BUFFER_SIZE) {
                printf("Video handshake buffer overflow for client FD %d\n", client->fd);
                close(client->fd);
                client->fd = -1;
                break;
            }
            memcpy(client->buffer + client->buffer_len, recv_buf, n);
            client->buffer_len += n;

            http_header header;
            memset(&header, 0, sizeof(header));
            size_t out_len = BUFFER_SIZE;
            ws_handshake(&header, client->buffer, client->buffer_len, &out_len);
            if (header.type == WS_OPENING_FRAME) {
                send(client->fd, client->buffer, out_len, 0);
                client->handshake_done = true;
                printf("Video client FD %d handshake done\n", client->fd);
                client->buffer_len = 0;
            }
        } else {
            // If you ever want to accept input from video clients, handle it here.
        }
    }
}


int init_video_server(int port) {
    g_video_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_video_server_fd < 0) {
        perror("socket()");
        return -1;
    }
    int opt = 1;
    if (setsockopt(g_video_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt()");
        close(g_video_server_fd);
        return -1;
    }
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(port);
    if (bind(g_video_server_fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("bind()");
        close(g_video_server_fd);
        return -1;
    }
    if (listen(g_video_server_fd, 10) < 0) {
        perror("listen()");
        close(g_video_server_fd);
        return -1;
    }
    set_nonblocking(g_video_server_fd);

    g_video_epoll_fd = epoll_create1(0);
    if (g_video_epoll_fd < 0) {
        perror("epoll_create1()");
        close(g_video_server_fd);
        return -1;
    }
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_video_server_fd;
    if (epoll_ctl(g_video_epoll_fd, EPOLL_CTL_ADD, g_video_server_fd, &ev) < 0) {
        perror("epoll_ctl() video server");
        close(g_video_server_fd);
        return -1;
    }

    g_video_clients = calloc(MAX_CLIENTS, sizeof(client_t));
    if (!g_video_clients) {
        fprintf(stderr, "Failed to allocate video clients\n");
        close(g_video_server_fd);
        return -1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_video_clients[i].fd = -1;
        g_video_clients[i].handshake_done = false;
        g_video_clients[i].buffer_len = 0;
    }
    printf("Video WebSocket server listening on port %d\n", port);
    return 0;
}

void handle_new_video_client() {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(g_video_server_fd, (struct sockaddr *)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept() video");
        return;
    }
    set_nonblocking(client_fd);

    pthread_mutex_lock(&g_video_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_video_clients[i].fd == -1) {
            g_video_clients[i].fd = client_fd;
            g_video_clients[i].handshake_done = false;
            g_video_clients[i].buffer_len = 0;
            struct epoll_event ev;
            ev.events = EPOLLIN | EPOLLET;
            ev.data.ptr = &g_video_clients[i];
            if (epoll_ctl(g_video_epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                perror("epoll_ctl() video client");
                close(client_fd);
                g_video_clients[i].fd = -1;
            } else {
                printf("New video client connected. FD = %d\n", client_fd);
            }
            pthread_mutex_unlock(&g_video_clients_mutex);
            return;
        }
    }
    pthread_mutex_unlock(&g_video_clients_mutex);
    printf("Max video clients reached, rejecting connection.\n");
    close(client_fd);
}

void video_epoll_loop() {
    struct epoll_event events[64];
    while (1) {
        int nfds = epoll_wait(g_video_epoll_fd, events, 64, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait() video");
            break;
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == g_video_server_fd) {
                handle_new_video_client();
            } else {
                client_t *client = (client_t *)events[i].data.ptr;
                if (client && client->fd != -1) {
                    handle_video_client_read(client);
                }
            }
        }
    }
}

