import cv2
import socket
import struct
import time
import pyautogui
import numpy as np
from io import BytesIO

# 配置参数
SERVER_IP = "127.0.0.1" # 本地测试，如果 serv.py 在另一台机器，请修改 IP
SERVER_PORT = 8888

# 初始化 UDP Socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

print(f"Sending screen capture to {SERVER_IP}:{SERVER_PORT}")

try:
    while True:
        # 1. 捕获屏幕截图
        screenshot = pyautogui.screenshot()
        
        # 为了模拟 ESP32-CAM，我们将截图缩小，确保 JPEG 大小不超过 UDP 单包限制 (约 64KB)
        # ESP32-CAM 常见的流分辨率是 QVGA (320x240) 或 VGA (640x480)
        # 这里我们缩小到 640x480
        screenshot = screenshot.resize((640, 480))
        
        # 转换 OpenCV 格式 (BGR)
        frame = cv2.cvtColor(np.array(screenshot), cv2.COLOR_RGB2BGR)
        
        # 2. 编码为 JPEG
        # 调整 quality 以控制大小
        encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 80]
        result, encimg = cv2.imencode('.jpg', frame, encode_param)
        
        if not result:
            print("Failed to encode image")
            continue
            
        jpeg_data = encimg.tobytes()
        
        # 检查大小
        if len(jpeg_data) > 60000:
            print(f"Warning: Packet too large ({len(jpeg_data)} bytes). Dropping frame.")
            # 简单策略：如果太大就跳过，或者进一步降低质量
            continue
            
        # 3. 构建数据包：帧编号(4 bytes) + JPEG数据
        # 模拟 millis()
        frame_num = int(time.time() * 1000) & 0xFFFFFFFF
        header = struct.pack('<I', frame_num)
        
        packet = header + jpeg_data
        
        # 4. 发送 UDP
        sock.sendto(packet, (SERVER_IP, SERVER_PORT))
        
        # 控制帧率，模拟 ESP32 的 delay(33)
        time.sleep(0.033) 
        
except KeyboardInterrupt:
    print("Stopping client...")
finally:
    sock.close()
