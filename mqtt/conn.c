#include "conn.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ==========================================
 * 常量与宏定义
 * ========================================== */
#define AT_TIMEOUT_DEFAULT      1000
#define AT_TIMEOUT_WIFI         20000
#define AT_TIMEOUT_TCP          10000

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
 * 全局变量
 * ========================================== */
static MQTT_State_t g_state = MQTT_STATE_RESET;
static uint8_t g_rx_buffer[MQTT_RX_BUFFER_SIZE];
static uint8_t g_tx_buffer[MQTT_TX_BUFFER_SIZE];
static volatile uint16_t g_rx_head = 0;
static volatile uint16_t g_rx_tail = 0;
static MQTT_MessageCallback g_msg_callback = NULL;
static uint32_t g_last_keepalive = 0;

/* ==========================================
 * 缓冲区与底层函数声明
 * ========================================== */
static void UART_Poll(void);
static void Log(const char *fmt, ...);
static void RingBuf_Write(uint8_t byte);
static uint16_t RingBuf_Available(void);
static uint8_t RingBuf_ReadByte(void);
static uint8_t RingBuf_Peek(uint16_t offset);
static void RingBuf_Skip(uint16_t len);
static void ProcessIncomingData(void);

/* ==========================================
 * 接口实现
 * ========================================== */

void MQTT_Init(void) {
    g_state = MQTT_STATE_RESET;
    g_rx_head = 0;
    g_rx_tail = 0;
    Log("MQTT 初始化...\r\n");
}

void MQTT_SetCallback(MQTT_MessageCallback cb) {
    g_msg_callback = cb;
}

MQTT_State_t MQTT_GetState(void) {
    return g_state;
}

/* ==========================================
 * 辅助功能函数 (阻塞式 AT 指令)
 * ========================================== */

/* 检查缓冲区是否包含字符串，如果包含则消耗掉之前的数据 */
static bool CheckBuffer(const char *expected) {
    uint16_t available = RingBuf_Available();
    uint16_t len = strlen(expected);
    
    if (available < len) return false;
    
    for (uint16_t i = 0; i <= available - len; i++) {
        bool match = true;
        for (uint16_t j = 0; j < len; j++) {
            if (RingBuf_Peek(i + j) != (uint8_t)expected[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            RingBuf_Skip(i + len); // 消耗掉匹配字符串及之前的数据
            return true;
        }
    }
    return false;
}

/* 阻塞等待响应 */
static bool WaitFor(const char *expected, uint32_t timeout_ms) {
    uint32_t start = HAL_GetTick();
    while (HAL_GetTick() - start < timeout_ms) {
        UART_Poll(); // 持续接收数据
        if (CheckBuffer(expected)) {
            return true;
        }
    }
    return false;
}

/* 发送指令并等待响应 */
static bool SendAT(const char *cmd, const char *expected, uint32_t timeout_ms) {
    HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t*)cmd, strlen(cmd), 100);
    // Log(">> %s", cmd); // 调试打印发送内容
    
    if (expected == NULL) return true;
    
    if (WaitFor(expected, timeout_ms)) {
        return true;
    }
    return false;
}

/* 发送原始数据 */
static void SendRaw(const uint8_t *data, uint16_t len) {
    HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t*)data, len, 100);
}

/* ==========================================
 * 编码辅助函数
 * ========================================== */
