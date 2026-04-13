#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#ifdef CLOSED
#undef CLOSED
#endif
#ifdef OPEN
#undef OPEN
#endif

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
	CMD_LOCK,
	CMD_UNLOCK,
	CMD_INVALID
} CommandType;

typedef enum {
    LOCKED = 0,
    UNLOCKED
} LockState;

typedef enum {
    CLOSED = 0,
    OPEN
} DoorState;

typedef enum {
    ALARM_INACTIVE = 0,
    ALARM_ACTIVE
} AlarmState;

typedef struct {
    LockState lock;
    DoorState door;
    AlarmState alarm;
} SystemState;

#endif /* PROTOCOL_H */
