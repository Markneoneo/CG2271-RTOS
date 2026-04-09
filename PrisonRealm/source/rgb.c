/*
 * rgb.c
 *
 *  Created on: 8 Apr 2026
 *      Author: gordon
 */

#include "rgb.h"

#define RED_PIN  30 // PTE30, TPM0-CH3
#define GREEN_PIN 2 // PTB2,  TPM2-CH0
#define BLUE_PIN  3 // PTB3,  TPM2-CH1

#define PWM_MOD 1000

void initRGB(void) {
	// Clock gating
	SIM->SCGC6 |= SIM_SCGC6_TPM0_MASK;
	SIM->SCGC6 |= SIM_SCGC6_TPM2_MASK;

	// TPM clock source of 8MHz (MCGIRCLK) already selected in initServo(), done again
	 SIM->SOPT2 &= ~SIM_SOPT2_TPMSRC_MASK;
	 SIM->SOPT2 |= SIM_SOPT2_TPMSRC(0b11);

	// Turn off TPM0,2, clear prescalar bits
	// Disable centre-aligned PWM
	TPM0->SC &= ~(TPM_SC_CMOD_MASK | TPM_SC_PS_MASK);
	TPM0->SC &= ~TPM_SC_CPWMS_MASK;
	TPM2->SC &= ~(TPM_SC_CMOD_MASK | TPM_SC_PS_MASK);
	TPM2->SC &= ~TPM_SC_CPWMS_MASK;

	// Initialise count to 0
	// Set prescalar to 8
	TPM0->CNT = 0;
	TPM0->SC |= TPM_SC_PS(0b011);
	TPM2->CNT = 0;
	TPM2->SC |= TPM_SC_PS(0b011);

	// Desired frequency of 1kHz
	// Since we are using edge-aligned, MOD = (8MHz * T) / (PS) = 1000
	TPM0->MOD = PWM_MOD;
	TPM2->MOD = PWM_MOD;

	// clock gating
	SIM->SCGC5 |= SIM_SCGC5_PORTE_MASK;
	SIM->SCGC5 |= SIM_SCGC5_PORTB_MASK;

	// set pin multiplexors
	PORTE->PCR[RED_PIN] = PORT_PCR_MUX(3); // TPM0_CH3
	PORTB->PCR[GREEN_PIN]  = PORT_PCR_MUX(3); // TPM2_CH0
	PORTB->PCR[BLUE_PIN]  = PORT_PCR_MUX(3); // TPM2_CH1

	// set to output
	GPIOE->PDDR |= (1 << RED_PIN);
	GPIOB->PDDR |= (1 << GREEN_PIN);
	GPIOB->PDDR |= (1 << BLUE_PIN);

	// MS=10, ELS=10 (edge-aligned, clear on match)
	TPM0->CONTROLS[3].CnSC &= ~(TPM_CnSC_MSA_MASK |
	                            TPM_CnSC_MSB_MASK |
	                            TPM_CnSC_ELSA_MASK |
	                            TPM_CnSC_ELSB_MASK);
	TPM0->CONTROLS[3].CnSC |= (TPM_CnSC_MSB(1) | TPM_CnSC_ELSB(1));
	TPM2->CONTROLS[0].CnSC &= ~(TPM_CnSC_MSA_MASK |
	                            TPM_CnSC_MSB_MASK |
	                            TPM_CnSC_ELSA_MASK |
	                            TPM_CnSC_ELSB_MASK);
	TPM2->CONTROLS[0].CnSC |= (TPM_CnSC_MSB(1) | TPM_CnSC_ELSB(1));
	TPM2->CONTROLS[1].CnSC &= ~(TPM_CnSC_MSA_MASK |
	                            TPM_CnSC_MSB_MASK |
	                            TPM_CnSC_ELSA_MASK |
	                            TPM_CnSC_ELSB_MASK);
	TPM2->CONTROLS[1].CnSC |= (TPM_CnSC_MSB(1) | TPM_CnSC_ELSB(1));

	// Initialise OFF
	TPM0->CONTROLS[3].CnV = 0;
	TPM2->CONTROLS[0].CnV = 0;
	TPM2->CONTROLS[1].CnV = 0;

	// Start timers
	TPM0->SC |= TPM_SC_CMOD(1);
	TPM2->SC |= TPM_SC_CMOD(1);

}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
	TPM0->CONTROLS[3].CnV = r * PWM_MOD / 255;
	TPM2->CONTROLS[0].CnV = g * PWM_MOD / 255;
	TPM2->CONTROLS[1].CnV = b * PWM_MOD / 255;
}
