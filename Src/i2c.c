#include "i2c.h"
#include "stdio.h"

I2C_HandleTypeDef I2cHandle;

void I2C_Init(void)
{
	/*##-1- Configure the I2C peripheral ######################################*/
	I2cHandle.Instance = I2Cx;

	I2cHandle.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	I2cHandle.Init.ClockSpeed = 1000; //??
	I2cHandle.Init.DualAddressMode = I2C_DUALADDRESS_DISABLED;
	I2cHandle.Init.DutyCycle = I2C_DUTYCYCLE_2;
	I2cHandle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLED;
	I2cHandle.Init.NoStretchMode = I2C_NOSTRETCH_DISABLED;
  I2cHandle.Init.OwnAddress1     = 0x16;
  I2cHandle.Init.OwnAddress2     = 0xFE;

	if (HAL_I2C_Init(&I2cHandle) != HAL_OK)
	{
		printf("*****I2C Init is error*****\r\n\r\n");
	}

}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
	GPIO_InitTypeDef GPIO_InitStruct;

	/*##-1- Enable GPIO Clocks #################################################*/
	/* Enable GPIO TX/RX clock */
	I2Cx_SCL_GPIO_CLK_ENABLE();
	I2Cx_SDA_GPIO_CLK_ENABLE();

	/*##-2- Configure peripheral GPIO ##########################################*/
	/* I2C TX GPIO pin configuration  */
	GPIO_InitStruct.Pin = I2Cx_SCL_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FAST;
	GPIO_InitStruct.Alternate = I2Cx_SCL_AF;
	HAL_GPIO_Init(I2Cx_SCL_GPIO_PORT, &GPIO_InitStruct);

	/* I2C RX GPIO pin configuration  */
	GPIO_InitStruct.Pin = I2Cx_SDA_PIN;
	GPIO_InitStruct.Alternate = I2Cx_SDA_AF;
	HAL_GPIO_Init(I2Cx_SDA_GPIO_PORT, &GPIO_InitStruct);

	/*##-3- Enable I2C peripheral Clock ########################################*/
	/* Enable I2C1 clock */
	I2Cx_CLK_ENABLE();
}





