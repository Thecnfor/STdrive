#include "conn.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ==========================================
 * 私有变量
 * ========================================== */
static bool is_connected = false;

/* ==========================================
 * 辅助函数
 * ========================================== */

/**
 * @brief 日志输出
 */
#ifdef MQTT_LOG_UART_HANDLE
static void MQTT_Log(const char *fmt, ...)
{
    char log_buf[256];
    va_list args;
    
    va_start(args, fmt);
    vsnprintf(log_buf, sizeof(log_buf), fmt, args);
    va_end(args);
    
    HAL_UART_Transmit(MQTT_LOG_UART_HANDLE, (uint8_t *)log_buf, strlen(log_buf), 100);
}
#else
#define MQTT_Log(...) ((void)0)
#endif

/**
 * @brief 执行 AT 指令并等待期望的响应字符串
 * 
 * @param cmd 要发送的指令 (NULL 则不发送，仅接收)
 * @param expected 期望收到的响应子串 (NULL 则不检查，读到超时)
 * @param out_buf 输出缓冲区，用于存储收到的数据 (NULL 则使用内部临时缓冲)
 * @param buf_len 输出缓冲区大小
 * @param timeout_ms 超时时间
 * @return true 找到 expected 字符串
 * @return false 超时或未找到
 */
static bool ESP_Execute(const char *cmd, const char *expected, char *out_buf, uint16_t buf_len, uint32_t timeout_ms)
{
    char local_buf[128]; // 若调用者不需要完整数据，使用小缓冲区
    char *p_buf = out_buf ? out_buf : local_buf;
    uint16_t p_len = out_buf ? buf_len : sizeof(local_buf);
    uint16_t idx = 0;
    uint32_t start_time = HAL_GetTick();

    /* 清空接收缓冲区 */
    memset(p_buf, 0, p_len);
    
    /* 发送指令 */
    if (cmd != NULL) {
        MQTT_Log("[CMD] %s", cmd);
        HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t *)cmd, strlen(cmd), 100);
    }

    /* 循环接收 */
    while ((HAL_GetTick() - start_time) < timeout_ms) {
        uint8_t rx_char;
        /* 使用短超时 (1ms) 轮询，提高响应速度 */
        if (HAL_UART_Receive(MQTT_UART_HANDLE, &rx_char, 1, 1) == HAL_OK) {
            if (idx < p_len - 1) {
                p_buf[idx++] = rx_char;
                p_buf[idx] = '\0';
                
                /* 实时检查是否包含期望响应 */
                if (expected != NULL && strstr(p_buf, expected) != NULL) {
                    MQTT_Log("[响应] 成功 (%s)\r\n", expected);
                    return true;
                }
            } else {
                /* 缓冲区满，重置索引 (循环覆盖，防止溢出) 
                   注意：这可能会截断长响应，实际应用根据需要调整 */
                idx = 0; 
                // 或者 break; 取决于策略
            }
        }
    }

    if (expected == NULL) {
        return true; /* 无期望响应，超时即完成 */
    }

    MQTT_Log("[响应] 超时或失败\r\n");
    return false;
}

/**
 * 内部分阶段自动重连例程说明
 * 1. 先检查 WiFi 状态（AT+CWJAP?），未连接则仅执行入网
 * 2. 检查 TCP（AT+CIPSTART），已连接则跳过，仅在断开时重建
 * 3. 发送 MQTT CONNECT；成功后置位 is_connected
 * 4. 全流程不重复已就绪阶段，提升重连效率
 * 该函数仅由后台服务或自动重连入口调用，用户无需手动触发
 */
