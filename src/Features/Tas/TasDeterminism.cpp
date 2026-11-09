#include "Features/Tas/Determinism/Internal.hpp"

#include "Cheats.hpp"
#include "Event.hpp"
#include "Features/Session.hpp"
#include "Features/Tas/TasPlayer.hpp"
#include "Modules/Console.hpp"
#include "Modules/Engine.hpp"
#include "Modules/EngineDemoRecorder.hpp"
#include "Modules/Server.hpp"
#include "Offsets.hpp"
#include "SAR.hpp"
#include "Utils.hpp"
#include "Variable.hpp"

#include <string>

static std::string g_missing;  // what the ON_INITs did not find
static bool g_tasOn;           // this TAS was started with the cvar on, and the cvar has not been turned off since

DECL_CVAR_CALLBACK(sar_tas_determinism);
Variable sar_tas_determinism("sar_tas_determinism", "0", 0, 1,
	"Deterministic TAS playback: every TAS plays like the first playback after launching the game, with SAR's fast loads "
	"(see docs/determinism.md).\n", FCVAR_NONE, sar_tas_determinism_callback);
DECL_CVAR_CALLBACK(sar_tas_determinism) {
	if (!sar_tas_determinism.GetBool()) {
		// final for the running TAS: setting the cvar to 1 again does not bring back what was reset for it
		g_tasOn = false;
		return;
	}
	if (!TasDeterminism::Supported()) {
		console->Print("sar_tas_determinism is only supported in Portal 2 on Linux.\n");
	} else if (!g_missing.empty()) {
		console->Print("sar_tas_determinism is not available: %s not found.\n", g_missing.c_str());
	} else {
		return;
	}
	sar_tas_determinism.SetValue(0);
}

bool TasDeterminism::Supported() {
#ifdef _WIN32
	return false;  // the fresh-launch values are only recorded on Linux so far
#else
	// The game directory excludes the mods that Game.cpp also runs as SourceGame_Portal2. The game build is not
	// checked: after a game update, a signature that no longer matches keeps the cvar at 0 (see Missing), but the
	// hooks installed by vtable index (LevelInit, AsyncLoadCache, BroadcastEvent) have no such check.
	return sar.game->Is(SourceGame_Portal2) && engine->GetGameDirectory && Utils::EndsWith(engine->GetGameDirectory(), "/portal2");
#endif
}

void TasDeterminism::Missing(const char *what) {
	console->DevWarning("sar_tas_determinism: %s not found\n", what);
	g_missing += g_missing.empty() ? what : std::string(", ") + what;
}

bool TasDeterminism::Enabled() { return sar_tas_determinism.GetBool(); }

// The start sequence of a map, cm or save script. A `start now` script has none: it stays in Warmup.
namespace {
	enum class StartStep {
		Warmup,  // until the reset load is requested
		Reset,   // from then until the first prerun restart is requested
		Prerun,  // from then on
	};
}  // namespace
static StartStep g_step;
static int g_prerunDone;           // prerun restarts requested so far
static int g_prerunWaitLeft = -1;  // ticks until the next prerun restart is requested, -1: none is waiting
static int g_processSessions;      // sessions started since SAR was loaded
static bool g_inResetSession;
static int g_sessionStartTick;

bool TasDeterminism::PlayingTas() { return Enabled() && g_tasOn && tasPlayer->IsActive(); }
bool TasDeterminism::InStartSequence() { return PlayingTas() && !tasPlayer->IsReady(); }
bool TasDeterminism::ResetLoadPending() { return InStartSequence() && g_step == StartStep::Reset; }

static bool canPlay(const TasPlaybackInfo &info) {
	if (!TasDeterminism::Enabled()) return true;
	auto h = info.GetMainHeader();
	if (h.rngManipFile != "") {
		console->Print("sar_tas_determinism: rngmanip replays another session's random seeds. Remove the line or set sar_tas_determinism 0.\n");
		return false;
	}
	if (h.startInfo.isNext || info.IsCoop()) {
		console->Print("sar_tas_determinism does not support %s scripts yet. Set sar_tas_determinism 0 to play this one.\n",
			h.startInfo.isNext ? "'start next'" : "coop");
		return false;
	}
	return true;
}

bool TasDeterminism::OnActivate(const TasPlaybackInfo &info) {
	if (!canPlay(info)) return false;
	g_tasOn = Enabled();
	g_step = StartStep::Warmup;
	g_prerunDone = 0;
	if (g_tasOn && !(sar_loads_frametime.GetBool() && sar_loads_uncap.GetBool())) {
		console->Print("sar_tas_determinism: turning on fast loads (sar_fast_load_preset full changes your loading settings)\n");
		engine->ExecuteCommand("sar_fast_load_preset full");
	}
	return true;
}

