#include "Features/Tas/Determinism/Internal.hpp"

#include "Event.hpp"
#include "Features/Session.hpp"
#include "Features/Tas/TasPlayer.hpp"
#include "Hook.hpp"
#include "Modules/Console.hpp"
#include "Modules/Engine.hpp"
#include "Modules/Server.hpp"
#include "Modules/Tier1.hpp"
#include "Offsets.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK.hpp"
#include "Utils/SDK/UserCmd.hpp"
#include "Variable.hpp"

#include <cstdint>
#include <cstring>
#include <set>

extern "C" {
#include <libavutil/md5.h>
}

using namespace TasDeterminism;

// The shared uniform random stream (RandomInt, RandomFloat). It is used through its vtable (IUniformRandomStream:
// 0 = SetSeed, 1 = RandomFloat) and not through the RandomSeed export, which RNGManip hooks to record or replace seeds.
static void **g_ppStream;  // &s_pUniformStream
static Variable cl_predict;
static void uniformSeed(int seed) {
	void *s = *g_ppStream;
	Memory::VMT<void(__rescall *)(void *, int)>(s, 0)(s, seed);
}
static void uniformAdvance(int n) {  // n draws: each RandomFloat generates one random number
	void *s = *g_ppStream;
	for (int i = 0; i < n; ++i) Memory::VMT<float(__rescall *)(void *, float, float)>(s, 1)(s, 0.0f, 1.0f);
}
ON_INIT {
	if (!Supported()) return;
	// RandomInt has a `mov ebx, [s_pUniformStream]` (8B 1D imm32)
	auto RandomInt = Memory::GetSymbolAddress<uint8_t *>(Memory::GetModuleHandleByName(tier1->Name()), "RandomInt");
	if (RandomInt && RandomInt[Offsets::RandomInt_s_pUniformStreamOff - 2] == 0x8B && RandomInt[Offsets::RandomInt_s_pUniformStreamOff - 1] == 0x1D)
		g_ppStream = Memory::Deref<void **>((uintptr_t)RandomInt + Offsets::RandomInt_s_pUniformStreamOff);
	else Missing("s_pUniformStream");
	cl_predict = Variable("cl_predict");
	if (!cl_predict.ThisPtr()) Missing("cl_predict");
}

// Only the first load of a process (a) draws 2 random numbers for the loopback client's challenge, at the first signon
// state change or server tick, and (b) executes the client's `cl_predict 0` while cl_predict still has its default
// value. The reset load does both again: (a) here, (b) by ResetRandomState setting cl_predict back to its default.
static bool g_firstConnectPending;
static void firstConnectDraws() {
	if (!g_firstConnectPending) return;
	g_firstConnectPending = false;
	if (InStartSequence()) uniformAdvance(2);
}
ON_EVENT_P(PRE_TICK, 5000) { firstConnectDraws(); }  // before every other PRE_TICK handler, and so before the tick's own draws

// The engine seeds the uniform stream once, at startup, from Plat_FloatTime. Everything that runs before the first
// level init then draws from it. Recorded on a fresh launch: startup at 3.520046 s, then 95291 draws up to the level
// init of a map, or 94779 up to that of a save.
static constexpr float STARTUP_TIME = 3.520046f;
static constexpr int MAP_START_DRAWS = 95291, SAVE_START_DRAWS = 94779;
static int *g_ivpSeed;
void TasDeterminism::ResetRandomState() {
	*g_ivpSeed = 1;  // the physics random seed: 1 in a new process, and loads do not reset it
	bool save = tasPlayer->playbackInfo.GetMainHeader().startInfo.type == TasScriptStartType::LoadQuicksave;
	uint32_t bits;
	memcpy(&bits, &STARTUP_TIME, 4);
	uniformSeed((int)(bits & 0x7fffffff));  // the engine's RandomSeed(bits((float)Plat_FloatTime()) & 0x7fffffff)
	uniformAdvance(save ? SAVE_START_DRAWS : MAP_START_DRAWS);
	g_firstConnectPending = true;
	cl_predict.SetValue("1.0");
}
ON_INIT {
	if (!Supported()) return;
	auto ivp_rand = Memory::Scan(MODULE("vphysics"), Offsets::ivp_rand);
	if (ivp_rand) g_ivpSeed = Memory::Deref<int *>(ivp_rand + Offsets::ivp_rand_seedOff);
	else Missing("ivp_rand");
}

