#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>

#include "common_ws.h"    // Provides BUFFER_SIZE, sensor_data_t, and globals/functions like set_nonblocking(), init_frontend_server(), handle_sigint()
#include "remote_ws.h"    // Provides REMOTE_WS_IP, REMOTE_WS_PORT, handle_remote_ws_read(), etc.
#include "frontend_ws.h"  // Provides FRONTEND_PORT, MAX_CLIENTS, g_clients, handle_new_client(), handle_client_read(), connect_remote_ws(), broadcast_sensor_data()
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>  // for PRIu64
// Global variable to control the event loop.
volatile sig_atomic_t running = 1;

// Simple signal handler wrapper (do not call exit() here so we can perform cleanup in main)
void handle_sigint_wrapper(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    // Set up signal handling for graceful shutdown.
    signal(SIGINT, handle_sigint_wrapper);

    // Initialize all frontend client slots.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].handshake_done = false;
        g_clients[i].buffer_len = 0;
    }

    // Initialize Redis connection.
    redis_ctx = redisConnect("127.0.0.1", 6379);
    if (!redis_ctx || redis_ctx->err) {
        if (redis_ctx) {
            fprintf(stderr, "Redis error: %s\n", redis_ctx->errstr);
            redisFree(redis_ctx);
        }
        return EXIT_FAILURE;
    }

    // Set up the frontend WebSocket server.
    g_server_fd = init_frontend_server(FRONTEND_PORT);
    if (g_server_fd < 0) {
        fprintf(stderr, "Failed to initialize frontend server\n");
        return EXIT_FAILURE;
    }
    printf("Frontend WebSocket server listening on port %d\n", FRONTEND_PORT);

    // Attempt to connect to the remote sensor WebSocket server.
    while ((g_remote_fd = connect_remote_ws(REMOTE_WS_IP, REMOTE_WS_PORT)) < 0) {
        fprintf(stderr, "Failed to connect to remote WebSocket server. Retrying...\n");
        sleep(1);
    }
    printf("Connected to remote WebSocket server at %s:%d\n", REMOTE_WS_IP, REMOTE_WS_PORT);

    // Create epoll instance.
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1()");
        return EXIT_FAILURE;
    }

    // Register the frontend server descriptor.
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_server_fd, &ev) < 0) {
        perror("epoll_ctl(): server_fd");
        return EXIT_FAILURE;
    }

    // Register the remote WebSocket descriptor (using edge-triggered mode).
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = g_remote_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_remote_fd, &ev) < 0) {
        perror("epoll_ctl(): remote_fd");
        return EXIT_FAILURE;
    }

    // Main event loop.
    struct epoll_event events[1024];
    while (running) {
        int nready = epoll_wait(epoll_fd, events, 1024, 100);
        if (nready < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait()");
            break;
        }
        for (int i = 0; i < nready; i++) {
            int fd = events[i].data.fd;
            if (fd == g_server_fd) {
                // New incoming frontend connection.
                printf("New frontend connection incoming\n");
                handle_new_client();
            } else if (fd == g_remote_fd) {
                // Data from the remote sensor server.
                printf("Data from remote incoming\n");
                struct timespec ts_debug;
clock_gettime(CLOCK_REALTIME, &ts_debug);
uint64_t debug_ns = (uint64_t)ts_debug.tv_sec * 1000000000 + ts_debug.tv_nsec;
printf("Current timestamp (ns): %" PRIu64 "\n", debug_ns);
                handle_remote_ws_read();
            } else {
                // Frontend client event.
                client_t *client = (client_t *)events[i].data.ptr;
                if (client) {
                    printf("Data from client incoming\n");
                    handle_client_read(client);
                }
            }
        }
        // Optionally, broadcast sensor data here if not handled automatically.
        // broadcast_sensor_data();
    }

    // Cleanup: call handle_sigint() from common_ws.c to perform shutdown tasks.
    handle_sigint(0);
    return EXIT_SUCCESS;
}
