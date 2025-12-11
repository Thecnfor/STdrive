#include "conn.h" /* 获取配置宏 */
#include "main.h"
#include "usart.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


/* 使用 conn.h 中定义的句柄 */
#define AT_UART MQTT_UART_HANDLE
#define LOG_UART MQTT_LOG_UART_HANDLE

/* 简单的日志输出 */
static void AT_Log(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  HAL_UART_Transmit(LOG_UART, (uint8_t *)buf, strlen(buf), 100);
}

/* 简单的 AT 发送与响应检查 */
static bool AT_Cmd(const char *cmd, const char *expected, uint32_t timeout) {
  char rx_buf[256] = {0};
  uint16_t idx = 0;
  uint32_t start = HAL_GetTick();

  /* 发送指令 */
  if (cmd) {
    AT_Log("[TX] %s", cmd);
    HAL_UART_Transmit(AT_UART, (uint8_t *)cmd, strlen(cmd), 100);
  }

  /* 接收响应 */
  while (HAL_GetTick() - start < timeout) {
    uint8_t ch;
    if (HAL_UART_Receive(AT_UART, &ch, 1, 1) == HAL_OK) {
      if (idx < sizeof(rx_buf) - 1) {
        rx_buf[idx++] = ch;
        rx_buf[idx] = 0;
      }
      /* 检查是否包含期望字符串 */
      if (expected && strstr(rx_buf, expected)) {
        AT_Log("[RX] %s\r\n", rx_buf);
        return true;
      }
    }
  }

  AT_Log("[RX Timeout/Fail] %s\r\n", rx_buf);
  return false;
}

/**
 * @brief 使用 ESP-AT 固件内置的 MQTT 指令进行测试
 * @details 仅适用于支持 AT+MQTTxxx 指令的固件版本
 */
void AT_MQTT_Test_Run(void) {
  char cmd[256];

  AT_Log("\r\n=== 开始 AT MQTT 原生指令测试 ===\r\n");

  /* 1. 基础检查 */
  AT_Cmd("AT\r\n", "OK", 1000);
  AT_Cmd("AT+CWMODE=1\r\n", "OK", 1000);

  /* 2. 连接 WiFi */
  AT_Log("正在连接 WiFi: %s...\r\n", WIFI_SSID);
  snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID,
           WIFI_PASSWORD);
  if (!AT_Cmd(cmd, "OK", 15000)) {
    AT_Log("WiFi 连接失败，请检查账号密码\r\n");
    // 即使失败也继续尝试，可能已经连上了
  }

  /* 3. 配置 MQTT User (LinkID=0, Scheme=1(TCP), ClientID, User, Pass, 0, 0, "")
   */
  /* 注意：AT+MQTTUSERCFG=<LinkID>,<Scheme>,<"ClientID">,<"Username">,<"Password">,<CertID>,<CAID>,<"Path">
   */
  snprintf(cmd, sizeof(cmd), "AT+MQTTUSERCFG=0,1,\"%s\",\"\",\"\",0,0,\"\"\r\n",
           MQTT_CLIENT_ID);
  if (!AT_Cmd(cmd, "OK", 2000)) {
    AT_Log("MQTT 配置失败 (可能固件不支持 AT+MQTT 指令)\r\n");
    return;
  }

  /* 4. 连接 MQTT Broker */
  AT_Log("正在连接 Broker: %s...\r\n", MQTT_BROKER);
  snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%d,1\r\n", MQTT_BROKER,
           MQTT_PORT);
  if (!AT_Cmd(cmd, "OK", 5000)) {
    AT_Log("MQTT 连接失败\r\n");
  } else {
    AT_Log("MQTT 连接成功！\r\n");
  }

  /* 5. 订阅主题 */
  AT_Cmd("AT+MQTTSUB=0,\"LED\",1\r\n", "OK", 2000);

  /* 6. 循环发布与监听 */
  while (1) {
    /* 发布心跳 */
    AT_Cmd("AT+MQTTPUB=0,\"test/status\",\"alive\",1,0\r\n", "OK", 1000);

    /* 监听串口输出 (简单打印接收到的数据) */
    /* AT 固件收到消息格式通常为: +MQTTSUBRECV:0,"topic",5,hello */
    uint8_t ch;
    if (HAL_UART_Receive(AT_UART, &ch, 1, 100) == HAL_OK) {
      HAL_UART_Transmit(LOG_UART, &ch, 1, 100);
    }

    HAL_Delay(1000);
  }
}