// SV_CreateBaseline skips the instance baselines while the local network backdoor exists. A fresh launch loads from
// the menu, where it does not exist. So it is switched off here, as it is at the menu; the engine sets it up again on
// the next frame.
static void(__cdecl *CL_SetupLocalNetworkBackDoor)(bool);
static void **g_pLocalNetworkBackdoor;
void TasDeterminism::DisableLocalBackdoor() {
	if (*g_pLocalNetworkBackdoor) CL_SetupLocalNetworkBackDoor(false);
}
ON_INIT {
	if (!Supported()) return;
	CL_SetupLocalNetworkBackDoor = Memory::Scan<decltype(CL_SetupLocalNetworkBackDoor)>(engine->Name(), Offsets::CL_SetupLocalNetworkBackDoor);
	if (CL_SetupLocalNetworkBackDoor) g_pLocalNetworkBackdoor = Memory::Deref<void **>((uintptr_t)CL_SetupLocalNetworkBackDoor + Offsets::CL_SetupLocalNetworkBackDoor_g_pLocalNetworkBackdoorOff);
	else Missing("CL_SetupLocalNetworkBackDoor");
}

static int hostTick() {
	int host = 0, srv = 0, cl = 0;
	engine->GetTicks(host, srv, cl);
	return host;
}

// Host ticks and command numbers keep counting across the loads of a process. In the reset session they are mapped to
// what a first playback has. Recorded on every first playback, with L = the length of its load in host ticks:
//   server tick at the session start       = L + SESSION_TICK_MINUS_LOAD_TICKS (map loads only)
//   host tick at the session start         = L + HOST_TICKS_BEFORE_LOAD
//   first command number of the session    = L - LOAD_TICKS_MINUS_FIRST_CMDNUM
// A save load has no such server tick, so its L is counted: from the host tick before signon state 1 to the session
// start.
static constexpr int SESSION_TICK_MINUS_LOAD_TICKS = 57, HOST_TICKS_BEFORE_LOAD = 120, LOAD_TICKS_MINUS_FIRST_CMDNUM = 3;
static constexpr int MAX_PLAYERS = 33;  // CGameMovement::m_flStuckCheckTime[MAX_PLAYERS + 1][2]
static int g_signon1HostTick = -1;      // the host tick of signon state 1 of the last start-sequence load
static int g_hostTickOffset;            // real host tick - first-playback host tick
static bool g_hostTickOffsetValid;
static int g_firstCmdnum;               // the first-playback number of the reset session's first command
void TasDeterminism::OnSignonState(int state) {
	firstConnectDraws();
	if (state == 1 && InStartSequence()) g_signon1HostTick = hostTick();
}
static int loadHostTicks() {
	if (g_signon1HostTick < 0) return -1;
	return hostTick() - g_signon1HostTick + 1;
}
static void resetSessionStart() {
	bool save = tasPlayer->playbackInfo.GetMainHeader().startInfo.type == TasScriptStartType::LoadQuicksave;
	int loadTicks = save ? loadHostTicks() : SessionStartTick() - SESSION_TICK_MINUS_LOAD_TICKS;
	g_firstCmdnum = loadTicks - LOAD_TICKS_MINUS_FIRST_CMDNUM;
	g_hostTickOffset = hostTick() - (loadTicks + HOST_TICKS_BEFORE_LOAD);
	g_hostTickOffsetValid = true;
	// the stuck timers of a new process
	if (server->g_GameMovement) memset((char *)server->g_GameMovement->ThisPtr() + Offsets::CGameMovement_m_flStuckCheckTime, 0, sizeof(float[MAX_PLAYERS + 1][2]));
}

// SAR runs CGameMovement::CheckStuck on host time. While a TAS started with the cvar on plays, that is the host time of
// a first playback. The offset is the one of the last reset session, so a `start now` TAS keeps the one of the TAS
// before it.
float TasDeterminism::HostTime(float real) {
	if (!PlayingTas() || !g_hostTickOffsetValid) return real;
	return (float)(hostTick() - g_hostTickOffset) * *engine->interval_per_tick;
}

