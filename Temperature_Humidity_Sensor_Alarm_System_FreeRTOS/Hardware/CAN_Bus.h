#ifndef CAN_BUS_H
#define CAN_BUS_H

#include "stm32f10x.h"

#define CAN_BUS_TEST_ID  ((uint16_t)0x581)

/* 协议 V1：0x181 采样结果，0x701 心跳；0x581 仅保留作后续反向/过滤实验。
 * 多字节字段逐字节打包，不直接发送结构体内存。
 */
#define CAN_BUS_SAMPLE_ID        ((uint16_t)0x181) // 包裹编号
#define CAN_BUS_HEARTBEAT_ID     ((uint16_t)0x701)
#define CAN_BUS_PROTOCOL_VERSION ((uint8_t)1)

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

/* 接收过滤器模式：A/B 共用同一份驱动，收哪些 ID 由调用方决定。
 * 0x581 只在联调阶段使用，业务阶段 B 只认 0x181/0x701。
 */
typedef enum {
    CAN_BUS_FILTER_TEST_ONLY = 0, /* 只放行 0x581 */
    CAN_BUS_FILTER_MANAGER        /* 放行 0x181 与 0x701 */
} CAN_BusFilterMode;

/* 复位后调用一次；CAN1 默认 PA11/PA12，PCLK1 必须为 36MHz。
 * 过滤器模式同时在这里生效，初始化后再改需要重新进入过滤器初始化模式。
 */
CAN_BusInitResult CAN_Bus_Init(CAN_BusFilterMode mode);

/* 初始化成功后由唯一发送任务调用，合法帧提交后最多等待约 20ms。
 * 超时请求取消；成功仅表示硬件发完，不表示 B 应用已处理。
 */
CAN_BusTxResult CAN_Bus_SendFrame(CanTxMsg *message);

/* 单个接收者：有帧时取出并释放 FIFO0，返回 1；无帧立即返回 0。 */
uint8_t CAN_Bus_TryReceive(CanRxMsg *message);

/* CAN 错误状态快照：用来解释“为什么发不出去”，而不是只看成功/失败。
 * tec 超过 127 进入错误被动，超过 255 进入 bus-off（ABOM 已开启，会自动恢复）。
 */
typedef struct {
    uint8_t tec;           /* 发送错误计数 */
    uint8_t rec;           /* 接收错误计数 */
    uint8_t lec;           /* 最近一次错误类型，0 表示无错误 */
    uint8_t bus_off;       /* 1 = 当前处于 bus-off */
    uint8_t error_passive; /* 1 = 当前处于错误被动 */
} CAN_BusErrorStatus;

/* 只读快照，不改变控制器状态；可在任意时刻调用。 */
void CAN_Bus_ReadErrorStatus(CAN_BusErrorStatus *status);

/* LEC 数值转短文本，串口日志用。 */
char *CAN_Bus_ErrorCodeText(uint8_t lec);

#endif
