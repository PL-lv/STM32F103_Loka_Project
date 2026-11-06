#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"
#include "USART.h"
#include "OLED.h"
#include "CAN_Bus.h"
#include "dht11.h"
#include "timer_delay.h"
#include <stdio.h>

/* S02.3：观察该变量递增，可确认 A 固件和任务调度均已运行。
 * 这里只是启动诊断计数，不是 CAN 心跳或传感器采样次数。
 */
volatile uint32_t g_environment_alive = 0;
volatile uint32_t g_environment_tx_ok = 0;
volatile uint32_t g_environment_tx_errors = 0;
/* 采样成功/失败次数：串口日志与调试器核对用；S03.2 起同时通过 0x181 上报。 */
volatile uint32_t g_environment_sample_ok = 0;
volatile uint32_t g_environment_sample_fail = 0;

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

/* 0x181：D0 版本、D1 采样尝试序号、D2 温度、D3 湿度、D4 结果、D5~D7 保留。
 * 失败时 D2/D3 必须置 0，不能把上一次读数当成真实环境。
 */
//  CAN 帧打包函数
static CAN_BusTxResult Environment_SendSampleFrame(uint8_t sequence, uint8_t ok,
                                                   uint8_t temperature, uint8_t humidity)
{
    CanTxMsg frame = {0}; // 准备一个空包裹，先把所有字节清0，避免残留脏数据。

    frame.StdId = CAN_BUS_SAMPLE_ID;   // 包裹编号 = 0x181
    frame.IDE   = CAN_Id_Standard;     // 用标准帧（不用扩展帧）
    frame.RTR   = CAN_RTR_Data;        // 这是数据帧，不是远程请求帧
    frame.DLC   = 8;                   // 包裹里装 8 个字节
    frame.Data[0] = CAN_BUS_PROTOCOL_VERSION; // 协议版本号
    frame.Data[1] = sequence; // 记录这是第几次采样
    frame.Data[2] = ok ? temperature : 0U;
    frame.Data[3] = ok ? humidity : 0U;
    frame.Data[4] = ok ? 0U : 1U; /* 0 有效，1 读取/校验失败。 */
    // Data[5]~Data[7] 保持 0，保留不用
    return CAN_Bus_SendFrame(&frame); // 真正发出去
}

/* 0x701：D0 版本、D1 心跳序号、D2 采样状态、D3 最近采样序号、D4~D7 运行毫秒。
 * 传感器一直失败也照常发心跳，B 才能区分“通信离线”和“传感器故障”。
 */
static CAN_BusTxResult Environment_SendHeartbeat(uint8_t sequence, uint8_t sample_state,
                                                 uint8_t last_sample_sequence,
                                                 uint32_t uptime_ms)
{
    CanTxMsg frame = {0};

    frame.StdId = CAN_BUS_HEARTBEAT_ID; // 编号 = 0x701
    frame.IDE = CAN_Id_Standard;
    frame.RTR = CAN_RTR_Data;
    frame.DLC = 8;
    frame.Data[0] = CAN_BUS_PROTOCOL_VERSION;
    frame.Data[1] = sequence;
    frame.Data[2] = sample_state;
    frame.Data[3] = last_sample_sequence;
    /* 运行时间按小端逐字节打包，不使用结构体内存布局。 */
    frame.Data[4] = (uint8_t)(uptime_ms & 0xFFU);
    frame.Data[5] = (uint8_t)((uptime_ms >> 8) & 0xFFU);
    frame.Data[6] = (uint8_t)((uptime_ms >> 16) & 0xFFU);
    frame.Data[7] = (uint8_t)((uptime_ms >> 24) & 0xFFU);
    return CAN_Bus_SendFrame(&frame);
}

/* 错误计数只在变化时上报：9600 波特下每秒刷一行会把有用信息冲掉。
 * 拔掉对端时这里能看到 TEC 涨到 128（错误被动）、255（bus-off），
 * 接回后 ABOM 自动恢复、TEC 归零——这是 S06 的核心观测点。
 */
static void Environment_ReportCanErrors(void)
{
    /* 用不可能出现的初值，强制上电第一轮必打一行。 */
    static CAN_BusErrorStatus last = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    CAN_BusErrorStatus now;
    char log[80];

    CAN_Bus_ReadErrorStatus(&now);
    if (now.tec == last.tec && now.rec == last.rec && now.lec == last.lec &&
        now.bus_off == last.bus_off && now.error_passive == last.error_passive) {
        return;
    }
    last = now;
    snprintf(log, sizeof(log),
             "[A] can tec=%u rec=%u lec=%u(%s) boff=%u epv=%u\r\n",
             (unsigned int)now.tec, (unsigned int)now.rec,
             (unsigned int)now.lec, CAN_Bus_ErrorCodeText(now.lec),
             (unsigned int)now.bus_off, (unsigned int)now.error_passive);
    USART_SendString(log);
}

