/*
 * rgb.h
 *
 *  Created on: 8 Apr 2026
 *      Author: gordon
 */

#ifndef RGB_H_
#define RGB_H_

#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

void initRGB(void);
void setRGB(uint8_t r, uint8_t g, uint8_t b);

#endif /* RGB_H_ */
