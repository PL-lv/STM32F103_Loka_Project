#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"
#include "USART.h"
#include "LED.h"
#include "buzzer.h"
#include "OLED.h"
#include "CAN_Bus.h"
#include <stdio.h>

/* Alive 是本地计数，RX 只在真正收到并校验测试帧后递增。 */
volatile uint32_t g_manager_alive = 0;
volatile uint32_t g_manager_rx_count = 0;
volatile uint32_t g_manager_rejected_frames = 0;
volatile uint8_t g_manager_last_sequence = 0;

static uint8_t Manager_IsTestFrame(const CanRxMsg *message)
{
    uint8_t i;

    if (message->IDE != CAN_Id_Standard || message->RTR != CAN_RTR_Data ||
        message->StdId != CAN_BUS_TEST_ID || message->DLC != 8) {
        return 0;
    }
    for (i = 0; i < 7; i++) {
        if (message->Data[i] != (uint8_t)(i + 1)) {
            return 0;
        }
    }
    return 1;
}

static void Manager_StartupTask(void *argument)
{
    TickType_t last_wake;
    TickType_t last_alive;
    TickType_t now;
    CAN_BusInitResult can_result;
    CanRxMsg received;
    uint8_t i;
    uint8_t rx_changed;
    char log[64];
    (void)argument;

    /* 在调度器启动后初始化 OLED，沿用现有驱动和接线。 */
    OLED_Init();
    OLED_Clear();
    OLED_ShowString(1, 1, "B S02.3 RX 581");
    OLED_ShowString(2, 1, "CAN: init...");
    OLED_ShowString(3, 1, "RX:00000 S:---");
    OLED_ShowString(4, 1, "Alive:");
    OLED_ShowNum(4, 7, g_manager_alive, 5);

    can_result = CAN_Bus_Init();
    if (can_result == CAN_BUS_INIT_OK) {
        OLED_ShowString(2, 1, "CAN: init OK");
        USART_SendString("[B] CAN init OK 500k normal\r\n");
    } else if (can_result == CAN_BUS_INIT_CLOCK_ERROR) {
        OLED_ShowString(2, 1, "CAN: clock ERR");
        USART_SendString("[B] CAN init failed: PCLK1 must be 36MHz\r\n");
    } else {
        OLED_ShowString(2, 1, "CAN: init FAIL");
        USART_SendString("[B] CAN init failed: controller timeout\r\n");
    }
    USART_SendString("[B] startup task running\r\n");

    last_wake = xTaskGetTickCount();
    last_alive = last_wake;
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
        rx_changed = 0;
        if (can_result == CAN_BUS_INIT_OK) {
            /* FIFO 深度为 3，限定单次处理量，不写无限收包循环。 */
            for (i = 0; i < 3; i++) {
                if (!CAN_Bus_TryReceive(&received)) {
                    break;
                }
                if (!Manager_IsTestFrame(&received)) {
                    g_manager_rejected_frames++;
                    continue;
                }
                g_manager_rx_count++;
                g_manager_last_sequence = received.Data[7];
                rx_changed = 1;
                snprintf(log, sizeof(log), "[B] RX 581 seq=%u count=%lu\r\n",
                         (unsigned int)g_manager_last_sequence,
                         (unsigned long)g_manager_rx_count);
                USART_SendString(log);
            }
        }
        if (rx_changed) {
            OLED_ShowNum(3, 4, g_manager_rx_count % 100000UL, 5);
            OLED_ShowNum(3, 12, g_manager_last_sequence, 3);
        }
        now = xTaskGetTickCount();
        if ((TickType_t)(now - last_alive) >= pdMS_TO_TICKS(1000)) {
            last_alive += pdMS_TO_TICKS(1000);
            g_manager_alive++;
            OLED_ShowNum(4, 7, g_manager_alive % 100000UL, 5);
            USART_SendString("[B] alive\r\n");
        }
    }
}

static void Manager_Fatal(char *message)
{
    USART_SendString(message);
    LED1_ON();
    Buzzer_off();
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    usart_Init();
    LED_Init();
    Buzzer_Init();
    LED1_OFF();
    Buzzer_off();
    USART_SendString("[B] B_Manager S02.3 boot\r\n");

    if (xTaskCreate(Manager_StartupTask, "BStartup", 256,
                    NULL, 1, NULL) != pdPASS) {
        Manager_Fatal("[B] startup task create failed\r\n");
    }

    vTaskStartScheduler();
    Manager_Fatal("[B] scheduler stopped\r\n");
    return 0;
}
