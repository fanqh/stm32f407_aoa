#ifndef __I2C_H
#define __I2C_H
//////////////////////////////////////////////////////////////////////////////////	 

#include "stm32f4xx_hal.h"

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

//-------------------------------------IIC--------------------------------------
#define IIC_PORT							GPIOB
#define IIC_ENABLE()					__GPIOB_CLK_ENABLE()
#define IIC_SDA_PIN					  GPIO_PIN_11
#define IIC_SCL_PIN						GPIO_PIN_10


#define IIC_SDA_PIN_L()					HAL_GPIO_WritePin(IIC_PORT, IIC_SDA_PIN, GPIO_PIN_RESET)
#define IIC_SDA_PIN_H()					HAL_GPIO_WritePin(IIC_PORT, IIC_SDA_PIN, GPIO_PIN_SET)

#define IIC_SCL_PIN_L()					HAL_GPIO_WritePin(IIC_PORT, IIC_SCL_PIN, GPIO_PIN_RESET)
#define IIC_SCL_PIN_H()					HAL_GPIO_WritePin(IIC_PORT, IIC_SCL_PIN, GPIO_PIN_SET)

#define IIC_SDA()               HAL_GPIO_ReadPin(IIC_PORT, IIC_SDA_PIN)

//IIC所有操作函数
void IIC_Init(void);                //初始化IIC的IO口
uint8_t IICputc(uint8_t sla, uint8_t c);
uint8_t IICwrite(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no);
uint8_t IICwriteExt(uint8_t sla, uint8_t *s, uint8_t no);
uint8_t IICgetc(uint8_t sla, uint8_t *c);
uint8_t IICread(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no);
uint8_t IICreadExt(uint8_t sla, uint8_t *s, uint8_t no);


//////////////////////////////////////////////////////////////////////////////////	 
#endif
















