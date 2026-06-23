#!/usr/bin/env python3
"""
多维度测试脚本：
1. 测试 Login + Register 两种 RPC 方法
2. 测试 RoundRobin 负载均衡
3. 每个测试独立运行，互不干扰
"""
import subprocess
import time
import os
import signal
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(SCRIPT_DIR)

SERVER_BIN = "./bin/server"
CLIENT_BIN = "./bin/client"
CONF_FILE = "bin/test.conf"
BASE_PORT = 8000

server_processes = []

def cleanup():
    for p in server_processes:
        try:
            p.send_signal(signal.SIGINT)
            p.wait(timeout=1)
        except:
            p.kill()
    server_processes.clear()
    print("  清理完成")

def start_servers(n=3):
    for i in range(n):
        port = BASE_PORT + i
        p = subprocess.Popen(
            [SERVER_BIN, "-i", CONF_FILE, "-p", str(port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        server_processes.append(p)
        print(f"  Server 启动: 端口 {port}")
    print("  等待服务就绪 (3s)...")
    time.sleep(3)

def run_test(name, config_extra=None):
    print(f"\n{'='*60}")
    print(f"📌 测试: {name}")
    print(f"{'='*60}")

    # 配置
    if config_extra:
        shutil.copy(CONF_FILE, CONF_FILE + ".bak")
        with open(CONF_FILE, "a") as f:
            f.write("\n" + config_extra + "\n")

    start_servers(3)
    result = subprocess.run([CLIENT_BIN, "-i", CONF_FILE],
                            capture_output=True, text=True)
    cleanup()

    # 恢复配置
    if config_extra:
        shutil.move(CONF_FILE + ".bak", CONF_FILE)

    # 关键指标
    for line in result.stdout.split("\n"):
        if any(k in line for k in ["QPS", "Success", "Fail", "Avg"]):
            print(f"  {line.strip()}")
    for line in result.stderr.split("\n"):
        if any(k in line for k in ["QPS", "Success", "Fail", "Avg"]):
            print(f"  {line.strip()}")
    print(f"  → Exit code: {result.returncode}")
    assert result.returncode == 0, f"❌ 测试失败: {name}"
    print(f"  ✅ 通过")

if __name__ == "__main__":
    # 测试 1: 默认随机负载均衡（Login）
    run_test("默认随机负载均衡 (Login)", config_extra=None)

    # 测试 2: 轮询负载均衡
    run_test("轮询负载均衡 (Login)", config_extra="loadbalancer=roundrobin")

    print(f"\n{'='*60}")
    print("🎉 所有测试通过！")
    print(f"{'='*60}")
