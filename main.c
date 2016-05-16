#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stm32f4xx_rcc.h>
#include <stm32f4xx_gpio.h>
#include <stm32f4xx.h>
#include "delay.h"
#include "serial.h"
#include "lcd.h"

// Max number of samples to take the tap-tempo average over
#define MAX_TAP_TEMPO_SAMPLES 6

// Masks for which button is pressed (GPIOE pins)
#define MASK_TAP_TEMPO    (1 << 0)
#define MASK_BPM_UP       (1 << 1)
#define MASK_BPM_DOWN     (1 << 2)
#define MASK_SYNCHRONISE  (1 << 3)
#define MASK_TIMESIG_UP   (1 << 6)
#define MASK_TIMESIG_DOWN (1 << 7)

// Constants for displaying time-signature information
const char     timesig_labels[9][4]         = {"2/2", "2/4", "3/4", "4/4", "5/4", 
                                               "6/8", "7/4", "7/8", "9/8"};
// Null-terminated sequence of LED patterns a time signature
const uint16_t timesig_flash_patterns[9][10] = {
	{0xFF, 0xF0, 0}, // 2/2
	{0xFF, 0xF0, 0}, // 2/4
	{0xFF, 0x1C, 0xE0, 0}, // 3/4
	{0xFF, 0x0C, 0x30, 0xC0, 0}, // 4/4
	{0xFF, 0x03, 0x0C, 0x30, 0xC0, 0}, // 5/4
	{0xFF, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0}, // 6/8
	{0xFF, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0}, // 7/4
	{0xFF, 0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0}, // 7/8
	{0xFF, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0} // 9/8
};

// Function prototypes (using these so I can define the initialisation/boilerplate
// funcs at the bottom of the program to make the main logic clearer)
void timer_init(void);
void led_init(void);
void buttons_init(void);
void TIM2_IRQHandler(void);

// Program state
uint16_t tempo          = 0;
uint32_t beat_period_ms = 0;

size_t   time_signature = 0;
size_t   this_beat      = 0;

uint8_t  pending_button_events = 0;

uint32_t ms_passed   = 0x0000;

uint32_t timestamp_last_screen_update = 0;
uint32_t timestamp_last_tap_tempo = 0;
uint32_t timestamp_last_beat = 0;

uint32_t tap_samples[MAX_TAP_TEMPO_SAMPLES] = { 0 };
uint8_t  tap_samples_num = 0;

/**
  * Checks if there's a button event waiting to be handled (specified with a mask
  * to select which button), invokes a given handler function if it is, then
  * marks the event as handled
  */
static inline void handle_event(uint8_t event_mask, void (*handler)(void)) {
    if (pending_button_events & event_mask) {
        handler();

        // Unset bit for this event (so the event is 'handled' and won't be triggered
        // again
        // Note that part of this architecture means that any event that is
        // received between the mask being checked and the mask being cleared
        // will effectively be ignored (overwritten).
        // This isn't a problem in practice as a user wouldn't press the user
        // multiple times per 2ms (and if they did it would probably be
        // erroneous switch bouncing)
        pending_button_events &= ~event_mask;
    }
}

/*
 * Sets a new tempo. This has its own function because the period in milliseconds
 * must be updated so the main loop can work out when the next beat is
 */ 
void set_tempo(int bpm) {
	tempo = bpm;
	beat_period_ms = (uint32_t) (1000.0/(tempo/60.0));
}

/*
 * Forces the current state such that this current tick is now the first beat
 * in the bar - all beats will follow from this point, remaining at the same BPM
 */
void synchronise() {
    timestamp_last_beat = ms_passed - beat_period_ms;
    this_beat = 0;
}

/*
 * These handlers are on one line because they are very simple - just set a property
 * within certain bounds
 */
void tempo_increase()   { if (tempo < 999) set_tempo(++tempo); }
void tempo_decrease()   { if (tempo > 1)   set_tempo(--tempo); }
void timesig_increase() { if (time_signature < 8) time_signature++; }
void timesig_decrease() { if (time_signature > 0) time_signature--; }

void tap_tempo_recalculate() {
    // Work out new tempo if the previous one was less than 1000
    if (tap_samples_num > 0 && ms_passed - tap_samples[tap_samples_num-1] < 1500) {
        double tempo = 0.0;
        
        if (tap_samples_num > 1) {
            for (size_t i = 0; i < tap_samples_num-1; i++) {
				// If this time interval is significantly greater than the most recent 
                // Time since last tap as a double
                tempo += (double) tap_samples[i+1] - tap_samples[i];
            }
        }
        
        // Add the final sample
        // This is done separately so that it is still added if there's only one
        tempo += ms_passed - tap_samples[tap_samples_num-1];
        
        tempo /= tap_samples_num; // Take average
        tempo /= 1000.0;          // Convert into seconds
        tempo = 60.0/tempo;       // Convert into BPM 
        set_tempo((int) tempo);   // Change the current state
    }
    else {
        // Reset tap samples
        tap_samples_num = 0;
    }

    if (tap_samples_num == MAX_TAP_TEMPO_SAMPLES) {
        for (size_t i = 0; i < MAX_TAP_TEMPO_SAMPLES - 1; i++) {
            tap_samples[i] = tap_samples[i+1];
        }
        
        tap_samples_num--;
    }

    timestamp_last_tap_tempo = ms_passed;
    tap_samples[tap_samples_num++] = ms_passed;
}

