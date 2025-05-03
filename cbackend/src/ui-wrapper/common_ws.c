#include "common_ws.h"
#include "frontend_ws.h"  // for g_clients[]
#include "remote_ws.h"    // for SENSOR_BUFFER_MAX, etc.

#include "websocket.h"    // ws_create_closing_frame
#include "hiredis.h"      // redisFree

/* Epoll file descriptor */
int epoll_fd = -1;

pthread_mutex_t g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

client_t g_clients[MAX_CLIENTS];
/* Global file descriptors */
int g_server_fd = -1; // Frontend WebSocket server (listening) socket
int g_remote_fd = -1; // Remote WebSocket connection (sensor data)

/* Redis connection context */
redisContext *redis_ctx = NULL;


sensor_data_t sensor_buffer[SENSOR_BUFFER_MAX];
sensor_data_t latest_sensor_buffer[SENSOR_BUFFER_MAX];
int latest_sensor_buffer_count = 0;
int sensor_buffer_count = 0;

 
 /*-------------------- Utility Functions --------------------*/
 // Set a file descriptor to non-blocking mode.
void set_nonblocking(int fd)
 {
     int flags = fcntl(fd, F_GETFL, 0);
     if (flags < 0)
         flags = 0;
     fcntl(fd, F_SETFL, flags | O_NONBLOCK);
 }
 
 // Remove a file descriptor from the epoll instance.
void remove_from_epoll(int fd)
 {
     epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
 }
 
 /*-------------------- Socket Initialization --------------------*/
 // Initialize the frontend server socket.
 int init_frontend_server(int port)
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
     struct sockaddr_in addr;
     memset(&addr, 0, sizeof(addr));
     addr.sin_family = AF_INET;
     addr.sin_addr.s_addr = INADDR_ANY;
     addr.sin_port = htons(port);
     if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
     {
         perror("bind()");
         close(fd);
         return -1;
     }
     if (listen(fd, MAX_CLIENTS) < 0)
     {
         perror("listen()");
         close(fd);
         return -1;
     }
     set_nonblocking(fd);
     return fd;
 }

 /*-------------------- Signal Handling & Shutdown --------------------*/
 void handle_sigint(int sig)
 {
     (void)sig;
     printf("SIGINT received, shutting down...\n");
     uint8_t close_buf[BUFFER_SIZE];
     size_t close_size = sizeof(close_buf);
     ws_create_closing_frame(close_buf, &close_size);
     pthread_mutex_lock(&g_clients_mutex);
     for (int i = 0; i < MAX_CLIENTS; i++)
     {
         if (g_clients[i].fd != -1)
         {
             send(g_clients[i].fd, close_buf, close_size, 0);
             close(g_clients[i].fd);
         }
     }
     pthread_mutex_unlock(&g_clients_mutex);
     if (g_server_fd != -1)
         close(g_server_fd);
     if (g_remote_fd != -1)
         close(g_remote_fd);
     if (redis_ctx)
         redisFree(redis_ctx);
     exit(0);
 }