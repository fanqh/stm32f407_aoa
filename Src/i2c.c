#include "i2c.h"
#include "time.h"

static uint8_t ack;
void I2C_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStruct;

	I2Cx_SDA_GPIO_CLK_ENABLE();

	GPIO_InitStruct.Pin = (I2Cx_SCL_PIN | I2Cx_SDA_PIN);
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;	       //开漏输出
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_LOW;
	HAL_GPIO_Init(I2Cx_SDA_GPIO_PORT, &GPIO_InitStruct);

}

//----产生IIC起始信号-------------------------------------------------------------
void IIC_Start(void)
{
	IIC_SDA_PIN_H();
	IIC_SCL_PIN_H();
	Time2_uDelay(9); //9
	IIC_SDA_PIN_L();				//START:when CLK is high,DATA change form high to low
	Time2_uDelay(9); //	9
	IIC_SCL_PIN_L();						  //钳住I2C总线，准备发送或接收数据
}
//----产生IIC停止信号-------------------------------------------------------------
void IIC_Stop(void)
{
	IIC_SCL_PIN_L();
	IIC_SDA_PIN_L();					//STOP:when CLK is high DATA change form low to high
	Time2_uDelay(9);	//9
	IIC_SCL_PIN_H();
	Time2_uDelay(9);	//9
	IIC_SDA_PIN_H();	//发送I2C总线结束信号
	Time2_uDelay(9);	//9
}
//----一个SCL时钟----------------------------------------------------
uint8_t IIC_Clock()
{
	uint8_t sample;

	Time2_uDelay(9);   //9
	IIC_SCL_PIN_H();      		//置时钟线为高，通知被控器开始接收数据位
	Time2_uDelay(9);		//保证时钟高电平周期大于4μs   9
	sample = IIC_SDA();					  //changed
	IIC_SCL_PIN_L();
	return sample;
}
//----发送一个字节----------------------------------------------------
void IIC_SendByte(uint8_t c)
{
	uint8_t BitCnt;
	uint8_t t;
	//要传送的数据长度为8位,
	for (BitCnt = 0; BitCnt < 8; BitCnt++)
	{
		t = (c & 0x80) >> 7;
		if (t == 0)
			IIC_SDA_PIN_L();
		else
			IIC_SDA_PIN_H();

		c <<= 1;
		IIC_Clock();
	}
	if (IIC_Clock())
		ack = 0;
	else
		ack = 1;
}
//----接收一个字节----------------------------------------------------
uint8_t IIC_RcvByte()
{
	uint8_t retc;
	uint8_t BitCnt;

	retc = 0;
	for (BitCnt = 0; BitCnt < 8; BitCnt++)
	{
		retc = retc << 1;
		if (IIC_Clock())
			retc++;
	}
	return retc;
}
//----应答----------------------------------------------------------
void IIC_Ack(uint8_t a)
{
	if (a == 0)
		IIC_SDA_PIN_L();
	else
		IIC_SDA_PIN_H();
	IIC_Clock();
	IIC_SDA_PIN_H();
}

///////////////////////////////////////////////////////////////////
//----写一个字节----------------------------------------------------
bool IICputc(uint8_t sla, uint8_t c)
{
	IIC_Start();          //启动总线
	IIC_SendByte(sla);		//发送器件地址
	if (ack == 0)
		return false;

	IIC_SendByte(c);          //发送数据
	if (ack == 0)
		return false;

	IIC_Stop();           //结束总线
	return true;
}
//----写有子地址----------------------------------------------------
bool IICwrite(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //启动总线
	IIC_SendByte(sla);        //发送器件地址
	if (ack == 0)
		return false;

	IIC_SendByte(suba);       //发送器件子地址
	if (ack == 0)
		return false;

	for (i = 0; i < no; i++)
	{
		IIC_SendByte(*s);      //发送数据
		if (ack == 0)
			return false;
		s++;
	}
	IIC_Stop();           //结束总线
	return true;
}
//----写无子地址----------------------------------------------------
bool IICwriteExt(uint8_t sla, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //启动总线
	IIC_SendByte(sla);        //发送器件地址
	if (ack == 0)
		return false;

	for (i = 0; i < no; i++)
	{
		IIC_SendByte(*s);      //发送数据
		if (ack == 0)
			return false;
		s++;
	}
	IIC_Stop();           //结束总线
	return true;
}
//----读一个字节----------------------------------------------------
bool IICgetc(uint8_t sla, uint8_t *c)
{
	IIC_Start();          //启动总线
	IIC_SendByte(sla + 1);      //发送器件地址
	if (ack == 0)
		return false;

	*c = IIC_RcvByte();         //接收数据
	IIC_Ack(1);           //接收完，发送非应答位，结束总线
	IIC_Stop();           //结束总线
	return true;
}
//----读有子地址----------------------------------------------------
bool IICread(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //启动总线
	IIC_SendByte(sla);        //发送器件地址
	if (ack == 0)
		return false;

	IIC_SendByte(suba);       //发送器件子地址
	if (ack == 0)
		return false;

	IIC_Start();			//重新启动总线
	IIC_SendByte(sla + 1);
	if (ack == 0)
		return false;

	for (i = 0; i < no - 1; i++)   //先接收前(no-1)字节
	{
		*s = IIC_RcvByte();      	//接收数据
		IIC_Ack(0);        //还未接收完，发送应答位
		s++;
	}
	*s = IIC_RcvByte();        //接收第no字节
	IIC_Ack(1);          //接收完，发送非应答位
	IIC_Stop();          //结束总线
	return true;
}
//----读无子地址----------------------------------------------------
bool IICreadExt(uint8_t sla, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();
	IIC_SendByte(sla + 1);		//R/W选择位，为1时为读， 为0 时为写
	if (ack == 0)
		return false;

	for (i = 0; i < no - 1; i++)   //先接收前（no-1)个字节
	{
		*s = IIC_RcvByte();      //接收数据
		IIC_Ack(0);        //未读取完，发送应答位
		s++;
	}
	*s = IIC_RcvByte();        //接收第no字节
	IIC_Ack(1);          //接收完，发送非应答位
	IIC_Stop();          //结束总线
	return true;
}

