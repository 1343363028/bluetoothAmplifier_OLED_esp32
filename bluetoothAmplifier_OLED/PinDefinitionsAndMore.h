/*
 * PinDefinitionsAndMore.h — ESP32 平台引脚定义
 * 配合 IRremote 库使用，适配 MakePico ESP32-D0WDQ6 开发板
 */

#if defined(ESP32)
  // 红外接收管 OUT → GPIO15
  #define IR_RECEIVE_PIN          15

  // 红外发送引脚（本项目不使用红外发送，预留定义）
  #define IR_SEND_PIN             4

  // 禁用红外发送功能（本项目仅接收）
  #define DISABLE_IR_SEND

#else
  // 非ESP32平台的默认定义（备用）
  #define IR_RECEIVE_PIN          15
  #define IR_SEND_PIN             4

#endif