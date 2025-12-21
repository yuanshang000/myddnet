/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/math.h>

#include <engine/client.h>
#include <engine/shared/config.h>
#include <deque>
#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <base/vmath.h>
#include <game/mapitems.h>
#include "controls.h"
extern bool g_TasPaused;
extern bool g_TasStep;

// ================= [高级自瞄辅助函数] =================

// 基础射线检测
bool IsLineBlocked(CCollision *pCol, vec2 Start, vec2 End)
{
	return pCol->IntersectLine(Start, End, 0, 0) != 0;
}

// [升级版] 宽射线检测 (模拟钩子厚度)
// 只有当钩子的中心、左边缘、右边缘都不会撞墙时，才算通过
bool IsHookPathClear(CCollision *pCol, vec2 Start, vec2 End)
{
	// 1. 检查中心线
	if(IsLineBlocked(pCol, Start, End))
		return false;

	// 2. 计算垂直向量，用于模拟钩子宽度 (钩子半径约为 2-3)
	vec2 Dir = normalize(End - Start);
	vec2 Perp = vec2(-Dir.y, Dir.x) * 3.0f; // 宽度偏移 3 像素 (留一点安全余量)

	// 3. 检查左右边缘线
	if(IsLineBlocked(pCol, Start + Perp, End + Perp))
		return false;
	if(IsLineBlocked(pCol, Start - Perp, End - Perp))
		return false;

	return true; // 三条线都通，这就是个完美的缝隙
}

// [升级版] 由内向外智能寻找钩点
vec2 GetViablePos(CCollision *pCol, vec2 MyPos, vec2 TargetPos)
{
	// 定义探测点列表 (按优先级排序：中心 -> 内圈 -> 外圈)
	// 这样可以确保：只要中心能钩，绝对不钩边缘，防止抖动
	struct Offset
	{
		float x, y;
	};
	Offset SearchPoints[] = {
		{0, 0}, // 1. 完美中心
		{0, -10}, // 2. 胸口/脖子
		{0, 10}, // 3. 腹部
		{-10, 0}, // 4. 左胸
		{10, 0}, // 5. 右胸
		{0, -24}, // 6. 头顶 (Tee 高度约 28)
		{0, 24}, // 7. 脚底
		{-24, 0}, // 8. 左臂外侧
		{24, 0}, // 9. 右臂外侧
		{-20, -20}, // 10. 左上角
		{20, -20}, // 11. 右上角
		{-20, 20}, // 12. 左下角
		{20, 20} // 13. 右下角
	};

	// 遍历所有点，做宽射线检测
	for(const auto &off : SearchPoints)
	{
		vec2 TryPos = TargetPos + vec2(off.x, off.y);

		// 使用宽射线检测，确保钩子不会擦边撞墙
		if(IsHookPathClear(pCol, MyPos, TryPos))
		{
			return TryPos; // 找到最稳的一个点，直接返回
		}
	}

	// 找不到任何安全路径
	return vec2(0, 0);
}
// ====================================================

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	mem_zero(m_aMousePos, sizeof(m_aMousePos));
	mem_zero(m_aMousePosOnAction, sizeof(m_aMousePosOnAction));
	mem_zero(m_aTargetPos, sizeof(m_aTargetPos));
	
	// 初始化新增的变量
	m_IsRecording = false;
	m_IsPlaying = false;
	m_PlaybackIndex = 0;
	m_TASBuffer.clear();
	m_AimbotEnabled = true;
	m_AutoBalanceEnabled = false; // 默认为关闭
	m_StackEnabled = false; // 默认为关闭
	m_AutoEdgeEnabled = false; // 默认为关闭
	m_AIEnabled = false; // 默认为关闭
	m_AimbotFOV = 50.0f; // 默认FOV角度为60度
	m_PseudoFlyEnabled = false; // 默认为关闭
	m_AutoRecordAfterPlay = false; // 新增
	m_IsPausedRecording = false; // 新增
	m_TargetID = -1;
	m_AntiFreezeEnabled = false; // 默认关闭
	m_GhostFollowEnabled = false;
	m_GhostTargetID = -1;
	m_GhostBuffer.clear();
	m_AutoWiggleEnabled = false;
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
}

// 检查某个坐标是不是“危险区域” (Freeze 或 Death)
bool IsDanger(CCollision *pCol, float x, float y)
{
	// 将像素坐标转为格子坐标
	int TileX = (int)(x / 32.0f);
	int TileY = (int)(y / 32.0f);

	// 获取该格子的索引
	int TileIndex = pCol->GetTileIndex(pCol->GetPureMapIndex(vec2(x, y)));

	// DDNet 常用危险图块索引：
	// 9 = Freeze (黑水/冻结)
	// TILE_DEATH = 死块
	// TILE_DFREEZE = 深冻结
	// 你可以在 game/mapitems.h 里查到具体的定义，这里写死 9 测试一下
	if(TileIndex == 9 ||  TileIndex == 2)
	{
		return true;
	}
	return false;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
	
	// [新增：TAS 命令注册]
	// =======================================================
	Console()->Register("/tassave", "s[name]", CFGFLAG_CLIENT, ConTasSave, this, "Save current TAS recording");
	Console()->Register("/tasload", "s[name]", CFGFLAG_CLIENT, ConTasLoad, this, "Load TAS recording");
	// =======================================================
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

// =================== [TAS 功能实现] ===================
void CControls::SaveTASDemo(const char *pFilename)
{
	if(m_TASBuffer.empty())
	{
		GameClient()->m_Chat.AddLine(-1, 0, "TAS Save: 缓冲区为空，无法保存。");
		return;
	}
	// 使用 IStorage 打开文件，确保路径正确 (例如，在用户数据文件夹下)
	IOHANDLE File = Storage()->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(File)
	{
		// 1. 写入头信息: 文件标识符 (魔术数字，确保文件类型正确)
		const char *pMagic = "TAS_V1";
		io_write(File, pMagic, 6);
		// 2. 写入总帧数
		int Count = (int)m_TASBuffer.size();
		io_write(File, &Count, sizeof(int));
		// 3. 写入每一帧数据
		io_write(File, m_TASBuffer.data(), Count * sizeof(TASFrame));
		io_close(File);
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "TAS Save: 成功保存 %d 帧到 %s。", Count, pFilename);
		GameClient()->m_Chat.AddLine(-1, 0, aBuf);
	}
	else
	{
		GameClient()->m_Chat.AddLine(-1, 0, "TAS Save: 错误 - 无法打开文件进行写入。");
	}
}

bool CControls::LoadTASDemo(const char *pFilename)
{
	m_TASBuffer.clear();
	m_IsPlaying = false;
	m_PlaybackIndex = 0;

	IOHANDLE File = Storage()->OpenFile(pFilename, IOFLAG_READ, IStorage::TYPE_ALL);

	if(!File)
	{
		GameClient()->m_Chat.AddLine(-1, 0, "TAS Load: 错误 - 文件不存在或无法打开。");
		return false;
	}

	char aMagic[7] = {0};
	io_read(File, aMagic, 6); // 读取魔术数字 "TAS_V1"

	if(str_comp(aMagic, "TAS_V1") != 0)
	{
		GameClient()->m_Chat.AddLine(-1, 0, "TAS Load: 错误 - 文件格式不正确。");
		io_close(File);
		return false;
	}

	int Count = 0;
	io_read(File, &Count, sizeof(int)); // 读取总帧数

	if(Count > 0)
	{
		m_TASBuffer.resize(Count);
		io_read(File, m_TASBuffer.data(), Count * sizeof(TASFrame));
		io_close(File);

		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "TAS Load: 成功加载 %d 帧。", Count);
		GameClient()->m_Chat.AddLine(-1, 0, aBuf);
		return true;
	}

	io_close(File);
	return false;
}

void CControls::ConTasSave(IConsole::IResult *pResult, void *pUserData)
{
	CControls *pSelf = (CControls *)pUserData;
	// 确保文件名带有正确的后缀，例如 ".tas"
	char aFilename[256];
	str_format(aFilename, sizeof(aFilename), "%s.tas", pResult->GetString(0));
	pSelf->SaveTASDemo(aFilename);
}

