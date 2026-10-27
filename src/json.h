#pragma once
#include <stddef.h>
#include <stdbool.h>
int json_link(char *b, size_t n);
int json_switch(char *b, size_t n);
int json_wifi(char *b, size_t n, int maxaps);
int json_test(char *b, size_t n, bool info);
int json_device(char *b, size_t n);
int json_gui(char *b, size_t n);
int json_summary(char *b, size_t n);
int json_mqtt(char *b, size_t n);
int json_alerts(char *b, size_t n);
int json_quality(char *b, size_t n);
