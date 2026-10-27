// Waveshare ESP32-S3-ETH (+ PoE module) with the 1.28" round touchscreen
#pragma once

// GC9A01 1.28" round LCD (SPI3) - NetAlly Buster original cable
#define LCD_CLK   38      // orange
#define LCD_MOSI  40      // white
#define LCD_MISO  39      // black (not used by the GC9A01 driver)
#define LCD_CS    42      // first purple
#define LCD_DC    41      // first yellow
#define LCD_RST   18      // brown
#define LCD_BL    15      // green
#define LCD_W     240
#define LCD_H     240
#define LCD_SPI_HZ (40 * 1000 * 1000)

// CST816S touch (I2C0)
#define TP_SDA    17      // grey
#define TP_SCL    16      // second blue
#define TP_INT    43      // second yellow (UART0 TX pin - console is on USB, so it's free)
#define TP_RST    44      // second purple (UART0 RX pin)
#define TP_ADDR   0x15

// W5500 Ethernet (SPI2) - on board
#define ETH_CLK   13
#define ETH_MISO  12
#define ETH_MOSI  11
#define ETH_CS    14
#define ETH_INT   10
#define ETH_RST   9

// GPIO45 / GPIO46 are strapping pins - leave unused

// ---- battery (optional, add later) ----
// 1-cell Li-ion -> 100k / 100k divider -> GPIO4 (ADC1). Turn it on in Settings > Battery.
#define BATT_ADC_GPIO   4
#define BATT_DIVIDER    2.0f    // (Rtop + Rbottom) / Rbottom
#define BATT_CHG_GPIO   5       // charger CHRG/STAT pin, active low (e.g. TP4056). -1 if not wired

