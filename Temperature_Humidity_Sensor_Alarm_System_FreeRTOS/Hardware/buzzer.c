#include "stm32f10x.h"                  // Device header
/* CAN 升级 S01：管理端蜂鸣器信号迁到 PA7，释放 PA12 给 CAN_TX。 */
#define ALARM_BUZZER_PIN GPIO_Pin_7

void Buzzer_Init(void) {
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    /* 三脚低电平触发模块：先写高再切换输出，默认关闭。 */
    GPIO_SetBits(GPIOA, ALARM_BUZZER_PIN);
    GPIO_InitTypeDef p;
    p.GPIO_Mode = GPIO_Mode_Out_PP;
    p.GPIO_Pin = ALARM_BUZZER_PIN;
    p.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &p);
}

void Buzzer_on(void) {
    GPIO_WriteBit(GPIOA, ALARM_BUZZER_PIN, Bit_RESET); //低电平驱动（响）
}

void Buzzer_off(void) {
    GPIO_WriteBit(GPIOA, ALARM_BUZZER_PIN, Bit_SET); //关
}