void CControls::ConTasLoad(IConsole::IResult *pResult, void *pUserData)
{
	CControls *pSelf = (CControls *)pUserData;
	char aFilename[256];
	str_format(aFilename, sizeof(aFilename), "%s.tas", pResult->GetString(0));
	pSelf->LoadTASDemo(aFilename);
}

// =================== [TAS 功能结束] ===================

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePos[g_Config.m_ClDummy].x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePos[g_Config.m_ClDummy].y;

		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)m_aMousePosOnAction[g_Config.m_ClDummy].x;
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)m_aMousePosOnAction[g_Config.m_ClDummy].y;
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
		{
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
			m_aMousePos[g_Config.m_ClDummy].x = 1;
		}

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
			pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
			pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
			pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
			pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
			pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
			pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

			if(!g_Config.m_ClDummyControl)
				pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

			pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
			pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
#ifdef CONF_DEBUG
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}
#endif
		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// =================== [新功能实现] ===================
	// =================== [新功能实现: F9 暂停剪辑版] ===================

	// 静态变量防连按
	static bool F3_Pressed = false, F4_Pressed = false, F9_Pressed = false, F10_Pressed = false;
	static int s_FireOffset = 0;

	// --------------------------------------------------------
	// 1. 按键检测部分
	// --------------------------------------------------------

	// [F3] 录制/停止
	if(Input()->KeyPress(KEY_F3))
	{
		if(!F3_Pressed)
		{
			if(m_IsRecording || m_IsPausedRecording)
			{
				// 任何录制状态 -> 彻底停止
				m_IsRecording = false;
				m_IsPausedRecording = false;
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: 录制已停止。");
			}
			else
			{
				// 空闲 -> 开始全新录制
				m_IsRecording = true;
				m_IsPlaying = false;
				m_IsPausedRecording = false;
				m_TASBuffer.clear();
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: >>> 开始全新录制 >>>");
			}
			F3_Pressed = true;
		}
	}
	else
		F3_Pressed = false;

	// [F9] 暂停/恢复/剪辑
	if(Input()->KeyPress(KEY_F9))
	{
		if(!F9_Pressed)
		{
			if(m_IsRecording)
			{
				// 正在录制 -> 暂停录制
				m_IsRecording = false;
				m_IsPausedRecording = true;
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: [已暂停] - 按左箭头剪辑，按 F9 恢复。");
			}
			else if(m_IsPausedRecording)
			{
				// 暂停录制 -> 恢复录制
				m_IsPausedRecording = false;
				m_IsRecording = true;
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: >>> 恢复录制 >>>");
			}
			F9_Pressed = true;
		}
	}
	else
		F9_Pressed = false;

	// [F4] 普通回放
	if(Input()->KeyPress(KEY_F4))
	{
		if(!F4_Pressed)
		{
			if(m_IsPlaying)
				m_IsPlaying = false; // 中断
			else if(!m_TASBuffer.empty())
			{
				m_IsPlaying = true;
				m_IsRecording = false;
				m_IsPausedRecording = false;
				m_AutoRecordAfterPlay = false; // 仅播放
				m_PlaybackIndex = 0;
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: 开始回放 (仅观看)...");
			}
			F4_Pressed = true;
		}
	}
	else
		F4_Pressed = false;

	// [F10] 接管模式
	if(Input()->KeyPress(KEY_F10))
	{
		if(!F10_Pressed)
		{
			if(m_IsPlaying)
				m_IsPlaying = false; // 中断
			else if(!m_TASBuffer.empty())
			{
				m_IsPlaying = true;
				m_IsRecording = false;
				m_IsPausedRecording = false;
				m_AutoRecordAfterPlay = true; // 播放后接管
				m_PlaybackIndex = 0;
				char buf[128];
				str_format(buf, sizeof(buf), "TAS: >>> 接管模式启动! 播放 %d 帧后自动录制 >>>", (int)m_TASBuffer.size());
				GameClient()->m_Chat.AddLine(-1, 0, buf);
			}
			F10_Pressed = true;
		}
	}
	else
		F10_Pressed = false;

	// --------------------------------------------------------
	// 2. 逻辑执行部分
	// --------------------------------------------------------

	// [剪辑逻辑] - 只有在“暂停录制”状态下才能剪辑
	if(m_IsPausedRecording)
	{
		// 支持按住快速删除
		if(Input()->KeyPress(80) || (Input()->KeyIsPressed(80) && Client()->GameTick(g_Config.m_ClDummy) % 5 == 0))
		{
			if(!m_TASBuffer.empty())
			{
				int DeleteCount = 5; // 每次删 2 帧
				for(int i = 0; i < DeleteCount && !m_TASBuffer.empty(); i++)
					m_TASBuffer.pop_back();

				// 打印提示
				char buf[64];
				str_format(buf, sizeof(buf), "TAS: 已剪辑. 剩余帧数: %d", (int)m_TASBuffer.size());
				GameClient()->m_Chat.AddLine(-1, 0, buf);
			}
		}

		// 在暂停期间，不记录任何新帧，也不覆盖任何输入，让玩家可以自由移动
		// return sizeof... 之前会把玩家的真实输入发出去
	}
	// [回放逻辑]
	else if(m_IsPlaying && !m_TASBuffer.empty())
	{
		if(m_PlaybackIndex < m_TASBuffer.size())
		{
			const TASFrame &frame = m_TASBuffer[m_PlaybackIndex];

			if(m_PlaybackIndex == 0)
				s_FireOffset = m_aInputData[g_Config.m_ClDummy].m_Fire - frame.m_Fire;

			m_aInputData[g_Config.m_ClDummy].m_Direction = frame.m_Direction;
			m_aInputData[g_Config.m_ClDummy].m_Jump = frame.m_Jump;
			m_aInputData[g_Config.m_ClDummy].m_Hook = frame.m_Hook;
			m_aInputData[g_Config.m_ClDummy].m_Fire = frame.m_Fire + s_FireOffset;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = frame.m_TargetX;
			m_aInputData[g_Config.m_ClDummy].m_TargetY = frame.m_TargetY;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = frame.m_WantedWeapon;
			m_aInputData[g_Config.m_ClDummy].m_NextWeapon = frame.m_NextWeapon;
			m_aInputData[g_Config.m_ClDummy].m_PrevWeapon = frame.m_PrevWeapon;

			Send = true;
			m_PlaybackIndex++;
		}
		else
		{
			m_IsPlaying = false;
			if(m_AutoRecordAfterPlay)
			{
				m_IsRecording = true; // 无缝切换到录制
				m_AutoRecordAfterPlay = false;
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: >>> 接管! 开始录制 >>>");
			}
			else
			{
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: 回放结束。");
			}
		}
	}
	// [录制逻辑] - 只有在 m_IsRecording 为 true 时才记录
	else if(m_IsRecording)
	{
		TASFrame frame;
		frame.m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
		frame.m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
		frame.m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
		frame.m_Fire = m_aInputData[g_Config.m_ClDummy].m_Fire;
		frame.m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
		frame.m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
		frame.m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;
		frame.m_NextWeapon = m_aInputData[g_Config.m_ClDummy].m_NextWeapon;
		frame.m_PrevWeapon = m_aInputData[g_Config.m_ClDummy].m_PrevWeapon;
		frame.m_DebugPos = GameClient()->m_PredictedChar.m_Pos;
		frame.m_DebugVel = GameClient()->m_PredictedChar.m_Vel;

		m_TASBuffer.push_back(frame);
	}

	// =================== [新功能结束] ===================

	// =================== [KRX 核心自瞄逻辑移植版] ===================
	// 逻辑：FOV过滤 -> 距离筛选 -> 武器弹道预判 -> 静默瞄准
	if(m_AimbotEnabled && GameClient()->m_Snap.m_pLocalCharacter)
	{
		int LocalID = GameClient()->m_Snap.m_LocalClientId;
		vec2 LocalPos = GameClient()->m_LocalCharacterPos; // 你的预测位置

		// 1. 获取当前准星朝向 (用于计算 FOV)
		// 我们使用当前帧的输入目标来计算，这样最准
		vec2 CurrentAimDir = normalize(vec2(m_aInputData[g_Config.m_ClDummy].m_TargetX, m_aInputData[g_Config.m_ClDummy].m_TargetY));

		int BestID = -1;
		float MinDist = 1000000.0f; // 找距离最近的

		// 预先计算 FOV 的余弦值 (性能优化)
		// 这里的 m_AimbotFOV 是角度 (例如 45.0f)
		// 如果 FOV >= 360，则 cos 值为 -2 (总是通过)
		float MinFovCos = (m_AimbotFOV < 360.0f) ? cosf((m_AimbotFOV / 2.0f) * (pi / 180.0f)) : -2.0f;

		// 2. 遍历所有玩家
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			// --- 基础过滤 ---
			if(i == LocalID)
				continue; // 排除自己
			if(!GameClient()->m_aClients[i].m_Active)
				continue; // 排除无效玩家
			if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
				continue; // 排除未生成的(死亡的)

			// --- 敌我判断 ---
			// IsOtherTeam 是 DDNet 自带函数，自动处理 PVP/DDRace/Solo 逻辑
			if(!GameClient()->IsOtherTeam(i))
				continue;

			// 获取敌人位置 (使用 DDNet 预测后的位置，这非常关键！)
			vec2 EnemyPos = GameClient()->m_aClients[i].m_Predicted.m_Pos;
			float Dist = distance(LocalPos, EnemyPos);

			// --- FOV 检查 ---
			if(m_AimbotFOV < 360.0f)
			{
				vec2 DirToEnemy = normalize(EnemyPos - LocalPos);
				// 点积 > MinFovCos 意味着在视野夹角内
				if(dot(CurrentAimDir, DirToEnemy) < MinFovCos)
					continue;
			}

			// --- 距离筛选 ---
			// KRX 逻辑通常优先锁距离最近的，这样更符合直觉
			if(Dist < MinDist)
			{
				MinDist = Dist;
				BestID = i;
			}
		}

		// 3. 如果找到了目标，执行 KRX 预判算法
		if(BestID != -1)
		{
			// 获取目标的预测位置和速度
			vec2 TargetPos = GameClient()->m_aClients[BestID].m_Predicted.m_Pos;
			vec2 TargetVel = GameClient()->m_aClients[BestID].m_Predicted.m_Vel;

			// --- 武器弹道计算 (Prediction) ---
			int Weapon = GameClient()->m_Snap.m_pLocalCharacter->m_Weapon;
			float BulletSpeed = 0.0f;

			// DDNet 标准武器速度参数
			if(Weapon == WEAPON_GUN)
				BulletSpeed = 2200.0f;
			else if(Weapon == WEAPON_SHOTGUN)
				BulletSpeed = 2000.0f;
			else if(Weapon == WEAPON_GRENADE)
				BulletSpeed = 1000.0f;
			else if(Weapon == WEAPON_LASER)
				BulletSpeed = 0.0f; // 激光是瞬发的

			// 线性预测公式: 预测位置 = 当前位置 + (速度 * 飞行时间)
			// 飞行时间 = 距离 / 子弹速度
			if(BulletSpeed > 0.0f)
			{
				float Time = distance(LocalPos, TargetPos) / BulletSpeed;
				TargetPos += TargetVel * Time;
			}

			// 修正：如果是榴弹，其实是抛物线，但在近中距离下，线性预测足够准了。
			// 如果需要极致的榴弹预测，需要极其复杂的物理模拟，KRX 通常也只是做线性近似。

			// 4. 计算最终瞄准向量
			vec2 AimVector = TargetPos - LocalPos;

			// 5. 应用输入 (Silent Aim)
			// 直接修改发包数据，屏幕准星不会乱晃，但子弹会打向敌人
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)AimVector.x;
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)AimVector.y;

			
		}
	}
	// =================== [KRX 逻辑结束] ===================

	// =================== [自动急停逻辑] ===================
	if(m_AutoBalanceEnabled && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// 只有当玩家【没有】按下任何移动键(左或右)时，才触发自动平衡
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] == 0 && m_aInputDirectionRight[g_Config.m_ClDummy] == 0)
		{
			// 获取水平速度
			float VelX = GameClient()->m_PredictedChar.m_Vel.x;
			float Threshold = 1.0f; // 速度阈值

			if(VelX > Threshold)
			{
				// 如果正在向右滑，强制向左按
				m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
			}
			else if(VelX < -Threshold)
			{
				// 如果正在向左滑，强制向右按
				m_aInputData[g_Config.m_ClDummy].m_Direction = 1;
			}
			else
			{
				// 如果速度已在阈值内，强制停止所有方向输入
				m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
			}
		}
	}

	// =================== [自动叠罗汉 / 垂直下落对齐] ===================
	if(m_StackEnabled && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// 获取自己的预测位置
		vec2 MyPos = GameClient()->m_PredictedChar.m_Pos;

		int ClosestStackID = -1;

		// 搜索策略变量
		float MinDistX = 10000.0f; // 用于记录最小水平差距
		float MaxScanX = 32.0f; // 左右检测范围 (1个Tee宽度约28-32，设为32比较稳)

		// 1. 寻找目标
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			const CGameClient::CClientData *pClient = &GameClient()->m_aClients[i];
			if(!pClient->m_Active || pClient->m_Team == TEAM_SPECTATORS)
				continue;
			if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
				continue;

			vec2 EnemyPos = pClient->m_RenderPos;

			// 排除自己
			if(distance(EnemyPos, MyPos) < 10.0f)
				continue;

			// 计算相对距离
			float DiffY = EnemyPos.y - MyPos.y; // >0 表示他在我下面
			float AbsDiffX = abs(EnemyPos.x - MyPos.x);

			// 规则 A: 必须在我下面 (忽略头顶的人)
			// 给一个 -10 的宽容度，防止因为微小的坐标波动导致已经站头上时丢失目标
			if(DiffY < -10.0f)
				continue;

			// 规则 B: 必须在左右极窄的范围内 (左右 1 个 Tee 宽度)
			// 只有当你已经大概在他头顶上时，才启动微调
			if(AbsDiffX > MaxScanX)
				continue;

			// 规则 C: 核心优先级 - 找水平距离最近的 (X轴对得最准的)
			if(AbsDiffX < MinDistX)
			{
				MinDistX = AbsDiffX;
				ClosestStackID = i;
			}
		}

		// 2. 执行对齐控制 (PID 柔性算法 - 保持不变，因为这个效果好)
		if(ClosestStackID != -1)
		{
			vec2 TargetPos = GameClient()->m_aClients[ClosestStackID].m_RenderPos;

			float DeltaX = TargetPos.x - MyPos.x; // 目标差距
			float MyVelX = GameClient()->m_PredictedChar.m_Vel.x; // 我的当前速度

			float StopZone = 0.5f; // 静止区
			int NewDir = 0;

			// 如果已经在完美区域内，且速度极小，停止操作
			if(abs(DeltaX) < StopZone && abs(MyVelX) < 0.1f)
			{
				NewDir = 0;
			}
			else
			{
				// 预测控制逻辑
				if(DeltaX > 0) // 目标在右边
				{
					// 如果速度过快往右冲，预测会过头 -> 反向刹车
					if(MyVelX > DeltaX * 0.5f + 2.0f)
						NewDir = -1;
					else
						NewDir = 1;
				}
				else // 目标在左边
				{
					// 如果速度过快往左冲，预测会过头 -> 反向刹车
					if(MyVelX < DeltaX * 0.5f - 2.0f)
						NewDir = 1;
					else
						NewDir = -1;
				}

				// 微调死区归零
				if(abs(DeltaX) < 2.0f && abs(MyVelX) < 0.5f)
					NewDir = 0;
			}

			m_aInputData[g_Config.m_ClDummy].m_Direction = NewDir;
		}
	}

	// =================== [自动防黑水 - 智能急停版] ===================
	if(m_AutoEdgeEnabled && GameClient()->m_Snap.m_pLocalCharacter)
	{
		CGameClient *pGC = GameClient();
		CCollision *pCol = pGC->Collision();

		// 1. 获取物理数据
		vec2 Pos = pGC->m_PredictedChar.m_Pos;
		vec2 Vel = pGC->m_PredictedChar.m_Vel;

		// 2. 探测参数设置
		// 水平探测：1 格左右的静态距离 + 少量速度预判
		float StaticCheckDist = 10.0f; // 1 格
		float VelFactor = 4.0f; // 速度系数，保证刹得住车

		// 计算水平探测点 X
		float CheckRightX = Pos.x + StaticCheckDist + (Vel.x > 0 ? Vel.x * VelFactor : 0);
		float CheckLeftX = Pos.x - StaticCheckDist + (Vel.x < 0 ? Vel.x * VelFactor : 0);

		// 3. 执行探测 (关键修改：扩大 Y 轴检测范围)
		// 我们不仅要检测脚下，还要检测头顶上方 3 格的区域
		bool DangerRight = false;
		bool DangerLeft = false;

		// 这里的逻辑是：检查从[脚底]到[头顶上方3格]这一条垂直线上，是否有黑水
		// Tee 高度约 28px，3格约 96px。我们检测几个关键点即可。
		float Y_Points[] = {
			Pos.y + 10.0f, // 脚底
			Pos.y - 10.0f, // 腰部
			Pos.y - 10.0f, // 头部
			Pos.y - 10.0f, // 头顶上方 1 格
			Pos.y - 10.0f // 头顶上方 2-3 格 (这里是你要求的重点)
		};

		// 循环检测右侧垂直线
		for(float CheckY : Y_Points)
		{
			if(IsDanger(pCol, CheckRightX, CheckY))
			{
				DangerRight = true;
				break; // 只要有一个点危险，这一侧就算危险
			}
		}

		// 循环检测左侧垂直线
		for(float CheckY : Y_Points)
		{
			if(IsDanger(pCol, CheckLeftX, CheckY))
			{
				DangerLeft = true;
				break;
			}
		}

		// 4. 控制逻辑 (强制覆盖用户输入 & 消除惯性)

		// --- 情况 A: 右边有危险 ---
		if(DangerRight)
		{
			// 1. 封锁输入
			if(m_aInputData[g_Config.m_ClDummy].m_Direction == 1)
				m_aInputData[g_Config.m_ClDummy].m_Direction = 0;

			// 2. 消除向右的惯性
			if(Vel.x > 0.5f)
				m_aInputData[g_Config.m_ClDummy].m_Direction = -1; // 反向刹车
			else if(Vel.x > 0.05f)
				m_aInputData[g_Config.m_ClDummy].m_Direction = 0; // 归零静止
		}

		// --- 情况 B: 左边有危险 ---
		if(DangerLeft)
		{
			// 1. 封锁输入
			if(m_aInputData[g_Config.m_ClDummy].m_Direction == -1)
				m_aInputData[g_Config.m_ClDummy].m_Direction = 0;

			// 2. 消除向左的惯性
			if(Vel.x < -0.5f)
				m_aInputData[g_Config.m_ClDummy].m_Direction = 1; // 反向刹车
			else if(Vel.x < -0.05f)
				m_aInputData[g_Config.m_ClDummy].m_Direction = 0; // 归零静止
		}
	}
	// =================== [必须加回来的结尾代码] ===================
	ProcessAutoWiggle(); 
	ProcessAntiFreeze(); 
	ProcessGhostFollow();
	ProcessAimbot();

	Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
	


	// 3. 记录发送时间
	m_LastSendTime = time_get();

	// 4. 将计算好的 InputData 复制到游戏引擎的指针 pData 中
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	// ===================================================

	// 5. 返回数据大小 (这是函数要求的返回值！)
	return sizeof(m_aInputData[0]);
}


