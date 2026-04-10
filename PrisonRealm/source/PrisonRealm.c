/*
 * Copyright 2016-2025 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause=====
 */

/**
 * @file    PrisonRealm.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"
#include <semphr.h>

#include "servo.h"
#include "rgb.h"
#include "hx711.h"

#include "../../common/protocol.h"

static SystemState systemState;
SemaphoreHandle_t systemStateMutex;
// systemState contains lockState and doorState.
// Needs to be mutexed since reed and UART write to it, and LED / ESP32 reads.

/* UART */
#define BAUD_RATE 115200
#define UART_TX_PTE22 	22
#define UART_RX_PTE23 	23
#define UART2_INT_PRIO	128

#define MAX_MSG_LEN		64
char send_buffer[MAX_MSG_LEN];

/* Sensors */

// Reed Sensor and Buzzer
#define REED_PIN        12 // PTA12
#define BUZZER_PIN      13 // PTA13
#define DOOR_OPEN       0
#define DOOR_CLOSED     1
#define TIMER_DELAY     2000 // 2s or 2000 ms

// Shock Sensor
#define SHOCK_PIN       2 // PTD 2

#define SWITCH_PIN 3 // SW2, PTC3
#define QLEN	5

typedef struct tm {
	char message[MAX_MSG_LEN];
} TMessage;

QueueHandle_t queue;
QueueHandle_t sensorDataQueue;
QueueHandle_t msgQueue; // UART Out

TaskHandle_t reedTaskHandle = NULL;
TaskHandle_t servoTaskHandle = NULL;
TaskHandle_t sendStateTaskHandle = NULL;
TaskHandle_t shockTaskHandle = NULL;

TimerHandle_t reedDebounceTimer;
TimerHandle_t reedAlarmTimer;

TimerHandle_t shockDebounceTimer;

SemaphoreHandle_t alarmTriggered;

void initUART2(uint32_t baud_rate) {
	NVIC_DisableIRQ(UART2_FLEXIO_IRQn);

	//enable clock to UART2 and PORTE
	SIM->SCGC4 |= SIM_SCGC4_UART2_MASK;
	SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;

	//Ensure Tx and Rx are disabled before configuration
	UART2->C2 &= ~((UART_C2_TE_MASK) | (UART_C2_RE_MASK));

	//connect UART pins for PTE22, PTE23
	PORTE->PCR[UART_TX_PTE22] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[UART_TX_PTE22] |= PORT_PCR_MUX(4);

	PORTE->PCR[UART_RX_PTE23] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[UART_RX_PTE23] |= PORT_PCR_MUX(4);

	// Set the baud rate
	uint32_t bus_clk = CLOCK_GetBusClkFreq();

	// This version of sbr does integer rounding.
	uint32_t sbr = (bus_clk + (baud_rate * 8)) / (baud_rate * 16);

	// Set SBR. Bits 8 to 12 in BDH, 0-7 in BDL.
	// MUST SET BDH FIRST!
	UART2->BDH &= ~UART_BDH_SBR_MASK;
	UART2->BDH |= ((sbr >> 8) & UART_BDH_SBR_MASK);
	UART2->BDL = (uint8_t) (sbr & 0xFF);

	// Disable loop mode
	UART2->C1 &= ~UART_C1_LOOPS_MASK;
	UART2->C1 &= ~UART_C1_RSRC_MASK;

	// Disable parity
	UART2->C1 &= ~UART_C1_PE_MASK;

	// 8-bit mode
	UART2->C1 &= ~UART_C1_M_MASK;

	//Enable RX interrupt
	UART2->C2 |= UART_C2_RIE_MASK;

	// Enable the receiver
	UART2->C2 |= UART_C2_RE_MASK;

	NVIC_SetPriority(UART2_FLEXIO_IRQn, UART2_INT_PRIO);
	NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
	NVIC_EnableIRQ(UART2_FLEXIO_IRQn);

}