static uint8_t EncodeLength(uint8_t *buf, uint32_t length) {
    uint8_t len_bytes = 0;
    do {
        uint8_t encoded_byte = length % 128;
        length /= 128;
        if (length > 0) encoded_byte |= 0x80;
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
 * 连接逻辑 (阻塞流程，清晰易懂)
 * ========================================== */
static void PerformConnect(void) {
    char cmd[128];
    
    Log("正在复位模块...\r\n");
    SendAT("AT+RST\r\n", NULL, 0);
    HAL_Delay(2000); // 等待重启
    // 清空重启时的乱码
    g_rx_head = g_rx_tail = 0; 
    
    SendAT("AT\r\n", "OK", 1000);
    SendAT("AT+CWMODE=1\r\n", "OK", 1000);
    SendAT("AT+CIPMUX=0\r\n", "OK", 1000);
    
    Log("正在连接 WiFi: %s ...\r\n", WIFI_SSID);
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", WIFI_SSID, WIFI_PASSWORD);
    HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t*)cmd, strlen(cmd), 100);
    
    // 等待连接结果 (兼容 WIFI CONNECTED, WIFI GOT IP, OK)
    uint32_t wifi_wait = HAL_GetTick();
    bool wifi_ok = false;
    while (HAL_GetTick() - wifi_wait < AT_TIMEOUT_WIFI) {
        UART_Poll();
        if (CheckBuffer("WIFI CONNECTED") || CheckBuffer("WIFI GOT IP") || CheckBuffer("OK")) {
            wifi_ok = true;
            break;
        }
        if (CheckBuffer("FAIL")) {
             Log("WiFi 连接失败 (密码错误)\r\n");
             break;
        }
    }
    
    if (!wifi_ok) {
        Log("WiFi 连接超时或失败!\r\n");
        g_state = MQTT_STATE_ERROR;
        return;
    }
    Log("WiFi 已连接.\r\n");

    Log("正在连接 TCP: %s:%d ...\r\n", MQTT_BROKER, MQTT_PORT);
    SendAT("AT+CIPCLOSE\r\n", NULL, 0); // 防御性关闭
    HAL_Delay(500);
    
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", MQTT_BROKER, MQTT_PORT);
    HAL_UART_Transmit(MQTT_UART_HANDLE, (uint8_t*)cmd, strlen(cmd), 100);
    
    // TCP 连接可能返回 CONNECT, OK, 或者 ERROR, DNS Fail 等
    // 我们在这里轮询几个可能的结果
    uint32_t start = HAL_GetTick();
    bool tcp_success = false;
    while (HAL_GetTick() - start < AT_TIMEOUT_TCP) {
        UART_Poll();
        if (CheckBuffer("CONNECT") || CheckBuffer("OK")) {
            tcp_success = true;
            break;
        }
        if (CheckBuffer("DNS Fail")) {
            Log("错误: DNS 解析失败! 请检查域名或网络.\r\n");
            break;
        }
        if (CheckBuffer("ERROR") || CheckBuffer("CLOSED")) {
            Log("错误: TCP 连接被拒绝或关闭.\r\n");
            break;
        }
    }
    
    if (!tcp_success) {
        Log("TCP 连接失败.\r\n");
        g_state = MQTT_STATE_ERROR;
        return;
    }
    Log("TCP 已连接.\r\n");
    
    // 发送 MQTT CONNECT 包
    Log("正在发送 MQTT 登录包...\r\n");
    uint16_t idx = 0;
    uint32_t rem_len = (2 + 4) + 1 + 1 + 2 + (2 + strlen(MQTT_CLIENT_ID));
    bool use_user = (strlen(MQTT_USERNAME) > 0);
    bool use_pass = (strlen(MQTT_PASSWORD) > 0);
    
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
    
    g_tx_buffer[idx++] = (MQTT_KEEPALIVE >> 8) & 0xFF;
    g_tx_buffer[idx++] = MQTT_KEEPALIVE & 0xFF;
    idx += EncodeString(&g_tx_buffer[idx], MQTT_CLIENT_ID);
    if (use_user) idx += EncodeString(&g_tx_buffer[idx], MQTT_USERNAME);
    if (use_pass) idx += EncodeString(&g_tx_buffer[idx], MQTT_PASSWORD);

    // 发送 CIPSEND
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", idx);
    if (!SendAT(cmd, ">", 2000)) {
        Log("发送 CIPSEND 失败.\r\n");
        g_state = MQTT_STATE_ERROR;
        return;
    }
    
    SendRaw(g_tx_buffer, idx);
    
    // 等待 CONNACK (ProcessIncomingData 会处理，但我们需要在这里确认状态)
    // 简单起见，我们等待状态变为 CONNECTED
    // 这里我们临时把状态设为 MQTT_CONNECTING，让 ProcessIncomingData 去处理 CONNACK
    g_state = MQTT_STATE_MQTT_CONNECTING;
    
    start = HAL_GetTick();
    while (HAL_GetTick() - start < 5000) {
        UART_Poll();
        ProcessIncomingData(); // 解析 CONNACK
        if (g_state == MQTT_STATE_CONNECTED) {
            Log("MQTT 登录成功!\r\n");
            g_last_keepalive = HAL_GetTick();
            return;
        }
        if (g_state == MQTT_STATE_ERROR) {
            Log("MQTT 登录被拒绝.\r\n");
            return;
        }
    }
    
    Log("等待 MQTT CONNACK 超时.\r\n");
    g_state = MQTT_STATE_ERROR;
}

