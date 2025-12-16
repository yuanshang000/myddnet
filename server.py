import socket
import struct
import time

HOST = '127.0.0.1'
PORT = 6666

def start_server():
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind((HOST, PORT))
    server.listen(1)
    print(f"python: 等待连接...")
    
    conn, addr = server.accept()
    print(f"python: 连接成功!")

    # 一个简单的计时器，用来测试自动控制
    start_time = time.time()

    while True:
        try:
            # 1. 接收游戏发来的坐标 (8字节)
            data = conn.recv(8)
            if not data: break
            
            x, y = struct.unpack('ff', data)
            # print(f"Pos: {x:.1f}, {y:.1f}") # 刷屏太快可以先注释掉
            
            # 2. 计算控制指令 (AI 逻辑)
            # 这里的逻辑是：每 2 秒一个循环，前 1 秒往右，后 1 秒往左
            current_time = time.time()
            elapsed = current_time - start_time
            
            if int(elapsed) % 2 == 0:
                move_dir = 1  # 向右
            else:
                move_dir = -1 # 向左
                
            # 3. 打包指令发送回 C++
            # 格式: 'i' (int, 4字节) -> 代表 Direction (-1, 0, 1)
            # 你以后可以在这里加 Jump, Hook 等
            command = struct.pack('i', move_dir) 
            conn.send(command)
            
        except Exception as e:
            print(f"Error: {e}")
            break
            
    conn.close()

if __name__ == "__main__":
    start_server()