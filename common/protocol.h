#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Sensor data structs
typedef enum {
    SENSOR_REED  = 0,
    SENSOR_TEMP  = 1,
    SENSOR_LOAD  = 2,
    SENSOR_SHOCK = 3,
} SensorType;

typedef struct {
    SensorType sensor;
    float      value;
} TSensorData;

#endif /* PROTOCOL_H */
