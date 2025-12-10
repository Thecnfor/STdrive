#ifndef __CONN_H
#define __CONN_H

#include "main.h"
#include "usart.h"
#include <stdbool.h>

/* ==========================================
 * 用户配置区域
 * 必选：配置 MQTT_UART_HANDLE 为 ESP8266 AT 指令串口（如 &huart1 或 &huart2）
 * 可选：配置 MQTT_LOG_UART_HANDLE
 * 作为日志输出串口；注释该宏即可禁用日志以节约带宽
 * 可选：若希望全自动后台服务，建议在工程中定义 `MQTT_TIM_HANDLE`
 * 指向一个定时器句柄 建议：日志串口与 AT 串口分离，避免输出与指令相互干扰
 * ========================================== */
#define MQTT_UART_HANDLE &huart1 /* 使用的串口句柄，例如 &huart1 或 &huart2 */
#define MQTT_LOG_UART_HANDLE &huart2 /* 日志配置（注释本宏可关闭日志） */
// #define MQTT_TIM_HANDLE         &htim3    /*
// 后台服务定时器（注释本宏可禁用定时驱动） */

#define WIFI_SSID ""
#define WIFI_PASSWORD ""

#define MQTT_BROKER ""
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID "xrak"
#define MQTT_KEEPALIVE 60

/* ==========================================
 * ESP8266 AT 指令配置
 * ========================================== */
#define AT_CMD_TIMEOUT_SHORT 200
#define AT_CMD_TIMEOUT_NORMAL 1000
#define AT_CMD_TIMEOUT_LONG 3000
#define AT_CMD_TIMEOUT_WIFI 10000

#define RX_BUFFER_SIZE 512 /* 接收缓冲区大小 */

/* ==========================================
 * MQTT 协议常量
 * ========================================== */
#define MQTT_PKT_CONNECT 0x10     /* 连接请求 */
#define MQTT_PKT_CONNACK 0x20     /* 连接确认 */
#define MQTT_PKT_PUBLISH 0x30     /* 发布消息 */
#define MQTT_PKT_PUBACK 0x40      /* 发布确认 */
#define MQTT_PKT_SUBSCRIBE 0x82   /* 订阅请求 (QoS 1) */
#define MQTT_PKT_SUBACK 0x90      /* 订阅确认 */
#define MQTT_PKT_UNSUBSCRIBE 0xA2 /* 取消订阅请求 */
#define MQTT_PKT_UNSUBACK 0xB0    /* 取消订阅确认 */
#define MQTT_PKT_PINGREQ 0xC0     /* 心跳请求 */
#define MQTT_PKT_PINGRESP 0xD0    /* 心跳响应 */
#define MQTT_PKT_DISCONNECT 0xE0  /* 断开连接 */

#define MQTT_PROTOCOL_NAME "MQTT"
#define MQTT_PROTOCOL_LEVEL 0x04     /* MQTT 3.1.1 */
#define MQTT_FLAG_CLEAN_SESSION 0x02 /* 清除会话标志 */

/* ==========================================
 * 公共接口函数
 * ========================================== */

/**
 * @brief MQTT 消息处理回调函数类型
 */
typedef void (*MQTT_MessageHandler)(const char *topic, const char *payload);

/**
 * @brief 一键启动 MQTT (初始化 + 入网 + TCP + CONNECT)
 * @details 初始化 ESP8266、配置 WiFi 并建立到服务器的 TCP 连接，随后发送 MQTT
 * CONNECT 完成会话建立。 使用方法： 1) 非 RTOS：在 main 初始化后调用一次
 * `MQTT_Start()`，随后在 while 循环中周期性调用 `MQTT_Service()`； 2)
 * RTOS：在已有任务中周期性调用 `MQTT_Service()`，或定义 `MQTT_TIM_HANDLE`
 * 由定时器中断驱动服务例程，无需手动轮询。 示例（非 RTOS）：
 *   // 初始化硬件...
 *   MQTT_Start();
 *   while (1) {
 *       MQTT_Service();
 *       if (MQTT_IsConnected()) {
 *           MQTT_Publish("test/status", "ok");
 *       }
 *       HAL_Delay(50);
 *   }
 * @return true 启动成功并进入已连接状态
 * @return false 任一阶段失败（AT、WiFi、TCP、MQTT）
 */
