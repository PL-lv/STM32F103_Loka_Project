#include "FreeRTOS.h"
#include "task.h"
#include "stm32f10x.h"
#include "USART.h"
#include "LED.h"
#include "buzzer.h"
#include "OLED.h"
#include "CAN_Bus.h"
#include "task_record.h"       /* vTask_Record：复用单板版的历史写入任务 */
#include "task_sensor.h"       /* SensorData_t 与 recordQueue 声明 */
#include "task_statemachine.h" /* historyMutex 声明 */
#include "Record_storage.h"    /* history_add / history_count / 环形缓冲 */
#include "UI.h"                /* run_ui / run_ui_invalid */
#include "Key.h"               /* Key_Init / Key_GetNum */
#include "task_key.h"          /* vTask_Key */
#include "task_alarm.h"        /* vTask_Alarm */
#include "timer_delay.h"       /* TIM2_Delay_Init：W25Q64 的短延时 */
#include "Threshold_storage.h" /* THRESHOLD_DEFAULT_TEMP / HUMI */
#include <stdio.h>

/* 两个超时故意不同，衡量的是两件事：
 * 心跳 1 秒一次，连续 3 次没来判“A 离线”（链路问题）；
 * 采样 2 秒一次，留一次丢帧余量，5 秒没有新样本才判“数据过期”（数据问题）。
 */
#define MANAGER_HEARTBEAT_TIMEOUT_MS 3000U
#define MANAGER_SAMPLE_STALE_MS      5000U

/* 采样序号是 8 位。跳变超过这个数说明不是“漏了几帧”，
 * 而是重启或长时间中断，不能计入漏帧统计，否则一次重启就把计数冲成大数。
 */
#define MANAGER_MAX_GAP 10U

/* 数据状态：屏幕第 2 行和串口都按这个分支。 */
#define MANAGER_DATA_NONE  0U /* 从未收到合法采样 */
#define MANAGER_DATA_OK    1U /* 最近成功且新鲜 */
#define MANAGER_DATA_FAIL  2U /* 最近这次读取失败 */
#define MANAGER_DATA_STALE 3U /* 数据过期或 A 已离线 */

/* 在线显示三态，与 g_manager_a_online 的 0/1 区分开：上电后未收到心跳是“未知”。 */
#define MANAGER_LINK_UNKNOWN 2U

/* Alive 是本地计数；其余计数只在帧通过校验后按规则递增。 */
volatile uint32_t g_manager_alive = 0;
volatile uint32_t g_manager_sample_count = 0;
volatile uint32_t g_manager_heartbeat_count = 0;
volatile uint32_t g_manager_rejected_frames = 0;
volatile uint32_t g_manager_duplicate_frames = 0;
volatile uint32_t g_manager_missed_samples = 0;
volatile uint32_t g_manager_a_restarts = 0;
volatile uint8_t g_manager_last_sample_sequence = 0;
volatile uint8_t g_manager_a_state = 0;
volatile uint32_t g_manager_a_uptime_ms = 0;
volatile uint8_t g_manager_a_online = 0;
volatile uint8_t g_manager_data_state = MANAGER_DATA_NONE;
volatile uint32_t g_manager_history_enqueued = 0;
volatile uint32_t g_manager_history_dropped = 0;

/* 单板版的历史链路原样复用：接收侧只负责入队，真正写缓冲区的是独立任务。
 * 这两个句柄在单板版里定义在 User/main.c，B 目标没有 main.c，所以在 B 的入口里定义。
 * 定义在这里而不是 task_record.c，是为了让目标入口掌握所有 FreeRTOS 资源的创建。
 */
QueueHandle_t recordQueue;
SemaphoreHandle_t historyMutex;

/* 按键队列与 OLED 锁：单板版定义在 main.c，B 目标不编译 main.c。
 * OLED 从此有两个使用者——本任务的运行页和状态机的菜单页，必须加锁。
 */
QueueHandle_t keyQueue;
SemaphoreHandle_t oledMutex;

/* 报警队列长度必须是 1：报警任务只关心“最新一次判断”，
 * 用 xQueueOverwrite 覆盖写，不会因为消费慢而堆积旧数据。
 */
