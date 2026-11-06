//
// Created by 32663 on 2026/5/15.
//

#ifndef TEMPLE_PROJECT_UI_H
#define TEMPLE_PROJECT_UI_H
void stop_ui(void);
void run_ui(uint8_t temp, uint8_t humi);
/* 没有可信数据时的运行页：数值位显示 --，第 4 行给出原因。
 * 单独一个函数，避免把占位 0 画成真实读数。
 */
void run_ui_invalid(char *reason);
void setting_menu_ui(void);
void setting_change_ui(void);
void setting_change_temp_ui(void);
void setting_change_humi_ui(void);
void setting_history_ui(void);
#endif //TEMPLE_PROJECT_UI_H
