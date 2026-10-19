#include "stm32f10x.h"
#include "CAN_Bus.h"
#include "FreeRTOS.h"
#include "task.h"

static void CAN_Bus_ConfigTestFilter(void)
{
    CAN_FilterInitTypeDef filter;

    /* 32 位过滤器：标准 ID 在位 31:21，IDE 在位 2，RTR 在位 1。 */
    filter.CAN_FilterNumber = 0;
    filter.CAN_FilterMode = CAN_FilterMode_IdMask;
    filter.CAN_FilterScale = CAN_FilterScale_32bit;
    filter.CAN_FilterIdHigh = (uint16_t)(CAN_BUS_TEST_ID << 5);
    filter.CAN_FilterIdLow = 0;
    filter.CAN_FilterMaskIdHigh = 0xFFE0;
    filter.CAN_FilterMaskIdLow = 0x0006; /* 同时要求标准帧、数据帧。 */
    filter.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0; // 选择 FIFO0 接收过滤通过的消息
    filter.CAN_FilterActivation = ENABLE;
    CAN_FilterInit(&filter);
}

CAN_BusInitResult CAN_Bus_Init(void)
{
    RCC_ClocksTypeDef clocks;
    GPIO_InitTypeDef gpio;
    CAN_InitTypeDef can;

    /* 不能只相信工程界面的晶振栏：按当前 RCC 寄存器核对 APB1。 */
    RCC_GetClocksFreq(&clocks);
    if (clocks.PCLK1_Frequency != 36000000UL) { // 检查 APB1 频率，如果实际 APB1 频率不对，那么最终 CAN 的通信速率也会不对。
        return CAN_BUS_INIT_CLOCK_ERROR;
    }

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);
    CAN_DeInit(CAN1); // 复位 CAN1 外设，把 CAN1 的相关寄存器恢复到默认复位状态，避免旧配置影响新的初始化。

    // 配置 CAN 的 TX 和 RX 引脚
    /* 默认映射 PA11/PA12，不做重映射，保留 OLED 的 PB8/PB9。 */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_12;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP; // 复用推挽输出，由 CAN 外设控制该引脚。
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);

    /* PA11 接 5V TJA1050 的 RXD，作为 FT 数字输入，不用内部上下拉。 */
    gpio.GPIO_Pin = GPIO_Pin_11;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; // 关闭内部上下拉，由 TJA1050 RXD 输出驱动。
    GPIO_Init(GPIOA, &gpio);

    CAN_StructInit(&can);
    can.CAN_TTCM = DISABLE; // 禁用时间触发模式。
    can.CAN_ABOM = ENABLE; // 总线关闭后允许自动恢复
    can.CAN_AWUM = DISABLE; // 禁用自动唤醒模式。
    can.CAN_NART = ENABLE;       /* ENABLE 的是“禁止自动重发”。 */
    can.CAN_RFLM = DISABLE; // 禁用接收 FIFO 锁定模式，不是禁用 FIFO。
    can.CAN_TXFP = DISABLE;      /* 后续发送按 ID 优先级仲裁。 */ // 标识符数值越小，仲裁优先级通常越高。
    can.CAN_Mode = CAN_Mode_Normal; // 正常通信模式
    can.CAN_SJW = CAN_SJW_1tq; // 发生重同步时，允许的最大时序调整宽度为 1 tq。
    can.CAN_BS1 = CAN_BS1_9tq;
    can.CAN_BS2 = CAN_BS2_2tq;
    can.CAN_Prescaler = 6;       /* 36MHz / [6 * (1 + 9 + 2)] = 500kbit/s。 */

    /* 标准库内部有有限轮询；失败交给界面报告，不在这里死循环。 */
    if (CAN_Init(CAN1, &can) != CAN_InitStatus_Success) {
        return CAN_BUS_INIT_CONTROLLER_ERROR;
    }

    CAN_Bus_ConfigTestFilter();
    /* S02.3 先用任务轮询，尚不启用 CAN 接收中断。 */
    return CAN_BUS_INIT_OK;
}

CAN_BusTxResult CAN_Bus_SendFrame(CanTxMsg *message)
{
    uint8_t mailbox;
    uint8_t status;
    TickType_t started;

    mailbox = CAN_Transmit(CAN1, message);
    if (mailbox == CAN_TxStatus_NoMailBox) {
        return CAN_BUS_TX_BUSY;
    }

    started = xTaskGetTickCount();
    for (;;) {
        status = CAN_TransmitStatus(CAN1, mailbox);
        if (status == CAN_TxStatus_Ok) {
            return CAN_BUS_TX_OK;
        }
        if (status == CAN_TxStatus_Failed) {
            return CAN_BUS_TX_FAILED;
        }
        /* 无符号时间差允许 Tick 回绕，不因无 ACK 永久等待。 */
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(20)) {
            CAN_CancelTransmit(CAN1, mailbox);
            return CAN_BUS_TX_TIMEOUT;
        }
        vTaskDelay(1); /* 等待期间让出 CPU；当前 1 tick = 1ms。 */
    }
}

uint8_t CAN_Bus_TryReceive(CanRxMsg *message)
{
    if (CAN_MessagePending(CAN1, CAN_FIFO0) == 0) {
        return 0;
    }
    CAN_Receive(CAN1, CAN_FIFO0, message); /* 内部已释放 FIFO 条目。 */
    return 1;
}