/* ==========================================
 * 主循环
 * ========================================== */
void MQTT_Run(void) {
    UART_Poll();
    ProcessIncomingData();
    
    switch (g_state) {
        case MQTT_STATE_RESET:
        case MQTT_STATE_ERROR:
            // 简单处理：出错后等待 5 秒重试
            HAL_Delay(5000); 
            PerformConnect();
            break;
            
        case MQTT_STATE_CONNECTED:
            // 心跳处理
            if (HAL_GetTick() - g_last_keepalive > (MQTT_KEEPALIVE * 1000) / 2) {
                MQTT_Publish(NULL, NULL, false); // PING
                g_last_keepalive = HAL_GetTick();
            }
            break;
            
        default: break;
    }
}

/* ==========================================
 * 数据接收与解析
 * ========================================== */

static void ProcessIncomingData(void) {
    // 简化解析：寻找 +IPD
    while (RingBuf_Available() > 0) {
        if (CheckBuffer("+IPD,")) {
            // 读取长度
            uint16_t len = 0;
            while (1) {
                if (RingBuf_Available() == 0) return; // 等待数据
                uint8_t c = RingBuf_Peek(0);
                if (c == ':') {
                    RingBuf_Skip(1);
                    break;
                }
                if (c >= '0' && c <= '9') {
                    len = len * 10 + (c - '0');
                    RingBuf_Skip(1);
                } else {
                    return; // 格式错误
                }
            }
            
            // 等待 Payload 全部到达
            uint32_t wait_start = HAL_GetTick();
            while (RingBuf_Available() < len) {
                UART_Poll();
                if (HAL_GetTick() - wait_start > 500) break; // 接收超时
            }
            
            if (RingBuf_Available() < len) return; // 数据不完整，下次再处理
            
            // 读取 MQTT 包头
            uint8_t header = RingBuf_ReadByte();
            len--;
            
            uint8_t type = header & 0xF0;
            
            if (type == MQTT_PKT_CONNACK) {
                RingBuf_Skip(len); // 跳过剩余部分
                g_state = MQTT_STATE_CONNECTED;
            }
            else if (type == MQTT_PKT_PUBLISH) {
                // 简单的 PUBLISH 解析 (假设 QoS=0)
                // 剩余长度字段 (变长)
                uint32_t rem_len = 0;
                uint32_t multiplier = 1;
                uint8_t byte;
                do {
                    byte = RingBuf_ReadByte();
                    rem_len += (byte & 127) * multiplier;
                    multiplier *= 128;
                    len--;
                } while ((byte & 128) != 0);
                
                // Topic 长度
                uint8_t msb = RingBuf_ReadByte();
                uint8_t lsb = RingBuf_ReadByte();
                uint16_t topic_len = (msb << 8) | lsb;
                len -= 2;
                
                // 读取 Topic
                char topic[64];
                uint16_t read_topic = (topic_len < 63) ? topic_len : 63;
                for(int i=0; i<read_topic; i++) topic[i] = RingBuf_ReadByte();
                topic[read_topic] = 0;
                if (topic_len > read_topic) for(int i=0; i<topic_len-read_topic; i++) RingBuf_ReadByte();
                len -= topic_len;
                
                // Payload
                uint8_t payload[128];
                uint16_t payload_len = len; // 剩下的都是 Payload (QoS0)
                uint16_t read_payload = (payload_len < 127) ? payload_len : 127;
                
                for(int i=0; i<read_payload; i++) payload[i] = RingBuf_ReadByte();
                payload[read_payload] = 0;
                
                // 消耗剩余字节
                if (payload_len > read_payload) {
                    for(int i=0; i<payload_len-read_payload; i++) RingBuf_ReadByte();
                }
                
                if (g_msg_callback) g_msg_callback(topic, payload, read_payload);
            }
            else {
                // 其他包直接跳过
                RingBuf_Skip(len);
            }
        } 
        else {
            // 如果不是 +IPD，逐字节前移，防止死锁
            // 但为了效率，我们只有在确定不是 +IPD 的开头时才跳过
            // 简单做法：如果没有匹配 +IPD，就丢弃一个字节
             RingBuf_Skip(1);
        }
    }
}

