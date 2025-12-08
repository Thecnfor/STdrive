#include "conn.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ==========================================
 * 常量与宏定义
 * ========================================== */
#define AT_CMD_TIMEOUT_DEFAULT  1000
#define AT_CMD_TIMEOUT_WIFI     20000   /* WiFi 连接可能较慢 */
#define AT_CMD_TIMEOUT_TCP      5000
#define AT_RETRY_MAX            3

/* MQTT 控制包类型 */
#define MQTT_PKT_CONNECT        0x10
#define MQTT_PKT_CONNACK        0x20
#define MQTT_PKT_PUBLISH        0x30
#define MQTT_PKT_PUBACK         0x40
#define MQTT_PKT_SUBSCRIBE      0x82
#define MQTT_PKT_SUBACK         0x90
#define MQTT_PKT_PINGREQ        0xC0
#define MQTT_PKT_PINGRESP       0xD0
#define MQTT_PKT_DISCONNECT     0xE0

#define MQTT_PROTOCOL_NAME      "MQTT"
#define MQTT_PROTOCOL_LEVEL     4
#define MQTT_FLAG_CLEAN_SESSION 0x02


/* ==========================================
 * 内部数据结构
 * ========================================== */
static MQTT_State_t g_state = MQTT_STATE_RESET;
static MQTT_Error_t g_last_error = MQTT_OK;

/* 接收环形缓冲区 */
static uint8_t g_rx_buffer[MQTT_RX_BUFFER_SIZE];
static volatile uint16_t g_rx_head = 0;
static volatile uint16_t g_rx_tail = 0;

/* 发送缓冲区 */
static uint8_t g_tx_buffer[MQTT_TX_BUFFER_SIZE];

/* 状态机变量 */
static uint32_t g_state_tick = 0;
static uint32_t g_keepalive_tick = 0;
static uint8_t  g_retry_count = 0;

/* AT 指令控制 */
typedef enum {
    AT_IDLE,
    AT_SENDING,
    AT_WAIT_RESPONSE
} AT_State_t;

static AT_State_t g_at_state = AT_IDLE;
static char g_at_expect[32];
static uint32_t g_at_timeout = 0;
static uint32_t g_at_start_tick = 0;

/* 回调函数 */
static MQTT_MessageCallback g_msg_callback = NULL;

/* ==========================================
 * 内部函数声明
 * ========================================== */
static void UART_Poll(void);
static bool RingBuf_Write(uint8_t byte);
static bool RingBuf_Read(uint8_t *byte);
static uint16_t RingBuf_Available(void);
static uint8_t RingBuf_Peek(uint16_t offset);
static void RingBuf_Skip(uint16_t len);
static void Log(const char *fmt, ...);
static void ProcessPacket(void);

/* MQTT 编码辅助 */
static uint8_t EncodeLength(uint8_t *buf, uint32_t length) {
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

static uint16_t EncodeString(uint8_t *buf, const char *str) {
    uint16_t len = strlen(str);
    buf[0] = (len >> 8) & 0xFF;
    buf[1] = len & 0xFF;
    memcpy(&buf[2], str, len);
    return len + 2;
}

/* ==========================================
 * 接口实现
 * ========================================== */

void MQTT_Init(void) {
    g_state = MQTT_STATE_RESET;
    g_last_error = MQTT_OK;
    g_rx_head = 0;
    g_rx_tail = 0;
    g_at_state = AT_IDLE;
    Log("MQTT 初始化\r\n");
}

void MQTT_SetCallback(MQTT_MessageCallback cb) {
    g_msg_callback = cb;
}

MQTT_State_t MQTT_GetState(void) {
    return g_state;
}

MQTT_Error_t MQTT_GetLastError(void) {
    return g_last_error;
}

/* ==========================================
 * 缓冲区操作
 * ========================================== */

static bool RingBuf_Write(uint8_t byte) {
    uint16_t next = (g_rx_head + 1) % MQTT_RX_BUFFER_SIZE;
    if (next == g_rx_tail) return false;
    g_rx_buffer[g_rx_head] = byte;
    g_rx_head = next;
    return true;
}

static bool RingBuf_Read(uint8_t *byte) {
    if (g_rx_head == g_rx_tail) return false;
    *byte = g_rx_buffer[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % MQTT_RX_BUFFER_SIZE;
    return true;
}

static uint16_t RingBuf_Available(void) {
    if (g_rx_head >= g_rx_tail) return g_rx_head - g_rx_tail;
    return MQTT_RX_BUFFER_SIZE - g_rx_tail + g_rx_head;
}

static uint8_t RingBuf_Peek(uint16_t offset) {
    return g_rx_buffer[(g_rx_tail + offset) % MQTT_RX_BUFFER_SIZE];
}

static void RingBuf_Skip(uint16_t len) {
    g_rx_tail = (g_rx_tail + len) % MQTT_RX_BUFFER_SIZE;
}

/* ==========================================
 * 底层通信
 * ========================================== */

static void UART_Poll(void) {
    uint8_t byte;
    /* 每次最多读取 64 字节，防止占用太久 */
    int count = 64;
    while (count-- > 0 && HAL_UART_Receive(MQTT_UART_HANDLE, &byte, 1, 0) == HAL_OK) {
        RingBuf_Write(byte);
        #if MQTT_DEBUG_ENABLE && defined(MQTT_DEBUG_UART)
        // 调试：将收到的 ESP8266 数据转发到调试串口
        HAL_UART_Transmit(MQTT_DEBUG_UART, &byte, 1, 10);
        #endif
    }
}

static void SendRaw(const uint8_t *data, uint16_t len) {
    HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t*)data, len, 100);
}