// The engine computes a usercmd's random_seed from its command number. While a TAS started with the cvar on plays, the
// seed is computed from the number the command has on a first playback. Movement code still gets the real number.
static bool g_newSession;         // the next command is the first of its session
static int g_sessionFirstCmdnum;  // the real number of the session's first command
static int g_lastCmdnum = -1;
// Real command number - first-playback command number. Set in the reset session. A later session keeps it, unless the
// engine starts numbering again from a lower number (a save load does): then it is 0.
static int g_cmdnumShift;
static unsigned int MD5PseudoRandom(unsigned int seed) {
	uint8_t digest[16];
	av_md5_sum(digest, (const uint8_t *)&seed, sizeof seed);
	unsigned int r;
	memcpy(&r, digest + 6, 4);
	return r;
}
void TasDeterminism::UsercmdSeed(CUserCmd *cmd) {
	if (g_newSession || cmd->command_number < g_sessionFirstCmdnum) {
		g_newSession = false;
		g_sessionFirstCmdnum = cmd->command_number;
		if (Enabled()) {
			if (InResetSession()) g_cmdnumShift = g_sessionFirstCmdnum - g_firstCmdnum;
			else if (g_sessionFirstCmdnum <= g_lastCmdnum) g_cmdnumShift = 0;
		}
	}
	g_lastCmdnum = cmd->command_number;
	if (PlayingTas()) cmd->random_seed = (int)(MD5PseudoRandom((unsigned int)(cmd->command_number - g_cmdnumShift)) & 0x7fffffff);
}

// CAI_BlendedMotor::m_flNextTurnGesture is never initialised: a motor's first think reads what the heap held before.
// The game only compares it with the current time. First playbacks read 0x0a0d3420 (as a float about 7e-33, a time in
// the past); later playbacks read large values, like 0x6afa85f0 (about 1.5e26, a time that is never reached).
// On each motor's first think in a session, an uninitialised value is replaced: by the first for the reset load and
// its session, by the second otherwise.
static constexpr uint32_t MOTOR_FIRST_PLAYBACK = 0x0a0d3420, MOTOR_LATER_PLAYBACK = 0x6afa85f0;
extern Hook g_MaintainTurnActivity_Hook;
// By address. Accepted limitation: a motor allocated at the address of one freed earlier in the session is not reset.
static std::set<void *> g_motorsSeen;
DECL_DETOUR_T(void, MaintainTurnActivity) {
	if (Enabled() && server->gpGlobals && g_motorsSeen.insert(thisptr).second) {
		float &g = *(float *)((char *)thisptr + Offsets::CAI_BlendedMotor_m_flNextTurnGesture);
		// Heuristic: a time the motor set itself is positive and less than a minute ahead. Anything else is taken to
		// be uninitialised memory.
		if (!(g > 0.001f && g <= server->gpGlobals->curtime + 60.0f)) {
			// during a load, InResetSession() still describes the session before it
			bool reset = session->isRunning ? InResetSession() : ResetLoadPending();
			uint32_t bits = reset ? MOTOR_FIRST_PLAYBACK : MOTOR_LATER_PLAYBACK;
			memcpy(&g, &bits, 4);
		}
	}
	g_MaintainTurnActivity_Hook.Disable();
	MaintainTurnActivity(thisptr);
	g_MaintainTurnActivity_Hook.Enable();
}
Hook g_MaintainTurnActivity_Hook(MaintainTurnActivity_Hook);
ON_INIT {
	if (!Supported()) return;
	MaintainTurnActivity = Memory::Scan<decltype(MaintainTurnActivity)>(server->Name(), Offsets::MaintainTurnActivity);
	if (MaintainTurnActivity) g_MaintainTurnActivity_Hook.SetFunc(MaintainTurnActivity);
	else Missing("MaintainTurnActivity");
}

// host_remainder is the time left over after a frame's last tick; the client interpolates by it. Its value depends on
// the frame times at the menu. At each session start of the start sequence, its fraction of a tick is set to a value
// recorded on a first playback. Whole ticks in it are kept.
static constexpr float TICK_PHASE = 0.664074f;
static float *g_hostRemainder;
ON_INIT {
	if (!Supported()) return;
	// movss xmm0, [host_remainder]; addss xmm0, [host_frametime]; movss [host_remainder], xmm0
	auto code = Memory::Scan(engine->Name(), Offsets::_Host_RunFrame_host_remainderSig);
	if (code && Memory::Deref<float *>(code + Offsets::_Host_RunFrame_host_remainderLoadOff) == Memory::Deref<float *>(code + Offsets::_Host_RunFrame_host_remainderStoreOff)) {
		g_hostRemainder = Memory::Deref<float *>(code + Offsets::_Host_RunFrame_host_remainderLoadOff);
	} else {
		Missing("host_remainder");
	}
}
static void tickPhaseStart() {
	float ipt = *engine->interval_per_tick;
	float r0 = *g_hostRemainder;
	int k = r0 > 0 ? (int)(r0 / ipt) : 0;
	*g_hostRemainder = ((float)k + TICK_PHASE) * ipt;
}

void TasDeterminism::StartStateSessionStart() {
	if (InResetSession()) resetSessionStart();
	if (InStartSequence()) tickPhaseStart();
	g_newSession = true;
	g_motorsSeen.clear();
	g_firstConnectPending = false;
}