void UART2_FLEXIO_IRQHandler(void) {
	// Send and receive pointers
	static int recv_ptr = 0, send_ptr = 0;
	char rx_data;
	static char recv_buffer[MAX_MSG_LEN];

//NVIC_ClearPendingIRQ(UART2_FLEXIO_IRQn);
	if (UART2->S1 & UART_S1_TDRE_MASK) // Send data
	{
		if (send_buffer[send_ptr] == '\0') {
			send_ptr = 0;

			// Disable the transmit interrupt
			UART2->C2 &= ~UART_C2_TIE_MASK;

			// Disable the transmitter
			UART2->C2 &= ~UART_C2_TE_MASK;
		} else {
			UART2->D = send_buffer[send_ptr++];
		}
	}

	if (UART2->S1 & UART_S1_RDRF_MASK) {
		TMessage msg;
		rx_data = UART2->D;
		recv_buffer[recv_ptr++] = rx_data;
		if (rx_data == '\n') {
			// Copy over the string
			BaseType_t hpw = pdFALSE;
			recv_buffer[recv_ptr] = '\0';
			strncpy(msg.message, recv_buffer, MAX_MSG_LEN);
			xQueueSendFromISR(queue, (void* )&msg, &hpw);
			portYIELD_FROM_ISR(hpw);
			recv_ptr = 0;
		}
	}
}

void setDoorState(DoorState door) {
	xSemaphoreTake(systemStateMutex, portMAX_DELAY);
	systemState.door = door;
	xSemaphoreGive(systemStateMutex);

	xTaskNotifyGive(sendStateTaskHandle); // Send changed state to ESP32
}
void setLockState(LockState lock) {
	xSemaphoreTake(systemStateMutex, portMAX_DELAY);
	systemState.lock = lock;
	xSemaphoreGive(systemStateMutex);

	xTaskNotifyGive(sendStateTaskHandle);
	xTaskNotifyGive(servoTaskHandle);     // Fire servo based on desired state
}
void setAlarmState(AlarmState alarm) {
	xSemaphoreTake(systemStateMutex, portMAX_DELAY);
	systemState.alarm = alarm;
	xSemaphoreGive(systemStateMutex);

	xTaskNotifyGive(sendStateTaskHandle);
}

