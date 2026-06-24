/*
 * ESP32 蓝牙音箱 + OLED + 彩色LED
 *
 * ★★★ 必须修改分区方案 ★★★
 *   Arduino IDE → 工具 → Partition Scheme →
 *   选择 "Huge APP (3MB No OTA/1MB SPIFFS)"
 *
 * 硬件连接：
 *   MAX98357A:  LRC→GP26, BCLK→GP25, DIN→GP27, VCC→5V, GND→GND
 *   OLED显示屏: SCK→GP22, SDA→GP21, VDD→5V, GND→GND
 *   WS2812B灯:  DI→GP2, VCC→5V, GND→GND
 *   红外接收管:  OUT→GP15, VCC→5V, GND→GND（10kΩ上拉）
 *   扬声器：接MAX98357A输出端（4Ω/3W）
 *
 * 红外遥控编码（NEC协议，已按参考遥控器调试）：
 *   0x43 → 播放/暂停    0x47 → CH+（同样触发播放/暂停）
 *   0x46 → 停止(CH-)
 *   0x15 → 音量+        0x07 → 音量-
 *   0x44 → 上一曲       0x40 → 下一曲
 *
 * OLED显示布局：
 *   未连接时：大字号居中"未连接"
 *   已连接时：
 *     第1行(y=14)：播放状态
 *     第2行(y=30)：歌手（Artist字段）
 *     第3行(y=46)：歌词（Title字段，超宽滚动）
 *     第4行(y=60)：音量进度条
 *
 * LED灯效：
 *   蓝色常亮  → 待机 (state=0)
 *   彩虹流动  → 播放中 (state=1)
 *   蓝色呼吸  → 暂停中 (state=2)
 *   绿色常亮  → 已停止 (state=3)
 *
 * 依赖库（库管理器搜索安装）：
 *   DFRobot_MAX98357A
 *   U8g2  （by oliver）
 *   ESP32_WS2812_Lib
 *   IRremote  （by Armin Joachimmeyer）
 */

#include <U8g2lib.h>
#include <Wire.h>

// SSD1306 128×64 I2C
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 22, 21);

#include <DFRobot_MAX98357A.h>
DFRobot_MAX98357A amplifier;

#include "ESP32_WS2812_Lib.h"
#define LEDS_COUNT  3
#define LEDS_PIN    2
#define CHANNEL     0
ESP32_WS2812 strip = ESP32_WS2812(LEDS_COUNT, LEDS_PIN, CHANNEL, TYPE_GRB);

#include "PinDefinitionsAndMore.h"
#include <IRremote.hpp>

// ===================== 全局变量 =====================
int state = 0;          // 0=待机 1=播放 2=暂停 3=停止
float volume = 5;       // 音量 0~10
bool connected = false;
String prevTitle = "";

// 滚动显示相关
unsigned long lastScrollTime = 0;
int scrollOffset = 0;
#define SCROLL_SPEED 3       // 每次移动像素数
#define SCROLL_INTERVAL 100  // 滚动间隔(ms)
#define SCROLL_GAP 24        // 文字重复间隔(像素)

// ===================== LED 灯效（非阻塞） =====================
// 动画状态变量
unsigned long lastLedUpdate = 0;
uint8_t rainbowHue = 0;      // 彩虹色相偏移 0~255
int8_t breathBright = 0;     // 呼吸亮度 0~50
int8_t breathDir = 1;        // 呼吸方向 +1=变亮 -1=变暗

#define LED_INTERVAL 20      // LED刷新间隔(ms)
#define LED_BRIGHT   30      // 常亮亮度
#define BREATH_MAX   50      // 呼吸最大亮度

