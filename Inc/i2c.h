#ifndef __I2C_H
#define __I2C_H
//////////////////////////////////////////////////////////////////////////////////	 

#include "stm32f4xx_hal.h"

#define I2Cx                             I2C2
#define I2Cx_CLK_ENABLE()                __I2C2_CLK_ENABLE()
#define I2Cx_SDA_GPIO_CLK_ENABLE()       __GPIOB_CLK_ENABLE()
#define I2Cx_SCL_GPIO_CLK_ENABLE()       __GPIOB_CLK_ENABLE()

#define I2Cx_FORCE_RESET()               __I2C2_FORCE_RESET()
#define I2Cx_RELEASE_RESET()             __I2C2_RELEASE_RESET()

/* Definition for I2Cx Pins */
#define I2Cx_SCL_PIN                    GPIO_PIN_10
#define I2Cx_SCL_GPIO_PORT              GPIOB
#define I2Cx_SCL_AF                     GPIO_AF4_I2C2
#define I2Cx_SDA_PIN                    GPIO_PIN_11
#define I2Cx_SDA_GPIO_PORT              GPIOB
#define I2Cx_SDA_AF                     GPIO_AF4_I2C2
extern I2C_HandleTypeDef I2cHandle;
void I2C_Init(void);
//////////////////////////////////////////////////////////////////////////////////	 
#endif
















