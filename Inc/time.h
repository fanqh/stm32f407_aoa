
#ifndef __time_H
#define __time_H


#include "stm32f4xx_hal.h"


extern TIM_HandleTypeDef htim2;


void Time2_Delay_Init ( void );
void Time2_uDelay (const uint32_t usec);
void Time2_mDelay (const uint32_t msec);

void time2_init(void);
void StartTimeCount(void);
void StopTimeCount(void);
uint32_t GetTimeCount(void);
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim);
void PWM_Init(void);
void EnablePWM(void);
void DisablePWM(void);

#endif
