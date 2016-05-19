#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stm32f4xx_rcc.h>
#include <stm32f4xx_gpio.h>
#include <stm32f4xx.h>
#include "delay.h"
#include "lcd.h"

// Max number of samples to take the tap-tempo average over
#define MAX_TAP_TEMPO_SAMPLES 6

// Number of milliseconds before a tap is counted as a new sequence rather
// than part of the previous sequence. 1500 means a lower limit of 40BPM
// which seems reasonable. If the user wants to go lower, they can still manually
// lower the BPM with the up/down buttons
#define TAP_TEMPO_FORGET_THRESHOLD 1500

// Masks for which button is pressed (GPIOE pins)
#define MASK_TAP_TEMPO    (1 << 0)
#define MASK_BPM_UP       (1 << 1)
#define MASK_BPM_DOWN     (1 << 2)
#define MASK_SYNCHRONISE  (1 << 3)
#define MASK_TIMESIG_UP   (1 << 6)
#define MASK_TIMESIG_DOWN (1 << 7)

// Constants for displaying time-signature information
const char timesig_labels[9][4] = {"2/2", "2/4", "3/4", "4/4", "5/4", "6/8", "7/4", "7/8", "9/8"};
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
void timesig_increase(void);
void timesig_decrease(void);
void tap_tempo_recalculate(void);
void handle_event(uint8_t event_mask, void (*handler)(void));
void synchronise(void);
void tempo_increase(void);
void tempo_decrease(void);
void set_tempo(uint16_t bpm);

// Program state
// Tempo (BPM) as set by the user
uint16_t tempo          = 0;
// Time between beats in milliseconds
uint32_t beat_period_ms = 0;

// Time signature is an index offset into the timesig_ arrays
size_t   time_signature = 0;
// Index for timesig_flash_patterns[] array
size_t   this_beat      = 0;

// Mask of any button events pending (corresponding to GPIOE pins)
uint8_t  pending_button_events = 0;

// Global system time since startup in milliseconds
// Note: using 64-bit unsigned int provides nominally 500,000 millenia of run-time.
// 32-bit would provide 49 days. Does a metronome need to run for more than 49 days
// with defined behaviour? Probably not, but I thought of a few exceptions: e.g. if it's
// part of an exhibition that lasts a few months. Given there's no particular space and
// resource constraints, and the rest of the implementation being fairly efficient, I chose
// 500,000 millenia. Practically the threshold would be sooner than this, as floating-point
// arithmetic is used for the tap-tempo implementation; IEEE 754 double-precision defines
// 52-bits of mantissa, so the limit is roughly 2^53. A mere 285 millenia. So the user
// should aim to reset the metronome at least every 284 millenia! ;)
uint64_t ms_passed = 0;

// We need to know when the last beat was so we know when it's time for the next
uint64_t timestamp_last_beat = 0;

// When this is high, the LCD will be updated and then lowered again. Prevents unnecessary rewrites.
bool     lcd_update_pending = false;

// These are used so the tap tempo implementation can track previous taps to take averages
uint64_t tap_samples[MAX_TAP_TEMPO_SAMPLES] = { 0 }; // Initialise to zeros
uint8_t  tap_samples_num = 0;

int main(void) {
	// Set-up peripherals/interrupts/etc
	lcd_init();
	buttons_init();
	led_init();
	timer_init();

	// And let everything sort itself out before using them ;)
	delay_ms(10);

	// Give an initial state
	lcd_move(0, 0);
	lcd_print("## METRONOME  ##");
	set_tempo(120);
	time_signature = 3; // 4/4

	// Never stop repeating
	while (1) {
		// Dispatch to the relevant handler function if there are pending events
		// for the following masks (corresponding to buttons on the board)
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

				// Look-up what pattern to write to the LEDs using pre-defined patterns (see
				// const defs at top of file). This is used so that certain beats
				// can be accented more than others.
				GPIO_Write(GPIOD, ((uint32_t) timesig_flash_patterns[time_signature][this_beat]) << 8);
				timestamp_last_beat = ms_passed;

				// Move on to the next beat
				this_beat++;
			}
			else {
				// Turn off the LEDs for the second half of each beat
				GPIO_Write(GPIOD, 0x0000);
			}
		}

		// Only write changes to the LCD when something has marked that it needs updating
		// this prevents wasteful updates when nothing has changed.
		if (lcd_update_pending) {
			static char label_line1[20];

			// Prepare text for display
			sprintf(label_line1, "%3" PRIu16 "bpm %9s" PRIu16, tempo, timesig_labels[time_signature]);

			// Display the system state
			lcd_move(0, 1); // Ensure it's printing to the right position
			lcd_print(label_line1); 

			// Unset pending flag
			lcd_update_pending = false;
		}

		// No need to loop indefinitely - nothing will have changed until the next
		// timer interrupt, so might as well put the processor to sleep until then
		__WFI();
	}

}

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

		// There has been user input so the system state may have changed,
		// so redraw the LCD.
		lcd_update_pending = true;
	}
}

