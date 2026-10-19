#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"
#include "USART.h"
#include "OLED.h"
#include "CAN_Bus.h"
#include <stdio.h>

/* S02.3：观察该变量递增，可确认 A 固件和任务调度均已运行。
 * 这里只是启动诊断计数，不是 CAN 心跳或传感器采样次数。
 */
volatile uint32_t g_environment_alive = 0;
volatile uint32_t g_environment_tx_ok = 0;
volatile uint32_t g_environment_tx_errors = 0;

static char *Environment_TxResultText(CAN_BusTxResult result)
{
    switch (result) {
    case CAN_BUS_TX_OK: return "OK";
    case CAN_BUS_TX_FAILED: return "FAILED";
    case CAN_BUS_TX_BUSY: return "BUSY";
    case CAN_BUS_TX_TIMEOUT: return "TIMEOUT";
    default: return "UNKNOWN";
    }
}

static void Environment_StartupTask(void *argument)
{
    TickType_t last_wake;
    CAN_BusInitResult can_result;
    CAN_BusTxResult tx_result;
    CanTxMsg test_frame = {0};
    uint8_t sequence = 0;
    uint8_t i;
    char log[64];
    (void)argument;

    /* 与 B 相同，在调度器启动后使用现有 OLED 驱动。 */
    OLED_Init();
    OLED_Clear();
    OLED_ShowString(1, 1, "A S02.3 TX 581");
    OLED_ShowString(2, 1, "CAN: init...");
    OLED_ShowString(3, 1, "TX:00000 E:00000");
    OLED_ShowString(4, 1, "Alive:");
    OLED_ShowNum(4, 7, g_environment_alive, 5);

    can_result = CAN_Bus_Init();
    if (can_result == CAN_BUS_INIT_OK) {
        OLED_ShowString(2, 1, "CAN: init OK");
        USART_SendString("[A] CAN init OK 500k normal\r\n");
    } else if (can_result == CAN_BUS_INIT_CLOCK_ERROR) {
        OLED_ShowString(2, 1, "CAN: clock ERR");
        USART_SendString("[A] CAN init failed: PCLK1 must be 36MHz\r\n");
    } else {
        OLED_ShowString(2, 1, "CAN: init FAIL");
        USART_SendString("[A] CAN init failed: controller timeout\r\n");
    }
    USART_SendString("[A] startup task running\r\n");

    /* 独立的链路测试数据，不是温湿度业务帧。 */
    test_frame.StdId = CAN_BUS_TEST_ID;
    test_frame.IDE = CAN_Id_Standard;
    test_frame.RTR = CAN_RTR_Data;
    test_frame.DLC = 8;
    for (i = 0; i < 7; i++) {
        test_frame.Data[i] = (uint8_t)(i + 1);
    }

    last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
        g_environment_alive++;
        OLED_ShowNum(4, 7, g_environment_alive % 100000UL, 5);
        if (can_result == CAN_BUS_INIT_OK) {
            test_frame.Data[7] = sequence;
            tx_result = CAN_Bus_SendFrame(&test_frame);
            if (tx_result == CAN_BUS_TX_OK) {
                g_environment_tx_ok++;
            } else {
                g_environment_tx_errors++;
            }
            OLED_ShowNum(3, 4, g_environment_tx_ok % 100000UL, 5);
            OLED_ShowNum(3, 12, g_environment_tx_errors % 100000UL, 5);
            snprintf(log, sizeof(log), "[A] TX 581 seq=%u %s\r\n",
                     (unsigned int)sequence, Environment_TxResultText(tx_result));
            USART_SendString(log);
            sequence++; /* 每次尝试递增，失败也消耗序号。 */
        }
        USART_SendString("[A] alive\r\n");
    }
}

static void Environment_Fatal(char *message)
{
    USART_SendString(message);
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

int main(void)
{
    /* 4 位均用于抢占优先级；后续 CAN 中断沿用此分组。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    usart_Init();
    USART_SendString("[A] A_Environment S02.3 boot\r\n");

    if (xTaskCreate(Environment_StartupTask, "AStartup", 256,
                    NULL, 1, NULL) != pdPASS) {
        Environment_Fatal("[A] startup task create failed\r\n");
    }

    vTaskStartScheduler();
    Environment_Fatal("[A] scheduler stopped\r\n");
    return 0;
}
