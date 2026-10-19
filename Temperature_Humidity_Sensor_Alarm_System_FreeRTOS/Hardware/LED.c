#include "stm32f10x.h"
#include "delay.h"

/* CAN 升级 S01：管理端报警 LED 迁到 PA6，释放 PA11 给 CAN_RX。 */
#define ALARM_LED_PIN GPIO_Pin_6

/**
  * 函    数：LED初始化
  * 参    数：无
  * 返 回 值：无
  */
void LED_Init(void)
{
    /*开启时钟*/
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);		//开启GPIOA的时钟

    /* 低电平点亮：先预置高电平，再配置输出，默认关闭。 */
    GPIO_SetBits(GPIOA, ALARM_LED_PIN);

    /*GPIO初始化*/
    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Pin = ALARM_LED_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure); // PA6 推挽输出
}

/**
  * 函    数：LED1开启
  * 参    数：无
  * 返 回 值：无
  */
void LED1_ON(void)
{
    GPIO_ResetBits(GPIOA, ALARM_LED_PIN); // PA6 低电平点亮
}

/**
  * 函    数：LED1关闭
  * 参    数：无
  * 返 回 值：无
  */
void LED1_OFF(void)
{
    GPIO_SetBits(GPIOA, ALARM_LED_PIN); // PA6 高电平关闭
}

/**
  * 函    数：LED1状态翻转
  * 参    数：无
  * 返 回 值：无
  */
void LED1_Turn(void)
{
    if (GPIO_ReadOutputDataBit(GPIOA, ALARM_LED_PIN) == 0)
    {
        GPIO_SetBits(GPIOA, ALARM_LED_PIN);
    }
    else													//否则，即当前引脚输出高电平
    {
        GPIO_ResetBits(GPIOA, ALARM_LED_PIN);
    }
}

/**
  * 函    数：LED2开启
  * 参    数：无
  * 返 回 值：无
  */
void LED2_ON(void)
{
    GPIO_ResetBits(GPIOA, GPIO_Pin_2);		//设置PA2引脚为低电平
}

/**
  * 函    数：LED2关闭
  * 参    数：无
  * 返 回 值：无
  */
void LED2_OFF(void)
{
    GPIO_SetBits(GPIOA, GPIO_Pin_2);		//设置PA2引脚为高电平
}

/**
  * 函    数：LED2状态翻转
  * 参    数：无
  * 返 回 值：无
  */
void LED2_Turn(void)
{
    if (GPIO_ReadOutputDataBit(GPIOA, GPIO_Pin_2) == 0)		//获取输出寄存器的状态，如果当前引脚输出低电平
    {
        GPIO_SetBits(GPIOA, GPIO_Pin_2);               		//则设置PA2引脚为高电平
    }
    else                                               		//否则，即当前引脚输出高电平
    {
        GPIO_ResetBits(GPIOA, GPIO_Pin_2);             		//则设置PA2引脚为低电平
    }
}

/**
  * 函    数：LED1闪烁
  * 参    数：无
  * 返 回 值：无
  */
// void LED1_flashing(void) {
//     //使用while1会让程序整体卡进led闪烁循环
//     while (1) {
//
//         LED1_ON();
//         Delay_ms(500);
//         LED1_OFF();
//         Delay_ms(500);
//     }
//
// }
