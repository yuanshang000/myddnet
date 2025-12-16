import socket
import struct
import numpy as np
import math
import gymnasium as gym
from gymnasium import spaces
from stable_baselines3 import PPO
import time

# ==========================================
# DDNet 训练环境 (全动作版)
# ==========================================
class DDNetEnv(gym.Env):
    def __init__(self):
        super(DDNetEnv, self).__init__()
        
        # 动作空间: MultiDiscrete 允许同时执行多种动作
        # [0]: 移动 (0=停, 1=左, 2=右)
        # [1]: 跳跃 (0=不跳, 1=跳)
        # [2]: 钩索 (0=不钩, 1=钩)
        # [3]: 瞄准角度 (0~35, 共36个方向, 每个10度)
        # 修改: 增加开火动作 (0=不开火, 1=开火)
        # 现在是 5 个维度: [移动(3), 跳跃(2), 钩索(2), 开火(2), 瞄准(36)]
        self.action_space = spaces.MultiDiscrete([3, 2, 2, 2, 36]) 
        
        # 观察空间: 目前还是只有 X, Y 坐标 (之后我们要加雷达)
        # 范围设大一点防止越界
        self.observation_space = spaces.Box(low=-np.inf, high=np.inf, shape=(123,), dtype=np.float32)
        
        # 网络连接配置
        self.host = '127.0.0.1'
        self.port = 6666
        self.sock = None
        self.conn = None
        
        # 状态记录
        self.prev_x = 0
        self.steps = 0
        
        self._start_server()

    def _start_server(self):
        """启动 Socket 服务器等待游戏连接"""
        if self.sock: self.sock.close()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.bind((self.host, self.port))
        self.sock.listen(1)
        print("------------------------------------------------")
        print(f"Python: 等待 DDNet 连接 ({self.host}:{self.port})...")
        print("请启动 DDNet 游戏并进入地图!")
        print("------------------------------------------------")
        self.conn, addr = self.sock.accept()
        print(f"Python: 游戏已连接! 来自 {addr}")
        # 设置阻塞模式，确保收发同步
        self.sock.setblocking(True)

    def reset(self, seed=None, options=None):
        """回合重置"""
        self.steps = 0
        # 获取第一帧数据
        obs = self._get_observation()
        self.prev_x = obs[0]
        return obs, {}

    def step(self, action):
        self.steps += 1
        
        # --- 1. 解析动作 ---
        move_idx = action[0]
        jump     = action[1]
        hook     = action[2]
        fire     = action[3] # 新增: 开火
        aim_idx  = action[4] # 注意: aim 变成第 5 个了

        
        # 转换移动指令: 1->左(-1), 2->右(1), 0->停(0)
        move_val = 0
        if move_idx == 1: move_val = -1
        elif move_idx == 2: move_val = 1
        
        # 转换瞄准角度 -> 鼠标坐标 (TargetX, TargetY)
        # 假设鼠标距离人物 100 像素
        angle_deg = aim_idx * 10
        angle_rad = math.radians(angle_deg)
        target_x = int(100 * math.cos(angle_rad))
        target_y = int(100 * math.sin(angle_rad))
        
        # --- 2. 发送指令给 C++ ---
        # 格式: 5个整数 (move, jump, hook, target_x, target_y)
        # 发送数据: 增加一个整数位 'iiiiii' (6个int)
        # [Move, Jump, Hook, Fire, TargetX, TargetY]
        try:
            cmd_data = struct.pack('iiiiii', 
                                   move_val, 
                                   int(jump), 
                                   int(hook), 
                                   int(fire), # 发送开火
                                   target_x, 
                                   target_y)
            self.conn.send(cmd_data)
        except Exception as e:
            print(f"发送出错: {e}, 尝试重连...")
            self._start_server()
            return np.array([0,0], dtype=np.float32), 0, True, False, {}

        # --- 3. 获取新状态 ---
        obs = self._get_observation()
        current_x = obs[0]
        
        # --- 4. 计算奖励 ---
        # A. 进度奖励：向右走给分 (权重高)
        progress = (current_x - self.prev_x)
        reward = progress * 2.0 

        # B. 速度奖励：如果速度太慢(卡住了)，扣分
        if abs(progress) < 0.05:
            reward -= 0.5  # 没动？狠狠地打手板！

        move_action = action[0]
        if move_action == 2: # 尝试往右
            reward += 0.1
        elif move_action == 1: # 尝试往左
            reward -= 0.1
            
        self.prev_x = current_x
        
        # --- 5. 结束条件 ---
        terminated = False
        # 如果跑了 2000 步强制重置 (防止卡死)
        if self.steps > 2000:
            terminated = True
            
        return obs, reward, terminated, False, {}

    def _get_observation(self):
        """接收 C++ 发来的 8 字节 (float x, float y)"""
        try:
            # 1. 先收坐标 (8 bytes)
            pos_data = self.conn.recv(8)
            if not pos_data: return np.zeros(123, dtype=np.float32)
            x, y = struct.unpack('ff', pos_data)
            
            # 2. 再收地图雷达 (121 * 4 = 484 bytes)
            map_bytes = 121 * 4
            map_raw = self.conn.recv(map_bytes)
            # 确保收满
            while len(map_raw) < map_bytes:
                map_raw += self.conn.recv(map_bytes - len(map_raw))
                
            # 解析成数组
            map_data = np.frombuffer(map_raw, dtype=np.int32).astype(np.float32)
            
            # 3. 拼接在一起
            obs = np.concatenate(([x, y], map_data))
            if self.steps % 60 == 0: # 每 60 帧打印一次，防止刷屏
                print(f"坐标: X={obs[0]:.1f}, Y={obs[1]:.1f}, 雷达墙壁数={np.sum(obs[2:])}")
            return obs
            
        except Exception as e:
            print(f"recv error: {e}")
            return np.zeros(123, dtype=np.float32)
        
        
    def close(self):
        if self.conn: self.conn.close()
        if self.sock: self.sock.close()

# ==========================================
# 主程序
# ==========================================
if __name__ == "__main__":
    # 1. 创建环境
    env = DDNetEnv()
    
    # 2. 定义模型
    # 使用 MLP (多层感知机) 策略
    # 稍微把学习率调低一点，让它学得稳一点
    model = PPO("MlpPolicy", env, verbose=1, learning_rate=0.0003, batch_size=64)
    
    print("开始训练... AI 现在可以控制 [移动, 跳跃, 钩索, 瞄准] 了！")
    
    # 3. 开始训练
    # 建议先跑个 50,000 步看看效果
    try:
        model.learn(total_timesteps=1000000)
    except KeyboardInterrupt:
        print("训练被用户手动停止")
    
    # 4. 保存模型
    model.save("ddnet_ai_full_action")
    print("模型已保存: ddnet_ai_full_action.zip")
    
    env.close()