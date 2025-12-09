#ifndef __CONN_H
#define __CONN_H

#include "main.h"
#include "usart.h"
#include <stdbool.h>

/* ==========================================
 * 用户配置区域
 * 必选：配置 MQTT_UART_HANDLE 为 ESP8266 AT 指令串口（如 &huart1 或 &huart2）
 * 可选：配置 MQTT_LOG_UART_HANDLE 作为日志输出串口；注释该宏即可禁用日志以节约带宽
 * 可选：若希望全自动后台服务，建议在工程中定义 `MQTT_TIM_HANDLE` 指向一个定时器句柄
 * 建议：日志串口与 AT 串口分离，避免输出与指令相互干扰
 * ========================================== */
#define MQTT_UART_HANDLE        &huart1   /* 使用的串口句柄，例如 &huart1 或 &huart2 */
#define MQTT_LOG_UART_HANDLE    &huart2   /* 日志配置（注释本宏可关闭日志） */
#define MQTT_TIM_HANDLE         &htim3    /* 后台服务定时器（注释本宏可禁用定时驱动） */

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

 

/**
 * @brief 快速测试 MQTT 完整功能 (连接 -> 订阅 -> 循环发布/接收)
 * @details 将此函数放在 main 函数的 while(1) 循环中调用
 */
void MQTT_Test_Run(void);

#endif /* __CONN_H */