void CControls::OnRender()
{

	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;
	
	// 自动抖动开关 (V 键)
	if(Input()->KeyPress(KEY_V))
	{
		m_AutoWiggleEnabled = !m_AutoWiggleEnabled;
		if(m_AutoWiggleEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Wiggle: [开启] - 按住左右键触发急速抖动");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Wiggle: [关闭]");
	}

	// 智能防冻结开关 (波浪键)
	if(Input()->KeyPress(KEY_GRAVE))
	{
		m_AntiFreezeEnabled = !m_AntiFreezeEnabled;
		if(m_AntiFreezeEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Anti-Freeze: [开启] - 智能边缘跳/防冻结");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Anti-Freeze: [关闭]");
	}

	if(Input()->KeyPress(KEY_X))
	{
		m_GhostFollowEnabled = !m_GhostFollowEnabled;
		if(m_GhostFollowEnabled)
		{
			m_GhostTargetID = -1; // 重置目标，重新寻找
			m_GhostBuffer.clear();
			GameClient()->m_Chat.AddLine(-1, 0, "Ghost Follow: [开启] - 正在寻找目标...");
		}
		else
		{
			GameClient()->m_Chat.AddLine(-1, 0, "Ghost Follow: [关闭]");
		}
	}


	// [TAS] 按键检测与消息提示
	// F3: 录制开关
	if(Input()->KeyPress(KEY_F3))
	{
		if(m_IsRecording)
		{
			m_IsRecording = false;
			char aBuf[128];
			float seconds = m_TASBuffer.size() / 50.0f; // 假设服务器是50tick
			str_format(aBuf, sizeof(aBuf), "TAS: 录制停止! 总帧数: %d (约 %.2f 秒)", (int)m_TASBuffer.size(), seconds);
			GameClient()->m_Chat.AddLine(-1, 0, aBuf);
		}
		else
		{
			m_IsRecording = true;
			m_IsPlaying = false;
			m_TASBuffer.clear(); // 清空旧数据
			GameClient()->m_Chat.AddLine(-1, 0, "TAS: >>> 开始录制 (按 F3 停止) >>>");
		}
	}
	// F4: 回放开关
	if(Input()->KeyPress(KEY_F4))
	{
		if(m_IsPlaying)
		{
			// 停止播放
			m_IsPlaying = false;
			GameClient()->m_Chat.AddLine(-1, 0, "TAS: 回放停止!");
		}
		else
		{
			if(m_TASBuffer.empty())
			{
				GameClient()->m_Chat.AddLine(-1, 0, "TAS: 错误 - 没有录制数据，无法回放!");
			}
			else
			{
				// 开始/重新开始播放
				m_IsPlaying = true;
				m_IsRecording = false;
				m_PlaybackIndex = 0; // 强制从头开始
				char aBuf[128];
				float seconds = m_TASBuffer.size() / 50.0f;
				str_format(aBuf, sizeof(aBuf), "TAS: >>> 开始回放 (共 %d 帧, %.2f 秒) >>>", (int)m_TASBuffer.size(), seconds);
				GameClient()->m_Chat.AddLine(-1, 0, aBuf);
			}
		}
	}

	// 自瞄开关 (mouse4 - 291)
	if(Input()->KeyPress(291))
	{
		m_AimbotEnabled = !m_AimbotEnabled;
		if(m_AimbotEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Aimbot: [开启] - 自动锁定最近的 Tee");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Aimbot: [关闭]");
	}

	// 自动急停开关 (反斜杠键)
	if(Input()->KeyPress(KEY_BACKSLASH))
	{
		m_AutoBalanceEnabled = !m_AutoBalanceEnabled;
		if(m_AutoBalanceEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Balance: [开启] - 急停");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Balance: [关闭]");
	}

	// 叠罗汉开关 (mouse5 - 295)
	if(Input()->KeyPress(295))
	{
		m_StackEnabled = !m_StackEnabled;
		if(m_StackEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Stack: [开启] - 自动对齐队友");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Stack: [关闭]");
	}

	/// 防黑水开关 (原防滑开关，波浪键)
	if(Input()->KeyPress(KEY_Z))
	{
		m_AutoEdgeEnabled = !m_AutoEdgeEnabled;
		if(m_AutoEdgeEnabled)
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Avoid: [开启] - 自动避开黑水/死块");
		else
			GameClient()->m_Chat.AddLine(-1, 0, "Auto Avoid: [关闭]");
	}

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}

	// =================== [自瞄目标 ESP 框 (完美修复版)] ===================
	if(m_AimbotEnabled && m_TargetID != -1 && m_TargetID < MAX_CLIENTS)
	{
		bool IsTargetValid = GameClient()->m_aClients[m_TargetID].m_Active &&
				     GameClient()->m_Snap.m_aCharacters[m_TargetID].m_Active;

		if(IsTargetValid)
		{
			// 1. 保存当前的 UI 坐标系 (画完要恢复)
			float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
			Graphics()->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

			// 2. 获取摄像机中心和缩放
			vec2 Center = GameClient()->m_Camera.m_Center;
			float Zoom = GameClient()->m_Camera.m_Zoom;

			// --- [关键修改] 使用引擎自带函数计算视野大小 ---
			// 这会自动处理 4:3, 16:9, 21:9 等各种分辨率的拉伸问题
			float WorldW, WorldH;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), Zoom, &WorldW, &WorldH);

			// 3. 将屏幕映射到游戏世界坐标
			Graphics()->MapScreen(
				Center.x - WorldW / 2.0f,
				Center.y - WorldH / 2.0f,
				Center.x + WorldW / 2.0f,
				Center.y + WorldH / 2.0f);

			// 4. 获取目标坐标
			vec2 Pos = GameClient()->m_aClients[m_TargetID].m_RenderPos;

			// 5. 绘图 (红框)
			Graphics()->TextureSet(IGraphics::CTextureHandle()); // 禁用纹理
			Graphics()->QuadsBegin(); // 开始画矩形
			Graphics()->SetColor(1.0f, 0.0f, 0.0f, 1.0f); // 红色，不透明

			float Size = 23.0f; // 框的半径大小
			float Thick = 3.0f; // 【这里设置粗细】数值越大越粗 (建议 3.0 - 5.0)

			IGraphics::CQuadItem Array[4];

			// 我们画 4 个长条矩形来组成一个框

			// 上边框 (长条横线)
			// 参数: X, Y, 宽, 高
			Array[0] = IGraphics::CQuadItem(Pos.x - Size, Pos.y - Size, Size * 2, Thick);

			// 下边框 (长条横线)
			// Y坐标向上偏移一个厚度，保证框在内部对齐
			Array[1] = IGraphics::CQuadItem(Pos.x - Size, Pos.y + Size - Thick, Size * 2, Thick);

			// 左边框 (长条竖线)
			Array[2] = IGraphics::CQuadItem(Pos.x - Size, Pos.y - Size, Thick, Size * 2);

			// 右边框 (长条竖线)
			// X坐标向左偏移一个厚度
			Array[3] = IGraphics::CQuadItem(Pos.x + Size - Thick, Pos.y - Size, Thick, Size * 2);

			// 绘制这4个矩形
			Graphics()->QuadsDrawTL(Array, 4);
			Graphics()->QuadsEnd();

			// 6. 恢复 UI 坐标系 (必须做，不然聊天框会炸)
			Graphics()->MapScreen(ScreenX0, ScreenY0, ScreenX1, ScreenY1);
		}
		else
		{
			m_TargetID = -1;
		}
	}

	RenderTAS();
	RenderFeatureHUD();


}



