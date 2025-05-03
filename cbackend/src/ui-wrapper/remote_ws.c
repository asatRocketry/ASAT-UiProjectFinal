#define _POSIX_C_SOURCE 200809L


#include "remote_ws.h"
#include "common_ws.h"
#include "frontend_ws.h" // for broadcast_sensor_data()

#include "websocket.h"
#include "cJSON.h"
#include "hiredis.h"
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>



uint64_t last_broadcast_time = 0;

FILE* csv_file = NULL;
char csv_filename[256] = {0};

int initialize_csv_logging() {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(csv_filename, sizeof(csv_filename), "sensor_log_%Y%m%d_%H%M%S.csv", t);
    csv_file = fopen(csv_filename, "w");
    if (csv_file) {
        fprintf(csv_file, "timestamp,sensor_name,value\n");
        fflush(csv_file);
    } else {
        perror("Failed to create CSV file");
    }
}

double apply_sensor_calculations(const char* sensor_name, double raw_value) {
    // Placeholder for future logic per sensor
    return raw_value;
}

// Connect to the remote WebSocket sensor data server.
int connect_remote_ws(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket() remote");
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton()");
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect() remote");
        close(fd);
        return -1;
    }
    printf("Connected to remote WebSocket server at %s:%d\n", ip, port);
    return fd;
}

/*-------------------- Sensor Data Handling --------------------*/
static void parse_sensor_data(const char *data) {
    cJSON *root = cJSON_Parse(data);
    if (!root || !cJSON_IsArray(root)) {
        printf("Error parsing JSON sensor data: %s\n", data);
        if (root)
            cJSON_Delete(root);
        return;
    }

    int array_size = cJSON_GetArraySize(root);
    for (int i = 0; i < array_size; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item))
            continue;
        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *value = cJSON_GetObjectItem(item, "value");
        cJSON *timestamp_json = cJSON_GetObjectItem(item, "timestamp");
        if (!title || !value)
            continue;
        if (!cJSON_IsString(title) || (!(cJSON_IsNumber(value) || cJSON_IsString(value))))
            continue;

        if (sensor_buffer_count < SENSOR_BUFFER_MAX) {
            sensor_data_t *sd = &sensor_buffer[sensor_buffer_count++];
            strncpy(sd->name, title->valuestring, sizeof(sd->name) - 1);
            sd->name[sizeof(sd->name) - 1] = '\0';

            double raw_value = cJSON_IsNumber(value) ? value->valuedouble : atof(value->valuestring);
            double processed_value = apply_sensor_calculations(title->valuestring, raw_value);
            sd->value = processed_value;

            if (timestamp_json && cJSON_IsNumber(timestamp_json))
                sd->timestamp = (uint64_t)timestamp_json->valuedouble;
            else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                sd->timestamp = (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
            }

            set_sensor_warning(sd);

            // CSV Logging
            if (csv_file) {
                fprintf(csv_file, "%" PRIu64 ",%s,%f\n", sd->timestamp, sd->name, sd->value);
                fflush(csv_file);
            }

            if (redis_ctx) {
                static int pipeline_count = 0;
                redisAppendCommand(redis_ctx, "TS.ADD %s %llu %f",
                                   sd->name, (unsigned long long)sd->timestamp, sd->value);
                pipeline_count++;
                if (pipeline_count >= PIPELINE_BATCH_SIZE) {
                    redisReply *reply;
                    for (int i = 0; i < pipeline_count; i++) {
                        if (redisGetReply(redis_ctx, (void **)&reply) == REDIS_ERR) {
                            printf("Redis error executing pipelined command\n");
                        }
                        if (reply)
                            freeReplyObject(reply);
                    }
                    pipeline_count = 0;
                }
            }
        }
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t current_time = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if ((current_time - last_broadcast_time) >= 100) {
        memcpy(latest_sensor_buffer, sensor_buffer, sizeof(sensor_data_t) * sensor_buffer_count);
        latest_sensor_buffer_count = sensor_buffer_count;
        broadcast_sensor_data();
        last_broadcast_time = current_time;
    }

    memset(sensor_buffer, 0, sizeof(sensor_data_t) * sensor_buffer_count);
    sensor_buffer_count = 0;

    cJSON_Delete(root);
}

void set_sensor_warning(sensor_data_t *sd) {
    if (!sd || sd->name[0] == '\0') return;
    uint64_t val = (uint64_t)(sd->value + 0.5);

    switch (sd->name[0]) {
        case 'E':
            if (sd->name[1] == '-' && sd->name[2] == 'T' && sd->name[3] == 'C') {
                if (sd->name[4] >= '1' && sd->name[4] <= '8') {}
            } else if (sd->name[1] == '-' && sd->name[2] == 'R' && sd->name[3] == 'T' && sd->name[4] == 'D') {
                switch (sd->name[5]) {
                    case '1': case '2': break;
                }
            }
            break;
        case 'P':
            if (sd->name[1] == 'T' && sd->name[2] == '-') {
                switch (sd->name[3]) {
                    case 'M':
                        switch (sd->name[4]) {
                            case '1': case '2':
                                if (val <= 51) sd->warning = 0;
                                else if (val <= 65) sd->warning = 1;
                                else if (val <= 100) sd->warning = 2;
                                break;
                        }
                        break;
                    case 'C':
                    case 'E': case 'D': case 'L':
                        if (val <= 51) sd->warning = 0;
                        else if (val <= 65) sd->warning = 1;
                        else if (val <= 100) sd->warning = 2;
                        break;
                    case 'P': case 'F':
                        if (val <= 190) sd->warning = 0;
                        else if (val <= 200) sd->warning = 1;
                        else if (val <= 300) sd->warning = 2;
                        break;
                }
            }
            break;
        case 'L':
            if (sd->name[1] == 'C' && sd->name[2] == '-') {
                switch (sd->name[3]) {
                    case 'L': case 'E': case 'T': break;
                }
            }
            break;
        default:
            break;
    }
}

/*-------------------- Remote WebSocket Handling --------------------*/
void handle_remote_ws_read() {
    uint8_t recv_buf[BUFFER_SIZE];
    ssize_t n = recv(g_remote_fd, recv_buf, sizeof(recv_buf), 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        printf("Remote WS connection lost. Reconnecting in 10 seconds...\n");
        close(g_remote_fd);

        while ((g_remote_fd = connect_remote_ws(REMOTE_WS_IP, REMOTE_WS_PORT)) < 0) {
            fprintf(stderr, "Reconnection attempt failed. Retrying in 10 seconds...\n");
            sleep(1);
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = g_remote_fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, g_remote_fd, &ev) < 0) {
            perror("epoll_ctl(): server_fd");
            return;
        }
        return;
    }

    ws_frame frame;
    memset(&frame, 0, sizeof(frame));
    ws_parse_frame(&frame, recv_buf, n);
    if (frame.type == WS_TEXT_FRAME && frame.payload) {
        char *text = strndup((char *)frame.payload, frame.payload_length);
        parse_sensor_data(text);
        free(text);
    }
}