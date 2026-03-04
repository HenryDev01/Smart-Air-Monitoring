#pragma once
#include "esp_err.h"
#include <stdint.h>

// RGB565 color helpers
#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF

esp_err_t display_init(void);
void display_clear(uint16_t color);
void display_print(int x, int y, const char *text, uint16_t fg, uint16_t bg);