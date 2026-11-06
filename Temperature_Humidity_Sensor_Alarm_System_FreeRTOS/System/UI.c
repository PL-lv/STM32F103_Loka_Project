#include "FreeRTOS.h"
#include <stdio.h>
#include "stm32f10x.h"
#include "oled.h"
#include "Record_storage.h"
#include "dht11.h"
#include "key.h"

extern uint8_t temperature, humidity;
extern volatile uint8_t temp_threshold, humi_threshold;
extern volatile uint8_t menu_index;
extern volatile uint8_t threshold_menu_index;
extern uint8_t keyNum;
extern uint8_t history_temp[];
extern uint8_t history_humi[];
extern uint8_t history_count;
uint8_t line_index = 1;

/* ========== 停滞界面 ========== */
void stop_ui(void) {

    OLED_ShowString(1, 1, "DHT11 Monitor");
    OLED_ShowString(2, 1, "State: STOP");
    OLED_ShowString(3, 1, "key1 to RUN");
    OLED_ShowString(4, 1, "key5 to setting");

}
/* ========== 运行界面 ========== */
void run_ui(uint8_t temp, uint8_t humi) {

    OLED_ShowString(1, 1, "DHT11 Monitor");
    OLED_ShowString(4, 1, "State: RUN  ");
    OLED_ShowString(2, 1, "Temp: ");
    OLED_ShowNum(2, 6, temp, 2);
    OLED_ShowChar(2, 8, 'C');
    OLED_ShowString(3, 1, "Humi: ");
    OLED_ShowNum(3, 6, humi, 2);
    OLED_ShowChar(3, 8, '%');

}
/* ========== 运行界面：无可信数据 ========== */
/* 数值位保持 --，不在屏幕上留下上一次的读数；第 4 行说明原因，
 * 让“传感器坏”“数据过期”“还没收到 A”三种情况在屏幕上有区别。
 */
void run_ui_invalid(char *reason) {

    OLED_ShowString(1, 1, "DHT11 Monitor");
    OLED_ShowString(2, 1, "Temp: --");
    OLED_ShowString(3, 1, "Humi: --");
    OLED_ShowString(4, 1, "                "); /* 先清整行，防止旧字符残留 */
    OLED_ShowString(4, 1, reason);
}
/* ========== 设置主菜单 ========== */

void setting_menu_ui(void) {
    char line2[17] = "  History        ";
    char line3[17] = "  Thresholds     ";
    char line4[17] = "  Back           ";//返回停止状态页面

    // 根据选中项绘制光标 '>'
    if (menu_index == 0)      line2[0] = '>';
    else if (menu_index == 1) line3[0] = '>';
    else if (menu_index == 2) line4[0] = '>';

    OLED_ShowString(1, 1, "Settings Menu  ");
    OLED_ShowString(2, 1, line2);
    OLED_ShowString(3, 1, line3);
    OLED_ShowString(4, 1, line4);
}
/* ========== 可更改选项展示页面 ========== */
void setting_change_ui(void) {
    char line2[17] = "  Temp           ";
    char line3[17] = "  Humi           ";
    char line4[17] = "  Back           ";

    if (threshold_menu_index == 0)      line2[0] = '>';
    else if (threshold_menu_index == 1) line3[0] = '>';
    else if (threshold_menu_index == 2) line4[0] = '>';

    OLED_ShowString(1, 1, "Set Thresholds  ");
    OLED_ShowString(2, 1, line2);
    OLED_ShowString(3, 1, line3);
    OLED_ShowString(4, 1, line4);
}
/* ========== 温度更改界面 ========== */
void setting_change_temp_ui(void) {
    OLED_ShowString(1, 1, "Set Temp Limit ");
    OLED_ShowString(2, 1, "Current: ");
    OLED_ShowNum(2, 9, temp_threshold, 2);
    OLED_ShowChar(2, 11, 'C');
    OLED_ShowString(3, 1, "UP/DN to change");
    OLED_ShowString(4, 1, "K5:Back and Save");
}
/* ========== 湿度更改界面 ========== */
void setting_change_humi_ui(void) {
    OLED_ShowString(1, 1, "Set Humi Limit ");
    OLED_ShowString(2, 1, "Current: ");
    OLED_ShowNum(2, 9, humi_threshold, 2);
    OLED_ShowChar(2, 11, '%');
    OLED_ShowString(3, 1, "UP/DN to change");
    OLED_ShowString(4, 1, "K5:Back and Save");
}
/* ========== 历史记录显示界面 ========== */
// 历史记录从环形缓冲区按时间顺序读取（最旧到最新），逐行显示
void setting_history_ui() {
    uint8_t i;
	
	// 空行填充
    if (history_count == 0) {
        for (i = 0; i < HISTORY_SIZE; i++) {
            OLED_ShowString(i + 1, 1, "                ");
        }
        return;
    }

    // 环形缓冲区读取：从 write_index（最旧）开始，顺时针绕回
    uint8_t start = (history_count < HISTORY_SIZE) ? 0 : write_index; //寻找最旧，没满最旧位置为0，满后。最旧位置为index

    for (i = 0; i < history_count; i++) {
        uint8_t idx = (start + i) % HISTORY_SIZE;
        OLED_ShowString(i + 1, 1, "T:");
        OLED_ShowNum(i + 1, 3, history_temp[idx], 2);
        OLED_ShowChar(i + 1, 5, 'C');
        OLED_ShowString(i + 1, 7, "H:");
        OLED_ShowNum(i + 1, 9, history_humi[idx], 2);
        OLED_ShowChar(i + 1, 11, '%');
    }

    // 空行填充
    for (i = history_count; i < HISTORY_SIZE; i++) {
        OLED_ShowString(i + 1, 1, "                ");
    }
}

