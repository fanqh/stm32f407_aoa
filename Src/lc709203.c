#include "lc709203.h"

#if 0
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
#endif
#if 0
uint16_t Get_IC_Version(void)
{

	struct{
		uint16_t read;
		uint8_t  crc;

	}data;
	if(IICread(LC709203F_ADDAR, IC_VERSION, (uint8_t*)&data, 3)==false)
		IIC_Stop();          //结束总线

	return (data.read);

}
#else

unsigned char crc8_msb(unsigned char poly, unsigned char* data, int size)
{
	unsigned char crc = 0x00;
	int bit;

	while (size--) {
		crc ^= *data++;
		for (bit = 0; bit < 8; bit++) {
			if (crc & 0x80) {
				crc = (crc << 1) ^ poly;
			} else {
				crc <<= 1;
			}
		}
	}

	return crc;
}

uint16_t LC709203f_Read_Word(uint8_t addr, uint8_t cmd)
{
	struct{
		uint16_t read;
		uint8_t  crc;

	}data;
	if(IICread(LC709203F_ADDAR, cmd, (uint8_t*)&data, 3)==false)
		IIC_Stop();          //结束总线
	return (data.read);
}

bool LC709203f_Write_Word(uint8_t addr, uint8_t cmd, uint16_t data)
{
	uint8_t buff[32];

	buff[0] = addr;
	buff[1] =  cmd;
	memcpy(&buff[2], &data, 2);
	buff[4] = crc8_msb(0x07, buff, 4);

	return IICwrite(addr, cmd, &buff[2], 3);
}
#endif