static void SendAT(const char *cmd) {
    SendRaw((uint8_t*)cmd, strlen(cmd));
}

static void Log(const char *fmt, ...) {
#if MQTT_DEBUG_ENABLE
    va_list args;
    char buf[128];
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    #ifdef MQTT_DEBUG_UART
    HAL_UART_Transmit(MQTT_DEBUG_UART, (uint8_t*)buf, strlen(buf), 100);
    #endif
#endif
}

/* ==========================================
 * 核心逻辑
 * ========================================== */

/* 检查环形缓冲区中是否包含子串 (全缓冲搜索) */
static bool CheckResponse(const char *expected) {
    uint16_t available = RingBuf_Available();
    uint16_t len = strlen(expected);
    
    if (available < len) return false;
    
    // 在 RingBuffer 中直接搜索
    // 搜索范围：从 0 到 available - len
    for (uint16_t i = 0; i <= available - len; i++) {
        bool match = true;
        for (uint16_t j = 0; j < len; j++) {
            if (RingBuf_Peek(i + j) != (uint8_t)expected[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            // 找到了，消耗掉直到该字符串结束的所有数据
            // 包括该字符串之前的所有数据（视为无效数据或噪声）
            RingBuf_Skip(i + len);
            return true;
        }
    }
    
    // 可选：如果缓冲区快满了（例如 > 90%），且没找到，可以丢弃一部分旧数据防止死锁
    // 但为安全起见，暂不自动丢弃，依赖状态机的超时机制来 Reset
    
    return false;
}

/* 主循环 */
void MQTT_Run(void) {
    UART_Poll();
    ProcessPacket(); /* 优先处理入站数据 */

    uint32_t now = HAL_GetTick();

    switch (g_state) {
        case MQTT_STATE_RESET:
            g_state = MQTT_STATE_INIT;
            g_at_state = AT_IDLE;
            // 清空缓冲区，避免旧数据干扰
            g_rx_head = 0;
            g_rx_tail = 0;
            break;

        case MQTT_STATE_INIT:
            if (g_at_state == AT_IDLE) {
                SendAT("AT\r\n");
                g_at_state = AT_WAIT_RESPONSE;
                g_at_start_tick = now;
                g_at_timeout = AT_CMD_TIMEOUT_DEFAULT;
            } else if (g_at_state == AT_WAIT_RESPONSE) {
                if (CheckResponse("OK")) {
                    SendAT("AT+CWMODE=1\r\n");
                    HAL_Delay(100);
                    SendAT("AT+CIPMUX=0\r\n"); // 强制单连接模式
                    g_at_state = AT_IDLE;
                    g_state = MQTT_STATE_WIFI_CONNECTING;
                    g_retry_count = 0;
                } else if (now - g_at_start_tick > g_at_timeout) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_UART;
                }
            }
            break;

        case MQTT_STATE_WIFI_CONNECTING:
            if (g_at_state == AT_IDLE) {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASSWORD);
                SendAT(cmd);
                g_at_state = AT_WAIT_RESPONSE;
                g_at_start_tick = now;
                g_at_timeout = AT_CMD_TIMEOUT_WIFI;
            } else if (g_at_state == AT_WAIT_RESPONSE) {
                if (CheckResponse("OK") || CheckResponse("WIFI CONNECTED")) {
                    g_state = MQTT_STATE_TCP_CONNECTING;
                    g_at_state = AT_IDLE;
                    g_retry_count = 0;
                } else if (CheckResponse("FAIL")) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_WIFI_TIMEOUT;
                } else if (now - g_at_start_tick > g_at_timeout) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_WIFI_TIMEOUT;
                }
            }
            break;

        case MQTT_STATE_TCP_CONNECTING:
            if (g_at_state == AT_IDLE) {
                // 先尝试关闭可能存在的旧连接，防止 ERROR ALREADY CONNECTED
                SendAT("AT+CIPCLOSE\r\n");
                HAL_Delay(200);
                
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", MQTT_BROKER, MQTT_PORT);
                SendAT(cmd);
                g_at_state = AT_WAIT_RESPONSE;
                g_at_start_tick = now;
                g_at_timeout = AT_CMD_TIMEOUT_TCP;
            } else {
                if (CheckResponse("CONNECT") || CheckResponse("OK") || CheckResponse("ALREADY CONNECTED")) {
                    g_state = MQTT_STATE_MQTT_CONNECTING;
                    g_at_state = AT_IDLE;
                } else if (CheckResponse("ERROR") || CheckResponse("CLOSED")) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_TCP_TIMEOUT;
                } else if (now - g_at_start_tick > g_at_timeout) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_TCP_TIMEOUT;
                }
            }
            break;

        case MQTT_STATE_MQTT_CONNECTING:
            if (g_at_state == AT_IDLE) {
                /* 构建 MQTT CONNECT */
                uint16_t idx = 0;
                
                // 强制禁用用户名密码 (Debug)
                // 如果这样能连上，说明之前的宏定义有问题
                bool use_user = false; // (strlen(MQTT_USERNAME) > 0);
                bool use_pass = false; // (strlen(MQTT_PASSWORD) > 0);

                uint32_t rem_len = (2 + 4) + 1 + 1 + 2 + (2 + strlen(MQTT_CLIENT_ID));
                if (use_user) rem_len += (2 + strlen(MQTT_USERNAME));
                if (use_pass) rem_len += (2 + strlen(MQTT_PASSWORD));

                g_tx_buffer[idx++] = MQTT_PKT_CONNECT;
                idx += EncodeLength(&g_tx_buffer[idx], rem_len);
                idx += EncodeString(&g_tx_buffer[idx], MQTT_PROTOCOL_NAME);
                g_tx_buffer[idx++] = MQTT_PROTOCOL_LEVEL;
                
                uint8_t flags = MQTT_FLAG_CLEAN_SESSION;
                if (use_user) flags |= 0x80;
                if (use_pass) flags |= 0x40;
                g_tx_buffer[idx++] = flags;
                
                Log("DEBUG: CONNECT Flags=0x%02X, CID_Len=%d\r\n", flags, strlen(MQTT_CLIENT_ID));

                g_tx_buffer[idx++] = (MQTT_KEEPALIVE >> 8) & 0xFF;
                g_tx_buffer[idx++] = MQTT_KEEPALIVE & 0xFF;
                idx += EncodeString(&g_tx_buffer[idx], MQTT_CLIENT_ID);
                if (use_user) idx += EncodeString(&g_tx_buffer[idx], MQTT_USERNAME);
                if (use_pass) idx += EncodeString(&g_tx_buffer[idx], MQTT_PASSWORD);

                char cmd[32];
                snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", idx);
                SendAT(cmd);
                
                // 等待 '>' 提示符
                uint32_t wait_start = HAL_GetTick();
                bool prompt_received = false;
                while (HAL_GetTick() - wait_start < 1000) {
                    UART_Poll(); 
                    if (CheckResponse(">")) {
                        prompt_received = true;
                        break;
                    }
                }

                if (prompt_received) {
                    SendRaw(g_tx_buffer, idx);
                    
                    g_at_state = AT_WAIT_RESPONSE;
                    g_at_start_tick = now;
                    g_at_timeout = 5000;
                } else {
                    Log("错误: 等待 '>' 超时\r\n");
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_UART;
                }
            } else {
                // 等待 CONNACK (在 ProcessPacket 中处理)
                if (now - g_at_start_tick > g_at_timeout) {
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_MQTT_REFUSED;
                }
            }
            break;

        case MQTT_STATE_CONNECTED:
            if (now - g_keepalive_tick > (MQTT_KEEPALIVE * 1000) / 2) {
                MQTT_Publish(NULL, NULL, false); // PING implemented internally
            }
            break;

        case MQTT_STATE_ERROR:
            if (now - g_state_tick > 5000) {
                Log("重试中... (上次错误码: %d)\r\n", g_last_error);
                
                // 强制断开 TCP 连接，确保下次重试环境干净
                SendAT("AT+CIPCLOSE\r\n");
                HAL_Delay(500);
                
                g_state = MQTT_STATE_RESET;
                g_state_tick = now;
            }
            break;

        default: break;
    }
}

