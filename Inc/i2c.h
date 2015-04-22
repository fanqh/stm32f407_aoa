#ifndef __I2C_H
#define __I2C_H
//////////////////////////////////////////////////////////////////////////////////	 

#include "stm32f4xx_hal.h"
#include "stdio.h"
#include "stdbool.h"

#define I2Cx_SDA_GPIO_CLK_ENABLE()       __GPIOB_CLK_ENABLE()
#define I2Cx_SCL_GPIO_CLK_ENABLE()       __GPIOB_CLK_ENABLE()

#define I2Cx_FORCE_RESET()               __I2C2_FORCE_RESET()
#define I2Cx_RELEASE_RESET()             __I2C2_RELEASE_RESET()

/* Definition for I2Cx Pins */
#define I2Cx_SCL_PIN                    GPIO_PIN_10
#define I2Cx_SCL_GPIO_PORT              GPIOB
#define I2Cx_SDA_PIN                    GPIO_PIN_11
#define I2Cx_SDA_GPIO_PORT              GPIOB


#define IIC_SDA_PIN_L()					HAL_GPIO_WritePin(I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN, GPIO_PIN_RESET)
#define IIC_SDA_PIN_H()					HAL_GPIO_WritePin(I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN, GPIO_PIN_SET)

#define IIC_SCL_PIN_L()					HAL_GPIO_WritePin(I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN, GPIO_PIN_RESET)
#define IIC_SCL_PIN_H()					HAL_GPIO_WritePin(I2Cx_SCL_GPIO_PORT, I2Cx_SCL_PIN, GPIO_PIN_SET)

#define IIC_SDA()               HAL_GPIO_ReadPin(I2Cx_SDA_GPIO_PORT, I2Cx_SDA_PIN)

void I2C_Init(void);
//////////////////////////////////////////////////////////////////////////////////	 
#endif
