void CControls::RenderTAS()
{
	if((m_IsRecording || m_IsPlaying || m_IsPausedRecording) && !m_TASBuffer.empty())
	{
		// ================= [修正核心] =================
		// 1. 获取屏幕的【物理分辨率】 (Physical Resolution)
		// 不要用 GetScreen，那个是 UI 分辨率
		float ScreenW = (float)Graphics()->ScreenWidth();
		float ScreenH = (float)Graphics()->ScreenHeight();

		// 2. 获取摄像机参数
		float Zoom = GameClient()->m_Camera.m_Zoom;
		vec2 Center = GameClient()->m_Camera.m_Center;

		// 3. 计算世界坐标视野 (World Viewport)
		// 公式：视野范围 = 屏幕物理像素 * 缩放倍率
		float WorldW = ScreenW * Zoom;
		float WorldH = ScreenH * Zoom;

		// 4. 重新映射坐标系
		// 让屏幕的 (0,0) 到 (W,H) 精确对应游戏世界里的坐标
		Graphics()->MapScreen(
			Center.x - WorldW / 2.0f,
			Center.y - WorldH / 2.0f,
			Center.x + WorldW / 2.0f,
			Center.y + WorldH / 2.0f);
		// ============================================

		// 5. 开始绘图
		Graphics()->TextureSet(IGraphics::CTextureHandle()); // 关闭纹理

		// A. 画红线 (路径)
		Graphics()->LinesBegin();
		Graphics()->SetColor(1.0f, 0.0f, 0.0f, 0.8f);

		for(size_t i = 0; i < m_TASBuffer.size() - 1; i++)
		{
			vec2 p1 = m_TASBuffer[i].m_DebugPos;
			vec2 p2 = m_TASBuffer[i + 1].m_DebugPos;

			// 只有距离不太远才画线 (防止瞬移时的跨屏长线)
			if(distance(p1, p2) < 300.0f)
			{
				IGraphics::CLineItem Line(p1.x, p1.y, p2.x, p2.y);
				Graphics()->LinesDraw(&Line, 1);
			}
		}
		Graphics()->LinesEnd();

		// B. 画绿点 (每一帧的位置)
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.0f, 1.0f, 0.0f, 0.6f);

		// 性能优化：只画最近 1000 帧
		int StartIdx = maximum(0, (int)m_TASBuffer.size() - 1000);

		for(size_t i = StartIdx; i < m_TASBuffer.size(); i++)
		{
			vec2 pos = m_TASBuffer[i].m_DebugPos;
			// 绘制大小：10x10 (游戏世界单位，约 1/3 个格子)
			// 这样无论你放大缩小地图，这个点相对于地图的大小永远不变
			IGraphics::CQuadItem Quad(pos.x, pos.y, 10, 10);
			Graphics()->QuadsDrawTL(&Quad, 1);
		}
		Graphics()->QuadsEnd();
	}
}



bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_msg("assert", "CControls::OnCursorMove CursorType %d", (int)CursorType);
			dbg_break();
			break;
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

/// =========================================================
// [AI 隔离区 & 防黑水逻辑]
// =========================================================

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

// Socket 变量
static SOCKET g_AISocket = INVALID_SOCKET;
static bool g_AIConnected = false;

// 数据包结构 (必须和 Python 对应)
struct AICommandPacket
{
	int move; // 0, 1, -1
	int jump; // 0, 1
	int hook; // 0, 1
	int fire; // 0, 1
	int targetX; // 鼠标 X
	int targetY; // 鼠标 Y
};

static AICommandPacket g_LastAICmd = {0, 0, 0, 0, 0, 0};

// 非阻塞接收函数
bool RecvNonBlocking(SOCKET sock, AICommandPacket *outCmd)
{
	u_long mode = 1;
	ioctlsocket(sock, FIONBIO, &mode);
	int bytes = recv(sock, (char *)outCmd, sizeof(AICommandPacket), 0);
	return (bytes == sizeof(AICommandPacket));
}



/// AI 主逻辑 (优化发热版)
void CControls::CallAI()
{
	// 1. AI 开关控制 (按键 '0')
	static bool AIKeyPressed = false;
	if(Input()->KeyPress(39))
	{
		if(!AIKeyPressed)
		{
			m_AIEnabled = !m_AIEnabled;
			AIKeyPressed = true;
			if(m_AIEnabled)
				GameClient()->m_Chat.AddLine(-1, 0, "AI: [ON] - Waiting for Python...");
			else
				GameClient()->m_Chat.AddLine(-1, 0, "AI: [OFF]");
		}
	}
	else
	{
		AIKeyPressed = false;
	}

	// 2. 基础检查
	if(!GameClient()->m_Snap.m_pLocalCharacter || !GameClient()->m_Snap.m_pGameInfoObj)
		return;

	// [重要] 移除这里的防黑水代码，因为 SnapInput 里已经有一套更强的了
	// 避免逻辑冲突和 CPU 浪费

	// 如果 AI 没开，直接返回
	if(!m_AIEnabled)
		return;

	// ----------------------------------------------------
	// [性能优化] 频率限制
	// DDNet 逻辑帧率是 50Hz，我们不需要每秒跑几百次 Socket
	// ----------------------------------------------------
	static int64_t LastCallTime = 0;
	int64_t Now = time_get();
	// time_freq() 是每秒的时钟数。除以 50 就是 20ms 的间隔。
	if(Now - LastCallTime < time_freq() / 50)
		return; // 还没到时间，直接跳过，省 CPU！

	LastCallTime = Now;

	// ----------------------------------------------------
	// [功能 B: Python Socket 通信]
	// ----------------------------------------------------

	// 连接逻辑
	if(!g_AIConnected)
	{
		// ... (保持原样)
		WSADATA wsaData;
		if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
			return;
		g_AISocket = socket(AF_INET, SOCK_STREAM, 0);
		if(g_AISocket == INVALID_SOCKET)
			return;

		sockaddr_in serverAddr;
		serverAddr.sin_family = AF_INET;
		serverAddr.sin_port = htons(6666);
		unsigned long addr = 0x0100007F;
		memcpy(&serverAddr.sin_addr, &addr, 4);

		// 设置非阻塞模式，防止连接卡顿
		u_long mode = 1;
		ioctlsocket(g_AISocket, FIONBIO, &mode);

		// 尝试连接 (非阻塞)
		int res = connect(g_AISocket, (sockaddr *)&serverAddr, sizeof(serverAddr));
		if(res == 0 || (res == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK))
		{
			// 这里还需要进一步检查是否真的连上了 (select)，简化起见先假设连上了
			// 如果想更严谨，可以加 select 检查
			// 为了防止反复重连发热，这里简单处理：
			// 只有当 send 成功时才认为 connected
		}

		// 设置 TCP_NODELAY
		int flag = 1;
		setsockopt(g_AISocket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

		g_AIConnected = true; // 乐观连接，如果发包失败会在下面断开
	}

	// 通信逻辑
	if(g_AIConnected)
	{
		// 1. 准备数据
		vec2 Pos = GameClient()->m_LocalCharacterPos;
		float SendPos[2] = {Pos.x, Pos.y};

		// 2. 准备雷达
		// ... (这部分计算也消耗 CPU，限制频率后就好多了)
		int MapData[121];
		int TileX = (int)(Pos.x / 32.0f);
		int TileY = (int)(Pos.y / 32.0f);
		int idx = 0;
		CCollision *pCol = Collision();

		for(int dy = -5; dy <= 5; dy++)
		{
			for(int dx = -5; dx <= 5; dx++)
			{
				vec2 CheckPos = vec2((TileX + dx) * 32.0f + 16.0f, (TileY + dy) * 32.0f + 16.0f);
				MapData[idx++] = pCol->CheckPoint(CheckPos) ? 1 : 0;
			}
		}

		// 3. 发送数据
		if(send(g_AISocket, (char *)SendPos, sizeof(SendPos), 0) == SOCKET_ERROR)
		{
			// 只有在这里才判定断开
			g_AIConnected = false;
			closesocket(g_AISocket);
			return;
		}
		if(send(g_AISocket, (char *)MapData, sizeof(MapData), 0) == SOCKET_ERROR)
		{
			g_AIConnected = false;
			closesocket(g_AISocket);
			return;
		}

		// 4. 接收指令
		AICommandPacket newCmd;
		if(RecvNonBlocking(g_AISocket, &newCmd))
		{
			g_LastAICmd = newCmd;
		}

		// 5. 执行指令
		int dummy = g_Config.m_ClDummy;

		if(g_LastAICmd.move == 1)
		{
			m_aInputDirectionRight[dummy] = 1;
			m_aInputDirectionLeft[dummy] = 0;
		}
		else if(g_LastAICmd.move == -1)
		{
			m_aInputDirectionRight[dummy] = 0;
			m_aInputDirectionLeft[dummy] = 1;
		}
		else
		{
			m_aInputDirectionRight[dummy] = 0;
			m_aInputDirectionLeft[dummy] = 0;
		}

		m_aInputData[dummy].m_Jump = g_LastAICmd.jump;
		m_aInputData[dummy].m_Hook = g_LastAICmd.hook;

		bool isFiring = (m_aInputData[dummy].m_Fire % 2) != 0;
		bool wantFire = (g_LastAICmd.fire == 1);
		if(isFiring != wantFire)
			m_aInputData[dummy].m_Fire++;

		m_aMousePos[dummy].x = (float)g_LastAICmd.targetX;
		m_aMousePos[dummy].y = (float)g_LastAICmd.targetY;
	}

}

// 引入必要的数学库
#include <cmath>


// ================= [新增功能: 智能防冻结/边缘跳] =================

// 检查是否贴着墙 (伪代码中的 "Ladder" 检查变体)
// 检查是否贴着墙 (使用标准的 CheckPoint 函数)
bool IsOnWall(CCollision *pCol, vec2 Pos)
{
	// 检查左右两侧是否有实体墙 (Pos.x +/- 18.0f)
	// CheckPoint 返回 true 表示是固体/不可穿透的墙
	if(pCol->CheckPoint(Pos.x + 18.0f, Pos.y) || pCol->CheckPoint(Pos.x - 18.0f, Pos.y))
		return true;
	return false;
}


void CControls::ProcessAntiFreeze()
{
	// 1. 总开关检查 (伪代码: DAT_1405dd300 != 0)
	if(!m_AntiFreezeEnabled)
		return;

	// 2. 有效性检查 (伪代码: check if alive, not in menu)
	if(!GameClient()->m_Snap.m_pLocalCharacter || !GameClient()->m_Snap.m_pGameInfoObj)
		return;

	// 如果正在聊天或在菜单中，不执行
	if(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & (PLAYERFLAG_CHATTING | PLAYERFLAG_IN_MENU))
		return;

	CGameClient *pGC = GameClient();
	CCollision *pCol = pGC->Collision();
	vec2 Pos = pGC->m_PredictedChar.m_Pos;
	vec2 Vel = pGC->m_PredictedChar.m_Vel;

	// 3. 状态检查：如果已经冻结了，或者已经死了，就没必要挣扎了
	if(IsDanger(pCol, Pos.x, Pos.y))
		return;

	// 4. 梯子/墙壁检查 (伪代码: isNotOnLadder)
	// 如果我们正贴着墙且正在向上运动 (Vel.y < 0)，说明我们在爬墙，此时不要触发自动跳
	if(IsOnWall(pCol, Pos) && Vel.y < -0.1f)
		return;

	// 5. 危险预测 (核心逻辑: 模拟那个 CUserCmd)
	// 我们向未来预测多少帧？速度越快，预测越远
	float PredictionTime = 20.0f; // 预测未来 10 帧 (约 0.2 秒)
	vec2 FuturePos = Pos + Vel * PredictionTime;

	// 检查未来位置是否危险 (Freeze 或 Death)
	// 注意：我们要检查一条线上的点，防止穿过危险区检测不到
	// 简化处理：检查中点和终点
	bool WillFreeze = IsDanger(pCol, FuturePos.x, FuturePos.y) ||
			  IsDanger(pCol, Pos.x + Vel.x * 5.0f, Pos.y + Vel.y * 5.0f);

	// 6. 覆盖指令 (伪代码: overwrite input struct)
	if(WillFreeze)
	{
		int Dummy = g_Config.m_ClDummy;

		// 策略 A: 正在下落 -> 跳跃
		// 只有当有跳跃次数时才跳 (这里简单判断 Vel.y，严谨需要读取 Jumped 计数)
		/*if(Vel.y > 0)
		{
			m_aInputData[Dummy].m_Jump = 1;
		}*/

		// 策略 B: 水平冲向危险 -> 急停或反向
		// 如果速度很快，向反方向按键
		if(abs(Vel.x) > 1.0f)
		{
			// 如果往右飞且右边危险 -> 按左
			if(Vel.x > 0)
				m_aInputData[Dummy].m_Direction = -1;
			// 如果往左飞且左边危险 -> 按右
			else if(Vel.x < 0)
				m_aInputData[Dummy].m_Direction = 1;
		}
		else
		{
			// 如果速度不快（比如慢慢滑下去），直接归零防止滑入
			m_aInputData[Dummy].m_Direction = 0;
		}

		// [额外] 如果是边缘跳，通常不钩墙
		// m_aInputData[Dummy].m_Hook = 0;
	}
}
// =============================================================

// ================= [新增功能: 幽灵跟随/模仿] =================

void CControls::ProcessGhostFollow()
{
	// 1. 开关检查
	if(!m_GhostFollowEnabled || !GameClient()->m_Snap.m_pLocalCharacter)
	{
		m_GhostBuffer.clear(); // 关闭时清空缓冲区
		m_GhostTargetID = -1;
		return;
	}

	// 2. 寻找目标 (如果是第一次运行或目标无效)
	if(m_GhostTargetID == -1 ||
		!GameClient()->m_aClients[m_GhostTargetID].m_Active ||
		!GameClient()->m_Snap.m_aCharacters[m_GhostTargetID].m_Active)
	{
		// 简单的寻找策略：找分数最高的人，或者除了自己以外的第一个人
		int LocalID = GameClient()->m_Snap.m_LocalClientId;
		int BestID = -1;
		int BestScore = -9999;

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(i == LocalID)
				continue;
			if(!GameClient()->m_aClients[i].m_Active)
				continue;
			if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
				continue;

			// 优先找同一队的
			// if (!GameClient()->IsOtherTeam(i)) { ... }

			// 这里简单找 ID 最小的活跃玩家作为演示
			// 你可以改成找 Score 最高的
			m_GhostTargetID = i;
			char buf[128];
			str_format(buf, sizeof(buf), "Ghost Follow: 已锁定目标 -> %s", GameClient()->m_aClients[i].m_aName);
			GameClient()->m_Chat.AddLine(-1, 0, buf);
			break;
		}

		// 如果找不到目标，直接返回
		if(m_GhostTargetID == -1)
			return;
	}

	// 3. 记录目标的当前状态
	// 获取目标玩家的最新 Snap 数据
	// 注意：我们无法直接获取目标的输入(Input)，只能通过 Character 数据反推，或者近似模拟
	// DDNet 的 Snap 中包含了 Hook, Jump 等状态标志，我们可以利用这些。

	const CGameClient::CClientData *pClientData = &GameClient()->m_aClients[m_GhostTargetID];
	if(!pClientData->m_Active)
		return;

	GhostFrame Frame;
	Frame.m_Time = time_get();
	Frame.m_Pos = pClientData->m_Predicted.m_Pos;


	m_GhostBuffer.push_back(Frame);

	// 保持缓冲区大小 (比如保留最近 5 秒的数据)
	while(m_GhostBuffer.size() > 50 * 5)
		m_GhostBuffer.pop_front();

	// 4. 执行跟随逻辑
	// 找到距离自己当前位置最近的一个历史点，或者是稍微靠前一点的点
	vec2 MyPos = GameClient()->m_LocalCharacterPos;

	// 寻找目标轨迹上，距离我最近的点
	float MinDist = 100000.0f;
	int BestIndex = -1;

	for(int i = 0; i < (int)m_GhostBuffer.size(); i++)
	{
		float d = distance(MyPos, m_GhostBuffer[i].m_Pos);
		if(d < MinDist)
		{
			MinDist = d;
			BestIndex = i;
		}
	}

	if(BestIndex != -1)
	{
		// 找到最近点后，为了“跟随”，我们要往“未来”走一点
		// 比如往后找 10 帧 (0.2秒) 的位置作为目标
		int TargetIndex = minimum(BestIndex + 10, (int)m_GhostBuffer.size() - 1);
		vec2 TargetPos = m_GhostBuffer[TargetIndex].m_Pos;

		// 简单的 AI 移动逻辑：走到 TargetPos
		int Dummy = g_Config.m_ClDummy;

		// X 轴移动
		if(TargetPos.x > MyPos.x + 10)
			m_aInputData[Dummy].m_Direction = 1;
		else if(TargetPos.x < MyPos.x - 10)
			m_aInputData[Dummy].m_Direction = -1;
		else
			m_aInputData[Dummy].m_Direction = 0;

		// 跳跃 (如果目标点比我高)
		if(TargetPos.y < MyPos.y - 20)
			m_aInputData[Dummy].m_Jump = 1;
		else
			m_aInputData[Dummy].m_Jump = 0;

		// 瞄准目标点
		m_aInputData[Dummy].m_TargetX = (int)(TargetPos.x - MyPos.x);
		m_aInputData[Dummy].m_TargetY = (int)(TargetPos.y - MyPos.y);

		// 钩索 (如果距离远且目标在上方或空中)
		if(distance(TargetPos, MyPos) > 100.0f)
		{
			// 简单的钩索逻辑：如果目标在上方，尝试钩
			if(TargetPos.y < MyPos.y)
				m_aInputData[Dummy].m_Hook = 1;
			else
				m_aInputData[Dummy].m_Hook = 0;
		}
		else
		{
			m_aInputData[Dummy].m_Hook = 0;
		}
	}
}



