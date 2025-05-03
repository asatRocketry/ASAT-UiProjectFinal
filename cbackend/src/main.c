// #include <stdio.h>
// #include <stdlib.h>
// #include <unistd.h>
// #include <signal.h>
// #include <errno.h>
// #include <sys/epoll.h>

// // Include our common and module-specific headers.
// #include "common_ws.h"    // Provides BUFFER_SIZE, sensor_data_t, and globals.
// #include "remote_ws.h"    // Provides REMOTE_WS_IP, REMOTE_WS_PORT, handle_remote_ws_read(), etc.
// #include "frontend_ws.h"  // Provides FRONTEND_PORT, MAX_CLIENTS, handle_new_client(), handle_client_read(), connect_remote_ws(), broadcast_sensor_data()

// // Global flag for main loop control.
// volatile sig_atomic_t running = 1;

// // Our custom signal handler: simply set the running flag to 0.
// void handle_sigint_wrapper(int sig) {
//     (void)sig;
//     running = 0;
// }

// // A cleanup routine that closes sockets, sends closing frames, and frees resources.
// // This version does not call exit(), so main can finish gracefully.
// void cleanup(void) {
//     uint8_t close_buf[BUFFER_SIZE];
//     size_t close_size = sizeof(close_buf);
//     ws_create_closing_frame(close_buf, &close_size);
    
//     // Close all connected client sockets.
//     for (int i = 0; i < MAX_CLIENTS; i++) {
//         if (g_clients[i].fd != -1) {
//             send(g_clients[i].fd, close_buf, close_size, 0);
//             close(g_clients[i].fd);
//         }
//     }
//     if (g_server_fd != -1)
//         close(g_server_fd);
//     if (g_remote_fd != -1)
//         close(g_remote_fd);
//     if (redis_ctx)
//         redisFree(redis_ctx);
// }

// int main(void) {
//     // Set up our signal handler.
//     signal(SIGINT, handle_sigint_wrapper);

//     // Initialize all frontend client slots.
//     for (int i = 0; i < MAX_CLIENTS; i++) {
//         g_clients[i].fd = -1;
//         g_clients[i].handshake_done = false;
//         g_clients[i].buffer_len = 0;
//     }

//     // Initialize Redis connection.
//     redis_ctx = redisConnect("127.0.0.1", 6379);
//     if (!redis_ctx || redis_ctx->err) {
//         if (redis_ctx) {
//             fprintf(stderr, "Redis error: %s\n", redis_ctx->errstr);
//             redisFree(redis_ctx);
//         }
//         return EXIT_FAILURE;
//     }

//     // Set up the frontend WebSocket server.
//     g_server_fd = init_frontend_server(FRONTEND_PORT);
//     if (g_server_fd < 0) {
//         fprintf(stderr, "Failed to initialize frontend server\n");
//         return EXIT_FAILURE;
//     }
//     printf("Frontend WebSocket server listening on port %d\n", FRONTEND_PORT);

//     // Attempt to connect to the remote sensor WebSocket server.
//     while (running && ((g_remote_fd = connect_remote_ws(REMOTE_WS_IP, REMOTE_WS_PORT)) < 0)) {
//         fprintf(stderr, "Failed to connect to remote WebSocket server. Retrying...\n");
//         sleep(1);
//     }
//     if (!running) {
//         // SIGINT occurred during the connection loop.
//         cleanup();
//         return EXIT_SUCCESS;
//     }
//     printf("Connected to remote WebSocket server at %s:%d\n", REMOTE_WS_IP, REMOTE_WS_PORT);

//     // Create the epoll instance.
//     epoll_fd = epoll_create1(0);
//     if (epoll_fd < 0) {
//         perror("epoll_create1()");
//         cleanup();
//         return EXIT_FAILURE;
//     }

//     // Register the frontend server socket.
//     struct epoll_event ev;
//     ev.events = EPOLLIN;
//     ev.data.fd = g_server_fd;
//     if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_server_fd, &ev) < 0) {
//         perror("epoll_ctl(): server_fd");
//         cleanup();
//         return EXIT_FAILURE;
//     }

//     // Register the remote WebSocket socket (using edge-triggered mode).
//     ev.events = EPOLLIN | EPOLLET;
//     ev.data.fd = g_remote_fd;
//     if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_remote_fd, &ev) < 0) {
//         perror("epoll_ctl(): remote_fd");
//         cleanup();
//         return EXIT_FAILURE;
//     }

//     // Main event loop.
//     struct epoll_event events[1024];
//     while (running) {
//         int nready = epoll_wait(epoll_fd, events, 1024, 100);
//         if (nready < 0) {
//             if (errno == EINTR)
//                 continue;
//             perror("epoll_wait()");
//             break;
//         }
//         for (int i = 0; i < nready; i++) {
//             int fd = events[i].data.fd;
//             if (fd == g_server_fd) {
//                 // New incoming frontend connection.
//                 printf("New frontend connection incoming\n");
//                 handle_new_client();
//             } else if (fd == g_remote_fd) {
//                 // Data from the remote sensor server.
//                 printf("Data from remote incoming\n");
//                 handle_remote_ws_read();
//             } else {
//                 // Frontend client event.
//                 client_t *client = (client_t *)events[i].data.ptr;
//                 if (client) {
//                     printf("Data from client incoming\n");
//                     handle_client_read(client);
//                 }
//             }
//         }
//         // Optionally: call broadcast_sensor_data() if needed.
//     }

