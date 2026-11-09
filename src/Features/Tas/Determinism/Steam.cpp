#include "Features/Tas/Determinism/Internal.hpp"

#include "Event.hpp"
#include "Features/Tas/TasPlayer.hpp"
#include "Hook.hpp"
#include "Interface.hpp"
#include "Modules/Console.hpp"
#include "Modules/SteamAPI.hpp"
#include "Offsets.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK/KeyValues.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace TasDeterminism;

// Steam answers at wall-clock moments, and some of its answers change the game:
// - the profile events make the engine exec config.cfg again, which draws a random number;
// - the auth ticket answer, a game-server callback, zeroes the player's bounds until the next command.
// So a map, cm or save TAS only starts once the profile has loaded (WaitForProfile). From the moment its reset load is
// requested until the TAS ends, the profile and workshop events and the game-server callbacks are held back.
static bool holdSteam() { return PlayingTas() && ResetLoadRequested(); }

using Clock = std::chrono::steady_clock;
static Interface *g_mmEvents;
static std::vector<std::string> g_warmupEvents;  // the events let through since the TAS was activated
static std::vector<std::pair<void *, KeyValues *>> g_heldEvents;
static bool g_profileLoaded;
static Clock::time_point g_lastSteamEvent;
DECL_DETOUR_T(void, BroadcastEvent, KeyValues *kv) {
	const char *name = kv ? kv->GetName() : "";
	bool profile = !strcmp(name, "OnProfileDataLoaded");
	if (profile || !strcmp(name, "CommunityMapListener_Added")) {  // the workshop's answer
		// a `start now` TAS has no start sequence and holds nothing
		if (PlayingTas() && tasPlayer->playbackInfo.GetMainHeader().startInfo.type != TasScriptStartType::StartImmediately) {
			// the warm-up loads let each event through once; a repeat is held like everything after them
			bool repeat = std::find(g_warmupEvents.begin(), g_warmupEvents.end(), name) != g_warmupEvents.end();
			if (holdSteam() || repeat) {
				g_heldEvents.push_back({thisptr, kv});
				return;
			}
			g_warmupEvents.push_back(name);
		}
		g_profileLoaded |= profile;
		g_lastSteamEvent = Clock::now();
	}
	BroadcastEvent(thisptr, kv);
}
void TasDeterminism::ForgetWarmupEvents() { g_warmupEvents.clear(); }
static void deliverHeldEvents() {
	if (g_heldEvents.empty() || PlayingTas()) return;
	auto held = std::move(g_heldEvents);
	g_heldEvents.clear();
	for (auto &e : held) BroadcastEvent(e.first, e.second);
}
ON_INIT {
	if (!Supported()) return;
	auto framework = Interface::Get(MODULE("matchmaking"), "MATCHFRAMEWORK_001");
	auto events = framework ? Memory::VMT<void *(__rescall *)(void *)>(framework, Offsets::GetEventsSubscription)(framework) : nullptr;
	g_mmEvents = Interface::Create(events);
	if (!g_mmEvents) return Missing("matchmaking events");
	g_mmEvents->Hook(BroadcastEvent_Hook, BroadcastEvent, Offsets::BroadcastEvent);
}
void TasDeterminism::UnhookSteam() {
	Interface::Delete(g_mmEvents);
	g_mmEvents = nullptr;
}
ON_EVENT(SAR_UNLOAD) {
	UnhookSteam();  // first, so that nothing these events trigger is held again
	for (auto &e : g_heldEvents) BroadcastEvent(e.first, e.second);
	g_heldEvents.clear();
}

// The engine runs the game-server callbacks every 0.1 s of wall time. Steam keeps them queued while they are not run.
// Holding them relies on single player ignoring a game-server deny (GSClientDeny): the same deny arrives with or
// without the hold, it is only delivered at the TAS end. Coop scripts are refused today (canPlay). Before they are
// allowed this has to be looked at again: a deny delivered at the TAS end would disconnect a partner who is not on
// the LAN.
static void (*SteamGameServer_RunCallbacks)();
static void SteamGameServer_RunCallbacks_Detour();
static Hook g_SteamGameServer_RunCallbacks_Hook(&SteamGameServer_RunCallbacks_Detour);
static void SteamGameServer_RunCallbacks_Detour() {
	if (holdSteam()) return;
	g_SteamGameServer_RunCallbacks_Hook.Disable();
	SteamGameServer_RunCallbacks();
	g_SteamGameServer_RunCallbacks_Hook.Enable();
}
ON_INIT {
	if (!Supported()) return;
	SteamGameServer_RunCallbacks = Memory::GetSymbolAddress<void (*)()>(Memory::GetModuleHandleByName(steam->Name()), "SteamGameServer_RunCallbacks");
	if (SteamGameServer_RunCallbacks) g_SteamGameServer_RunCallbacks_Hook.SetFunc(SteamGameServer_RunCallbacks);
	else Missing("SteamGameServer_RunCallbacks");
}

// A map, cm or save TAS is only activated once Steam's profile data has loaded, like a player who waits at the menu
// after launching the game. Loaded means: the first profile event has arrived, and no profile or workshop event has
// arrived for PROFILE_QUIET_SECS. The wait gives up after PROFILE_WAIT_SECS, and then no later TAS waits.
static constexpr double PROFILE_QUIET_SECS = 1.5, PROFILE_WAIT_SECS = 10;
static bool g_waiting, g_waitTimedOut;
static Clock::time_point g_waitStart;
static double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
static bool profileLoaded() { return g_profileLoaded && since(g_lastSteamEvent) >= PROFILE_QUIET_SECS; }
bool TasDeterminism::WaitForProfile(const TasPlaybackInfo &info) {
	if (!Enabled() || g_waitTimedOut || profileLoaded() || info.GetMainHeader().startInfo.type == TasScriptStartType::StartImmediately) return false;
	// The player holds the waiting script as its current one, so that Replay's counters, the TAS protocol and a stop
	// treat it like a script that was activated.
	tasPlayer->playbackInfo = info;
	g_waiting = true;
	g_waitStart = Clock::now();
	console->Print("sar_tas_determinism: waiting for Steam's profile data (at most %g s)\n", PROFILE_WAIT_SECS);
	return true;
}
bool TasDeterminism::WaitingForProfile() { return g_waiting; }
void TasDeterminism::CancelProfileWait() { g_waiting = false; }
static void profileWaitFrame() {
	if (!g_waiting) return;
	if (Enabled() && !profileLoaded()) {  // setting the cvar to 0 ends the wait
		if (since(g_waitStart) < PROFILE_WAIT_SECS) return;
		console->Warning("sar_tas_determinism: Steam's profile data did not load in %g s; starting the TAS\n", PROFILE_WAIT_SECS);
		g_waitTimedOut = true;
	}
	g_waiting = false;
	tasPlayer->Activate(tasPlayer->playbackInfo);
}

void TasDeterminism::SteamFrame() {
	profileWaitFrame();
	deliverHeldEvents();
}
