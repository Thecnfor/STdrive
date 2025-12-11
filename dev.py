import paho.mqtt.client as mqtt
import msvcrt
import time
import random

# MQTT 配置
BROKER = "emqx.van.xrak.xyz"
PORT = 1883
TOPIC = "LED"

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"成功连接到 MQTT Broker: {BROKER}")
        print("操作说明:")
        print("  按 '1' 发送 ON")
        print("  按 '0' 发送 OFF")
        print("  按 't' 切换 ON/OFF")
        print("  按 'q' 退出程序")
    else:
        print(f"连接失败，返回码: {rc}")

def main():
    # 兼容 paho-mqtt 2.x 和 1.x
    if hasattr(mqtt, 'CallbackAPIVersion'):
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    else:
        client = mqtt.Client()
        
    client.on_connect = on_connect

    print(f"正在连接到 {BROKER}...")
    try:
        client.connect(BROKER, PORT, 60)
    except Exception as e:
        print(f"连接出错: {e}")
        return

    client.loop_start()

    is_on = False

    try:
        while True:
            if msvcrt.kbhit():
                # 读取按键
                key_byte = msvcrt.getch()
                try:
                    key = key_byte.decode('utf-8').lower()
                except UnicodeDecodeError:
                    continue
                
                msg = None
                
                if key == '1':
                    msg = "ON"
                    is_on = True
                elif key == '0':
                    msg = "OFF"
                    is_on = False
                elif key == 't':
                    is_on = not is_on
                    msg = "ON" if is_on else "OFF"
                elif key == 'q':
                    print("退出程序...")
                    break
                
                if msg:
                    client.publish(TOPIC, msg)
                    print(f"已向主题 '{TOPIC}' 发送: {msg}")
                
            time.sleep(0.1)

    except KeyboardInterrupt:
        print("\n用户中断")
    finally:
        client.loop_stop()
        client.disconnect()
        print("连接已断开")

if __name__ == "__main__":
    main()
