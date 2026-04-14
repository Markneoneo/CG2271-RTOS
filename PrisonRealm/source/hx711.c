/*
 * hx711.c
 *
 *  Created on: 8 Apr 2026
 *      Author: gordon
 */

#include "hx711.h"

#define HX711_DOUT_PIN  0 // PTB 0
#define HX711_SCK_PIN   1 // PTB 1
static const int32_t HX711_OFFSET = 532166;
static const int32_t HX711_SCALE  = 368;

void initHX711() {
	// DOUT is HIGH when data is not ready.
	// SCK should be set to LOW. When DOUT goes low, pulse SCK 25 times to read in 24 bits.
	// DOUT will go back HIGH on the 25th pulse.
	SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;

	// Set as GPIO
	PORTB->PCR[HX711_DOUT_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTB->PCR[HX711_DOUT_PIN] |= PORT_PCR_MUX(1);
	PORTB->PCR[HX711_SCK_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTB->PCR[HX711_SCK_PIN] |= PORT_PCR_MUX(1);

	// Set DOUT input, SCK output
	GPIOB->PDDR &= ~(1 << HX711_DOUT_PIN);
	GPIOB->PDDR |= (1 << HX711_SCK_PIN);

	// Set SCK LOW
	GPIOB->PCOR |= (1 << HX711_SCK_PIN);
}

bool hx711IsReady(void) {
	// Returns true if DOUT reads 0
	return !(GPIOB->PDIR & (1 << HX711_DOUT_PIN));
}

static inline void hx711SCKHigh(void) {
	// drive SCK pin high
	GPIOB->PSOR |= (1 << HX711_SCK_PIN);
}

static inline void hx711SCKLow(void) {
	// drive SCK pin low
	GPIOB->PCOR |= (1 << HX711_SCK_PIN);
}

static int hx711ReadBit(void) {
	int bit;

	hx711SCKHigh();

	bit = (GPIOB->PDIR & (1 << HX711_DOUT_PIN)) ? 1 : 0;

	hx711SCKLow();

	return bit;
}

int32_t hx711ReadData(void) {
	uint32_t raw = 0;

	for (int i = 0; i < 24; i++) {
		raw <<= 1;
		raw |= hx711ReadBit();
	}

	// one extra pulse for gain/channel selection of 128 (default)
	hx711SCKHigh();
	hx711SCKLow();

	// HX711 returns 24 bit in 2s complement, so fill with 1s as necessary
	if (raw & 0x800000) {
		raw |= 0xFF000000;
	}

	return (HX711_OFFSET - (int32_t) raw) / HX711_SCALE;
}