static bool MQTT_ReconnectStep(void)
{
    char buf[RX_BUFFER_SIZE];
    char cmd_buf[128];
    uint8_t packet[128];
    uint16_t idx = 0;

    bool wifi_connected = false;
    if (ESP_Execute("AT+CWJAP?\r\n", "OK", buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_NORMAL)) {
        if (strstr(buf, WIFI_SSID)) {
            wifi_connected = true;
        }
    }
    if (!wifi_connected) {
        sprintf(cmd_buf, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASSWORD);
        if (!ESP_SendAT(cmd_buf, "OK", AT_CMD_TIMEOUT_WIFI)) {
            return false;
        }
    }

    sprintf(cmd_buf, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", MQTT_BROKER, MQTT_PORT);
    ESP_Execute(cmd_buf, NULL, buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_LONG);
    if (!(strstr(buf, "CONNECT") || strstr(buf, "ALREADY CONNECTED"))) {
        return false;
    }

    uint32_t remaining_len = (2 + 4) + 1 + 1 + 2 + (2 + strlen(MQTT_CLIENT_ID));
    idx = 0;
    packet[idx++] = MQTT_PKT_CONNECT;
    idx += mqtt_encode_len(&packet[idx], remaining_len);
    idx += mqtt_encode_string(&packet[idx], MQTT_PROTOCOL_NAME);
    packet[idx++] = MQTT_PROTOCOL_LEVEL;
    packet[idx++] = MQTT_FLAG_CLEAN_SESSION;
    packet[idx++] = (MQTT_KEEPALIVE >> 8) & 0xFF;
    packet[idx++] = MQTT_KEEPALIVE & 0xFF;
    idx += mqtt_encode_string(&packet[idx], MQTT_CLIENT_ID);

    if (MQTT_SendPacket(packet, idx)) {
        is_connected = true;
        return true;
    }
    return false;
}

void MQTT_AutoReconnect(void)
{
    if (!is_connected) {
        MQTT_Log("检测到连接断开，尝试重连...\r\n");
        MQTT_ReconnectStep();
    }
}

/**
 * @brief 简化版发送 AT 指令
 */
static bool ESP_SendAT(const char *cmd, const char *expected, uint32_t timeout_ms)
{
    return ESP_Execute(cmd, expected, NULL, 0, timeout_ms);
}

/**
 * @brief 发送原始数据 (用于 MQTT 报文)
 */
static bool ESP_SendRaw(uint8_t *data, uint16_t len)
{
    HAL_UART_Transmit(MQTT_UART_HANDLE, data, len, 100);
    return ESP_Execute(NULL, "SEND OK", NULL, 0, AT_CMD_TIMEOUT_LONG);
}

/* ==========================================
 * MQTT 协议层
 * ========================================== */

/**
 * @brief 编码剩余长度
 * @param buf 输出缓冲区
 * @param length 剩余长度值
 * @return 写入的字节数
 */
static uint8_t mqtt_encode_len(uint8_t *buf, uint32_t length)
{
    uint8_t len_bytes = 0;
    do {
        uint8_t encoded_byte = length % 128;
        length /= 128;
        if (length > 0) {
            encoded_byte |= 0x80;
        }
        buf[len_bytes++] = encoded_byte;
    } while (length > 0);
    return len_bytes;
}

/**
 * @brief 编码 MQTT 字符串 (2字节长度 + 字符串内容)
 * @param buf 输出缓冲区
 * @param str 字符串
 * @return 写入的字节数
 */
static uint16_t mqtt_encode_string(uint8_t *buf, const char *str)
{
    uint16_t len = strlen(str);
    buf[0] = (len >> 8) & 0xFF;
    buf[1] = len & 0xFF;
    memcpy(&buf[2], str, len);
    return len + 2;
}

/**
 * @brief 发送 MQTT 报文 (自动处理 AT+CIPSEND)
 */
static bool MQTT_SendPacket(uint8_t *packet, uint16_t len)
{
    char cmd_buf[32];
    sprintf(cmd_buf, "AT+CIPSEND=%d\r\n", len);
    
    if (ESP_SendAT(cmd_buf, ">", AT_CMD_TIMEOUT_LONG)) {
        if (ESP_SendRaw(packet, len)) {
            return true;
        }
    }
    
    /* 发送失败视为连接断开 */
    is_connected = false;
    return false;
}

/* ==========================================
 * 公共接口函数实现
 * ========================================== */

bool MQTT_IsConnected(void)
{
    return is_connected;
}

bool MQTT_Start(void)
{
    char buf[RX_BUFFER_SIZE];
    char cmd_buf[128];
    uint8_t packet[128];
    uint16_t idx = 0;

    is_connected = false;

    MQTT_Log("=== MQTT 启动 ===\r\n");

    /* 1. 基础 AT 检查 */
    if (!ESP_SendAT("AT\r\n", "OK", AT_CMD_TIMEOUT_SHORT)) {
        MQTT_Log("AT 检查失败\r\n");
        return false;
    }
    
    /* 2. WiFi 配置与连接 */
    ESP_SendAT("AT+CWMODE=1\r\n", "OK", AT_CMD_TIMEOUT_NORMAL);

    /* 检查是否已连接目标 WiFi */
    /* 发送 AT+CWJAP? 并检查响应中是否包含 SSID */
    bool wifi_connected = false;
    if (ESP_Execute("AT+CWJAP?\r\n", "OK", buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_NORMAL)) {
        if (strstr(buf, WIFI_SSID)) {
            wifi_connected = true;
            MQTT_Log("WiFi 已连接\r\n");
        }
    }

    /* 未连接则尝试连接 */
    if (!wifi_connected) {
        MQTT_Log("正在连接 WiFi: %s...\r\n", WIFI_SSID);
        sprintf(cmd_buf, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASSWORD);
        if (!ESP_SendAT(cmd_buf, "OK", AT_CMD_TIMEOUT_WIFI)) {
            // 这里也可以检查 "WIFI CONNECTED" 或 "FAIL"
            // 简单起见，如果超时或无 OK 视为失败
            // 注意：部分固件返回 WIFI CONNECTED 后才返回 OK，或者只返回 WIFI CONNECTED
            // 严谨做法应检查 buf
            MQTT_Log("WiFi 连接失败\r\n");
        } else {
             MQTT_Log("WiFi 连接成功\r\n");
        }
    }

    /* 3. 建立 TCP 连接 */
    /* 检查是否已连接或建立新连接 */
    MQTT_Log("正在连接 TCP: %s:%d...\r\n", MQTT_BROKER, MQTT_PORT);
    sprintf(cmd_buf, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", MQTT_BROKER, MQTT_PORT);
    
    /* 发送指令并读取响应到 buf */
    /* 期望 CONNECT，但也可能已经是 ALREADY CONNECTED */
    ESP_Execute(cmd_buf, NULL, buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_LONG);
    
    if (strstr(buf, "CONNECT") || strstr(buf, "ALREADY CONNECTED")) {
        // TCP 连接成功
        MQTT_Log("TCP 已连接\r\n");
    } else {
        MQTT_Log("TCP 连接失败\r\n");
        return false;
    }

    /* 4. 构建并发送 MQTT CONNECT 报文 */
    /* Variable Header: Protocol Name(string) + Level(1) + Flags(1) + KeepAlive(2) */
    /* Payload: Client ID (string) */
    uint32_t remaining_len = (2 + 4) + 1 + 1 + 2 + (2 + strlen(MQTT_CLIENT_ID));

    idx = 0;
    packet[idx++] = MQTT_PKT_CONNECT;
    idx += mqtt_encode_len(&packet[idx], remaining_len);

    /* Variable Header */
    idx += mqtt_encode_string(&packet[idx], MQTT_PROTOCOL_NAME);
    packet[idx++] = MQTT_PROTOCOL_LEVEL;
    packet[idx++] = MQTT_FLAG_CLEAN_SESSION;
    packet[idx++] = (MQTT_KEEPALIVE >> 8) & 0xFF;
    packet[idx++] = MQTT_KEEPALIVE & 0xFF;

    /* Payload: Client ID */
    idx += mqtt_encode_string(&packet[idx], MQTT_CLIENT_ID);

    /* 发送报文 */
    if (MQTT_SendPacket(packet, idx)) {
        is_connected = true;
        MQTT_Log("MQTT 已连接\r\n");
        /* 启动后台服务驱动
         * 若定义 `MQTT_TIM_HANDLE` 为某定时器句柄，则使用定时中断周期性调用服务例程
         * 未定义亦可工作：服务例程在关键路径按需触发，无需用户额外轮询
         */
        #ifdef MQTT_TIM_HANDLE
        HAL_TIM_Base_Start_IT(MQTT_TIM_HANDLE);
        #endif
        return true;
    }
    
    MQTT_Log("MQTT 连接失败\r\n");
    return false;
}

bool MQTT_Publish(const char *topic, const char *message)
{
    if (!is_connected) {
        MQTT_Log("发布失败: 未连接\r\n");
        MQTT_ServiceTick();
        return false;
    }
    
    if (topic == NULL || message == NULL) {
        MQTT_Log("发布失败: 参数为空\r\n");
        return false;
    }

    /* 1. 计算总长度并检查缓冲区 */
    uint16_t topic_len = strlen(topic);
    uint16_t msg_len = strlen(message);
    uint32_t remaining_len = (2 + topic_len) + msg_len;
    
    /* 估算最大包长: FixedHeader(1+4) + VarHeader + Payload */
    /* 为安全起见，使用较大的静态缓冲区或栈缓冲区 */
    #define MQTT_TX_BUF_SIZE 1024
    static uint8_t packet[MQTT_TX_BUF_SIZE]; 
    
    /* 简单预判：头部最多5字节 + 剩余长度 */
    if (remaining_len + 5 > MQTT_TX_BUF_SIZE) {
        MQTT_Log("发布失败: 数据过长 (Topic+Msg > %d)\r\n", MQTT_TX_BUF_SIZE - 5);
        return false;
    }

    MQTT_Log("发布: %s -> %s\r\n", topic, message);

    uint16_t idx = 0;

    /* Fixed Header */
    packet[idx++] = MQTT_PKT_PUBLISH;
    idx += mqtt_encode_len(&packet[idx], remaining_len);

    /* Variable Header: Topic */
    idx += mqtt_encode_string(&packet[idx], topic);

    /* Payload: Message */
    memcpy(&packet[idx], message, msg_len);
    idx += msg_len;

    return MQTT_SendPacket(packet, idx);
}

bool MQTT_Subscribe(const char *topic)
{
    if (!is_connected) {
        MQTT_Log("订阅失败: 未连接\r\n");
        MQTT_ServiceTick();
        return false;
    }

    MQTT_Log("订阅: %s\r\n", topic);

    uint8_t packet[128];
    uint16_t idx = 0;
    /* Variable Header: Packet ID(2) */
    /* Payload: Topic Filter(string) + QoS(1) */
    uint32_t remaining_len = 2 + (2 + strlen(topic)) + 1;

    /* Fixed Header */
    packet[idx++] = MQTT_PKT_SUBSCRIBE;
    idx += mqtt_encode_len(&packet[idx], remaining_len);

    /* Variable Header: Packet ID */
    packet[idx++] = 0x00;
    packet[idx++] = 0x01; 

    /* Payload: Topic Filter + QoS */
    idx += mqtt_encode_string(&packet[idx], topic);
    packet[idx++] = 0x00; /* QoS 0 */

    return MQTT_SendPacket(packet, idx);
}

void MQTT_Heartbeat(void)
{
    uint8_t packet[2] = {MQTT_PKT_PINGREQ, 0x00};
    MQTT_SendPacket(packet, 2);
}

static void MQTT_ServiceTick(void)
{
    static uint32_t last_ping = 0;
    if (is_connected) {
        if (HAL_GetTick() - last_ping > (MQTT_KEEPALIVE * 1000) / 2) {
            last_ping = HAL_GetTick();
            MQTT_Heartbeat();
        }
    } else {
        MQTT_AutoReconnect();
    }
}

/* 定时器中断回调挂钩
 * 仅当中断来源为 `MQTT_TIM_HANDLE` 时驱动后台服务
 * 请确保该定时器按所需周期启动（在 `MQTT_Start` 中已启动）
 */
#ifdef MQTT_TIM_HANDLE
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim == MQTT_TIM_HANDLE) {
        MQTT_ServiceTick();
    }
}
#endif

