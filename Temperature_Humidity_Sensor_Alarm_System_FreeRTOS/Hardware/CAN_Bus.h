#ifndef CAN_BUS_H
#define CAN_BUS_H

#include "stm32f10x.h"

#define CAN_BUS_TEST_ID  ((uint16_t)0x581)

/* S02.2：只报告本地初始化结果，不代表对端在线或已完成收发。 */
typedef enum {
    CAN_BUS_INIT_OK = 0,
    CAN_BUS_INIT_CLOCK_ERROR, // 晶振错误。分频后不为 36MHz
    CAN_BUS_INIT_CONTROLLER_ERROR // 控制器错误
} CAN_BusInitResult;

typedef enum {
    CAN_BUS_TX_OK = 0,
    CAN_BUS_TX_FAILED,
    CAN_BUS_TX_BUSY,
    CAN_BUS_TX_TIMEOUT
} CAN_BusTxResult;

/* 复位后调用一次；CAN1 默认 PA11/PA12，PCLK1 必须为 36MHz。 */
CAN_BusInitResult CAN_Bus_Init(void);

/* 初始化成功后由唯一发送任务调用，合法帧提交后最多等待约 20ms。
 * 超时请求取消；成功仅表示硬件发完，不表示 B 应用已处理。
 */
CAN_BusTxResult CAN_Bus_SendFrame(CanTxMsg *message);

/* 单个接收者：有帧时取出并释放 FIFO0，返回 1；无帧立即返回 0。 */
uint8_t CAN_Bus_TryReceive(CanRxMsg *message);

#endif