static void Environment_StartupTask(void *argument)
{
    TickType_t last_wake;
    CAN_BusInitResult can_result;
    CAN_BusTxResult tx_result;
    uint8_t sample_sequence = 0;      /* 0x181 的采样尝试序号，失败也递增 */
    uint8_t last_sample_sequence = 0; /* 0x701 携带的最近一次尝试序号 */
    uint8_t heartbeat_sequence = 0;
    uint8_t sample_state = 0;         /* 0 等待首次采样，1 最近成功，2 最近失败 */
    uint8_t sample_ok = 0;
    uint8_t temperature = 0;
    uint8_t humidity = 0;
    uint8_t sample_toggle = 0; /* 每两次 1 秒循环取反一次，形成 2 秒采样周期 */
    char number_text[4];       /* 两位右对齐结果，留出结束符余量 */
    char log[64];
    (void)argument;

    /* 与 B 相同，在调度器启动后使用现有 OLED 驱动。 */
    OLED_Init();
    OLED_Clear();
    OLED_ShowString(1, 1, "A S06.1 181/701");
    OLED_ShowString(2, 1, "CAN: init...");
    /* 第 3 行改为本地采样结果，CAN 发送计数仍在后台统计和串口输出。 */
    OLED_ShowString(3, 1, "T:-- H:--");
    OLED_ShowString(4, 1, "Alive:");
    OLED_ShowNum(4, 7, g_environment_alive, 5);

    /* A 只发送，不做业务接收；过滤器保留 0x581 供后续反向实验。 */
    can_result = CAN_Bus_Init(CAN_BUS_FILTER_TEST_ONLY);
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
    USART_SendString("[A] startup task running\r\n"); // 准备进入循环

    last_wake = xTaskGetTickCount(); // 拿到了当前的系统节拍数
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000)); // 等到刚好1000个节拍，刚好1秒，再继续。
        g_environment_alive++;
        OLED_ShowNum(4, 7, g_environment_alive % 100000UL, 5);

        /* S03.2：每 2 秒尝试一次采样；失败也发 0x181，B 才能区分
         * “没有数据”和“有数据但传感器故障”。data_Check 内部有超时。
         */
        sample_toggle ^= 1U;
        if (sample_toggle == 0U) {
            sample_ok = (uint8_t)data_Check(&temperature, &humidity);
            last_sample_sequence = sample_sequence;
            sample_sequence++; /* 尝试序号每次递增，失败也消耗 */
            sample_state = sample_ok ? 1U : 2U;

            if (sample_ok) {
                g_environment_sample_ok++;
                snprintf(log, sizeof(log), "[A] DHT11 ok T=%u H=%u\r\n",
                         (unsigned int)temperature, (unsigned int)humidity);
                USART_SendString(log);
                /* 两位右对齐，个位数前面补空格，避免显示成 05 这种前导零。 */
                snprintf(number_text, sizeof(number_text), "%2u",
                         (unsigned int)temperature);
                OLED_ShowString(3, 3, number_text);
                snprintf(number_text, sizeof(number_text), "%2u",
                         (unsigned int)humidity);
                OLED_ShowString(3, 8, number_text);
            } else {
                g_environment_sample_fail++;
                USART_SendString("[A] DHT11 fail\r\n");
                /* 失败时保留位置显示 --，不把上一次数值当成当前值。 */
                OLED_ShowString(3, 3, "--");
                OLED_ShowString(3, 8, "--");
            }

            if (can_result == CAN_BUS_INIT_OK) {
                tx_result = Environment_SendSampleFrame(last_sample_sequence, sample_ok,
                                                        temperature, humidity);
                if (tx_result == CAN_BUS_TX_OK) {
                    g_environment_tx_ok++;
                } else {
                    g_environment_tx_errors++;
                }
                snprintf(log, sizeof(log), "[A] 181 seq=%u res=%u T=%u H=%u %s\r\n",
                         (unsigned int)last_sample_sequence,
                         (unsigned int)(sample_ok ? 0U : 1U),
                         (unsigned int)(sample_ok ? temperature : 0U),
                         (unsigned int)(sample_ok ? humidity : 0U),
                         Environment_TxResultText(tx_result));
                USART_SendString(log);
            }
        }

        /* 心跳每秒一次，独立于采样结果；传感器坏不等于节点离线。 */
        if (can_result == CAN_BUS_INIT_OK) {
            tx_result = Environment_SendHeartbeat(heartbeat_sequence, sample_state,
                                                  last_sample_sequence,
                                                  (uint32_t)(xTaskGetTickCount() *
                                                             portTICK_PERIOD_MS));
            if (tx_result == CAN_BUS_TX_OK) {
                g_environment_tx_ok++;
            } else {
                g_environment_tx_errors++;
            }
            snprintf(log, sizeof(log), "[A] 701 seq=%u state=%u %s\r\n",
                     (unsigned int)heartbeat_sequence, (unsigned int)sample_state,
                     Environment_TxResultText(tx_result));
            USART_SendString(log);
            heartbeat_sequence++;

            /* 第 2 行改为发送计数；初始化结果保留在启动阶段和串口日志里。 */
            OLED_ShowString(2, 1, "TX:");
            OLED_ShowNum(2, 4, g_environment_tx_ok % 100000UL, 5);
            OLED_ShowString(2, 10, "E:");
            OLED_ShowNum(2, 12, g_environment_tx_errors % 100000UL, 5);

            /* 每秒取一次错误快照，只在变化时打印。 */
            Environment_ReportCanErrors();
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
    /* DHT11 的电平等待用 TIM2 硬件延时，必须先初始化定时器再采样。
     * 两处都在调度器启动前完成，与旧单板 main.c 的顺序一致。
     */
    TIM2_Delay_Init();
    DHT11_Init();
    USART_SendString("[A] A_Environment S06.1 boot\r\n");

    // 创建一个任务，名字叫 AStartup，栈 256 个字，优先级 1。创建失败就进 Fatal。
    if (xTaskCreate(Environment_StartupTask, "AStartup", 256,
                    NULL, 1, NULL) != pdPASS) {
        Environment_Fatal("[A] startup task create failed\r\n");
    }

    vTaskStartScheduler(); // 启动调度器，开始运行任务,这个函数正常情况下永远不返回。
    Environment_Fatal("[A] scheduler stopped\r\n");
    return 0;
}
