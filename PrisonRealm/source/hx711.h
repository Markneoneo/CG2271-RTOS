/*
 * hx711.h
 *
 *  Created on: 9 Apr 2026
 *      Author: gordon
 */

#ifndef HX711_H_
#define HX711_H_

#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

#define HX711_N         20 // Averaged readings

void initHX711(void);

bool hx711IsReady(void);
int32_t hx711ReadData(void);

#endif /* HX711_H_ */
