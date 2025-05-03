
#include "frontend_ws.h"
#include "common_ws.h"
#include "remote_ws.h"   // for sensor_buffer, warnings

#include "wshandshake.h"
#include "websocket.h"
#include "cJSON.h"



 /*-------------------- Frontend Client Handling --------------------*/
 // Accept a new frontend client connection.
void handle_new_client()
 {
     struct sockaddr_in client_addr;
     socklen_t addr_len = sizeof(client_addr);
     int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &addr_len);
     if (client_fd < 0)
     {
         if (errno != EAGAIN && errno != EWOULDBLOCK)
             perror("accept()");
         return;
     }
     set_nonblocking(client_fd);
     // Find a free slot for the new client.
     pthread_mutex_lock(&g_clients_mutex);
     for (int i = 0; i < MAX_CLIENTS; i++)
     {
         if (g_clients[i].fd == -1)
         {
             g_clients[i].fd = client_fd;
             g_clients[i].handshake_done = false;
             g_clients[i].buffer_len = 0;
             // add_to_epoll(client_fd, EPOLLIN | EPOLLET, &g_clients[i]);
             struct epoll_event ev;
             ev.events = EPOLLIN | EPOLLET;
             ev.data.ptr = &g_clients[i];
             if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
             {
                 perror("epoll_ctl(): new client");
                 close(client_fd);
                 g_clients[i].fd = -1;
             }
             else
             {
                 printf("New frontend client connected. FD = %d\n", client_fd);
             }
             pthread_mutex_unlock(&g_clients_mutex);
             return;
         }
     }
     pthread_mutex_unlock(&g_clients_mutex);
     printf("Max clients reached, rejecting connection.\n");
     close(client_fd);
 }
 
 // Close a frontend client connection.
 void close_client(client_t *client)
 {
     if (client->fd != -1)
     {
         remove_from_epoll(client->fd);
         close(client->fd);
         client->fd = -1;
         client->handshake_done = false;
         client->buffer_len = 0;
     }
 }
 
 // Send a text message (as a WebSocket frame) to a specific frontend client.
 static void ws_send_text(int fd, const char *msg)
 {
     uint8_t out_buf[BUFFER_SIZE];
     size_t out_size = sizeof(out_buf);
     ws_frame frame;
     memset(&frame, 0, sizeof(frame));
     frame.type = WS_TEXT_FRAME;
     frame.payload = (uint8_t *)msg;
     frame.payload_length = strlen(msg);
     ws_create_frame(&frame, out_buf, &out_size);
     if (send(fd, out_buf, out_size, 0) < 0)
     {
         perror("send() ws_send_text");
     }
 }
 
//  // Broadcast a text message to all connected (and handshaken) frontend clients.
//  static void broadcast_text(const char *msg)
//  {
//      for (int i = 0; i < MAX_CLIENTS; i++)
//      {
//          if (g_clients[i].fd != -1 && g_clients[i].handshake_done)
//          {
//              ws_send_text(g_clients[i].fd, msg);
//          }
//      }
//  }




 // Broadcast the buffered sensor data to all connected frontend clients
 // and then clear the sensor buffer.
 void broadcast_sensor_data()
 {
     if (latest_sensor_buffer_count == 0)
         return;
     cJSON *json_array = cJSON_CreateArray();
     for (int i = 0; i < latest_sensor_buffer_count; i++)
     {
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "name", latest_sensor_buffer[i].name);
         cJSON_AddNumberToObject(item, "value", latest_sensor_buffer[i].value);
         cJSON_AddNumberToObject(item, "timestamp", latest_sensor_buffer[i].timestamp);
         cJSON_AddNumberToObject(item, "warning", latest_sensor_buffer[i].warning);
         cJSON_AddItemToArray(json_array, item);
     }
     char *json_str = cJSON_PrintUnformatted(json_array);
     cJSON_Delete(json_array);
 
     // Debug print: show data being forwarded.
    //  printf("Forwarding sensor data to frontend: %s\n", json_str);
 
     uint8_t frame_data[BUFFER_SIZE];
     size_t frame_len = sizeof(frame_data);
     ws_create_text_frame(json_str, frame_data, &frame_len);
     free(json_str);
 
     for (int i = 0; i < MAX_CLIENTS; i++)
     {
         if (g_clients[i].fd != -1 && g_clients[i].handshake_done)
         {
             if (send(g_clients[i].fd, frame_data, frame_len, 0) < 0)
             {
                 perror("send() broadcast_sensor_data");
             }
         }
     }
     latest_sensor_buffer_count = 0;
 }
 

 /*-------------------- Frontend Client Read Handling --------------------*/
 // Handle data from a frontend client. Here we process handshake data if needed.
void handle_client_read(client_t *client)
 {
     while (1)
     {
         uint8_t recv_buf[BUFFER_SIZE];
         ssize_t n = recv(client->fd, recv_buf, sizeof(recv_buf), 0);
         if (n <= 0)
         {
             if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                 break;
             printf("Client FD %d disconnected.\n", client->fd);
             close_client(client);
             break;
         }
         if (!client->handshake_done)
         {
             if (client->buffer_len + n > BUFFER_SIZE)
             {
                 printf("Handshake buffer overflow for client FD %d\n", client->fd);
                 close_client(client);
                 break;
             }
             memcpy(client->buffer + client->buffer_len, recv_buf, n);
             client->buffer_len += n;
             http_header header;
             memset(&header, 0, sizeof(header));
             size_t out_len = BUFFER_SIZE;
             ws_handshake(&header, client->buffer, client->buffer_len, &out_len);
             if (header.type == WS_OPENING_FRAME)
             {
                 send(client->fd, client->buffer, out_len, 0);
                 client->handshake_done = true;
                 //  ws_send_text(client->fd, "Welcome to sensor server");
                 printf("Client FD %d handshake done (Key=%s)\n", client->fd, header.key);
                 client->buffer_len = 0;
             }
             else if (out_len > 0)
             {
                 send(client->fd, client->buffer, out_len, 0);
                 close_client(client);
                 break;
             }
         }
         else
         {
             // For this application, frontend clients only receive data.
         }
     }
 }