void CControls::ProcessAimbot()
{
	// 1. 基础开关与有效性检查
	if(!m_AimbotEnabled || !GameClient()->m_Snap.m_pLocalCharacter)
	{
		m_TargetID = -1; // 关闭时重置目标ID
		return;
	}

	int LocalID = GameClient()->m_Snap.m_LocalClientId;
	vec2 LocalPos = GameClient()->m_LocalCharacterPos;
	int CurrentDummy = g_Config.m_ClDummy;
	CCollision *pCol = GameClient()->Collision();

	// ================= [动态距离判断逻辑] =================

	// 获取当前手持武器
	int CurrentWeapon = GameClient()->m_Snap.m_pLocalCharacter->m_Weapon;

	// 默认距离 (钩子/锤子模式)
	float CurrentMaxRange = 400.0f;

	// 判断是否为射击类武器
	bool IsShootingWeapon = (CurrentWeapon == WEAPON_GUN ||
				 CurrentWeapon == WEAPON_SHOTGUN ||
				 CurrentWeapon == WEAPON_GRENADE ||
				 CurrentWeapon == WEAPON_LASER);

	if(IsShootingWeapon)
	{
		// 如果是射击武器，使用大范围
		CurrentMaxRange = 800.0f;
	}
	else
	{
		// 如果是锤子或忍者，使用钩子范围 (防止钩不到的人也被锁)
		CurrentMaxRange = 400.0f;
	}

	// 准星权重 (数值越大越优先锁准星指向的人)
	const float CrosshairWeight = 3000.0f;
	// ======================================================

	// 获取当前鼠标输入的朝向
	vec2 CurrentAimDir = normalize(vec2(m_aInputData[CurrentDummy].m_TargetX, m_aInputData[CurrentDummy].m_TargetY));

	int BestID = -1;
	float BestScore = 100000000.0f;
	vec2 BestFinalPos = vec2(0, 0);

	// 计算 FOV (视野角度) 的余弦值
	float MinFovCos = -2.0f;
	if(m_AimbotFOV < 360.0f && m_AimbotFOV > 0.0f)
		MinFovCos = cosf((m_AimbotFOV / 2.0f) * (pi / 180.0f));

	// 2. 遍历所有玩家寻找最佳目标
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalID)
			continue;
		if(!GameClient()->m_aClients[i].m_Active)
			continue;
		if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
			continue;

		// 排除队友 (需要时取消注释)
		// if(!GameClient()->IsOtherTeam(i)) continue;

		// --- A: 距离硬性检查 (使用动态计算的 CurrentMaxRange) ---
		vec2 EnemyPos = GameClient()->m_aClients[i].m_Predicted.m_Pos;
		float Dist = distance(LocalPos, EnemyPos);

		// [核心修改] 这里使用的是刚才根据武器判断出来的距离
		if(Dist > CurrentMaxRange)
			continue;

		// --- B: FOV 角度过滤 ---
		vec2 DirToEnemy = normalize(EnemyPos - LocalPos);
		float Dot = dot(CurrentAimDir, DirToEnemy);

		if(MinFovCos > -1.5f && Dot < MinFovCos)
			continue;

		// --- C: 预判逻辑 (Prediction) ---
		vec2 EnemyVel = GameClient()->m_aClients[i].m_Predicted.m_Vel;

		// 这里的 Weapon 变量用于弹道计算，直接复用 CurrentWeapon 即可
		float BulletSpeed = 0.0f;

		if(CurrentWeapon == WEAPON_GUN)
			BulletSpeed = 2200.0f;
		else if(CurrentWeapon == WEAPON_SHOTGUN)
			BulletSpeed = 2000.0f;
		else if(CurrentWeapon == WEAPON_GRENADE)
			BulletSpeed = 1000.0f;
		// 激光速度无限快，保持 0.0f 不做预判

		vec2 PredictedPos = EnemyPos;
		if(BulletSpeed > 0.0f)
		{
			float Time = Dist / BulletSpeed;
			PredictedPos += EnemyVel * Time;
		}

		// --- D: 墙壁/缝隙处理 (智能回退策略) ---
		vec2 ViablePos = GetViablePos(pCol, LocalPos, PredictedPos);
		vec2 TargetCandidatePos;
		bool IsSmartPath = (ViablePos.x != 0 || ViablePos.y != 0);

		if(IsSmartPath)
			TargetCandidatePos = ViablePos; // 优先钻缝
		else
			TargetCandidatePos = PredictedPos; // 没缝就硬锁(针对假墙)

		// --- E: 评分系统 (Score) ---
		float AngleDiff = 1.0f - Dot;
		float Score = (AngleDiff * CrosshairWeight) + Dist;

		if(!IsSmartPath)
			Score += 50.0f;

		if(Score < BestScore)
		{
			BestScore = Score;
			BestID = i;
			BestFinalPos = TargetCandidatePos;
		}
	}

	m_TargetID = BestID;

	// 3. 执行瞄准输入
	if(BestID != -1)
	{
		vec2 AimVector = BestFinalPos - LocalPos;
		m_aInputData[CurrentDummy].m_TargetX = (int)AimVector.x;
		m_aInputData[CurrentDummy].m_TargetY = (int)AimVector.y;
	}
	else
	{
		// 没有目标时重置为鼠标位置
		m_aInputData[CurrentDummy].m_TargetX = (int)m_aMousePos[CurrentDummy].x;
		m_aInputData[CurrentDummy].m_TargetY = (int)m_aMousePos[CurrentDummy].y;
	}
}



