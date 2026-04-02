/*
 * servo.c
 *
 *  Created on: 2 Apr 2026
 *      Author: gordon
 */

#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "fsl_debug_console.h"

#define SERVO_PIN        21    // PTE21 for PWM, TPM1-CH1
#define SERVO_PERIOD_US  20000
#define SERVO_MIN_US     100
#define SERVO_CENTER_US  1500
#define SERVO_MAX_US     3500

void setMCGIRClk(void) { // 8MHz
	// 1. Choose LIRC, enable IRC source for low freq
	MCG->C1 &= ~MCG_C1_CLKS_MASK; // 0 out bits first
	MCG->C1 |= ((MCG_C1_CLKS(0b01) | MCG_C1_IRCLKEN_MASK));

	// 2. Choose 8M by setting to 1
	MCG->C2 |= MCG_C2_IRCS_MASK;

	// 3. Choose FCRDIV and LIRC_DIV2 to divide 1 by setting to 0
	MCG->SC &= ~MCG_SC_FCRDIV_MASK;
	MCG->SC |= MCG_SC_FCRDIV(0b000);
	MCG->MC &= ~MCG_MC_LIRC_DIV2_MASK;
	MCG->MC |= MCG_MC_LIRC_DIV2(0b000);
}

void initServo(void) {
	setMCGIRClk();

	// Turn on clock gating
	SIM->SCGC6 |= SIM_SCGC6_TPM1_MASK;

	// Set TPM clock source to MCGIRCLK
	SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
	SIM->SOPT2 |= SIM_SOPT2_TPMSRC(0b11);

	// Turn off TPM1, clear prescalar bits
	TPM1->SC &= ~(TPM_SC_CMOD_MASK | TPM_SC_PS_MASK);
	// Disable centre-aligned PWM
	TPM1->SC &= ~TPM_SC_CPWMS_MASK;

	// Initialise count to 0
	TPM1->CNT = 0;

	// Set prescalar to 8 (1 count = 1 us)
	TPM1->SC |= TPM_SC_PS(0b011);

	// T = 20ms
	// Since we are using edge-aligned, MOD = (8MHz * T) / (PS)
	TPM1->MOD = SERVO_PERIOD_US;

	// clock gating
	SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
	// set pin multiplexors
	PORTE->PCR[SERVO_PIN] &= ~PORT_PCR_MUX_MASK;
	PORTE->PCR[SERVO_PIN] |= PORT_PCR_MUX(0b11);

	// set to output
	GPIOE->PDDR |= (1 << SERVO_PIN);

	// MS=10, ELS=10 (edge-aligned, clear on match)
	TPM1->CONTROLS[1].CnSC &= ~(TPM_CnSC_MSA_MASK |
	                            TPM_CnSC_MSB_MASK |
	                            TPM_CnSC_ELSA_MASK |
	                            TPM_CnSC_ELSB_MASK);
	TPM1->CONTROLS[1].CnSC |= (TPM_CnSC_MSB(1) | TPM_CnSC_ELSB(1));

	// Set centre position
	TPM1->CONTROLS[1].CnV = SERVO_CENTER_US;

	// Start timer
	TPM1->SC |= TPM_SC_CMOD(0b1);

}

void setServoUs(uint16_t pulse_us) {
	if (pulse_us < SERVO_MIN_US) {
		pulse_us = SERVO_MIN_US;
	}
	if (pulse_us > SERVO_MAX_US) {
		pulse_us = SERVO_MAX_US;
	}

	TPM1->CONTROLS[1].CnV = pulse_us;
}