QueueHandle_t alarmQueue;

/* B 上电直接进 RUN：管理端的存在意义就是显示 A 的数据。
 * 单板版默认 STOP 是因为要等用户确认，这里没有这个必要。
 */
volatile SystemState currentState = RUN;

/* 阈值和菜单索引在单板版里定义于 User/main.c，B 目标不编译 main.c。
 * 这里先给默认值，S05.3 接入 Flash 后改为上电读取。
 * 目前没有任何代码修改它们，等 S05.2 的按键任务接管。
 */
volatile uint8_t temp_threshold = THRESHOLD_DEFAULT_TEMP;
volatile uint8_t humi_threshold = THRESHOLD_DEFAULT_HUMI;
volatile uint8_t menu_index = 0;
volatile uint8_t threshold_menu_index = 0;

/* 逐项校验，任一项不符都算“收到了但不合格”，计入 rejected。
 * 除了帧头，还要校验内容：版本不符、结果位越界、量程越界、占位不为 0 都拒绝。
 */
static uint8_t Manager_IsSampleFrame(const CanRxMsg *message)
{
    if (message->IDE != CAN_Id_Standard || message->RTR != CAN_RTR_Data ||
        message->StdId != CAN_BUS_SAMPLE_ID || message->DLC != 8) {
        return 0;
    }
    if (message->Data[0] != CAN_BUS_PROTOCOL_VERSION) {
        return 0;
    }
    if (message->Data[4] > 1U) {
        return 0;
    }
    if (message->Data[4] == 0U) {
        /* 有效样本才查量程：DHT11 标称 0~50℃ / 20~90%RH，这里放宽到 60/100，
         * 只拦明显不可能的值，不替传感器做精度判断。
         */
        if (message->Data[2] > 60U || message->Data[3] > 100U) {
            return 0;
        }
    } else {
        /* 协议规定失败时 D2/D3 必须是占位 0，其他值说明发送端不守约定。 */
        if (message->Data[2] != 0U || message->Data[3] != 0U) {
            return 0;
        }
    }
    return 1;
}

static uint8_t Manager_IsHeartbeatFrame(const CanRxMsg *message)
{
    if (message->IDE != CAN_Id_Standard || message->RTR != CAN_RTR_Data ||
        message->StdId != CAN_BUS_HEARTBEAT_ID || message->DLC != 8) {
        return 0;
    }
    if (message->Data[0] != CAN_BUS_PROTOCOL_VERSION) {
        return 0;
    }
    return (uint8_t)(message->Data[2] <= 2U); /* 0 等待 / 1 成功 / 2 失败 */
}

/* D4~D7 小端拼回 uint32，与 A 端的逐字节打包严格对应。 */
static uint32_t Manager_ReadUptimeMs(const CanRxMsg *message)
{
    return ((uint32_t)message->Data[4]) |
           ((uint32_t)message->Data[5] << 8) |
           ((uint32_t)message->Data[6] << 16) |
           ((uint32_t)message->Data[7] << 24);
}

/* 状态由三件事共同决定：收没收到过、链路在不在、数据新不新。
 * 链路断了不能因为“时间还没到 5 秒”就继续显示 OK。
 */
static uint8_t Manager_DataState(uint8_t have_sample, uint8_t online,
                                 uint8_t last_result, TickType_t last_tick,
                                 TickType_t now)
{
    if (!have_sample) {
        return MANAGER_DATA_NONE;
    }
    if (!online) {
        return MANAGER_DATA_STALE;
    }
    if ((TickType_t)(now - last_tick) > pdMS_TO_TICKS(MANAGER_SAMPLE_STALE_MS)) {
        return MANAGER_DATA_STALE;
    }
    return (last_result == 0U) ? MANAGER_DATA_OK : MANAGER_DATA_FAIL;
}

/* 运行页：有可信数据就画数值，否则画 -- 并在第 4 行说明原因。
 * 本步只负责 RUN 页；页面切换由 S05.2 的状态机接管。
 */