bool MQTT_Start(void);

/**
 * @brief MQTT 服务例程（非阻塞）
 * @details 放入 while 循环或定时器/任务中周期性调用（建议 50~200ms）。
 * 自动执行心跳与分阶段重连：仅补齐未就绪阶段（WiFi/TCP/MQTT
 * CONNECT），避免每次全重连。
 */
void MQTT_Service(void);

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
 * @brief 订阅主题并注册特定回调函数
 * @details 收到匹配该主题的消息时，将自动调用 registered handler。
 *          支持 MQTT 通配符 '+' 和 '#' 的匹配。
 *
 * @param topic 主题 (如 "sensor/temp" 或 "sensor/+")
 * @param handler 回调函数
 * @return true 订阅成功
 * @return false 失败（如列表已满）
 */
bool MQTT_SubscribeCallback(const char *topic, MQTT_MessageHandler handler);

/**
 * @brief 取消订阅主题
 *
 * @param topic 主题
 * @return true 取消订阅请求发送成功
 * @return false 发送失败
 */
bool MQTT_Unsubscribe(const char *topic);

/* ***************************无需调用******************************* */

/**
 * @brief 设置消息回调并启用回调式接收
 * @details 使用方法：
 * 1) 非 RTOS：
 *    - `MQTT_Start();`
 *    - `MQTT_SetMessageHandler(handler);`
 *    - `MQTT_Subscribe("your/topic");`
 *    - `while (1) { MQTT_Service(); HAL_Delay(50); }`
 * 2) RTOS：
 *    - `MQTT_SetMessageHandler(handler);`
 *    - 在已有任务中周期性调用 `MQTT_Service()`，或定义 `MQTT_TIM_HANDLE`
 * 由定时器中断驱动；
 *    - 在合适位置调用 `MQTT_Subscribe("your/topic");`
 * 3) 定时器驱动：若定义 `MQTT_TIM_HANDLE`，无需在主循环或任务中调用
 * `MQTT_Service`，回调同样生效。 注意：若改用“轮询式”接收（调用 `MQTT_Process`
 * 自行拉取消息），请不要调用本函数，避免两种模式混用。
 * @param handler 回调函数原型：`void handler(const char *topic, const char
 * *payload)`
 */
void MQTT_SetMessageHandler(MQTT_MessageHandler handler);

/**
 * @brief 不要直接调用！处理 MQTT 接收数据
 * @details 服务例程内部调用，用于解析传入的 MQTT
 * 包。若手动轮询接收，需在主循环中调用。
 *
 * @param topic [out] 输出主题缓冲区
 * @param topic_len 主题缓冲区大小
 * @param payload [out] 输出消息缓冲区
 * @param payload_len 消息缓冲区大小
 * @return true 收到新消息
 * @return false 无新消息
 */
bool MQTT_Process(char *topic, uint16_t topic_len, char *payload,
                  uint16_t payload_len);

/**
 * @brief 检查 MQTT 是否已连接
 * @return true 已建立 MQTT 会话
 * @return false 未连接或发送失败导致断开
 */
bool MQTT_IsConnected(void);

/**
 * @brief 发送心跳包 (PINGREQ)
 * @details 通常由服务例程在半个 keepalive 周期触发；也可手动调用以维持会话活性
 */
void MQTT_Heartbeat(void);

/**
 * @brief 快速测试 MQTT 完整功能 (连接 -> 订阅 -> 循环发布/接收)
 * @details 将此函数放在 main 函数的 while(1) 循环中调用
 */
void MQTT_Test_Run(void);

#endif /* __CONN_H */