/*
 * Sets a new tempo. This has its own function because the period in milliseconds
 * must be updated so the main loop can work out when the next beat is
 */ 
void set_tempo(uint16_t bpm) {
	tempo = bpm;

	// Convert the tempo into the time in ms between each beat (i.e. the period)
	beat_period_ms = (uint32_t) (1000.0/(tempo/60.0));
}

/*
 * Forces the current state such that this current tick is now the first beat
 * in the bar - all beats will follow from this point, remaining at the same BPM
 */
static inline void synchronise() {
	// This is the easiest way to do this - just makes the system believe that the last beat
	// happened exactly one beat period ago (so a new beat is due now).
	timestamp_last_beat = ms_passed - beat_period_ms;
	this_beat = 0;
}

/*
 * These handlers are on one line because they are very simple - just set a property
 * within certain bounds
 */
static inline void tempo_increase()   { if (tempo < 999) set_tempo(++tempo); }
static inline void tempo_decrease()   { if (tempo > 1)   set_tempo(--tempo); }
static inline void timesig_increase() { if (time_signature < 8) time_signature++; }
static inline void timesig_decrease() { if (time_signature > 0) time_signature--; }

/*
 * Works out a new tempo by taking the average period between each of the recent taps,
 * up to a maximum number of samples specified at the top of the file.
 *
 * Taps must happen within a certain time threshold, defined at the top of the file,
 * in order to be considered part of the same sequence.
 */
static inline void tap_tempo_recalculate() {
	// Work out new tempo if the previous one was less than 1000
	if (tap_samples_num > 0 && ms_passed - tap_samples[tap_samples_num-1] < TAP_TEMPO_FORGET_THRESHOLD) {
		double tempo = 0.0;

		// Don't look at previous samples if there aren't any
		if (tap_samples_num > 1) {
			for (size_t i = 0; i < tap_samples_num-1; i++) {
				// Time since last tap as a double
				tempo += (double) tap_samples[i+1] - tap_samples[i];
			}
		}

		// Add the final sample
		// This is done separately so that it is still added if there's only one
		tempo += ms_passed - tap_samples[tap_samples_num-1];

		tempo /= tap_samples_num;   // Take average
		tempo /= 1000.0;            // Convert into seconds
		tempo = 60.0/tempo;         // Convert into BPM 
		set_tempo((int) tempo);     // Change the current state
	}
	else {
		// Reset tap samples
		tap_samples_num = 0;
	}

	// If the samples have filled the sample array, we need to shift all elements
	// of the array along one so that there's still space for a new sample at the end.
	// So long as MAX_TAP_TEMPO_SAMPLES is a relatively low number, this isn't too
	// expensive
	if (tap_samples_num == MAX_TAP_TEMPO_SAMPLES) {
		// Don't use memcpy as behaviour is undefined for overlapping memory
		memmove(tap_samples, &tap_samples[1], sizeof(tap_samples) - sizeof(*tap_samples));
		// There's now one less element
		tap_samples_num--;
	}

	// Add the current time to the samples for the next button press
	tap_samples[tap_samples_num++] = ms_passed;
}

/**
 * Interrupt fires every 2ms and updates the system current-time in ms, then
 * checks for button state and raises a button event if a button that was not
 * down in the previous tick is now down.
 */
void TIM2_IRQHandler(void) {
	// Need to remember the previous button state so we can do edge detection
	static uint8_t button_state;

	if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET) {
		// Make sure the interrupt doesn't call again
		TIM_ClearITPendingBit(TIM2, TIM_IT_Update);

		// Track the global time
		ms_passed += 2;

		// Don't need the lower 8 bits
		uint8_t new_button_state = (uint8_t)(GPIO_ReadInputData(GPIOE) >> 8);

		// Raise a button event where the button was not previously pressed but is now
		// This makes sure the event only triggers on the 'edge' of the button state change
		pending_button_events |= ~button_state & new_button_state;

		// Update the memory of last button state
		button_state = new_button_state;
	}
}

/*
 * Sets up the LEDs
 */
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

/*
 * Sets up the buttons
 */
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

/*
 * Sets up the timer and interrupts for the timer.
 */
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

