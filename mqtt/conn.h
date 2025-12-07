#ifndef __CONN_H
#define __CONN_H

#include "main.h"
#include "usart.h"
#include <stdbool.h>

/* ==========================================
 * 用户配置区域
 * ========================================== */
#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""

#define MQTT_BROKER     ""
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "xrak"
#define MQTT_KEEPALIVE  60

/* ==========================================
 * ESP8266 AT 指令配置
 * ========================================== */
#define AT_CMD_TIMEOUT_SHORT    200
#define AT_CMD_TIMEOUT_NORMAL   1000
#define AT_CMD_TIMEOUT_LONG     3000
#define AT_CMD_TIMEOUT_WIFI     10000

#define RX_BUFFER_SIZE          512     /* 接收缓冲区大小 */

/* ==========================================
 * MQTT 协议常量
 * ========================================== */
#define MQTT_PKT_CONNECT        0x10    /* 连接请求 */
#define MQTT_PKT_CONNACK        0x20    /* 连接确认 */
#define MQTT_PKT_PUBLISH        0x30    /* 发布消息 */
#define MQTT_PKT_PUBACK         0x40    /* 发布确认 */
#define MQTT_PKT_SUBSCRIBE      0x82    /* 订阅请求 (QoS 1) */
#define MQTT_PKT_SUBACK         0x90    /* 订阅确认 */
#define MQTT_PKT_PINGREQ        0xC0    /* 心跳请求 */
#define MQTT_PKT_PINGRESP       0xD0    /* 心跳响应 */
#define MQTT_PKT_DISCONNECT     0xE0    /* 断开连接 */

#define MQTT_PROTOCOL_NAME      "MQTT"
#define MQTT_PROTOCOL_LEVEL     0x04    /* MQTT 3.1.1 */
#define MQTT_FLAG_CLEAN_SESSION 0x02    /* 清除会话标志 */

/* ==========================================
 * 公共接口函数
 * ========================================== */

/**
 * @brief 一键启动 MQTT (初始化 + 连接)
 * @details 初始化 ESP8266 并连接到 MQTT 服务器
 * @return true 启动成功
 * @return false 启动失败
 */
bool MQTT_Start(void);

/**
 * @brief 检查 MQTT 是否已连接
 */
bool MQTT_IsConnected(void);

/**
 * @brief 发布消息
 * 
 * @param topic 主题
 * @param message 消息内容
 * @return true 发送成功
 * @return false 发送失败
 */
bool MQTT_Publish(const char *topic, const char *message);

/**
 * @brief 订阅主题
 * 
 * @param topic 主题
 * @return true 订阅请求发送成功
 * @return false 发送失败
 */
bool MQTT_Subscribe(const char *topic);

/**
 * @brief 发送心跳包
 */
void MQTT_Heartbeat(void);

/**
 * @brief 处理 MQTT 接收数据 (需要在主循环中调用)
 * 
 * @param topic [out] 输出主题缓冲区
 * @param topic_len 主题缓冲区大小
 * @param payload [out] 输出消息缓冲区
 * @param payload_len 消息缓冲区大小
 * @return true 收到新消息
 * @return false 无新消息
 */
bool MQTT_Process(char *topic, uint16_t topic_len, char *payload, uint16_t payload_len);

#endif /* __CONN_H */