int main(void) {
	serial_init();
	lcd_init();
	buttons_init();
	led_init();
	timer_init();

	delay_ms(10);

	// Give an initial state
	lcd_move(0, 0);
	lcd_print("## METRONOME  ##");
	set_tempo(120);
	time_signature = 3; // 4/4

	while (1) {     
        handle_event(MASK_TAP_TEMPO,    tap_tempo_recalculate);
        handle_event(MASK_BPM_UP,       tempo_increase);
        handle_event(MASK_BPM_DOWN,     tempo_decrease);
        handle_event(MASK_SYNCHRONISE,  synchronise);
        handle_event(MASK_TIMESIG_UP,   timesig_increase);
        handle_event(MASK_TIMESIG_DOWN, timesig_decrease);
		
        // If at least half the period has passed (lights flash for half a period)
		if (ms_passed - timestamp_last_beat > beat_period_ms/2) {
			if (ms_passed - timestamp_last_beat > beat_period_ms) {
				if (timesig_flash_patterns[time_signature][this_beat] == 0) {
					this_beat = 0;
				}

				GPIO_Write(GPIOD, ((uint32_t) timesig_flash_patterns[time_signature][this_beat]) << 8);
				timestamp_last_beat = ms_passed;

				this_beat++;
			}
			else {
				GPIO_Write(GPIOD, 0x0000);
			}
		}

		// Don't need to update the screen more than 100ms - humans are not able to 
		// detect changes much faster than this 
		if (ms_passed - timestamp_last_screen_update > 100) {
            static char label_line1[20];
			sprintf(label_line1, "%3" PRIu16 "bpm %9s" PRIu16, tempo, timesig_labels[time_signature]);
			lcd_move(0, 1);
			lcd_print(label_line1); 
			timestamp_last_screen_update = ms_passed;
		}

		// No need to loop indefinitely - nothing will have changed until the next
        // timer interrupt, so might as well put the processor to sleep until then
		__WFI();
	}

}

/**
  * Interrupt fires every 2ms and updates the system current-time in ms, then
  * checks for button state and raises a button event if a button that was not
  * down in the previous tick is now down.
  */
void TIM2_IRQHandler(void) {
	static uint8_t button_state;
	
	if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET) {
		TIM_ClearITPendingBit(TIM2, TIM_IT_Update);

		ms_passed += 2;

		uint8_t new_button_state = (uint8_t)(GPIO_ReadInputData(GPIOE) >> 8);
		
		// Check each button
		for (uint8_t i = 0; i < 8; i++) {
			// If button was not previously pressed
			if (!(button_state & (1 << i))) {
				// And button is now pressed
				if (new_button_state & (1 << i)) {
					// Raise a button_pressed for this button
					pending_button_events |= (1 << i);
				}
			}
		}
		
		// Update the memory of last button state
		button_state = new_button_state;
	}
}

void led_init(void) {
	// LEDs use GPIO-D and are Outputs
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
	GPIO_InitTypeDef init_data;
	init_data.GPIO_Pin   = GPIO_Pin_8  | GPIO_Pin_9  | GPIO_Pin_10 | 
                           GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | 
                           GPIO_Pin_14 | GPIO_Pin_15;
	init_data.GPIO_Mode  = GPIO_Mode_OUT;
	init_data.GPIO_Speed = GPIO_Speed_50MHz;
	init_data.GPIO_OType = GPIO_OType_PP;
	init_data.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOD , &init_data);
}

void buttons_init(void) {
	// Buttons use GPIO-E and are Inputs
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
	GPIO_InitTypeDef init_data;
	init_data.GPIO_Pin   = GPIO_Pin_8  | GPIO_Pin_9  | GPIO_Pin_10 | 
                           GPIO_Pin_11 | GPIO_Pin_12 | GPIO_Pin_13 | 
                           GPIO_Pin_14 | GPIO_Pin_15;
	init_data.GPIO_Mode  = GPIO_Mode_IN;
	init_data.GPIO_Speed = GPIO_Speed_50MHz;
	init_data.GPIO_OType = GPIO_OType_PP;
	init_data.GPIO_PuPd  = GPIO_PuPd_NOPULL;
	GPIO_Init(GPIOD , &init_data);
}

void timer_init(void) {
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    // Set-up the timer
	TIM_TimeBaseInitTypeDef init_data; 
	init_data.TIM_Prescaler     = 41999; // 0.5ms
	init_data.TIM_CounterMode   = TIM_CounterMode_Up;
	init_data.TIM_Period        = 1;
	init_data.TIM_ClockDivision = TIM_CKD_DIV1;
	init_data.TIM_RepetitionCounter = 0;
	TIM_TimeBaseInit(TIM2, &init_data);
	TIM_Cmd(TIM2, ENABLE);
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

    // Then set-up interrupts for the timer (fires every timer period)
	NVIC_InitTypeDef nvic_init_data;
	nvic_init_data.NVIC_IRQChannel    = TIM2_IRQn;
	nvic_init_data.NVIC_IRQChannelCmd = ENABLE;
	nvic_init_data.NVIC_IRQChannelPreemptionPriority = 0;
	nvic_init_data.NVIC_IRQChannelSubPriority = 1;
	NVIC_Init(&nvic_init_data);
}
