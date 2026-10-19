#include "stm32f10x.h"
#include <stdio.h>
/**
  * 函    数：USART串口初始化
  * 参    数：无
  * 返 回 值：无
  */
 void usart_Init(void) {

     //打开时钟
     RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
     RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

     GPIO_InitTypeDef s;
     s.GPIO_Mode = GPIO_Mode_AF_PP;
     s.GPIO_Speed = GPIO_Speed_50MHz;
     s.GPIO_Pin = GPIO_Pin_9; // PA 9
     GPIO_Init(GPIOA, &s);


     //初始化
     USART_InitTypeDef p;
     p.USART_BaudRate = 9600;
     p.USART_HardwareFlowControl = USART_HardwareFlowControl_None;//不需要硬件控制流
     p.USART_Mode = USART_Mode_Tx;//配置为发送模式，单片机发送数据至电脑，串口助手显示
     p.USART_Parity = USART_Parity_No;
     p.USART_StopBits = USART_StopBits_1;
     p.USART_WordLength = USART_WordLength_8b;
     USART_Init(USART1, &p);
     USART_Cmd(USART1, ENABLE);//启用USART1外设

 }
/**
  * 函    数：温湿度数据发送
  * 参    数：uint8_t temp，uint8_t humi
  * 返 回 值：无
  */
//void usart_send(uint8_t temp, uint8_t humi) {
//    // 发送温度
//    USART_SendData(USART1, temp);
//    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);

//    // 发送湿度
//    USART_SendData(USART1, humi);
//    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
//}
void usart_send(uint8_t temp, uint8_t humi) {
	
    char str[20];
    // 将温湿度格式化为字符串 "T:26 H:23\r\n"
    // \r\n 是换行符，让数据在串口助手里一行行显示
    int len = sprintf(str, "Temp:%d Humi:%d\r\n", temp, humi);
    
    for (int i = 0; i < len; i++) {
		
        USART_SendData(USART1, str[i]);
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
		
    }
	
}
/**
 * 函    数：USART_SendString
 * 功    能：通过串口发送一串字符（ASCII 字符串）
 * 参    数：str 指向要发送的字符串（必须以 '\0' 结尾）
 * 返 回 值：无
 */
void USART_SendString(char *str)
{
    while (*str != '\0')          // 只要还没到字符串结尾
    {
        USART_SendData(USART1, *str);   // 发送当前字符
        // 等待发送数据寄存器空（保证字符发完）
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        str++;                     // 指向下一个字符
    }
}