// ================= [新增功能: 自动抖动/原地鬼畜] =================
// 伪代码逻辑：当同时按住左右键时，根据上一帧的方向进行取反
void CControls::ProcessAutoWiggle()
{
	if(!m_AutoWiggleEnabled)
		return;

	int Dummy = g_Config.m_ClDummy;

	// 1. 检查是否同时按下了 左键 和 右键
	// 在 DDNet 代码中，m_aInputDirectionLeft/Right 存储的是按键状态
	if(m_aInputDirectionLeft[Dummy] != 0 && m_aInputDirectionRight[Dummy] != 0)
	{
		// 2. 获取上一帧的方向 (lVar1 + 0x5204f8 对应 LastData.Direction)
		int LastDir = m_aLastData[Dummy].m_Direction;

		// 3. 执行取反逻辑
		if(LastDir != 0)
		{
			// 如果上一帧在动，这一帧反向动
			m_aInputData[Dummy].m_Direction = -LastDir;
		}
		else
		{
			// 如果上一帧没动，给一个初始力 (默认向右)
			m_aInputData[Dummy].m_Direction = 1;
		}
	}
}




// ================= [功能 HUD 面板 - KRX 风格] =================
void CControls::RenderFeatureHUD()
{
	// 1. 获取屏幕尺寸
	float ScreenW = (float)Graphics()->ScreenWidth();
	float ScreenH = (float)Graphics()->ScreenHeight();

	// 2. 切换 UI 坐标系
	Graphics()->MapScreen(0, 0, ScreenW, ScreenH);

	// 3. 样式参数
	float FontSize = 20.0f; // 字体稍大一点，看起来更霸气
	float LineHeight = 24.0f; // 行高
	float RightMargin = 10.0f; // 距离屏幕右边的距离
	float StartY = 300.0f; // 起始高度 (向下挪，避开右上角自带信息)

	// 4. 定义功能列表
	struct HUDItem
	{
		const char *pText;
		vec4 Color;
	};
	std::vector<HUDItem> ActiveFeatures;

	// --- 检查并添加开启的功能 (配色参考 KRX 风格) ---

	// 自瞄 (鲜红色)
	if(m_AimbotEnabled)
		ActiveFeatures.push_back({"自瞄--开启-Mouse4", vec4(1.0f, 0.2f, 0.2f, 1.0f)});

	// 智能防冻结 (青色)
	if(m_AntiFreezeEnabled)
		ActiveFeatures.push_back({"防冻 [ON] -波浪号", vec4(0.2f, 1.0f, 1.0f, 1.0f)});

	// 幽灵跟随 (基佬紫/粉色)
	if(m_GhostFollowEnabled)
		ActiveFeatures.push_back({"幽灵跟随 [ON] -X", vec4(0.8f, 0.4f, 1.0f, 1.0f)});

	// 自动抖动 (橙色)
	if(m_AutoWiggleEnabled)
		ActiveFeatures.push_back({"月步 [ON] -V", vec4(1.0f, 0.6f, 0.0f, 1.0f)});

	// 自动急停 (深蓝色)
	if(m_AutoBalanceEnabled)
		ActiveFeatures.push_back({"急停 [ON] -\\", vec4(0.3f, 0.3f, 1.0f, 1.0f)});

	// 叠罗汉 (淡粉色)
	if(m_StackEnabled)
		ActiveFeatures.push_back({"叠罗汉 [ON] -Mouse5", vec4(1.0f, 0.7f, 0.8f, 1.0f)});

	// 自动避黑水 (草绿色)
	if(m_AutoEdgeEnabled)
		ActiveFeatures.push_back({"自制防冻 [ON] -Z", vec4(0.4f, 1.0f, 0.4f, 1.0f)});

	// TAS 状态 (最高优先级，放在最上面或最下面)
	if(m_IsRecording)
		ActiveFeatures.push_back({"[TAS] 录制 -f3", vec4(1.0f, 0.0f, 0.0f, 1.0f)});
	else if(m_IsPlaying)
		ActiveFeatures.push_back({"[TAS] 播放 -f4", vec4(0.0f, 1.0f, 0.0f, 1.0f)});
	else if(m_IsPausedRecording)
		ActiveFeatures.push_back({"[TAS] 暂停 -f9", vec4(1.0f, 1.0f, 0.0f, 1.0f)});

	// 如果没有功能开启，直接返回
	if(ActiveFeatures.empty())
		return;

	// 5. 循环绘制每一行
	float CurY = StartY;
	for(const auto &Item : ActiveFeatures)
	{
		// A. 计算文字宽度 (实现右对齐的关键)
		// TextWidth 的参数通常是 (0, Size, Text, Length)
		float TextW = TextRender()->TextWidth(FontSize, Item.pText, -1);

		// 计算 X 坐标：屏幕宽 - 边距 - 文字宽
		float CurX = ScreenW - RightMargin - TextW;

		// B. 绘制阴影 (黑色，稍微偏移)
		TextRender()->TextColor(0.0f, 0.0f, 0.0f, 1.0f); // 纯黑
		// 偏移 2px
		TextRender()->Text(CurX + 2.0f, CurY + 2.0f, FontSize, Item.pText, -1.0f);

		// C. 绘制主体文字 (彩色)
		TextRender()->TextColor(Item.Color.r, Item.Color.g, Item.Color.b, Item.Color.a);
		TextRender()->Text(CurX, CurY, FontSize, Item.pText, -1.0f);

		// 换行
		CurY += LineHeight;
	}

	// 6. 恢复白色，避免影响其他渲染
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
}