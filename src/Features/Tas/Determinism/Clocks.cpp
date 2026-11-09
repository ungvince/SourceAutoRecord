#include "Features/Tas/Determinism/Internal.hpp"

#include "Event.hpp"
#include "Hook.hpp"
#include "Modules/Engine.hpp"
#include "Offsets.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Variable.hpp"

#include <algorithm>

using namespace TasDeterminism;

// Every clock here advances by exactly 1/host_framerate per frame (lockstep) instead of following the wall clock.
// The TAS player sets host_framerate while a TAS is active. With host_framerate 0 they follow the wall clock.

static float lockstepDt() { return host_framerate.GetFloat() > 0 ? 1.0f / host_framerate.GetFloat() : 0.0f; }

// realtime: Host_AccumulateTime adds this instead of the measured frame time
float TasDeterminism::FrameTime(float dt) {
	float l = lockstepDt();
	return Enabled() && l > 0 ? l : dt;
}

// net_time decides when snapshots and usercmds are sent. NET_RunFrame sets it with NET_SetTime(Plat_FloatTime()).
// It must not go back, so when lockstep ends ahead of the wall clock (uncapped playback) it keeps that lead.
static double g_netTime = -1;  // the lockstep net_time, < 0: following the wall clock
static double g_netLead;
static void (*NET_SetTime)(double);
static void NET_SetTime_Detour(double t);
static Hook g_NET_SetTime_Hook(&NET_SetTime_Detour);
static void NET_SetTime_Detour(double t) {
	if (Enabled() && lockstepDt() > 0) {
		if (g_netTime < 0) g_netTime = t + g_netLead;
		t = g_netTime;
	} else {
		if (g_netTime >= 0) g_netLead = std::max(0.0, g_netTime - t);
		g_netTime = -1;
		t += g_netLead;
	}
	g_NET_SetTime_Hook.Disable();
	NET_SetTime(t);
	g_NET_SetTime_Hook.Enable();
}
ON_INIT {
	if (!Supported()) return;
	NET_SetTime = (void (*)(double))Memory::Scan(engine->Name(), Offsets::NET_SetTime);
	if (NET_SetTime) g_NET_SetTime_Hook.SetFunc(NET_SetTime);
	else Missing("NET_SetTime");
}

void TasDeterminism::LockstepFrame() {
	float dt = lockstepDt();
	if (!(dt > 0)) return;
	if (g_netTime >= 0) g_netTime += dt;
}
