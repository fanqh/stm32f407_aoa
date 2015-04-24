#ifndef __ADC_H_
#define __ADC_H_
#include "stm32f4xx_hal.h"

extern ADC_HandleTypeDef    AdcHandle;
//extern BatteryTypeDef BatteryInfor;
void ADC_Init(void);
void Battery_Process(void);

#endif

