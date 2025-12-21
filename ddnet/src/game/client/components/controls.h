/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>
#include <vector>

#include <engine/client.h>
#include <deque>
#include <game/client/component.h>
#include <game/generated/protocol.h>


// [新增] 自定义 TAS 帧数据
struct TASFrame
{
	// 基础输入
	int m_Direction;
	int m_Jump;
	int m_Hook;
	int m_Fire;
	int m_TargetX;
	int m_TargetY;
	int m_PlayerFlags; // 最好也带上 Flag (比如聊天状态)

	// 武器系统 (重点修复)
	int m_WantedWeapon;
	int m_NextWeapon;
	int m_PrevWeapon;

	// [新增] 记录这一帧人物在哪里 (用于回退)
	vec2 m_DebugPos;
	vec2 m_DebugVel;
};


class CControls : public CComponent
{
public:

	struct GhostFrame
	{
		int64_t m_Time; // 记录时间
		vec2 m_Pos; // 当时位置
		int m_Direction;
		int m_Jump;
		int m_Hook;
		int m_Fire;
		int m_TargetX;
		int m_TargetY;
	};

	// [新增变量]
	bool m_GhostFollowEnabled;
	int m_GhostTargetID;
	std::deque<GhostFrame> m_GhostBuffer; // 使用双端队列存储历史
	bool m_AutoWiggleEnabled; // 自动抖动开关
	void ProcessAutoWiggle();
	// [新增函数]
	void ProcessGhostFollow();
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;
	void RenderFeatureHUD(); 
	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];

	// TAS 相关成员变量
	bool m_IsRecording;
	bool m_IsPlaying;
	int m_PlaybackIndex;
	std::vector<TASFrame> m_TASBuffer;
	bool m_TAS_Paused;
	bool m_StepPressed;
	bool m_UndoPressed;
	bool m_PausePressed;
	bool g_TasPaused;
	bool g_TasStep;
	// 在 CControls 类中添加:
	bool m_AutoRecordAfterPlay; // 标记是否在回放结束后自动切换为录制
	bool m_IsPausedRecording; // 新增：用于 F9 暂停录制模式

	// 功能开关变量
	bool m_AimbotEnabled;
	bool m_AutoBalanceEnabled;
	bool m_StackEnabled;
	bool m_AutoEdgeEnabled;
	bool m_AIEnabled;
	// TAS 绘图函数
	void RenderTAS(); // [新增]
	int m_TargetID;
	// 新增 Pseudo Fly 开关
	bool m_PseudoFlyEnabled; 
	bool m_AntiFreezeEnabled; // 新增变量
	void ProcessAntiFreeze(); // 新增函数声明
	// 自瞄FOV角度
	float m_AimbotFOV;

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	void ProcessAimbot(CNetObj_PlayerInput *pInput);
	void ProcessAimbot(); // 添加这一行
private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
	
	// TAS 相关命令回调函数
	static void ConTasSave(IConsole::IResult *pResult, void *pUserData);
	static void ConTasLoad(IConsole::IResult *pResult, void *pUserData);
	
	// TAS 相关方法
	void SaveTASDemo(const char *pFilename);
	bool LoadTASDemo(const char *pFilename);
	
	// AI 相关方法
	void CallAI();
};
#endif
