/*
 * servo.h
 *
 *  Created on: 2 Apr 2026
 *      Author: gordon
 */

#ifndef SERVO_H_
#define SERVO_H_

#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

void initServo(void);
void servoLock(void);
void servoUnlock(void);

#endif /* SERVO_H_ */