static void Manager_ShowRunPage(uint8_t state, uint8_t temperature, uint8_t humidity)
{
    if (state == MANAGER_DATA_OK) {
        run_ui(temperature, humidity);
    } else if (state == MANAGER_DATA_FAIL) {
        run_ui_invalid("NO DATA: SENSOR");
    } else if (state == MANAGER_DATA_STALE) {
        run_ui_invalid("NO DATA: STALE ");
    } else {
        run_ui_invalid("WAITING FOR A   ");
    }
}

/* 每 10 秒打一行累积计数。S07 的长稳测试直接读这行，
 * 不用靠肉眼数屏幕；单位统一用 unsigned long（32 位平台上与 uint32_t 同宽）。
 */
static void Manager_ReportStats(void)
{
    char line[96];

    snprintf(line, sizeof(line),
             "[B] stat rx=%lu hb=%lu dup=%lu miss=%lu rej=%lu rst=%lu\r\n",
             (unsigned long)g_manager_sample_count,
             (unsigned long)g_manager_heartbeat_count,
             (unsigned long)g_manager_duplicate_frames,
             (unsigned long)g_manager_missed_samples,
             (unsigned long)g_manager_rejected_frames,
             (unsigned long)g_manager_a_restarts);
    USART_SendString(line);
}

/* 每 10 秒把环形缓冲按“最旧→最新”打一遍，用来证明写入顺序和覆盖都对。
 * 读取期间必须持有 historyMutex：写侧在另一个任务里，裸读可能拿到写了一半的内容。
 */