/* ==========================================
 * 测试函数
 * ========================================== */

/**
 * @brief 快速测试 MQTT 完整功能 (连接 -> 订阅 -> 循环发布/接收)
 * @details 将此函数放在 main 函数的 while(1) 循环中调用
 */
void MQTT_Test_Run(void)
{
    static uint32_t last_pub_time = 0;
    static bool started = false;
    static bool is_subscribed = false;

    if (!started) {
        MQTT_Start();
        started = true;
    }

    if (!is_subscribed && MQTT_IsConnected()) {
        HAL_Delay(500);
        if (MQTT_Subscribe("test/cmd")) {
            is_subscribed = true;
        }
    }

    if (MQTT_IsConnected() && (HAL_GetTick() - last_pub_time > 5000)) {
        last_pub_time = HAL_GetTick();
        char msg[64];
        snprintf(msg, sizeof(msg), "online_tick_%lu", HAL_GetTick());
        MQTT_Publish("test/status", msg);
    }

    char topic[64];
    char payload[128];
    if (MQTT_Process(topic, sizeof(topic), payload, sizeof(payload))) {
        char reply[128];
        snprintf(reply, sizeof(reply), "Echo: %s", payload);
        MQTT_Publish("test/reply", reply);
    }
}

/* ==========================================
 * MQTT 接收处理
 * ========================================== */
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static uint16_t rx_idx = 0;

