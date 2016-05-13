#include "serial.h"
#include "stm32f4xx.h"

static void _configUSART2(uint32_t BAUD, uint32_t fosc)
{
  uint32_t tmpreg = 0x00, apbclock = 0x00;
  uint32_t integerdivider = 0x00;
  uint32_t fractionaldivider = 0x00;

  apbclock = fosc/16;
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  RCC->APB1ENR |= RCC_APB1ENR_USART2EN;	/* Enable USART2 Clock */

	GPIOA->MODER &= ~GPIO_MODER_MODER2;
  GPIOA->MODER |=  GPIO_MODER_MODER2_1;		/* Setup TX pin for Alternate Function */

  GPIOA->AFR[0] |= (7 << (4*2));		/* Setup TX as the Alternate Function */

  USART2->CR1 |= USART_CR1_UE;	/* Enable USART */

  integerdivider = ((25 * apbclock) / (2 * (BAUD)));  
  tmpreg = (integerdivider / 100) << 4;
  fractionaldivider = integerdivider - (100 * (tmpreg >> 4));

  tmpreg |= ((((fractionaldivider * 8) + 50) / 100)) & ((uint8_t)0x07);

  USART2->BRR = (uint16_t)tmpreg;
  USART2->CR1 |= USART_CR1_TE;	/* Enable Tx */
}

void serial_init(void) {
	_configUSART2(38400, 168000000);
}
