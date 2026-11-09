#include "Features/Tas/Determinism/Internal.hpp"

#include "Event.hpp"
#include "Features/Renderer.hpp"
#include "Hook.hpp"
#include "Modules/Console.hpp"
#include "Modules/Engine.hpp"
#include "Offsets.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Variable.hpp"

#include <algorithm>
#include <vector>

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

// The sound mixer normally mixes as much audio as the device has played. In the engine's movie mode it mixes
// host_frametime of audio per frame, and Renderer can turn that mode on without an output file. Re-applied every
// frame because the end of a sar_render clears the movie name.
void TasDeterminism::SoundLockstepFrame() { Renderer::SetLockstepSound(Enabled()); }

// Some engine code reads the wall clock with a `call rel32` or `jmp rel32` to a tier0 function. patchCall makes such
// an instruction call a function of this file instead. Deleting the patch restores the instruction.
static std::vector<Memory::Patch *> g_callPatches;
static void *callTarget(uintptr_t site) {
	uint8_t opcode = *(uint8_t *)site;
	return opcode == 0xE8 || opcode == 0xE9 ? Memory::Read<void *>(site + 1) : nullptr;
}
static void patchCall(uintptr_t site, void *target) {
	int32_t rel = (int32_t)((uintptr_t)target - (site + 5));
	auto patch = new Memory::Patch();
	patch->Execute(site + 1, (unsigned char *)&rel, sizeof rel);
	g_callPatches.push_back(patch);
}
void TasDeterminism::UnhookClocks() {
	for (auto patch : g_callPatches) delete patch;
	g_callPatches.clear();
}

// The sound clock. Each frame the mixer paints samples up to soundtime + the mix-ahead time, rounded down to a multiple
// of 4. soundtime, its fraction and the mix-ahead filter depend on earlier wall-clock frames, so at each session start
// of the start sequence they are set to values that only depend on host_framerate (see soundClockStart).
static long long *g_soundtime, *g_paintedtime;
static double *g_soundtimeFrac;
static float *g_mixaheadFilter;
static Variable snd_mixahead;
ON_INIT {
	if (!Supported()) return;
	auto getSoundTime = Memory::Scan(engine->Name(), Offsets::GetSoundTime_soundtimeSig);
	auto guts = Memory::Scan(engine->Name(), Offsets::S_Update_Guts_paintedtimeSig, Offsets::S_Update_Guts_paintedtimeOff);
	auto update = Memory::Scan(engine->Name(), Offsets::S_Update_mixaheadFilterSig, Offsets::S_Update_mixaheadFilterOff);
	snd_mixahead = Variable("snd_mixahead");
	if (!getSoundTime || !guts || !update || !snd_mixahead.ThisPtr()) return Missing("the sound clock");
	g_soundtime = Memory::Deref<long long *>(getSoundTime + Offsets::GetSoundTime_soundtimeOff);
	g_soundtimeFrac = Memory::Deref<double *>(getSoundTime + Offsets::GetSoundTime_soundtimeFracOff);
	g_mixaheadFilter = Memory::Deref<float *>(update);
	g_paintedtime = Memory::Deref<long long *>(guts);
}
static constexpr double SOUND_RATE = 44100.0;  // samples per second the engine mixes at
static void soundClockStart() {
	// soundtime: down to a multiple of 4, no fraction, so the paint end is always rounded the same way
	long long st0 = *g_soundtime;
	*g_soundtime = st0 - (long long)(((st0 % 4) + 4) % 4);
	*g_soundtimeFrac = 0.0;

	// The engine low-pass filters the frame time as f = 0.9 f + 0.1 dt, in float. Iterate to the value that
	// converges to: float rounding makes it differ slightly from dt.
	float dt = lockstepDt();
	if (dt > 0) {
		float f = dt;
		for (int i = 0; i < 256; ++i) f = 0.9f * f + dt * 0.1f;
		*g_mixaheadFilter = f;
	}

	// Long wall-clock frames may have painted further than this frame's paint end. Move soundtime forward by that
	// backlog, so the mixer paints again from the next frame on.
	double ahead = ((double)snd_mixahead.GetFloat() + (double)*g_mixaheadFilter) * SOUND_RATE;
	long long end = (*g_soundtime + (long long)ahead) & ~3LL;
	long long backlog = *g_paintedtime - end;
	if (backlog > 0) *g_soundtime += (backlog + 3) & ~3LL;
}