/* ==========================================
 * 发送接口
 * ========================================== */

MQTT_Error_t MQTT_Publish(const char *topic, const char *message, bool retain) {
    if (g_state != MQTT_STATE_CONNECTED && topic != NULL) return MQTT_ERR_NOT_CONNECTED;
    
    // PINGREQ
    if (topic == NULL) {
        SendAT("AT+CIPSEND=2\r\n", ">", 1000);
        uint8_t pkt[2] = {MQTT_PKT_PINGREQ, 0x00};
        SendRaw(pkt, 2);
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
    if (SendAT(cmd, ">", 2000)) {
        SendRaw(g_tx_buffer, idx);
        return MQTT_OK;
    }
    return MQTT_ERR_UART;
}

MQTT_Error_t MQTT_Subscribe(const char *topic, uint8_t qos) {
    if (g_state != MQTT_STATE_CONNECTED) return MQTT_ERR_NOT_CONNECTED;
    
    uint16_t idx = 0;
    uint32_t rem_len = 2 + (2 + strlen(topic)) + 1;
    
    g_tx_buffer[idx++] = MQTT_PKT_SUBSCRIBE | 0x02; 
    idx += EncodeLength(&g_tx_buffer[idx], rem_len);
    g_tx_buffer[idx++] = 0x00; 
    g_tx_buffer[idx++] = 0x01; 
    idx += EncodeString(&g_tx_buffer[idx], topic);
    g_tx_buffer[idx++] = qos;
    
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", idx);
    if (SendAT(cmd, ">", 2000)) {
        SendRaw(g_tx_buffer, idx);
        return MQTT_OK;
    }
    return MQTT_ERR_UART;
}

/* ==========================================
 * 缓冲区底层实现
 * ========================================== */
static void UART_Poll(void) {
    uint8_t byte;
    // 每次最多读 128 字节
    int count = 128;
    while (count-- > 0 && HAL_UART_Receive(MQTT_UART_HANDLE, &byte, 1, 0) == HAL_OK) {
        RingBuf_Write(byte);
        #if MQTT_DEBUG_ENABLE && defined(MQTT_DEBUG_UART)
        HAL_UART_Transmit(MQTT_DEBUG_UART, &byte, 1, 10);
        #endif
    }
}

static void RingBuf_Write(uint8_t byte) {
    uint16_t next = (g_rx_head + 1) % MQTT_RX_BUFFER_SIZE;
    if (next != g_rx_tail) {
        g_rx_buffer[g_rx_head] = byte;
        g_rx_head = next;
    }
}

static uint16_t RingBuf_Available(void) {
    if (g_rx_head >= g_rx_tail) return g_rx_head - g_rx_tail;
    return MQTT_RX_BUFFER_SIZE - g_rx_tail + g_rx_head;
}

static uint8_t RingBuf_ReadByte(void) {
    if (g_rx_head == g_rx_tail) return 0;
    uint8_t byte = g_rx_buffer[g_rx_tail];
    g_rx_tail = (g_rx_tail + 1) % MQTT_RX_BUFFER_SIZE;
    return byte;
}

static uint8_t RingBuf_Peek(uint16_t offset) {
    return g_rx_buffer[(g_rx_tail + offset) % MQTT_RX_BUFFER_SIZE];
}

static void RingBuf_Skip(uint16_t len) {
    g_rx_tail = (g_rx_tail + len) % MQTT_RX_BUFFER_SIZE;
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
