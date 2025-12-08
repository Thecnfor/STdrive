#ifndef __CONN_H
#define __CONN_H

#include "main.h"
#include "usart.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * ============================================================================
 *                              MQTT 客户端快速上手指南
 * ============================================================================
 * 
 * 1. 硬件连接:
 *    - ESP8266 TX -> STM32 RX (对应 MQTT_UART_HANDLE)
 *    - ESP8266 RX -> STM32 TX (对应 MQTT_UART_HANDLE)
 *    - ESP8266 RST -> 3.3V (或 GPIO 控制复位)
 *    - ESP8266 CH_PD -> 3.3V
 *    - GND 共地
 * 
 * 2. 软件配置 (下方 "用户配置区域"):
 *    - 修改 [必填] WIFI_SSID 和 WIFI_PASSWORD
 *    - 确认 MQTT_UART_HANDLE 对应的串口句柄 (如 &huart1)
 *    - (可选) 修改 MQTT 服务器信息 (默认使用 EMQX 公共服务器测试)
 * 
 * 3. 代码调用示例 (main.c):
 *    
 *    // 1. 包含头文件
 *    #include "conn.h"
 * 
 *    // 2. 定义回调函数
 *    void OnMessage(const char *topic, const uint8_t *payload, uint16_t len) {
 *        printf("收到消息: 主题=%s, 内容=%.*s\r\n", topic, len, payload);
 *    }
 * 
 *    int main(void) {
 *        HAL_Init();
 *        SystemClock_Config();
 *        MX_USART1_UART_Init();
 *        MX_USART2_UART_Init(); // 调试串口
 * 
 *        // 3. 初始化 MQTT
 *        MQTT_Init();
 *        MQTT_SetCallback(OnMessage);
 * 
 *        while (1) {
 *            // 4. 主循环必须调用
 *            MQTT_Run();
 * 
 *            // 5. 检查连接状态并发送数据
 *            if (MQTT_GetState() == MQTT_STATE_CONNECTED) {
 *                static uint32_t last_pub = 0;
 *                if (HAL_GetTick() - last_pub > 5000) {
 *                    MQTT_Publish("test/topic", "Hello STM32", false);
 *                    last_pub = HAL_GetTick();
 *                }
 *            }
 *        }
 *    }
 * ============================================================================
 */

/* ==========================================
 * 用户配置区域 (请根据实际情况修改)
 * ========================================== */

/* --- 1. 串口配置 --- */
/* [必填] 连接 ESP8266 的串口句柄 (需在 usart.h 中定义) */
#define MQTT_UART_HANDLE    &huart1  

/* [可选] 调试日志输出串口 (如不需要日志可注释掉 MQTT_DEBUG_ENABLE) */
#define MQTT_DEBUG_UART     &huart2  

/* --- 2. WiFi 配置 --- */
/* [必填] WiFi 名称和密码 */
#define WIFI_SSID           "YOUR_WIFI_SSID"      /* 请修改这里 */
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"  /* 请修改这里 */

/* --- 3. MQTT 服务器配置 --- */
/* [可选] 默认使用 EMQX 公共服务器用于测试 */
#define MQTT_BROKER         "broker.emqx.io"
#define MQTT_PORT           1883

/* [可选] 客户端 ID (保持唯一，或留空自动生成逻辑需自行实现) */
#define MQTT_CLIENT_ID      "stm32_client_test_001"

/* [可选] 账号密码 (公共服务器通常不需要) */
#define MQTT_USERNAME       ""
#define MQTT_PASSWORD       ""

/* [可选] 心跳间隔 (秒) */
#define MQTT_KEEPALIVE      60

/* --- 4. 高级配置 (一般无需修改) --- */
#define MQTT_RX_BUFFER_SIZE     1024    /* 接收环形缓冲区大小 (字节) */
#define MQTT_TX_BUFFER_SIZE     512     /* 发送缓冲区大小 (字节) */
#define MQTT_TOPIC_MAX_LEN      64      /* 支持的最大主题长度 */
#define MQTT_PAYLOAD_MAX_LEN    256     /* 单包最大负载长度 */

/* [开关] 调试日志 (1: 开启, 0: 关闭) */
#define MQTT_DEBUG_ENABLE       1       


/* ==========================================
 * 类型定义
 * ========================================== */

/* 错误码定义 */
typedef enum {
    MQTT_OK = 0,
    MQTT_ERR_PARAM,         /* 参数错误 */
    MQTT_ERR_BUSY,          /* 系统忙 */
    MQTT_ERR_UART,          /* 串口错误 */
    MQTT_ERR_WIFI_TIMEOUT,  /* WiFi 连接超时 */
    MQTT_ERR_TCP_TIMEOUT,   /* TCP 连接超时 */
    MQTT_ERR_MQTT_REFUSED,  /* MQTT 连接被拒绝 */
    MQTT_ERR_MALLOC,        /* 内存不足 (若使用) */
    MQTT_ERR_OVERFLOW,      /* 缓冲区溢出 */
    MQTT_ERR_PROTOCOL,      /* 协议解析错误 */
    MQTT_ERR_NOT_CONNECTED, /* 未连接 */
    MQTT_ERR_UNKNOWN
} MQTT_Error_t;

/* 连接状态机 */
typedef enum {
    MQTT_STATE_RESET = 0,
    MQTT_STATE_INIT,            /* 初始化 ESP8266 */
    MQTT_STATE_WIFI_CONNECTING, /* 连接 WiFi */
    MQTT_STATE_TCP_CONNECTING,  /* 建立 TCP 连接 */
    MQTT_STATE_MQTT_CONNECTING, /* 发送 MQTT CONNECT */
    MQTT_STATE_CONNECTED,       /* 已连接，正常工作 */
    MQTT_STATE_DISCONNECTED,    /* 断开连接 */
    MQTT_STATE_ERROR            /* 错误状态 (自动重试) */
} MQTT_State_t;

/* 回调函数定义 */
typedef void (*MQTT_MessageCallback)(const char *topic, const uint8_t *payload, uint16_t len);

/* ==========================================
 * 公共接口函数
 * ========================================== */

/**
 * @brief 初始化 MQTT 客户端
 * @details 复位状态机，清空缓冲区
 */
void MQTT_Init(void);

/**
 * @brief MQTT 主循环 (核心函数)
 * @details 必须在 main while(1) 中高频调用，负责处理状态机流转、数据接收和心跳保活
 */
void MQTT_Run(void);

/**
 * @brief 获取当前连接状态
 * @return MQTT_State_t 当前状态 (用于判断是否可以发送数据)
 */
MQTT_State_t MQTT_GetState(void);

/**
 * @brief 获取最后一次错误码
 */
MQTT_Error_t MQTT_GetLastError(void);

/**
 * @brief 注册消息接收回调函数
 * @param cb 回调函数指针
 */
void MQTT_SetCallback(MQTT_MessageCallback cb);

/**
 * @brief 发布消息
 * 
 * @param topic 主题 (例如 "sensor/temp")
 * @param message 消息内容字符串
 * @param retain 保留标志 (true: 服务器保留最后一条消息)
 * @return MQTT_OK 请求发送成功 (不代表服务器已确认接收)
 */
MQTT_Error_t MQTT_Publish(const char *topic, const char *message, bool retain);

/**
 * @brief 订阅主题
 * 
 * @param topic 主题 (例如 "cmd/light")
 * @param qos QoS 等级 (0 或 1)
 * @return MQTT_OK 请求发送成功
 */
MQTT_Error_t MQTT_Subscribe(const char *topic, uint8_t qos);

#endif /* __CONN_H */
