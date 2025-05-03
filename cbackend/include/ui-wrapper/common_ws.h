#ifndef COMMON_WS_H
#define COMMON_WS_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <pthread.h>

#include "wshandshake.h" // Custom WebSocket handshake functions
#include "websocket.h"   // Custom WebSocket frame functions
#include "cJSON.h"       // JSON parsing library
#include "hiredis.h"     // Redis connectivity

#define BUFFER_SIZE 4096
#define SENSOR_BUFFER_MAX 100000
#define MAX_CLIENTS 1024

 /* Sensor data structure and buffer */
 typedef struct
 {
     char name[64];
     double value;
     uint64_t timestamp;
     int warning;
 } sensor_data_t;

 typedef struct
 {
     int fd;
     bool handshake_done;
     uint8_t buffer[BUFFER_SIZE];
     size_t buffer_len;
 } client_t;
 
extern pthread_mutex_t g_clients_mutex;

extern client_t g_clients[MAX_CLIENTS];
extern int epoll_fd;
extern int g_remote_fd;
extern int g_server_fd;
extern redisContext *redis_ctx;

extern sensor_data_t sensor_buffer[SENSOR_BUFFER_MAX];
extern sensor_data_t latest_sensor_buffer[SENSOR_BUFFER_MAX];
extern int latest_sensor_buffer_count;
extern int sensor_buffer_count;
extern uint64_t last_broadcast_time;

void remove_from_epoll(int fd);
void set_nonblocking(int fd);
int init_frontend_server(int port);
void handle_sigint(int sig);

#endif // COMMON_WS_H