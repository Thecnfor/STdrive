/*
 * ESP32-CAM 网络摄像头主程序
 * 功能：通过WiFi实现摄像头实时视频流传输、拍照和参数控制
 */

// 包含摄像头库
#include "esp_camera.h"
// 包含WiFi库
#include <WiFi.h>

// ===========================
// 摄像头型号配置（在board_config.h中选择）
// ===========================
#include "board_config.h"

// ===========================
// WiFi网络配置
// ===========================
const char *ssid = "van";          // WiFi名称
const char *password = "amkk5637"; // WiFi密码

// 定义你的接收端服务器地址（比如你电脑的 IP）
// const char* serverIP = "192.168.1.100";
// const uint16_t serverPort = 8888;

// 函数声明
void startCameraServer(); // 启动摄像头HTTP服务器
void setupLedFlash();     // 初始化闪光灯（如果有）

void setup() {
  // 初始化串口，波特率115200，用于调试输出
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // 配置摄像头参数
  camera_config_t config;
  // LED PWM通道配置（用于闪光灯）
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  // 摄像头数据总线引脚映射
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  // 摄像头控制信号引脚映射
  config.pin_xclk = XCLK_GPIO_NUM;   // 时钟信号
  config.pin_pclk = PCLK_GPIO_NUM;   // 像素时钟
  config.pin_vsync = VSYNC_GPIO_NUM; // 垂直同步
  config.pin_href = HREF_GPIO_NUM;   // 水平参考

  // I2C接口引脚映射（用于摄像头寄存器配置）
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;

  // 摄像头电源控制引脚
  config.pin_pwdn = PWDN_GPIO_NUM;   // 电源控制
  config.pin_reset = RESET_GPIO_NUM; // 复位引脚

  // 摄像头时钟频率（20MHz）
  config.xclk_freq_hz = 20000000;
  // 帧大小配置（UXGA: 1600x1200）
  config.frame_size = FRAMESIZE_UXGA;
  // 像素格式（JPEG格式，适合网络传输）
  config.pixel_format = PIXFORMAT_JPEG; // for streaming
  // config.pixel_format = PIXFORMAT_RGB565; // 用于人脸检测/识别

  // 图像抓取模式（当缓冲区为空时抓取）
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  // 帧缓冲区位置（使用PSRAM，需模块支持）
  config.fb_location = CAMERA_FB_IN_PSRAM;
  // JPEG图像质量（0-63，越小质量越高）
  config.jpeg_quality = 12;
  // 帧缓冲区数量
  config.fb_count = 1;

  // 根据硬件情况调整摄像头参数
  // 如果存在PSRAM（扩展内存），使用更高分辨率和质量
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;              // 提高JPEG质量
      config.fb_count = 2;                   // 增加帧缓冲数量，提高流畅度
      config.grab_mode = CAMERA_GRAB_LATEST; // 抓取最新帧
    } else {
      // 如果没有PSRAM，限制帧大小以节省内存
      config.frame_size = FRAMESIZE_SVGA;     // SVGA: 800x600
      config.fb_location = CAMERA_FB_IN_DRAM; // 使用内部RAM
    }
  } else {
    // 对于非JPEG格式（如人脸检测），使用最佳帧大小
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

// 特定摄像头型号的额外配置
#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // 初始化摄像头
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  // 获取摄像头传感器指针，用于后续参数调整
  sensor_t *s = esp_camera_sensor_get();
  // 针对特定传感器（OV3660）进行初始设置
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);       // 垂直翻转图像
    s->set_brightness(s, 1);  // 增加亮度
    s->set_saturation(s, -2); // 降低饱和度
  }
  // 降低初始帧大小以提高帧率
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA); // QVGA: 320x240
  }

// 特定摄像头型号的额外传感器配置
#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);   // 垂直翻转
  s->set_hmirror(s, 1); // 水平镜像
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1); // 垂直翻转
#endif

// 如果定义了LED_GPIO_NUM，则设置闪光灯
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // 连接WiFi网络
  WiFi.begin(ssid, password);
  WiFi.setSleep(false); // 关闭WiFi休眠，保持连接稳定

  Serial.print("WiFi connecting");
  // 等待WiFi连接成功
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  udp.begin(8888);
}

#include <WiFiUdp.h>
// 主循环函数
void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    // UDP发送JPEG帧
    udp.beginPacket(serverIP, serverPort);

    // 添加帧头：帧编号 + 时间戳 + 数据长度
    uint32_t frame_num = millis();
    udp.write((uint8_t *)&frame_num, 4);
    udp.write((uint8_t *)fb->buf, fb->len);

    udp.endPacket();
    esp_camera_fb_return(fb);
  }
  delay(33); // ~30fps
}
