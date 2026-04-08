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

typedef enum {
    LOCKED = 0,
    UNLOCKED
} LockState;

typedef enum {
    CLOSED = 0,
    OPEN
} DoorState;

typedef struct {
    LockState lock;
    DoorState door;
} SystemState;

#endif /* PROTOCOL_H */
