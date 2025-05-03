#ifndef FRONTEND_WS_H
#define FRONTEND_WS_H

#include "common_ws.h" // for BUFFER_SIZE
#include "config.h"



void handle_new_client();
void close_client(client_t *client);
void broadcast_sensor_data();
void handle_client_read(client_t *client);
#endif // FRONTEND_WS_H