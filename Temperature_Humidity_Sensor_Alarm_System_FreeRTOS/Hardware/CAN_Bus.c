#include "stm32f10x.h"
#include "CAN_Bus.h"
#include "FreeRTOS.h"
#include "task.h"

// static 修饰函数，表示这个函数只能在当前 .c 文件中调用。
// 函数 CAN_Bus_ConfigTestFilter 用于配置 CAN 过滤器，只接收特定 ID 的消息。
static void CAN_Bus_ConfigTestFilter(void)
{
    CAN_FilterInitTypeDef filter;

    /* 32 位过滤器：标准 ID 在位 31:21，IDE 在位 2，RTR 在位 1。 */
    filter.CAN_FilterNumber = 0; // 一共 14 个过滤器，编号 0~13，这里选择编号 0 的过滤器
    filter.CAN_FilterMode = CAN_FilterMode_IdMask; // 掩码模式，掩码中的 1 表示必须比较，0 表示忽略这一位。
    // 32 位过滤器：标准 ID 在位 31:21，IDE 在位 2，RTR 在位 1。
    filter.CAN_FilterScale = CAN_FilterScale_32bit; // 32 位过滤器
//  标准 CAN ID 是 11 位，存放在 32 位过滤器的 bit 31～21。
//  而 CAN_FilterIdHigh 只代表整个 32 位过滤器的高 16 位。
//  所以，标准 ID 需要在这个 16 位字段中左移 5 位。
//  过滤失败后，B端的 CAN 控制器仍然可以在 ACK 槽发送显性位，确认物理总线上的报文被正确接收。
    filter.CAN_FilterIdHigh = (uint16_t)(CAN_BUS_TEST_ID << 5); // 过滤器预先配置的目标 ID
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
    uint8_t mailbox; // CAN 控制器有三个发送邮箱，返回值表示使用的邮箱编号。
    uint8_t status;
    TickType_t started;

    mailbox = CAN_Transmit(CAN1, message); // 发送消息，返回邮箱编号
    if (mailbox == CAN_TxStatus_NoMailBox) {
        return CAN_BUS_TX_BUSY; // 如果三个邮箱都无法提供空闲位置，就返回 CAN_BUS_TX_BUSY。
    }

    started = xTaskGetTickCount(); // 获取当前 tick 计数，用于超时判断
    for (;;) {
        status = CAN_TransmitStatus(CAN1, mailbox);
        if (status == CAN_TxStatus_Ok) {
            return CAN_BUS_TX_OK;
        }
        if (status == CAN_TxStatus_Failed) {
            return CAN_BUS_TX_FAILED;
        }
        /* 无符号时间差允许 Tick 回绕，不因无 ACK 永久等待。 */
        // pdMS_TO_TICKS(20) 负责把 20ms 转换成 FreeRTOS 的 Tick 数。
        if ((TickType_t)(xTaskGetTickCount() - started) >= pdMS_TO_TICKS(20)) {
            // 超时后取消发送，如果不执行取消发送操作，CAN 控制器最终还是会把消息发送出去
            // 这样软件显示和硬件最终收到的信息不符，导致业务混乱
            CAN_CancelTransmit(CAN1, mailbox);
            return CAN_BUS_TX_TIMEOUT;
        }
        vTaskDelay(1); /* 等待期间让出 CPU；当前 1 tick = 1ms。 */
    }
}

uint8_t CAN_Bus_TryReceive(CanRxMsg *message)
{
    // 检查 FIFO0 中是否有未处理的消息，没有就立刻返回 0，绝不阻塞
    if (CAN_MessagePending(CAN1, CAN_FIFO0) == 0) {
        return 0;
    }
    // 接收消息，参数指定从 FIFO0 接收，存放在 message 指向的结构体中
    CAN_Receive(CAN1, CAN_FIFO0, message); /* 内部已释放 FIFO 条目。 */
    return 1;
}