void updateLED(void) {
  unsigned long now = millis();
  if (now - lastLedUpdate < LED_INTERVAL) return;
  lastLedUpdate = now;

  switch (state) {
    case 1: // 播放 → 彩虹流动
      strip.setBrightness(LED_BRIGHT);
      for (int i = 0; i < LEDS_COUNT; i++) {
        strip.setLedColorData(i,
          strip.Wheel((i * 256 / LEDS_COUNT + rainbowHue) & 255));
      }
      strip.show();
      rainbowHue += 2;  // 每帧色相偏移
      break;

    case 2: // 暂停 → 蓝色呼吸
      strip.setAllLedsColor(0, 0, 255);
      strip.setBrightness(breathBright);
      strip.show();
      breathBright += breathDir;
      if (breathBright >= BREATH_MAX) breathDir = -1;
      if (breathBright <= 0) breathDir = 1;
      break;

    case 3: // 停止 → 绿色常亮
      strip.setBrightness(LED_BRIGHT);
      strip.setAllLedsColor(0, 255, 0);
      strip.show();
      break;

    default: // 待机/未连接 → 蓝色常亮
      strip.setBrightness(LED_BRIGHT);
      strip.setAllLedsColor(0, 0, 255);
      strip.show();
      break;
  }
}

// ===================== OLED 滚动文字绘制 =====================
void drawScrollText(int y, const char* text, int &offset) {
  int textW = u8g2.getUTF8Width(text);
  if (textW <= 128) {
    int x = (128 - textW) / 2;
    u8g2.drawUTF8(x, y, text);
    offset = 0;
    return;
  }
  int totalDist = textW + SCROLL_GAP;
  int pos = offset % totalDist;
  u8g2.setClipWindow(0, y - 14, 128, y + 2);
  u8g2.drawUTF8(-pos, y, text);
  u8g2.drawUTF8(-pos + totalDist, y, text);
  u8g2.setMaxClipWindow();
}

// ===================== 音量条绘制 =====================
void drawVolumeBar(void) {
  const char* label = "音量";
  int labelW = u8g2.getUTF8Width(label);

  int barX = labelW + 2;
  int barW = 128 - barX - 2;
  int barH = 8;
  int barY = 53;

  u8g2.drawUTF8(0, 60, label);
  u8g2.drawFrame(barX, barY, barW, barH);

  int fillW = (int)(barW * volume / 10.0);
  if (fillW > 0) {
    u8g2.drawBox(barX + 1, barY + 1, fillW - 2, barH - 2);
  }
}

// ===================== OLED 整屏刷新 =====================
void updateDisplay(String title, String artist) {
  u8g2.clearBuffer();

  // 未连接时：大字号居中显示"未连接"
  if (!connected) {
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    const char* msg = "未连接";
    int w = u8g2.getUTF8Width(msg);
    int x = (128 - w) / 2;
    u8g2.drawUTF8(x, 38, msg);
    u8g2.sendBuffer();
    return;
  }

  // ---- 以下为已连接时的正常显示 ----
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);

  // 第1行(y=14)：状态
  switch (state) {
    case 1: u8g2.drawUTF8(0, 14, "播放中"); break;
    case 2: u8g2.drawUTF8(0, 14, "已暂停"); break;
    case 3: u8g2.drawUTF8(0, 14, "已停止"); break;
    default: u8g2.drawUTF8(0, 14, "蓝牙已连接"); break;
  }

  // 第2行(y=30)：歌手
  if (artist.length() > 0) {
    char artistBuf[97] = {0};
    artist.toCharArray(artistBuf, 97);
    int w = u8g2.getUTF8Width(artistBuf);
    int x = (w <= 128) ? (128 - w) / 2 : 0;
    u8g2.drawUTF8(x, 30, artistBuf);
  }

  // 第3行(y=46)：歌词（Title字段，音乐App实时推送）
  if (title.length() > 0) {
    char titleBuf[129] = {0};
    title.toCharArray(titleBuf, 129);
    drawScrollText(46, titleBuf, scrollOffset);
  }

  // 第4行(y=60)：音量条
  drawVolumeBar();

  u8g2.sendBuffer();
}

// ===================== 初始化 =====================
void setup(void) {
  Serial.begin(115200);

  // ---- OLED ----
  u8g2.begin();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setFontMode(1);
  u8g2.clearBuffer();
  u8g2.drawUTF8(10, 24, "蓝牙音箱");
  u8g2.drawUTF8(10, 44, "等待蓝牙...");
  u8g2.sendBuffer();

  // ---- 蓝牙音箱 ----
  while (!amplifier.begin(
    /*btName=*/"LSWNB666",
    /*bclk=*/GPIO_NUM_25,
    /*lrclk=*/GPIO_NUM_26,
    /*din=*/GPIO_NUM_27))
  {
    Serial.println("Initialize failed !");
    delay(3000);
  }
  Serial.println("Initialize succeed!");

  // ---- 红外 ----
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  Serial.println("IR receiver ready");

  // ---- 音量 ----
  amplifier.setVolume(5);

  // ---- LED ----
  strip.begin();
  strip.setBrightness(20);
  strip.setAllLedsColor(0, 0, 255);
  strip.show();
}

