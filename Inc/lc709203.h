#ifndef __LC709203_H_
#define __LC709203_H_
#include "i2c.h"


#define LC709203F_ADDAR		0x16
#define INIT_RSOC  				0X07
#define CELL_VOLTAGE      0X09
#define CURRENT_DIR				0X0A
#define APA								0X0B
#define APT								0X0C
#define RSOC              0X0D
#define FG_UNIT						0X0F
#define IC_VERSION				0X11
#define PROFILE_SELECT		0x12
#define ALARM_RSOC				0x13
#define ALARM_VOLTAGE			0x14

uint16_t Get_IC_Version(void);
uint16_t LC709203f_Read_Word(uint8_t addr, uint8_t cmd);
bool LC709203f_Write_Word(uint8_t addr, uint8_t cmd, uint16_t data);

#endif