bool MQTT_Process(char *topic, uint16_t topic_size, char *payload, uint16_t payload_size)
{
    uint8_t byte;
    bool msg_received = false;

    /* 非阻塞读取所有可用数据 */
    while (HAL_UART_Receive(MQTT_UART_HANDLE, &byte, 1, 0) == HAL_OK) {
        if (rx_idx < RX_BUFFER_SIZE - 1) {
            rx_buffer[rx_idx++] = byte;
            rx_buffer[rx_idx] = 0;
        } else {
            /* 缓冲区满，移位丢弃旧数据 */
            memmove(rx_buffer, rx_buffer + 1, RX_BUFFER_SIZE - 2);
            rx_buffer[RX_BUFFER_SIZE - 2] = byte;
            rx_buffer[RX_BUFFER_SIZE - 1] = 0;
        }
    }

    if (rx_idx == 0) return false;
    
    /* 简单的解析逻辑：寻找 +IPD, */
    char *ipd_ptr = strstr((char*)rx_buffer, "+IPD,");
    if (ipd_ptr) {
        /* 解析长度 */
        int len = 0;
        char *colon_ptr = strchr(ipd_ptr, ':');
        if (colon_ptr) {
            /* +IPD,123: */
            if (sscanf(ipd_ptr, "+IPD,%d:", &len) == 1) {
                int header_len = (colon_ptr - ipd_ptr) + 1; // "+IPD,len:" 的长度
                int total_len = header_len + len;
                
                /* 检查是否接收完整 */
                int ipd_offset = ipd_ptr - (char*)rx_buffer;
                if (rx_idx >= ipd_offset + total_len) {
                    /* 获取 MQTT 数据起始地址 */
                    uint8_t *mqtt_data = (uint8_t*)(colon_ptr + 1);
                    
                    /* 解析 MQTT PUBLISH 报文 */
                    /* Fixed Header: Byte 0 (0x30 - 0x3F) */
                    if ((mqtt_data[0] & 0xF0) == 0x30) {
                        /* 是 PUBLISH 报文 */
                        uint8_t qos = (mqtt_data[0] >> 1) & 0x03;
                        uint32_t rem_len = 0;
                        uint32_t multiplier = 1;
                        int i = 1;
                        
                        /* 解析剩余长度 */
                        do {
                            rem_len += (mqtt_data[i] & 127) * multiplier;
                            multiplier *= 128;
                            i++;
                        } while ((mqtt_data[i-1] & 128) != 0 && i < 5);
                        
                        int var_header_start = i;
                        /* Topic Length */
                        uint16_t t_len = (mqtt_data[var_header_start] << 8) | mqtt_data[var_header_start+1];
                        
                        /* Payload Start */
                        int payload_start = var_header_start + 2 + t_len;
                        if (qos > 0) {
                            payload_start += 2; /* Packet ID */
                        }
                        
                        int p_len = rem_len - (payload_start - var_header_start);
                        
                        /* 复制到用户缓冲区 */
                        if (topic != NULL && topic_size > 0) {
                             uint16_t copy_len = (t_len < topic_size) ? t_len : (topic_size - 1);
                             memcpy(topic, &mqtt_data[var_header_start+2], copy_len);
                             topic[copy_len] = 0;
                        }
                        

                        if (payload != NULL && payload_size > 0) {
                            uint16_t copy_len = (p_len < payload_size) ? p_len : (payload_size - 1);
                            memcpy(payload, &mqtt_data[payload_start], copy_len);
                            payload[copy_len] = 0;
                        }
                        
                        msg_received = true;
                        
                        if (topic && payload) {
                            MQTT_Log("接收: %s -> %s\r\n", topic, payload);
                        }
                    }
                    
                    /* 移除已处理的数据 */
                    int bytes_processed = ipd_offset + total_len;
                    memmove(rx_buffer, rx_buffer + bytes_processed, rx_idx - bytes_processed);
                    rx_idx -= bytes_processed;

                    return msg_received;
                }
            }
        }
    }
    
    /* 防止缓冲区溢出且未找到有效数据的情况 */
    if (rx_idx == RX_BUFFER_SIZE && !strstr((char*)rx_buffer, "+IPD,")) {
         rx_idx = 0; 
    }
    return false;
}
