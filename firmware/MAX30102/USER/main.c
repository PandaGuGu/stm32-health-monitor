/*************************************************************************************
接线

MAX30102 心率血氧传感器:
	VCC<->3.3V
	GND<->GND
	SCL<->PB7
	SDA<->PB8
	INT<->PB9
0.96OLED 屏幕:
	VCC<->3.3V
	GND<->GND
	SCK<->PA5
	SDA<->PA6
MPU6050 姿态传感器:
	VCC<->3.3V
	GND<->GND
	SCL<->PB10
	SDA<->PB11
DS18B20 温度传感器:
	VCC<->3.3V
	GND<->GND
	DQ<->PA8
蜂鸣器:
	VCC<->3.3V
	GND<->GND
	I/O<->PA12
**************************************************************************************/
#include "delay.h"
#include "sys.h"
#include "max30102.h" 
#include "myiic.h"
#include "algorithm.h"
#include "OLED.h"
#include "MPU6050.h"
#include "ds18b20.h"
#include "Buzzer.h"
#include <math.h>
#include <stdbool.h>
 
int16_t AX, AY, AZ, GX, GY, GZ;			//定义用于存放各个数据的变量
int16_t Last_AX, Last_AY;			//定义用于存放各个数据的变量
 
uint32_t aun_ir_buffer[500]; //IR LED sensor data
int32_t n_ir_buffer_length;    //data length
uint32_t aun_red_buffer[500];    //Red LED sensor data
int32_t n_sp02; //SPO2 value
int8_t ch_spo2_valid;   //indicator to show if the SP02 calculation is valid
int32_t n_heart_rate;   //heart rate value
int8_t  ch_hr_valid;    //indicator to show if the heart rate calculation is valid
uint8_t uch_dummy;
uint8_t Temp;

#define MAX_BRIGHTNESS 255

