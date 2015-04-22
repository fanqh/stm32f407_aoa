#include "lc709203.h"

uint16_t Get_IC_Version(void)
{
//	HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t DevAddress, uint8_t *pData, uint16_t Size, uint32_t Timeout);
//	HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t DevAddress, uint8_t *pData, uint16_t Size, uint32_t Timeout);
	uint8_t command;
	uint8_t temp[3];
	uint16_t version;

	command = CELL_VOLTAGE;
	if(HAL_I2C_Master_Transmit(&I2cHandle,LC709203F_ADDAR , &command, 1, 1000)==HAL_OK)
	{
		if(HAL_I2C_Master_Receive(&I2cHandle,LC709203F_ADDAR , temp, 3, 1000)==HAL_OK)
		{
			version = temp[0] || temp[1]<<8;
			return version;
		}
	}
	return 0;
}

