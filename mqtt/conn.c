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
        HAL_UART_Transmit(&huart1, (uint8_t *)cmd, strlen(cmd), 100);
    }

    /* 循环接收 */
    while ((HAL_GetTick() - start_time) < timeout_ms) {
        uint8_t rx_char;
        /* 使用短超时 (1ms) 轮询，提高响应速度 */
        if (HAL_UART_Receive(&huart1, &rx_char, 1, 1) == HAL_OK) {
            if (idx < p_len - 1) {
                p_buf[idx++] = rx_char;
                p_buf[idx] = '\0';
                
                /* 实时检查是否包含期望响应 */
                if (expected != NULL && strstr(p_buf, expected) != NULL) {
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

    return false;
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
    HAL_UART_Transmit(&huart1, data, len, 100);
    return ESP_Execute(NULL, "SEND OK", NULL, 0, AT_CMD_TIMEOUT_NORMAL);
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
    
    if (ESP_SendAT(cmd_buf, ">", AT_CMD_TIMEOUT_SHORT)) {
        return ESP_SendRaw(packet, len);
    }
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

    /* 1. 基础 AT 检查 */
    if (!ESP_SendAT("AT\r\n", "OK", AT_CMD_TIMEOUT_SHORT)) return false;
    
    /* 2. WiFi 配置与连接 */
    ESP_SendAT("AT+CWMODE=1\r\n", "OK", AT_CMD_TIMEOUT_NORMAL);

    /* 检查是否已连接目标 WiFi */
    /* 发送 AT+CWJAP? 并检查响应中是否包含 SSID */
    bool wifi_connected = false;
    if (ESP_Execute("AT+CWJAP?\r\n", "OK", buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_NORMAL)) {
        if (strstr(buf, WIFI_SSID)) {
            wifi_connected = true;
        }
    }

    /* 未连接则尝试连接 */
    if (!wifi_connected) {
        sprintf(cmd_buf, "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASSWORD);
        if (!ESP_SendAT(cmd_buf, "OK", AT_CMD_TIMEOUT_WIFI)) {
            // 这里也可以检查 "WIFI CONNECTED" 或 "FAIL"
            // 简单起见，如果超时或无 OK 视为失败
            // 注意：部分固件返回 WIFI CONNECTED 后才返回 OK，或者只返回 WIFI CONNECTED
            // 严谨做法应检查 buf
        }
    }

    /* 3. 建立 TCP 连接 */
    /* 检查是否已连接或建立新连接 */
    sprintf(cmd_buf, "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", MQTT_BROKER, MQTT_PORT);
    
    /* 发送指令并读取响应到 buf */
    /* 期望 CONNECT，但也可能已经是 ALREADY CONNECTED */
    ESP_Execute(cmd_buf, NULL, buf, RX_BUFFER_SIZE, AT_CMD_TIMEOUT_LONG);
    
    if (strstr(buf, "CONNECT") || strstr(buf, "ALREADY CONNECTED")) {
        // TCP 连接成功
    } else {
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
        return true;
    }

    return false;
}

bool MQTT_Publish(const char *topic, const char *message)
{
    if (!is_connected) return false;

    uint8_t packet[256]; // 确保够大
    uint16_t msg_len = strlen(message);
    uint16_t idx = 0;
    uint32_t remaining_len = (2 + strlen(topic)) + msg_len;

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
    if (!is_connected) return false;

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
    while (HAL_UART_Receive(&huart1, &byte, 1, 0) == HAL_OK) {
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
