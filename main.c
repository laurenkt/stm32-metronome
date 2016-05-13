#include <stdio.h>
#include <stdlib.h>
#include <stm32f4xx_rcc.h>
#include <stm32f4xx_gpio.h>
#include "stm32f4xx.h"
#include <stdint.h>
#include "delay.h"
#include <inttypes.h>
#include "serial.h"
#include "lcd.h"

// Stack array because it only works for arrays defined on the stack, i.e. not pointers
#define STACK_ARRAY_LENGTH(ARR) (sizeof(ARR)/sizeof(ARR[0]))

#define BTN_TAP_TEMPO btn1

#define BTN_BPM_UP   btn3
#define BTN_BPM_DOWN btn4

uint16_t bpm = 0;
uint32_t beat_period_ms = 0;
uint16_t beats_per_bar = 4;
uint16_t beat_unit = 4;

char label_line1[20];

typedef union {
    struct {
        unsigned char btn1 : 1;
        unsigned char btn2 : 1;
        unsigned char btn3 : 1;
        unsigned char btn4 : 1;
        unsigned char btn5 : 1;
        unsigned char btn6 : 1;
        unsigned char btn7 : 1;
        unsigned char btn8 : 1;
    } buttons;
    uint8_t state;
} ButtonState;

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

void buttons_init(void) {
	/* button initialisation goes here */
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE , ENABLE);
	GPIO_InitTypeDef GPIO_InitStruct;
	GPIO_InitStruct.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 |
	GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12 |
	GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15;
	GPIO_InitStruct.GPIO_Mode = GPIO_Mode_IN;
	GPIO_InitStruct.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_InitStruct.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStruct.GPIO_PuPd = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOD , &GPIO_InitStruct);
}

uint32_t ms_passed   = 0x0000;

uint32_t timestamp_last_screen_update = 0;
uint32_t timestamp_last_tap_tempo = 0;
uint32_t timestamp_last_beat = 0;

uint32_t tap_samples[6];
uint8_t tap_samples_num = 0;

ButtonState button_pressed;

void TIM2_IRQHandler(void)
{
    static ButtonState button_state;
    
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
        ms_passed += 2;
 
        uint8_t new_button_state = (uint8_t)(GPIO_ReadInputData(GPIOE) >> 8);
        
        // Check each button
        for (uint8_t i = 0; i < 8; i++) {
            // If button was not previously pressed
            if (!(button_state.state & (1 << i))) {
                // And button is now pressed
                if (new_button_state & (1 << i)) {
                    // Raise a button_pressed for this button
                    button_pressed.state |= (1 << i);
                }
            }
        }
        
        // Update the memory of last button state
        button_state.state = new_button_state;
    }
}

void timer_init(void) {
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
 
    TIM_TimeBaseInitTypeDef timerInitStructure; 
    timerInitStructure.TIM_Prescaler = 41999; // 0.5ms
    timerInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    timerInitStructure.TIM_Period = 1;
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
}

int main(void) {
	serial_init();
    lcd_init();
    buttons_init();
 	led_init();
    timer_init();

    delay_ms(50);
    lcd_move(0, 0);
    lcd_print("BPM         TIME");

	while (1) {        
        if (button_pressed.buttons.BTN_TAP_TEMPO) {
            // Work out new tempo if the previous one was less than 1000
            if (tap_samples_num > 0 && ms_passed - tap_samples[tap_samples_num-1] < 2000) {
                printf("This is fast enough");
                
                double tempo = 0.0;
                
                if (tap_samples_num > 1) {
                    printf("Averaging");
                    for (size_t i = 0; i < tap_samples_num-1; i++) {
                        // Time since last tap as a double
                        tempo += (double) tap_samples[i+1] - tap_samples[i];
                    }
                }
                
                tempo += ms_passed - tap_samples[tap_samples_num-1];
                
                // Take average
                tempo /= tap_samples_num;
                
                // Convert into seconds
                tempo /= 1000.0;
                
                // How many of these happen per minute
                tempo = 60.0/tempo;
                // Back to int
                bpm = (int) tempo;
                beat_period_ms = (uint32_t) (1000.0/(bpm/60.0));
            } else {
                printf("Reset");
                // Reset tap samples
                tap_samples_num = 0;
            }
            
            if (tap_samples_num == STACK_ARRAY_LENGTH(tap_samples)) {
                printf("Shift array down");
                for (size_t i = 0; i < STACK_ARRAY_LENGTH(tap_samples) - 1; i++) {
                    tap_samples[i] = tap_samples[i+1];
                }
                
                tap_samples_num--;
            }
            
            timestamp_last_tap_tempo = ms_passed;
            tap_samples[tap_samples_num++] = ms_passed;
            printf("#Samples: %d\r\n", tap_samples_num);
            
            button_pressed.buttons.BTN_TAP_TEMPO = 0;
        }
        
        if (button_pressed.buttons.BTN_BPM_UP) {
            bpm++;
            // Unset the button
            button_pressed.buttons.BTN_BPM_UP = 0;
        }
        
        if (button_pressed.buttons.BTN_BPM_DOWN) {
            bpm--;
            // Unset the button
            button_pressed.buttons.BTN_BPM_DOWN = 0;
        }
        
        if (ms_passed - timestamp_last_beat > beat_period_ms/2) {
            if (ms_passed - timestamp_last_beat > beat_period_ms) {
                //gpio_out = ~gpio_out;
                GPIO_Write(GPIOD , 0xFF00);
                timestamp_last_beat = ms_passed;
            }
            else {
                GPIO_Write(GPIOD , 0x0000);
            }
        }
        

        
        if (ms_passed - timestamp_last_screen_update > 100) {
          sprintf(label_line1, "%-6" PRIu16 " %6" PRIu16"/%-2" PRIu16, bpm, beats_per_bar, beat_unit);
          lcd_move(0, 1);
          lcd_print(label_line1); 
          timestamp_last_screen_update = ms_passed;
        }
	}

}