/* ==========================================
 * 数据包解析 (支持分片)
 * ========================================== */

static void ProcessPacket(void) {
    static enum { STATE_FIND_IPD, STATE_READ_LEN, STATE_READ_HEADER, STATE_READ_PAYLOAD } p_state = STATE_FIND_IPD;
    static uint16_t pkt_len = 0;
    static uint16_t bytes_read = 0;
    static uint8_t mqtt_header[5]; // Fixed header
    
    // 状态机处理 +IPD
    // 注意：这里需要从 RingBuffer 中仔细解析
    // 简化逻辑：每次循环尝试查找 +IPD
    
    if (RingBuf_Available() == 0) return;

    // 寻找 +IPD,
    // 这是一个简化实现，实际可能需要逐字节状态机
    if (p_state == STATE_FIND_IPD) {
        if (CheckResponse("+IPD,")) {
            p_state = STATE_READ_LEN;
            pkt_len = 0;
        }
    }
    
    if (p_state == STATE_READ_LEN) {
        // 读取长度直到 ':'
        while (RingBuf_Available() > 0) {
            uint8_t c = RingBuf_Peek(0);
            if (c >= '0' && c <= '9') {
                pkt_len = pkt_len * 10 + (c - '0');
                RingBuf_Skip(1);
            } else if (c == ':') {
                RingBuf_Skip(1);
                p_state = STATE_READ_HEADER;
                bytes_read = 0;
                break;
            } else {
                // 格式错误
                p_state = STATE_FIND_IPD;
                break;
            }
        }
    }
    
    if (p_state == STATE_READ_HEADER || p_state == STATE_READ_PAYLOAD) {
        // 等待足够的数据
        if (RingBuf_Available() < pkt_len) return; // 数据未齐，等待
        
        // 读取 MQTT Fixed Header
        uint8_t cmd;
        RingBuf_Read(&cmd);
        pkt_len--;
        
        uint8_t type = cmd & 0xF0;
        
        if (type == MQTT_PKT_CONNACK) {
            // 跳过剩余长度(1) + Flags(1) + ReturnCode(1)
            // 严谨做法：读取剩余长度
            uint8_t len_byte;
            RingBuf_Read(&len_byte);
            // 跳过变长部分的剩余字节(如果有)
            while ((len_byte & 0x80) != 0) {
                 RingBuf_Read(&len_byte);
            }
            
            uint8_t flags, rc;
            if (RingBuf_Read(&flags) && RingBuf_Read(&rc)) {
                if (rc == 0) {
                    g_state = MQTT_STATE_CONNECTED;
                    g_at_state = AT_IDLE;
                    g_keepalive_tick = HAL_GetTick();
                    Log("MQTT 已连接!\r\n");
                } else {
                    Log("MQTT 连接拒绝! RC=%d\r\n", rc);
                    g_state = MQTT_STATE_ERROR;
                    g_last_error = MQTT_ERR_MQTT_REFUSED;
                }
            } else {
                g_state = MQTT_STATE_ERROR;
            }
        } 
        else if (type == MQTT_PKT_SUBACK) {
            // SUBACK: 90 <Len> <ID MSB> <ID LSB> <RC>
             uint8_t len_byte;
            RingBuf_Read(&len_byte);
            while ((len_byte & 0x80) != 0) {
                 RingBuf_Read(&len_byte);
            }
            RingBuf_Skip(3); // ID(2) + RC(1)
            Log("订阅成功!\r\n");
        }
        else if (type == MQTT_PKT_PUBLISH) {
            // 解析 Publish
            uint32_t rem_len = 0;
            uint32_t multiplier = 1;
            uint8_t byte;
            
            // 解码剩余长度
            do {
                RingBuf_Read(&byte);
                rem_len += (byte & 127) * multiplier;
                multiplier *= 128;
            } while ((byte & 128) != 0);
            
            // 读取 Topic Length
            uint8_t msb, lsb;
            RingBuf_Read(&msb);
            RingBuf_Read(&lsb);
            uint16_t topic_len = (msb << 8) | lsb;
            
            // 读取 Topic
            char topic[64];
            uint16_t read_len = (topic_len < sizeof(topic)-1) ? topic_len : sizeof(topic)-1;
            for(int i=0; i<read_len; i++) {
                uint8_t c;
                RingBuf_Read(&c);
                topic[i] = (char)c;
            }
            topic[read_len] = 0;
            if (topic_len > read_len) RingBuf_Skip(topic_len - read_len);
            
            // Payload
            // 剩余长度 - TopicLen(2) - Topic - PacketID(if QoS>0)
            uint8_t qos = (cmd >> 1) & 0x03;
            uint16_t header_overhead = 2 + topic_len;
            if (qos > 0) header_overhead += 2;

            if (rem_len >= header_overhead) {
                uint32_t payload_len = rem_len - header_overhead;
                
                // Skip Packet ID if QoS > 0
                if (qos > 0) RingBuf_Skip(2);
                
                uint8_t payload[128];
                read_len = (payload_len < sizeof(payload)) ? payload_len : sizeof(payload);
                 for(int i=0; i<read_len; i++) {
                    RingBuf_Read(&payload[i]);
                }
                if (payload_len > read_len) RingBuf_Skip(payload_len - read_len);
                
                if (g_msg_callback) {
                    g_msg_callback(topic, payload, read_len);
                }
            }
        }
        else {
            // 其他包，直接丢弃
            RingBuf_Skip(pkt_len);
        }
        
        p_state = STATE_FIND_IPD;
    }
}