int main(void)
{ 
	//variables to calculate the on-board LED brightness that reflects the heartbeats
	uint32_t un_min, un_max, un_prev_data;  
	int i;
	int32_t n_brightness;
	float f_temp;
	u8 temp_num=0;
	u8 temp[6];
	u8 str[100];
	u8 dis_hr=0,dis_spo2=0;

	NVIC_Configuration();
	delay_init();	    	 //延时函数初始化	  
	
	//OLED
	OLED_Init();

	OLED_ShowChinese(0, 0, "温度：");
	OLED_ShowChinese(94, 0, "℃");
	OLED_ShowChinese(0, 16, "心率：");
	OLED_ShowString(94,16,"BPM",OLED_8X16);
	OLED_ShowChinese(0, 32, "血氧：");
	OLED_ShowString(94,32,"%",OLED_8X16);
	OLED_ShowChinese(0, 48, "是否跌倒：否");
	OLED_Update();

	//MAX30102
	max30102_init();
	//MPU6050
	MPU6050_Init();
	//DS18B20
	ds18b20_init();
	//Buzzer
	Buzzer_init();

	
	MPU6050_GetData(&AX, &AY, &AZ, &GX, &GY, &GZ);
	Last_AX = AX;
	Last_AY = AY;
	
	un_min=0x3FFFF;
	un_max=0;
	
	n_ir_buffer_length=500; //buffer length of 100 stores 5 seconds of samples running at 100sps
	//read the first 500 samples, and determine the signal range
	for(i=0;i<n_ir_buffer_length;i++)
	{
			while(MAX30102_INT==1);   //wait until the interrupt pin asserts
			
	max30102_FIFO_ReadBytes(REG_FIFO_DATA,temp);
	aun_red_buffer[i] =  (long)((long)((long)temp[0]&0x03)<<16) | (long)temp[1]<<8 | (long)temp[2];    // Combine values to get the actual number
	aun_ir_buffer[i] = (long)((long)((long)temp[3] & 0x03)<<16) |(long)temp[4]<<8 | (long)temp[5];   // Combine values to get the actual number
					
			if(un_min>aun_red_buffer[i])
					un_min=aun_red_buffer[i];    //update signal min
			if(un_max<aun_red_buffer[i])
					un_max=aun_red_buffer[i];    //update signal max
	}
	un_prev_data=aun_red_buffer[i];
	//calculate heart rate and SpO2 after first 500 samples (first 5 seconds of samples)
	maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid); 
	
	while(1)
	{
			i=0;
			un_min=0x3FFFF;
			un_max=0;
		
			//dumping the first 100 sets of samples in the memory and shift the last 400 sets of samples to the top
			for(i=100;i<500;i++)
			{
					aun_red_buffer[i-100]=aun_red_buffer[i];
					aun_ir_buffer[i-100]=aun_ir_buffer[i];
					
					//update the signal min and max
					if(un_min>aun_red_buffer[i])
					un_min=aun_red_buffer[i];
					if(un_max<aun_red_buffer[i])
					un_max=aun_red_buffer[i];
			}
			//take 100 sets of samples before calculating the heart rate.
			for(i=400;i<500;i++)
			{
					un_prev_data=aun_red_buffer[i-1];
					while(MAX30102_INT==1);
					max30102_FIFO_ReadBytes(REG_FIFO_DATA,temp);
			aun_red_buffer[i] =  (long)((long)((long)temp[0]&0x03)<<16) | (long)temp[1]<<8 | (long)temp[2];    // Combine values to get the actual number
			aun_ir_buffer[i] = (long)((long)((long)temp[3] & 0x03)<<16) |(long)temp[4]<<8 | (long)temp[5];   // Combine values to get the actual number
			
					if(aun_red_buffer[i]>un_prev_data)
					{
							f_temp=aun_red_buffer[i]-un_prev_data;
							f_temp/=(un_max-un_min);
							f_temp*=MAX_BRIGHTNESS;
							n_brightness-=(int)f_temp;
							if(n_brightness<0)
									n_brightness=0;
					}
					else
					{
							f_temp=un_prev_data-aun_red_buffer[i];
							f_temp/=(un_max-un_min);
							f_temp*=MAX_BRIGHTNESS;
							n_brightness+=(int)f_temp;
							if(n_brightness>MAX_BRIGHTNESS)
									n_brightness=MAX_BRIGHTNESS;
					}
			//send samples and calculation result to terminal program through UART
			if(ch_hr_valid == 1 && n_heart_rate<120)//**/ ch_hr_valid == 1 && ch_spo2_valid ==1 && n_heart_rate<120 && n_sp02<101
			{
				dis_hr = n_heart_rate;
				dis_spo2 = n_sp02;
			}
			else
			{
				dis_hr = 0;
				dis_spo2 = 0;
			}
		}
		maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid);
		
		//MAX30102
		OLED_ShowNum(47,16,dis_hr,2,OLED_8X16);
		OLED_ShowNum(47,32,dis_spo2,2,OLED_8X16);
		
		//DS18B20
		Temp = get_tempetature()/16;
		OLED_ShowNum(47,0,Temp,2,OLED_8X16);
		
		Last_AX = AX;
		Last_AY = AY;
		//MPU6050  跌倒报警
		MPU6050_GetData(&AX, &AY, &AZ, &GX, &GY, &GZ);
		if (Last_AX - AX >=500 || AX - Last_AX >=500 || Last_AY - AY >=500 || AY - Last_AY >=500) {   //平放后，向上、向下、向左、向右翻超过500报警
			OLED_ShowChinese(0, 48, "是否跌倒：是");
			// 触发蜂鸣器响
			Buzzer_on();
		} else if (Temp > 32) {  
			OLED_ShowChinese(0, 48, "是否跌倒：否");
			// 触发蜂鸣器响
			Buzzer_on();
		} else {
			OLED_ShowChinese(0, 48, "是否跌倒：否");
			// 关闭蜂鸣器
			Buzzer_off();
		}
		OLED_Update();
	}
}

