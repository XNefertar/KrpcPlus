import subprocess
import time
import os
import signal
import sys

# 切换到脚本所在目录，确保路径正确
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(SCRIPT_DIR)

SERVER_NUM = 5
BASE_PORT = 8000
CONF_FILE = "bin/test.conf"
SERVER_BIN = "./bin/server"
CLIENT_BIN = "./bin/client"

server_processes = []

def cleanup():
    print("\n--- 正在关闭所有服务端 ---")
    for p in server_processes:
        try:
            p.send_signal(signal.SIGINT)
            p.wait(timeout=1)
        except:
            p.kill()
    print("清理完成。")

try:
    print(f"--- 启动 {SERVER_NUM} 个服务端 ---")
    for i in range(SERVER_NUM):
        port = BASE_PORT + i
        cmd = [SERVER_BIN, "-i", CONF_FILE, "-p", str(port)]
        # 服务端输出重定向到 devnull，防止日志刷屏掩盖性能结果
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        server_processes.append(p)
        print(f"Server 启动在端口: {port}")

    # 等待 ZK 注册和 Server 就绪
    print("等待服务端就绪 (3s)...")
    time.sleep(3)

    print("\n--- 启动客户端进行性能测试 ---")
    # subprocess.run 会实时显示输出到当前终端，且不会在测试没结束前执行后续代码
    subprocess.run([CLIENT_BIN, "-i", CONF_FILE])

except KeyboardInterrupt:
    print("\n用户手动中止测试")
finally:
    cleanup()

print("\n测试结束")
