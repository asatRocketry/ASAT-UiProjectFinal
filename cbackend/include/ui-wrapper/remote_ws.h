#ifndef REMOTE_WS_H
#define REMOTE_WS_H

#include "common_ws.h" // for sensor_data_t, BUFFER_SIZE
#include "config.h"

#define PIPELINE_BATCH_SIZE 100

int connect_remote_ws(const char *ip, int port);
void handle_remote_ws_read();
void set_sensor_warning(sensor_data_t *sd);

// New additions
int initialize_csv_logging();
double apply_sensor_calculations(const char *sensor_name, double raw_value);

#endif // REMOTE_WS_H
