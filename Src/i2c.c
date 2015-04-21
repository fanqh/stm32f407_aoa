#include "i2c.h"
//////////////////////////////////////////////////////////////////////////////////	 
////////////////////////////////////////////////////////////////////////////////// 	  

uint8_t ack;


void IIC_Init(void)
{
	GPIO_InitTypeDef  GPIO_InitStruct;

	IIC_ENABLE();

	GPIO_InitStruct.Pin = (IIC_SDA_PIN | IIC_SCL_PIN);
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;	       //��©���
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_LOW;
	HAL_GPIO_Init(IIC_PORT, &GPIO_InitStruct);

}
//----����IIC��ʼ�ź�-------------------------------------------------------------
void IIC_Start(void)						 
{
	IIC_SDA_PIN_H();
	IIC_SCL_PIN_H();
	delay_us(9); //9
	IIC_SDA_PIN_L();										  //START:when CLK is high,DATA change form high to low 
	delay_us(9); //	9
	IIC_SCL_PIN_L();						  //ǯסI2C���ߣ�׼�����ͻ�������� 
}	  
//----����IICֹͣ�ź�-------------------------------------------------------------
void IIC_Stop(void)
{
	IIC_SCL_PIN_L();
	IIC_SDA_PIN_L();											  //STOP:when CLK is high DATA change form low to high
 	delay_us(9);	//9
	IIC_SCL_PIN_H();
	delay_us(9);	//9						   	
	IIC_SDA_PIN_H();//����I2C���߽����ź�
	delay_us(9);	//9						   	
}
//----һ��SCLʱ��----------------------------------------------------
uint8_t IIC_Clock()
{
	uint8_t sample;
	
	delay_us(9);   //9
	IIC_SCL_PIN_H();      		//��ʱ����Ϊ�ߣ�֪ͨ��������ʼ��������λ
	delay_us(9);		//��֤ʱ�Ӹߵ�ƽ���ڴ���4��s   9
	sample = IIC_SDA();					  //changed
	IIC_SCL_PIN_L(); 
	return sample;
}
//----����һ���ֽ�----------------------------------------------------
void IIC_SendByte(uint8_t c)
{
	uint8_t BitCnt;
  	uint8_t t;
	//Ҫ���͵����ݳ���Ϊ8λ,
	for(BitCnt=0; BitCnt<8; BitCnt++)  
   	{
        t = (c & 0x80) >> 7;
		if (t == 0)
			IIC_SDA_PIN_L();
		else
			IIC_SDA_PIN_H();

		c<<=1;
		IIC_Clock();
	}
	if(IIC_Clock())
    	ack=0;
	else 
    	ack=1;
}
//----����һ���ֽ�----------------------------------------------------
uint8_t IIC_RcvByte()
{
	uint8_t retc;
	uint8_t BitCnt;
  
	retc=0; 
	for(BitCnt=0;BitCnt<8;BitCnt++)
    {
     	retc=retc<<1;
		if(IIC_Clock())
        	retc++;
    }
	return retc;
}
//----Ӧ��----------------------------------------------------------
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
//----дһ���ֽ�----------------------------------------------------
uint8_t IICputc(uint8_t sla, uint8_t c)
{
	IIC_Start();          //��������
	IIC_SendByte(sla);		//����������ַ
	if(ack==0)
    	return FALSE;

	IIC_SendByte(c);          //��������
	if(ack==0)            
    	return FALSE;

	IIC_Stop();           //�������� 
	return TRUE;
}
//----д���ӵ�ַ----------------------------------------------------
uint8_t IICwrite(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //��������
	IIC_SendByte(sla);        //����������ַ
	if(ack==0)           
    	return FALSE;

	IIC_SendByte(suba);       //���������ӵ�ַ
	if(ack==0)
	    return FALSE;

	for(i=0;i<no;i++)
    {   
    	IIC_SendByte(*s);      //��������
		if(ack==0)
       		return FALSE;
		s++;
    } 
	IIC_Stop();           //��������
	return TRUE;
}
//----д���ӵ�ַ----------------------------------------------------
uint8_t IICwriteExt(uint8_t sla, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //��������
	IIC_SendByte(sla);        //����������ַ
	if(ack==0)
    	return FALSE;

	for(i=0;i<no;i++)
    {   
    	IIC_SendByte(*s);      //��������
		if(ack==0)
			return FALSE;
		s++;
    } 
	IIC_Stop();           //�������� 
	return TRUE;
}
//----��һ���ֽ�----------------------------------------------------
uint8_t IICgetc(uint8_t sla, uint8_t *c)
{
	IIC_Start();          //��������
	IIC_SendByte(sla+1);      //����������ַ
	if(ack==0)
		return FALSE;

	*c=IIC_RcvByte();         //��������
	IIC_Ack(1);           //�����꣬���ͷ�Ӧ��λ����������
	IIC_Stop();           //�������� 
	return TRUE;
}
//----�����ӵ�ַ----------------------------------------------------
uint8_t IICread(uint8_t sla, uint8_t suba, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();          //��������
	IIC_SendByte(sla);        //����������ַ
	if(ack==0)
    	return FALSE;

	IIC_SendByte(suba);       //���������ӵ�ַ
	if(ack==0)
    	return FALSE;
		
	IIC_Start();			//������������
  	IIC_SendByte(sla+1);
  	if(ack==0)
    	return FALSE;

	for(i=0;i<no-1;i++)   //�Ƚ���ǰ(no-1)�ֽ�
	{   
    	*s=IIC_RcvByte();      //��������
     	IIC_Ack(0);        //��δ�����꣬����Ӧ��λ  
     	s++;
   	} 
	*s=IIC_RcvByte();        //���յ�no�ֽ�
	IIC_Ack(1);          //�����꣬���ͷ�Ӧ��λ
	IIC_Stop();          //�������� 
	return TRUE;
}
//----�����ӵ�ַ----------------------------------------------------
uint8_t IICreadExt(uint8_t sla, uint8_t *s, uint8_t no)
{
	uint8_t i;

	IIC_Start();
	IIC_SendByte(sla+1);		//R/Wѡ��λ��Ϊ1ʱΪ���� Ϊ0 ʱΪд
	if(ack==0)
		return FALSE;

	for(i=0;i<no-1;i++)   //�Ƚ���ǰ��no-1)���ֽ�
   	{   
    	*s=IIC_RcvByte();      //��������
     	IIC_Ack(0);        //δ��ȡ�꣬����Ӧ��λ  
     	s++;
	} 
   	*s=IIC_RcvByte();        //���յ�no�ֽ�
   	IIC_Ack(1);          //�����꣬���ͷ�Ӧ��λ
   	IIC_Stop();          //�������� 
   	return TRUE;
}