/* ==========================================
 * 公共操作
 * ========================================== */

MQTT_Error_t MQTT_Publish(const char *topic, const char *message, bool retain) {
    if (g_state != MQTT_STATE_CONNECTED) return MQTT_ERR_NOT_CONNECTED;
    
    if (topic == NULL && message == NULL) {
        // PINGREQ
        char cmd[32];
        uint8_t pkt[2] = {MQTT_PKT_PINGREQ, 0x00};
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=2\r\n");
        SendAT(cmd);
        HAL_Delay(10);
        SendRaw(pkt, 2);
        g_keepalive_tick = HAL_GetTick();
        return MQTT_OK;
    }

    uint16_t idx = 0;
    uint16_t msg_len = strlen(message);
    uint32_t rem_len = (2 + strlen(topic)) + msg_len;
    
    g_tx_buffer[idx++] = MQTT_PKT_PUBLISH | (retain ? 1 : 0);
    idx += EncodeLength(&g_tx_buffer[idx], rem_len);
    idx += EncodeString(&g_tx_buffer[idx], topic);
    memcpy(&g_tx_buffer[idx], message, msg_len);
    idx += msg_len;
    
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", idx);
    SendAT(cmd);
    HAL_Delay(10);
    SendRaw(g_tx_buffer, idx);
    
    return MQTT_OK;
}

MQTT_Error_t MQTT_Subscribe(const char *topic, uint8_t qos) {
    if (g_state != MQTT_STATE_CONNECTED) return MQTT_ERR_NOT_CONNECTED;
    
    uint16_t idx = 0;
    uint32_t rem_len = 2 + (2 + strlen(topic)) + 1;
    
    g_tx_buffer[idx++] = MQTT_PKT_SUBSCRIBE | 0x02; // QoS 1
    idx += EncodeLength(&g_tx_buffer[idx], rem_len);
    g_tx_buffer[idx++] = 0x00; // Packet ID MSB
    g_tx_buffer[idx++] = 0x01; // Packet ID LSB
    idx += EncodeString(&g_tx_buffer[idx], topic);
    g_tx_buffer[idx++] = qos;
    
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", idx);
    SendAT(cmd);
    HAL_Delay(10);
    SendRaw(g_tx_buffer, idx);
    
    return MQTT_OK;
}
