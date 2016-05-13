#ifndef _LCD_H_
#define _LCD_H_

#include <stdint.h>

void lcd_init(void);
void lcd_print(const char *text);
void lcd_move(uint8_t column, uint8_t row);

#endif /*_LCD_H_*/
