#include <stdio.h>
#include <stm32f4xx_rcc.h>
#include <stm32f4xx_gpio.h>
#include "stm32f4xx.h"
#include <stdint.h>
#include "delay.h"
#include <inttypes.h>
#include "serial.h"

void led_init(void) {
	/* LED initialisation goes here */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD , ENABLE);
	GPIO_InitTypeDef GPIO_InitStruct;
	GPIO_InitStruct.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 |
	GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12 |
	GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
	GPIO_InitStruct.GPIO_Mode = GPIO_Mode_OUT;
	GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOD , &GPIO_InitStruct);
}

uint32_t ms_passed = 0;

void TIM2_IRQHandler()
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        GPIO_ToggleBits(GPIOD, GPIO_Pin_12);
        ms_passed += 1;
    }
}

int main(void) {
	serial_init();
	
	led_init();
  
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
 
    TIM_TimeBaseInitTypeDef timerInitStructure; 
    timerInitStructure.TIM_Prescaler = 41999; // 0.5ms
    timerInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    timerInitStructure.TIM_Period = 10; // 10 ms
    timerInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    timerInitStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM2, &timerInitStructure);
    TIM_Cmd(TIM2, ENABLE);
  TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);
  
  NVIC_InitTypeDef nvicStructure;
    nvicStructure.NVIC_IRQChannel = TIM2_IRQn;
    nvicStructure.NVIC_IRQChannelPreemptionPriority = 0;
    nvicStructure.NVIC_IRQChannelSubPriority = 1;
    nvicStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvicStructure);
    
	uint32_t gpio_out = 0xFFFF;
	uint32_t seconds_passed = 0;
	while (1) {
    while (ms_passed > 100) {
      printf("Seconds: %" PRIu32 " MS: %" PRIu32 "\r\n", seconds_passed, ms_passed);
      ms_passed -= 100;
      seconds_passed++;
      GPIO_Write(GPIOD , gpio_out);
			gpio_out = ~gpio_out;
    }
	}

}
