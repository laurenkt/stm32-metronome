#include "delay.h"
#include "stm32f4xx.h"
#include <stdint.h>

static uint_fast8_t _init = 0;

static void _delay_init(void) {
	TIM14->DIER = 0x00000000;		/* Disable TIM14 interrupts */
	TIM14->PSC = 83;					/* 1us per tick from a 84MHz clock */
	TIM14->EGR = TIM_EGR_UG;		/* Force register update */
	RCC->APB1ENR |= RCC_APB1ENR_TIM14EN;	/* Enable TIM14 clock */
}

void delay_us(uint16_t us) {
	if (!_init) _delay_init();
	TIM14->CNT = 0;							/* Reset TIM14 */
	TIM14->CR1 = TIM_CR1_CEN;		/* Start TIM14, source CLK_INT */
	while (TIM14->CNT < us);
	TIM14->CR1 = 0x00000000;		/* Stop TIM14 */
}

void delay_ms(uint16_t ms) {
	uint16_t i;
	for (i = 0; i < ms; ++i)
		delay_us(1000);
}
