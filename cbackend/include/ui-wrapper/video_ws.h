#ifndef VIDEO_WS_H
#define VIDEO_WS_H

#include "common_ws.h"

extern client_t *g_video_clients;
extern pthread_mutex_t g_video_clients_mutex;

int init_video_server(int port);
void video_epoll_loop(void);
void handle_new_video_client(void);

#endif // VIDEO_WS_H