// DSP presets expire and cross-fade on Plat_MSTime. The DSP code's calls to it (not the engine's other ones) are
// patched to read a lockstep clock, which is restarted at each session start of the start sequence. Like net_time it
// does not go back. It is read by the mixer, which in movie mode runs on the main thread after the frame's
// LockstepFrame.
static unsigned (*g_realMSTime)();
static double g_dspSeconds = -1;  // lockstep time since the clock was restarted, < 0: following Plat_MSTime
static unsigned g_dspRestartMS;   // what the clock read when it was restarted
static unsigned g_dspLead;        // how far it was ahead of Plat_MSTime when lockstep last ended
static unsigned LockstepMSTime() {
	if (g_dspSeconds < 0) return g_realMSTime() + g_dspLead;
	return g_dspRestartMS + (unsigned)(g_dspSeconds * 1000.0 + 0.5);
}
static void dspClockRestart() {
	g_dspRestartMS = LockstepMSTime();
	g_dspSeconds = 0;
}
static void dspClockFrame(float dt) {
	if (Enabled() && dt > 0) {
		if (g_dspSeconds >= 0) g_dspSeconds += dt;
		else dspClockRestart();
	} else if (g_dspSeconds >= 0) {
		unsigned t = LockstepMSTime(), real = g_realMSTime();
		g_dspLead = t > real ? t - real : 0;
		g_dspSeconds = -1;
	}
}
ON_INIT {
	if (!Supported()) return;
	g_realMSTime = Memory::GetSymbolAddress<unsigned (*)()>(Memory::GetModuleHandleByName(console->Name()), "Plat_MSTime");  // tier0
	auto first = Memory::Scan(engine->Name(), Offsets::DSP_firstMSTimeCallSig, Offsets::DSP_MSTimeCallOff);
	auto last = Memory::Scan(engine->Name(), Offsets::DSP_lastMSTimeCallSig, Offsets::DSP_MSTimeCallOff);
	// Every call to Plat_MSTime between the two is taken to be the DSP code's (40 KB of code in this build). A much
	// larger distance means that a signature matched something else.
	std::vector<uintptr_t> sites;
	if (g_realMSTime && first && last && last - first < 0x10000) {
		for (auto p = first; p <= last; ++p)
			if (callTarget(p) == (void *)g_realMSTime) sites.push_back(p);
	}
	if (sites.empty()) return Missing("the DSP clock");
	for (auto site : sites) patchCall(site, (void *)&LockstepMSTime);
}

// The sound operator math_random reseeds itself from Plat_FloatTime() * 100. That call is patched to read a clock that
// is set to SOUND_RANDOM_START_TIME at each session start of the start sequence and then advances in lockstep. The
// value is the engine time recorded at the first tick of a first playback in real time (not uncapped). The clock keeps
// advancing between TASes, and is read while the cvar is on and host_framerate is set.
static constexpr double SOUND_RANDOM_START_TIME = 10.992413;
static double (*g_realFloatTime)();
static double g_soundRandomSeconds = -1;  // lockstep time since the clock was last set, < 0: never set
static double LockstepSoundRandomTime() {
	if (g_soundRandomSeconds < 0 || !Enabled() || lockstepDt() <= 0) return g_realFloatTime();
	return SOUND_RANDOM_START_TIME + g_soundRandomSeconds;
}
ON_INIT {
	if (!Supported()) return;
	g_realFloatTime = Memory::GetSymbolAddress<double (*)()>(Memory::GetModuleHandleByName(console->Name()), "Plat_FloatTime");  // tier0
	auto site = Memory::Scan(engine->Name(), Offsets::CSosOperatorRandom_Execute_FloatTime);
	if (!g_realFloatTime || !site || callTarget(site) != (void *)g_realFloatTime) return Missing("CSosOperatorRandom::Execute's clock");
	patchCall(site, (void *)&LockstepSoundRandomTime);
}

void TasDeterminism::ClocksSessionStart() {
	if (!InStartSequence()) return;
	soundClockStart();
	if (lockstepDt() > 0) dspClockRestart();
	g_soundRandomSeconds = 0;
}

void TasDeterminism::LockstepFrame() {
	float dt = lockstepDt();
	dspClockFrame(dt);
	if (!(dt > 0)) return;
	if (g_netTime >= 0) g_netTime += dt;
	if (g_soundRandomSeconds >= 0) g_soundRandomSeconds += dt;
}
