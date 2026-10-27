#pragma once
#include <stdbool.h>
void ui_init(void);
void ui_update(void);              // call every ~300 ms from the LVGL task
void ui_goto(int page, bool animate);
int  ui_page_count(void);
int  ui_current_page(void);