// ===================== 主循环 =====================
void loop(void) {
  // ---- 读取元数据 ----
  String Title = amplifier.getMetadata(ESP_AVRC_MD_ATTR_TITLE);
  String Artist = amplifier.getMetadata(ESP_AVRC_MD_ATTR_ARTIST);

  if (Title.length() > 0) {
    connected = true;
    Serial.print("Title: "); Serial.println(Title);
  }
  if (Artist.length() > 0) {
    Serial.print("Artist: "); Serial.println(Artist);
  }

  bool prevConnected = connected;

  uint8_t *addr = amplifier.getRemoteAddress();
  if (addr == NULL) {
    connected = false;
  }

  // ---- 蓝牙连接状态驱动LED ----
  if (!connected) {
    state = 0;  // 断开 → 待机
  } else if (prevConnected != connected) {
    // 刚连接上 → 默认播放
    state = 1;
  }
  // 有歌名数据且还在待机 → 说明音乐已在播放
  if (connected && Title.length() > 0 && state == 0) {
    state = 1;
  }

  // ---- 歌词变化时重置滚动 ----
  if (Title != prevTitle) {
    scrollOffset = 0;
    prevTitle = Title;
  }

  // ---- 红外遥控 ----
  if (IrReceiver.decode()) {
    if (IrReceiver.decodedIRData.protocol == UNKNOWN) {
      IrReceiver.resume();
    } else {
      IrReceiver.resume();

      switch (IrReceiver.decodedIRData.command) {

        // 0x43 / 0x47 → 播放/暂停（CH+ 同效）
        case 0x43:
        case 0x47:
          if (state == 1) {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE,
              ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE,
              ESP_AVRC_PT_CMD_STATE_RELEASED);
            state = 2;
            Serial.println("[IR] PAUSE");
          } else {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY,
              ESP_AVRC_PT_CMD_STATE_PRESSED);
            delay(50);
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY,
              ESP_AVRC_PT_CMD_STATE_RELEASED);
            state = 1;
            Serial.println("[IR] PLAY");
          }
          break;

        // 0x46 → 停止(CH-)
        case 0x46:
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_STOP,
            ESP_AVRC_PT_CMD_STATE_PRESSED);
          delay(50);
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_STOP,
            ESP_AVRC_PT_CMD_STATE_RELEASED);
          state = 3;
          Serial.println("[IR] STOP");
          break;

        // 0x15 → 音量+
        case 0x15:
          volume = min(volume + 2, 10.0f);
          amplifier.setVolume(volume);
          Serial.print("Vol+: "); Serial.println(volume);
          break;

        // 0x07 → 音量-
        case 0x07:
          volume = max(volume - 2, 0.0f);
          amplifier.setVolume(volume);
          Serial.print("Vol-: "); Serial.println(volume);
          break;

        // 0x44 → 上一曲
        case 0x44:
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD,
            ESP_AVRC_PT_CMD_STATE_PRESSED);
          delay(50);
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD,
            ESP_AVRC_PT_CMD_STATE_RELEASED);
          Serial.println("[IR] PREV");
          break;

        // 0x40 → 下一曲
        case 0x40:
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD,
            ESP_AVRC_PT_CMD_STATE_PRESSED);
          delay(50);
          esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD,
            ESP_AVRC_PT_CMD_STATE_RELEASED);
          Serial.println("[IR] NEXT");
          break;
      }
    }
  }

  // ---- 滚动推进 ----
  if (millis() - lastScrollTime > SCROLL_INTERVAL) {
    lastScrollTime = millis();
    scrollOffset += SCROLL_SPEED;
  }

  // ---- 刷新OLED ----
  updateDisplay(Title, Artist);

  // ---- LED灯效 ----
  updateLED();
}
