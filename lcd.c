#include "lcd.h"
#include "delay.h"

#define _h(x) (*((volatile uint16_t *)(0x4002<<16|x)))
#define _w(x) (*((volatile uint32_t *)(0x4002<<16|x)))
#define _d(x) _w(0xC18)=0xFF<<16|(x&0xFF)
#define _e(x) {_h(0x418)=4;delay_us(1);_h(0x41A)=4;delay_us(x);}
#define _c(x,y) {_d(x);_e(y);}

void lcd_init(void) {
	_w(0x3830)|=11;
	_w(0)&=~0xC0000000UL;_w(0)|=4<<28;_w(4)&=0x7FFF;
	_w(0x400)&=~0x3F;_w(0x400)|=0x15;_w(0x404)&=~7;
	_w(0xC00)&=~0xFFFF;_w(0xC00)|=0x5555;_w(0xC04)&=~0xFF;
	_h(0x18)=8<<12;_h(0x41A)=7;delay_ms(50);
	_c(56,4100);_e(100);_e(100);_c(12,45);_c(1,1640);_c(6,45);_c(1,1640);
}

void lcd_print(const char *t) {
	const char*p=t;_h(0x0418)=1;while(*p)_c(*(p++),45);
}

void lcd_move(uint8_t c, uint8_t r) {
	_h(0x041A)=1;_c(r<<5|r<<3|c|0x80,100);
}