static void Manager_DumpHistory(void)
{
    char line[48];
    uint8_t count;
    uint8_t start;
    uint8_t index;
    uint8_t i;
    int length = 0;

    if (xSemaphoreTake(historyMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    count = history_count;
    /* 未写满时最旧的一条在下标 0；写满后 write_index 正好指向最旧的一条。 */
    start = (count < HISTORY_SIZE) ? 0U : write_index;
    for (i = 0; i < count; i++) {
        index = (uint8_t)((start + i) % HISTORY_SIZE);
        length += snprintf(line + length, sizeof(line) - (size_t)length,
                           "%u/%u ", (unsigned int)history_temp[index],
                           (unsigned int)history_humi[index]);
        if (length >= (int)sizeof(line)) {
            length = (int)sizeof(line) - 1;
            break;
        }
    }
    xSemaphoreGive(historyMutex);

    if (count == 0U) {
        return;
    }
    USART_SendString("[B] hist ");
    USART_SendString(line);
    USART_SendString("\r\n");
}

static void Manager_StartupTask(void *argument)
{
    TickType_t last_wake;
    TickType_t last_alive;
    TickType_t now;
    TickType_t last_sample_tick = 0;
    TickType_t last_heartbeat_tick = 0;
    TickType_t last_dump;
    CAN_BusInitResult can_result;
    CanRxMsg received;
    uint8_t i;
    uint8_t sample_changed;
    uint8_t sample_valid = 0;
    uint8_t last_result = 0;
    uint8_t last_seq = 0;
    uint8_t seq_valid = 0;
    uint8_t uptime_valid = 0;
    uint8_t heartbeat_seen = 0;
    uint8_t online;
    uint8_t data_state;
    uint8_t shown_state = 0xFF;
    uint8_t last_online = 0xFF;
    uint8_t last_alarm_state = 0xFF;
    SystemState last_state = RUN;
    uint32_t uptime;
    uint32_t last_uptime = 0;
    uint8_t temperature = 0;
    uint8_t humidity = 0;
    SensorData_t record_item;
    SensorData_t alarm_item;
    char log[64];
    (void)argument;

    /* 在调度器启动后初始化 OLED，沿用现有驱动和接线。 */
    OLED_Init();
    OLED_Clear();
    /* 上电先画“等 A”，收到第一帧合法数据后由循环切到数值页。
     * 屏幕从此有两个使用者，画之前先拿锁。
     */
    xSemaphoreTake(oledMutex, portMAX_DELAY);
    run_ui_invalid("WAITING FOR A   ");
    xSemaphoreGive(oledMutex);

    can_result = CAN_Bus_Init(CAN_BUS_FILTER_MANAGER);
    if (can_result == CAN_BUS_INIT_OK) {
        USART_SendString("[B] CAN init OK 500k normal\r\n");
    } else if (can_result == CAN_BUS_INIT_CLOCK_ERROR) {
        xSemaphoreTake(oledMutex, portMAX_DELAY);
        run_ui_invalid("CAN: CLOCK ERR ");
        xSemaphoreGive(oledMutex);
        /* 预置数据状态，避免主循环第一轮把错误页覆盖成“等 A”。 */
        shown_state = MANAGER_DATA_NONE;
        USART_SendString("[B] CAN init failed: PCLK1 must be 36MHz\r\n");
    } else {
        xSemaphoreTake(oledMutex, portMAX_DELAY);
        run_ui_invalid("CAN: INIT FAIL ");
        xSemaphoreGive(oledMutex);
        shown_state = MANAGER_DATA_NONE;
        USART_SendString("[B] CAN init failed: controller timeout\r\n");
    }
    USART_SendString("[B] startup task running\r\n");

    last_wake = xTaskGetTickCount();
    last_alive = last_wake;
    last_dump = last_wake;
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(10));
        sample_changed = 0;
        if (can_result == CAN_BUS_INIT_OK) {
            /* FIFO 深度为 3，限定单次处理量，不写无限收包循环。 */
            for (i = 0; i < 3; i++) {
                if (!CAN_Bus_TryReceive(&received)) {
                    break;
                }
                if (Manager_IsSampleFrame(&received)) {
                    uint8_t seq = received.Data[1];

                    /* 序号与上一条相同：重复帧，直接丢弃，不刷新任何时间戳。
                     * 刷新时间戳等于把重复帧当成新数据，数据年龄会永远年轻。
                     */
                    if (seq_valid && seq == last_seq) {
                        g_manager_duplicate_frames++;
                        snprintf(log, sizeof(log), "[B] dup seq=%u\r\n",
                                 (unsigned int)seq);
                        USART_SendString(log);
                        continue;
                    }

                    /* 序号不连续：先记漏帧。跳变过大视为重启或长时间中断，
                     * 只打日志，不计入漏帧数。
                     */
                    if (seq_valid) {
                        uint8_t gap = (uint8_t)(seq - last_seq - 1U);
                        if (gap > 0U && gap <= MANAGER_MAX_GAP) {
                            g_manager_missed_samples += gap;
                            snprintf(log, sizeof(log), "[B] gap=%u missed=%lu\r\n",
                                     (unsigned int)gap,
                                     (unsigned long)g_manager_missed_samples);
                            USART_SendString(log);
                        } else if (gap > MANAGER_MAX_GAP) {
                            snprintf(log, sizeof(log),
                                     "[B] seq jump %u->%u\r\n",
                                     (unsigned int)last_seq, (unsigned int)seq);
                            USART_SendString(log);
                        }
                    }
                    last_seq = seq;
                    seq_valid = 1;
                    g_manager_last_sample_sequence = seq;
                    g_manager_sample_count++;

                    last_result = received.Data[4];
                    last_sample_tick = xTaskGetTickCount();
                    if (last_result == 0U) {
                        temperature = received.Data[2];
                        humidity = received.Data[3];
                        /* 只有“本次读取成功”的样本才进历史：失败帧的 0 是占位值，
                         * 入历史会变成两条假的 0℃/0%RH。
                         * 队列满时超时写 0：宁可少存一条，也不拖慢接收。
                         */
                        record_item.temperature = temperature;
                        record_item.humidity = humidity;
                        if (xQueueSend(recordQueue, &record_item, 0) == pdPASS) {
                            g_manager_history_enqueued++;
                        } else {
                            g_manager_history_dropped++;
                        }
                    }
                    /* 报警只认“最近一次有效读数”：失败样本送 0，
                     * 让报警任务立刻停，而不是继续按上一次的高温鸣叫。
                     */
                    alarm_item.temperature = (last_result == 0U) ? temperature : 0U;
                    alarm_item.humidity = (last_result == 0U) ? humidity : 0U;
                    (void)xQueueOverwrite(alarmQueue, &alarm_item);

                    sample_valid = 1;
                    sample_changed = 1;
                    snprintf(log, sizeof(log), "[B] 181 seq=%u res=%u T=%u H=%u\r\n",
                             (unsigned int)seq,
                             (unsigned int)received.Data[4],
                             (unsigned int)received.Data[2],
                             (unsigned int)received.Data[3]);
                    USART_SendString(log);
                } else if (Manager_IsHeartbeatFrame(&received)) {
                    g_manager_heartbeat_count++;
                    g_manager_a_state = received.Data[2];
                    uptime = Manager_ReadUptimeMs(&received);
                    g_manager_a_uptime_ms = uptime;

                    /* 运行时间回退说明 A 重新上电；序号基线随即失效，
                     * 否则重启后的第一个低序号会被误判成重复帧。
                     * 已知局限：uptime 是 32 位毫秒，49.7 天回绕时也会走到这里。
                     */
                    if (uptime_valid && uptime < last_uptime) {
                        g_manager_a_restarts++;
                        seq_valid = 0;
                        snprintf(log, sizeof(log), "[B] A restarted up=%lu\r\n",
                                 (unsigned long)uptime);
                        USART_SendString(log);
                    }
                    last_uptime = uptime;
                    uptime_valid = 1;

                    heartbeat_seen = 1;
                    last_heartbeat_tick = xTaskGetTickCount();
                    snprintf(log, sizeof(log), "[B] 701 hb=%u state=%u t=%lu\r\n",
                             (unsigned int)received.Data[1],
                             (unsigned int)received.Data[2],
                             (unsigned long)uptime);
                    USART_SendString(log);
                } else {
                    g_manager_rejected_frames++;
                }
            }
        }

        now = xTaskGetTickCount();
        if (!heartbeat_seen) {
            online = MANAGER_LINK_UNKNOWN;
        } else if ((TickType_t)(now - last_heartbeat_tick) >
                   pdMS_TO_TICKS(MANAGER_HEARTBEAT_TIMEOUT_MS)) {
            online = 0;
        } else {
            online = 1;
        }

        if (online != last_online) {
            uint8_t previous = last_online;
            last_online = online;
            g_manager_a_online = (online == 1U) ? 1U : 0U;
            /* 只在“在线”和“离线”之间跳变时打日志，上电未知态不刷屏。
             * 屏幕不再单独留一行显示在线状态：离线时数据会直接变 OLD，
             * 用户看到的是“没数据”，不是“心跳没了”。
             */
            if (previous <= 1U && online <= 1U) {
                USART_SendString(online ? "[B] A online\r\n" : "[B] A offline\r\n");
            }
        }

        /* 只有“确认离线”才让数据降级；上电未收到心跳的未知态不算离线，
         * 否则刚上电那一秒会把明明是新鲜的样本显示成 OLD。
         */
        data_state = Manager_DataState(sample_valid, (uint8_t)(online != 0U),
                                       last_result, last_sample_tick, now);
        g_manager_data_state = data_state;

        /* 数据过期或 A 离线时取消阈值报警：值已经不可信，
         * 继续按它闪烁等于用旧数据报警。屏幕会同步显示原因。
         */
        if (data_state != last_alarm_state) {
            if (data_state != MANAGER_DATA_OK) {
                alarm_item.temperature = 0U;
                alarm_item.humidity = 0U;
                (void)xQueueOverwrite(alarmQueue, &alarm_item);
            }
            last_alarm_state = data_state;
        }

        /* 只有 RUN 页归本任务画；STOP 和菜单页由状态机任务独占屏幕。
         * 从菜单切回 RUN 时强制重画一次：否则要等到下一次采样（最多 2 秒）
         * 才恢复显示，中间屏幕上会一直留着菜单。
         */
        if (currentState == RUN &&
            (sample_changed || data_state != shown_state || last_state != RUN)) {
            xSemaphoreTake(oledMutex, portMAX_DELAY);
            Manager_ShowRunPage(data_state, temperature, humidity);
            xSemaphoreGive(oledMutex);
            shown_state = data_state;
        }
        last_state = currentState;

        if ((TickType_t)(now - last_alive) >= pdMS_TO_TICKS(1000)) {
            last_alive += pdMS_TO_TICKS(1000);
            g_manager_alive++;
            USART_SendString("[B] alive\r\n");

            /* 每 10 秒打一次计数与环形缓冲内容，用来核对链路与写入。 */
            if ((TickType_t)(now - last_dump) >= pdMS_TO_TICKS(10000)) {
                last_dump += pdMS_TO_TICKS(10000);
                Manager_ReportStats();
                Manager_DumpHistory();
            }
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
    Key_Init();        /* 5 个按键的 GPIO，调度器启动前完成 */
    TIM2_Delay_Init(); /* W25Q64 驱动用 TIM2 做短延时，保存阈值前必须可用 */
    USART_SendString("[B] B_Manager S05.4 boot\r\n");

    /* 上电恢复阈值。必须在调度器启动前做：Flash 驱动用 TIM2 等待，
     * 此时还不能调用 vTaskDelay。空白 Flash、校验失败或芯片没接，
     * ThresholdStore_Init 都会把默认值留在输出参数里，直接赋值即可。
     */
    {
        uint8_t loaded_temp = THRESHOLD_DEFAULT_TEMP;
        uint8_t loaded_humi = THRESHOLD_DEFAULT_HUMI;
        ThresholdStore_Result load_result;
        char message[100];

        load_result = ThresholdStore_Init(&loaded_temp, &loaded_humi);
        temp_threshold = loaded_temp;
        humi_threshold = loaded_humi;
        sprintf(message, "[B] flash id=%06lX load=%s driver=%u T=%u H=%u\r\n",
                (unsigned long)ThresholdStore_GetJedecID(),
                ThresholdStore_ResultText(load_result),
                (unsigned int)ThresholdStore_GetFlashResult(),
                (unsigned int)loaded_temp, (unsigned int)loaded_humi);
        USART_SendString(message);
    }

    /* 历史链路：互斥锁保护环形缓冲，队列把“接收”和“写入”解耦。
     * 先创建资源再建任务，避免任务第一次入队时句柄还是 NULL。
     */
    historyMutex = xSemaphoreCreateMutex();
    oledMutex = xSemaphoreCreateMutex();
    recordQueue = xQueueCreate(4, sizeof(SensorData_t));
    keyQueue = xQueueCreate(5, sizeof(uint8_t));
    /* 长度必须为 1：报警任务只保留最新一次判断，配合 xQueueOverwrite。 */
    alarmQueue = xQueueCreate(1, sizeof(SensorData_t));
    if (historyMutex == NULL || oledMutex == NULL || recordQueue == NULL ||
        keyQueue == NULL || alarmQueue == NULL) {
        Manager_Fatal("[B] queue/mutex create failed\r\n");
    }

    if (xTaskCreate(Manager_StartupTask, "BStartup", 256,
                    NULL, 1, NULL) != pdPASS) {
        Manager_Fatal("[B] startup task create failed\r\n");
    }
    if (xTaskCreate(vTask_Record, "BRecord", 128,
                    NULL, 1, NULL) != pdPASS) {
        Manager_Fatal("[B] record task create failed\r\n");
    }
    if (xTaskCreate(vTask_Key, "BKey", 128,
                    NULL, 1, NULL) != pdPASS) {
        Manager_Fatal("[B] key task create failed\r\n");
    }
    /* 报警与菜单同优先级：一次闪烁判断只有几个 GPIO 操作，不会拖慢按键响应。 */
    if (xTaskCreate(vTask_Alarm, "BAlarm", 128,
                    NULL, 2, NULL) != pdPASS) {
        Manager_Fatal("[B] alarm task create failed\r\n");
    }
    /* 菜单优先级高于接收任务：一次按键要立刻响应；
     * 接收任务靠 FIFO 队列吸收这几毫秒的延迟。
     */
    if (xTaskCreate(vTask_StateMachine, "BMenu", 256,
                    NULL, 2, NULL) != pdPASS) {
        Manager_Fatal("[B] menu task create failed\r\n");
    }

    vTaskStartScheduler();
    Manager_Fatal("[B] scheduler stopped\r\n");
    return 0;
}