//     // Cleanup resources before exiting.
//     cleanup();
//     return EXIT_SUCCESS;
// }
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/epoll.h>

#include "common_ws.h"       // Provides BUFFER_SIZE, sensor_data_t, globals, g_clients_mutex
#include "remote_ws.h"       // Remote data source
#include "frontend_ws.h"     // Frontend server & client handler
#include "config.h"
#include "video_ws.h"
#include "rtsp2ws_video.h"
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>  // for PRIu64
#include <time.h>

volatile sig_atomic_t running = 1;

pthread_t video_server_thread;

void *video_server_thread_func(void *arg) {
    (void)arg;
    video_epoll_loop();
    return NULL;
}

// Handle SIGINT to exit cleanly
void handle_sigint_wrapper(int sig) {
    (void)sig;
    running = 0;
}

// Clean up sockets and other resources
void cleanup(void) {
    uint8_t close_buf[BUFFER_SIZE];
    size_t close_size = sizeof(close_buf);
    ws_create_closing_frame(close_buf, &close_size);

    // Stop RTSP video thread
    rtsp_video_stop();

    // Lock client list while shutting down connections
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd != -1) {
            send(g_clients[i].fd, close_buf, close_size, 0);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_clients[i].handshake_done = false;
            g_clients[i].buffer_len = 0;
        }
    }
    pthread_mutex_unlock(&g_clients_mutex);

    if (g_server_fd != -1)
        close(g_server_fd);
    if (g_remote_fd != -1)
        close(g_remote_fd);
    if (redis_ctx)
        redisFree(redis_ctx);
}

int main(void) {

    if (initialize_csv_logging() < 0) {
        fprintf(stderr, "Failed to initialize CSV logging.\n");
        cleanup();
        return EXIT_FAILURE;
    }
    
    signal(SIGINT, handle_sigint);

    // Initialize frontend client slots
    pthread_mutex_lock(&g_clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].handshake_done = false;
        g_clients[i].buffer_len = 0;
    }
    pthread_mutex_unlock(&g_clients_mutex);

    // Connect to Redis
    redis_ctx = redisConnect("127.0.0.1", 6379);
    if (!redis_ctx || redis_ctx->err) {
        if (redis_ctx) {
            fprintf(stderr, "Redis error: %s\n", redis_ctx->errstr);
            redisFree(redis_ctx);
        }
        return EXIT_FAILURE;
    }

    // Start frontend WebSocket server
    g_server_fd = init_frontend_server(FRONTEND_PORT);
    if (g_server_fd < 0) {
        fprintf(stderr, "Failed to initialize frontend server\n");
        return EXIT_FAILURE;
    }
    printf("Frontend WebSocket server listening on port %d\n", FRONTEND_PORT);

    // Connect to remote data source
    while (running && ((g_remote_fd = connect_remote_ws(REMOTE_WS_IP, REMOTE_WS_PORT)) < 0)) {
        fprintf(stderr, "Failed to connect to remote WebSocket server. Retrying...\n");
        sleep(1);
    }
    if (!running) {
        cleanup();
        return EXIT_SUCCESS;
    }
    printf("Connected to remote WebSocket server at %s:%d\n", REMOTE_WS_IP, REMOTE_WS_PORT);

    // Set up epoll
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1()");
        cleanup();
        return EXIT_FAILURE;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_server_fd, &ev) < 0) {
        perror("epoll_ctl(): server_fd");
        cleanup();
        return EXIT_FAILURE;
    }

    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = g_remote_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_remote_fd, &ev) < 0) {
        perror("epoll_ctl(): remote_fd");
        cleanup();
        return EXIT_FAILURE;
    }

    // // Initialize and start the RTSP video streaming module.
    // if (rtsp_video_init("rtsp://adminsat:asatisgaysat@192.168.2.21/stream2") != 0) {
    //     fprintf(stderr, "Failed to initialize RTSP video module\n");
    //     cleanup();
    //     return EXIT_FAILURE;
    // }
    // if (rtsp_video_start() != 0) {
    //     fprintf(stderr, "Failed to start RTSP video streaming\n");
    //     cleanup();
    //     return EXIT_FAILURE;
    // }

    // // Initialize the video WebSocket server on a separate port (e.g., 8002).
    // if (init_video_server(8002) != 0) {
    //     fprintf(stderr, "Failed to initialize video WebSocket server\n");
    //     cleanup();
    //     return EXIT_FAILURE;
    // }
    // // Start the video server thread.
    // if (pthread_create(&video_server_thread, NULL, video_server_thread_func, NULL) != 0) {
    //     perror("pthread_create() video server thread");
    //     cleanup();
    //     return EXIT_FAILURE;
    // }

    // Main epoll loop
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
                printf("New frontend connection incoming\n");
                handle_new_client();  // Assumes it uses mutex internally
            } else if (fd == g_remote_fd) {
                printf("Data from remote incoming\n");
                handle_remote_ws_read();  // Also assumes mutex where needed
            } else {
                client_t *client = (client_t *)events[i].data.ptr;
                if (client) {
                    printf("Data from client incoming\n");
                    handle_client_read(client);  // Assumes internal locking if it modifies clients
                }
            }
        }
    }

    cleanup();
    return EXIT_SUCCESS;
}