void initReed() {
	NVIC_DisableIRQ(PORTA_IRQn);
	SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

	// Enable pull-up
	PORTA->PCR[REED_PIN] &= ~PORT_PCR_PS_MASK;
	PORTA->PCR[REED_PIN] |= PORT_PCR_PS(1);
	PORTA->PCR[REED_PIN] &= ~PORT_PCR_PE_MASK;
	PORTA->PCR[REED_PIN] |= PORT_PCR_PE(1);

	// Set as GPIO
	PORTA->PCR[REED_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTA->PCR[REED_PIN] |= PORT_PCR_MUX(1);

	// Set as input
	GPIOA->PDDR &= ~(1 << REED_PIN);

	// Enable interrupt on BOTH edges
	PORTA->PCR[REED_PIN] &= ~PORT_PCR_IRQC_MASK;
	PORTA->PCR[REED_PIN] |= PORT_PCR_IRQC(0b1011);

	NVIC_SetPriority(PORTA_IRQn, 192);
	NVIC_ClearPendingIRQ(PORTA_IRQn);
	NVIC_EnableIRQ(PORTA_IRQn);
}

void initBuzzer() {
	SIM->SCGC5 |= SIM_SCGC5_PORTA_MASK;

	// Set as GPIO
	PORTA->PCR[BUZZER_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTA->PCR[BUZZER_PIN] |= PORT_PCR_MUX(1);

	// Set as output
	GPIOA->PDDR |= (1 << BUZZER_PIN);
}

void initShock() {
	NVIC_DisableIRQ(PORTC_PORTD_IRQn);
	SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;

	// Enable pull-down
	PORTD->PCR[SHOCK_PIN] &= ~PORT_PCR_PS_MASK;
	PORTD->PCR[SHOCK_PIN] |= PORT_PCR_PS(0);
	PORTD->PCR[SHOCK_PIN] &= ~PORT_PCR_PE_MASK;
	PORTD->PCR[SHOCK_PIN] |= PORT_PCR_PE(1);

	// Set as GPIO
	PORTD->PCR[SHOCK_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTD->PCR[SHOCK_PIN] |= PORT_PCR_MUX(1);

	// Set as input
	GPIOD->PDDR &= ~(1 << SHOCK_PIN);

	// Enable interrupt on rising edge
	PORTD->PCR[SHOCK_PIN] &= ~PORT_PCR_IRQC_MASK;
	PORTD->PCR[SHOCK_PIN] |= PORT_PCR_IRQC(0b1001);

	NVIC_SetPriority(PORTC_PORTD_IRQn, 192);
	NVIC_ClearPendingIRQ(PORTC_PORTD_IRQn);

	NVIC_EnableIRQ(PORTC_PORTD_IRQn);
}

void hx711Task(void *p) {
	while (1) {
		TSensorData hx711Data;
		hx711Data.sensor = SENSOR_LOAD;
		int32_t sum = 0;

		for (int i = 0; i < HX711_N; i++) {
			while (!hx711IsReady()) {
				vTaskDelay(pdMS_TO_TICKS(5));
			}

			sum += hx711ReadData();
		}
		hx711Data.value = sum / HX711_N;
		xQueueSend(sensorDataQueue, &hx711Data, portMAX_DELAY);
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

void PORTA_IRQHandler() {
	NVIC_ClearPendingIRQ(PORTA_IRQn);
	BaseType_t hpw = pdFALSE;

	if (PORTA->ISFR & (1 << REED_PIN)) {
		PORTA->ISFR = (1 << REED_PIN);
		xTimerStartFromISR(reedDebounceTimer, &hpw);
	}

	portYIELD_FROM_ISR(hpw);
}

void PORTC_PORTD_IRQHandler() {
	NVIC_ClearPendingIRQ(PORTC_PORTD_IRQn);
	BaseType_t hpw = pdFALSE;

	if (PORTD->ISFR & (1 << SHOCK_PIN)) {
		PORTD->ISFR = (1 << SHOCK_PIN);
		xTimerStartFromISR(shockDebounceTimer, &hpw);
	}

	portYIELD_FROM_ISR(hpw);
}

void reedDebouncedCallback(TimerHandle_t xTimer) {
	uint32_t event;
	// Check if rising or falling edge
	if (GPIOA->PDIR & (1 << REED_PIN)) {
		// Pin is 1, rising edge
		event = DOOR_CLOSED;
	} else {
		// Falling edge
		event = DOOR_OPEN;
	}
	xTaskNotify(reedTaskHandle, event, eSetValueWithOverwrite);
}

static void reedTask(void *p) {
	while (1) {
		uint32_t event;

		// Clear all bits on exit
		xTaskNotifyWait(0, 0xFFFFFFFF, &event, portMAX_DELAY);
		TSensorData reedData;
		reedData.sensor = SENSOR_REED;

		setDoorState((event == DOOR_CLOSED) ? CLOSED : OPEN);

		// Actions
		switch (event) {
		case DOOR_CLOSED:
			setAlarmState(ALARM_INACTIVE);
			xTimerStop(reedAlarmTimer, 0);
			GPIOA->PCOR |= (1 << BUZZER_PIN);
			reedData.value = DOOR_CLOSED;
			break;
		case DOOR_OPEN:
			xTimerStart(reedAlarmTimer, 0);
			reedData.value = DOOR_OPEN;
			break;
		default:
			break;
		}

		xQueueSend(sensorDataQueue, &reedData, portMAX_DELAY);
	}
}

void reedAlarmCallback(TimerHandle_t xTimer) {
	xSemaphoreGive(alarmTriggered);
}

static void alarmTask(void *p) {
	while (1) {
		xSemaphoreTake(alarmTriggered, portMAX_DELAY);
		setAlarmState(ALARM_ACTIVE);
		PRINTF("Timer has gone off\r\n");
		GPIOA->PSOR |= (1 << BUZZER_PIN);
	}
}

void shockDebouncedCallback(TimerHandle_t xTimer) {
	if (shockTaskHandle != NULL)
		xTaskNotifyGive(shockTaskHandle);
}

static void shockTask(void *p) {
	TSensorData shockData;
	shockData.sensor = SENSOR_SHOCK;
	shockData.value = 1;

	while (1) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		//PRINTF("Shock triggered!\r\n");
		xQueueSend(sensorDataQueue, &shockData, portMAX_DELAY);
	}
}

static void sendSensorDataTask(void *p) {
	while (1) {
		TSensorData sensorData;
		xQueueReceive(sensorDataQueue, &sensorData, portMAX_DELAY);
		TMessage msg;
		snprintf(msg.message, MAX_MSG_LEN,
				"{\"type\":\"sensor\", \"sensor\":%d, \"value\":%d}\r\n",
				(int32_t) sensorData.sensor, (uint32_t) sensorData.value);
		PRINTF("Sending message: %s", msg.message);
		xQueueSend(msgQueue, &msg, portMAX_DELAY);
	}
}

void sendMessage(char *message) {
	strncpy(send_buffer, message, MAX_MSG_LEN);

	// Enable the TIE interrupt
	UART2->C2 |= UART_C2_TIE_MASK;

	// Enable the transmitter
	UART2->C2 |= UART_C2_TE_MASK;
}

static void recvTask(void *p) {
	while (1) {
		TMessage msg;
		xQueueReceive(queue, (TMessage*) &msg, portMAX_DELAY);
		PRINTF("Received command: %s\r\n", msg.message);

		// Parse command JSON from ESP32-S3.
		// Format: {"command":"lock"}, {"command":"unlock"}, {"command":"silence"}
		// Check "unlock" before "lock" — "unlock" contains the substring "lock".
		if (strstr(msg.message, "\"unlock\"")) {
			PRINTF("[CMD] Unlock received.\r\n");
			setLockState(UNLOCKED);
		} else if (strstr(msg.message, "\"lock\"")) {
			PRINTF("[CMD] Lock received.\r\n");
			setLockState(LOCKED);
		} else if (strstr(msg.message, "\"silence\"")) {
			PRINTF("[CMD] Silence received.\r\n");
			setAlarmState(ALARM_INACTIVE);
			GPIOA->PCOR |= (1 << BUZZER_PIN);
		}
	}
}

static void sendTask(void *p) {
	while (1) {
		TMessage msg;
		xQueueReceive(msgQueue, &msg, portMAX_DELAY);
		sendMessage(msg.message);
	}
}

static void servoTask(void *p) {
	while (1) {
		// Wait until something changes
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		SystemState s;

		xSemaphoreTake(systemStateMutex, portMAX_DELAY);
		s = systemState;
		xSemaphoreGive(systemStateMutex);

		if (s.lock == LOCKED)
			servoLock();
		else
			servoUnlock();
	}
}

static void ledTask(void *p) {
	bool blink = 0;

	while (1) {
		blink = !blink; // for toggling

	    SystemState s;

	    xSemaphoreTake(systemStateMutex, portMAX_DELAY);
	    s = systemState;
	    xSemaphoreGive(systemStateMutex);

	    // LED behavior:
	    // door CLOSED, lock   LOCKED: GREEN
	    // door CLOSED, lock UNLOCKED: YELLOW
	    // door   OPEN, lock   LOCKED: ORANGE (BLINKING AFTER 2S, SYNCED TO ALARM)
	    // door   OPEN, lock UNLOCKED:    RED (BLINKING AFTER 2S, SYNCED TO ALARM)
	    if (s.door == CLOSED && s.lock == LOCKED) {
	        setRGB(0,255,0); // green
	    }
	    else if (s.door == CLOSED && s.lock == UNLOCKED) {
	        setRGB(255,180,0); // yellow
	    }
	    else if (s.door == OPEN && s.lock == LOCKED) {
	    	if ((s.alarm == ALARM_ACTIVE && blink) || s.alarm == ALARM_INACTIVE)
	            setRGB(255,50,0);
	        else
	            setRGB(0,0,0);
	    }
	    else if (s.door == OPEN && s.lock == UNLOCKED) {
	    	if ((s.alarm == ALARM_ACTIVE && blink) || s.alarm == ALARM_INACTIVE)
	            setRGB(255,0,0);
	        else
	            setRGB(0,0,0);
	    }

	    vTaskDelay(pdMS_TO_TICKS(200));
	}
}

// Sends system state (door = CLOSED/OPEN, lock = LOCKED/UNLOCKED)
static void sendStateTask(void *p) {
	while (1) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		SystemState s;

		xSemaphoreTake(systemStateMutex, portMAX_DELAY);
		s = systemState;
		xSemaphoreGive(systemStateMutex);
		TMessage msg;
		snprintf(msg.message, MAX_MSG_LEN,
				"{\"type\":\"state\",\"door\":%d,\"lock\":%d,\"alarm\":%d}\r\n",
				s.door, s.lock, s.alarm);
		PRINTF("Sending message: %s", msg.message);
		xQueueSend(msgQueue, &msg, portMAX_DELAY);
	}
}

static void initSystemState() {
	systemState.door = OPEN;
	systemState.lock = UNLOCKED;
	systemState.alarm = ALARM_INACTIVE;
	systemStateMutex = xSemaphoreCreateMutex();
}



static void servoTestTask(void *p) {

	static bool servoTestLockState = false; // local toggle state
    while (1) {
        servoTestLockState = !servoTestLockState;
        PRINTF("Toggling servo lock state.\r\n");
        setLockState(servoTestLockState ? LOCKED : UNLOCKED);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void initSW2(void) {
    NVIC_DisableIRQ(PORTC_PORTD_IRQn);
    SIM->SCGC5 |= SIM_SCGC5_PORTC_MASK;

    PORTC->PCR[SWITCH_PIN] &= ~PORT_PCR_MUX_MASK;
    PORTC->PCR[SWITCH_PIN] |=  PORT_PCR_MUX(1);

    // Enable internal pull-up
    PORTC->PCR[SWITCH_PIN] |= PORT_PCR_PE_MASK | PORT_PCR_PS_MASK;

    GPIOC->PDDR &= ~(1 << SWITCH_PIN);

    // Falling edge (active-low button)
    PORTC->PCR[SWITCH_PIN] &= ~PORT_PCR_IRQC_MASK;
    PORTC->PCR[SWITCH_PIN] |=  PORT_PCR_IRQC(0b1010);

    NVIC_SetPriority(PORTC_PORTD_IRQn, 192);
    NVIC_ClearPendingIRQ(PORTC_PORTD_IRQn);
    NVIC_EnableIRQ(PORTC_PORTD_IRQn);
}

static void rgbTestTask(void *p) {
    SystemState testState;
    uint8_t step = 0;

    while (1) {
        switch (step % 4) {
            case 0: // Door CLOSED, Lock LOCKED, Alarm inactive
                testState.door  = CLOSED;
                testState.lock  = LOCKED;
                testState.alarm = ALARM_INACTIVE;
                PRINTF("Should see GREEN\r\n");
                break;
            case 1: // Door CLOSED, Lock UNLOCKED, Alarm inactive
                testState.door  = CLOSED;
                testState.lock  = UNLOCKED;
                testState.alarm = ALARM_INACTIVE;
                PRINTF("Should see YELLOW\r\n");
                break;
            case 2: // Door OPEN, Lock LOCKED, Alarm active (blinking)
                testState.door  = OPEN;
                testState.lock  = LOCKED;
                testState.alarm = ALARM_ACTIVE;
                PRINTF("Should see BLINKING ORANGE\r\n");
                break;
            case 3: // Door OPEN, Lock UNLOCKED, Alarm active (blinking)
                testState.door  = OPEN;
                testState.lock  = UNLOCKED;
                testState.alarm = ALARM_ACTIVE;
                PRINTF("Should see BLINKING RED\r\n");
                break;
        }

        // Update the global systemState with mutex protection
        xSemaphoreTake(systemStateMutex, portMAX_DELAY);
        systemState = testState;
        xSemaphoreGive(systemStateMutex);

        step++;
        vTaskDelay(pdMS_TO_TICKS(2000)); // 5s per state
    }
}

/*
 * @brief   Application entry point.
 */
int main(void) {

	/* Init board hardware. */
	BOARD_InitBootPins();
	BOARD_InitBootClocks();
	BOARD_InitBootPeripherals();
#ifndef BOARD_INIT_DEBUG_CONSOLE_PERIPHERAL
	/* Init FSL debug console. */
	BOARD_InitDebugConsole();
#endif

	initUART2(BAUD_RATE);
	initHX711();
	initReed();
	initServo();
	initBuzzer();
	initShock();
	initSystemState();
	initRGB();

	queue = xQueueCreate(QLEN, sizeof(TMessage));
	msgQueue = xQueueCreate(QLEN, sizeof(TMessage));
	sensorDataQueue = xQueueCreate(QLEN, sizeof(TSensorData));

	reedDebounceTimer = xTimerCreate("Debounce Timer", pdMS_TO_TICKS(50),
	pdFALSE,
	NULL, reedDebouncedCallback);
	reedAlarmTimer = xTimerCreate("Alarm Timer", pdMS_TO_TICKS(TIMER_DELAY),
	pdFALSE, NULL, reedAlarmCallback);
	shockDebounceTimer = xTimerCreate("Shock Debounce", pdMS_TO_TICKS(50),
	pdFALSE, NULL, shockDebouncedCallback);
	alarmTriggered = xSemaphoreCreateBinary();

	xTaskCreate(recvTask, "recvTask", configMINIMAL_STACK_SIZE + 100, NULL, 2,
	NULL);
	xTaskCreate(sendTask, "sendTask", configMINIMAL_STACK_SIZE + 100, NULL, 1,
	NULL);
	xTaskCreate(reedTask, "reedTask", configMINIMAL_STACK_SIZE + 50, NULL, 2,
		&reedTaskHandle);
	xTaskCreate(alarmTask, "alarmTask", configMINIMAL_STACK_SIZE + 50, NULL, 3,
	NULL);
	xTaskCreate(hx711Task, "hx711Task", configMINIMAL_STACK_SIZE + 100, NULL, 1,
	NULL);
	xTaskCreate(servoTask, "servoTask", configMINIMAL_STACK_SIZE + 50, NULL, 1,
		&servoTaskHandle);

	xTaskCreate(shockTask, "shockTask", configMINIMAL_STACK_SIZE + 100, NULL, 1,
		&shockTaskHandle);
	xTaskCreate(sendStateTask, "sendStateTask", configMINIMAL_STACK_SIZE + 100, NULL, 1,
		&sendStateTaskHandle);
	xTaskCreate(ledTask, "ledTask", configMINIMAL_STACK_SIZE + 100, NULL, 1,
	NULL);
	xTaskCreate(sendSensorDataTask, "sendSensorDataTask", configMINIMAL_STACK_SIZE + 100,
	NULL, 2, NULL);

	PRINTF("Free heap: %d\r\n", xPortGetFreeHeapSize());
	vTaskStartScheduler();

	/* Force the counter to be placed into memory. */
	volatile static int i = 0;
	/* Enter an infinite loop, just incrementing a counter. */
	while (1) {
		i++;
		/* 'Dummy' NOP to allow source level single stepping of
		 tight while() loop */
		__asm volatile ("nop");
	}
	return 0;
}