void TasDeterminism::OnStop() {
	g_prerunWaitLeft = -1;
}

// A fresh launch records no demo during its first load, so none is recorded during the start sequence
static void stopDemo() {
	auto *dr = engine->demorecorder;
	if (!dr || !dr->hasLoaded || !dr->m_bRecording) return;
	bool rec = dr->isRecordingDemo, armed = *dr->m_bRecording;
	if (!rec && !armed) return;
	if (rec) dr->Stop();
	*dr->m_bRecording = false;
	dr->isRecordingDemo = false;
	console->Print("sar_tas_determinism: demo recording stopped during the loads before the TAS; use sar_record_at to record the TAS.\n");
}

// The reset load is a restart_level (a `load` for a save script). Exception: on sp_a5_credits a restart_level load is
// 2 ticks longer than the `map` load of a fresh launch, so that map is loaded with `map` again.
static void reload(const TasScriptStartInfo &si, bool resetLoad) {
	if (si.type == TasScriptStartType::LoadQuicksave) {
		engine->ExecuteCommand(Utils::ssprintf("load %s", si.param.c_str()).c_str());
	} else if (resetLoad && engine->GetCurrentMapName() == "sp_a5_credits") {
		engine->ExecuteCommand("disconnect");
		engine->ExecuteCommand("map sp_a5_credits");
	} else {
		engine->ExecuteCommand("restart_level");
	}
}

// Requests the next prerun restart, now or after the script's prerun_wait. False: they are all done.
static bool prerunStep(const TasScriptHeader &h) {
	if (g_prerunDone >= h.prerunRestarts) return false;
	g_step = StartStep::Prerun;
	++g_prerunDone;
	if (h.prerunWait > 0) g_prerunWaitLeft = h.prerunWait;
	else reload(h.startInfo, false);
	return true;
}
ON_EVENT(POST_TICK) {
	if (g_prerunWaitLeft < 0 || !event.simulating || --g_prerunWaitLeft > 0) return;
	g_prerunWaitLeft = -1;
	reload(tasPlayer->playbackInfo.GetMainHeader().startInfo, false);
}

// The reset load is never the second load of a process. As the second load it was seen to send the client one more
// string table update, which shifted a recorded playback by 2 ticks. So the first TAS of a process gets a second
// warm-up load.
static constexpr int WARMUP_SESSIONS = 2;

// Runs at the start of the session the script would start in. True: another load was requested first.
static bool startStep() {
	if (!TasDeterminism::InStartSequence() || tasPlayer->numSessionsBeforeStart != 1) return false;
	auto h = tasPlayer->playbackInfo.GetMainHeader();
	if (h.startInfo.type == TasScriptStartType::StartImmediately) return false;
	if (g_step == StartStep::Warmup) {
		if (g_processSessions >= WARMUP_SESSIONS) g_step = StartStep::Reset;
		reload(h.startInfo, g_step == StartStep::Reset);
		return true;
	}
	if (g_step == StartStep::Reset && h.expectLoadTicks >= 0 && g_sessionStartTick != h.expectLoadTicks) {
		console->Warning("sar_tas_determinism: the session started at tick %d, not expect_load_ticks %d: the TAS can play differently.\n",
			g_sessionStartTick, h.expectLoadTicks);
	}
	return prerunStep(h);
}

bool TasDeterminism::OnSessionStart() {
	g_inResetSession = ResetLoadPending();
	g_sessionStartTick = server->gpGlobals ? server->gpGlobals->tickcount : -1;
	++g_processSessions;
	bool reloading = startStep();
	return reloading;
}

ON_EVENT(SESSION_END) {
	// With sar_autorecord 0 or 1 a running demo would continue into the next load of the start sequence. With -1,
	// EngineDemoRecorder's own SESSION_END handler stops it.
	if (TasDeterminism::InStartSequence() && sar_autorecord.GetInt() != -1) stopDemo();
}

// CServerGameDLL::LevelInit, before the map's entities spawn
DECL_DETOUR_T(bool, ServerLevelInit, const char *map, const char *ents, const char *oldLevel, const char *landmark, bool loadGame, bool background) {
	using namespace TasDeterminism;
	if (InStartSequence()) {
		stopDemo();
	}
	return ServerLevelInit(thisptr, map, ents, oldLevel, landmark, loadGame, background);
}
ON_INIT {
	if (!TasDeterminism::Supported()) return;
	if (!server->g_ServerGameDLL || !server->g_ServerGameDLL->Hook(ServerLevelInit_Hook, ServerLevelInit, Offsets::LevelInit)) TasDeterminism::Missing("LevelInit");
}
