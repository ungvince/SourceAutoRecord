#pragma once

#include "Features/Tas/TasDeterminism.hpp"

// Shared by TasDeterminism.cpp and the files in this directory.
//
// Terms used in the comments:
// - Fresh launch: the game right after it was launched, loading its first map or save from the menu.
// - First playback: the playback of a TAS that starts with that first load. The fixed values in these files were
//   recorded on first playbacks in the unmodified game (Linux, with SAR's fast loads).
// - Start sequence: the loads SAR runs after sar_tas_play and before the script's first tick: one or two warm-up
//   loads, the reset load, then the script's prerun restarts.
// - Reset load: the load during which SAR sets hidden state to its fresh-launch values. The session it starts is the
//   reset session.
// - Lockstep: advancing by exactly 1/host_framerate per frame instead of following the wall clock.
namespace TasDeterminism {
	// Portal 2 itself, on Linux. Otherwise every ON_INIT returns early, so nothing is scanned or hooked.
	bool Supported();
	// An ON_INIT calls this when it does not find what it needs. sar_tas_determinism then cannot be set to 1, so code
	// that only runs with the cvar on may use what the ON_INITs looked up without null checks.
	void Missing(const char *what);

	// State of the start sequence (TasDeterminism.cpp)
	bool PlayingTas();            // a TAS started with the cvar on is active, and the cvar has not been turned off since
	bool ResetLoadPending();      // InStartSequence(), and the next or current load is the reset load
	bool InResetSession();        // the last session start was the end of a reset load
	int SessionStartTick();       // the server tick at the last session start

	// Session start. These run after InResetSession() and SessionStartTick() are updated, and do not depend on each other.
	void StartStateSessionStart();  // StartState.cpp

	// Server level init. TasDeterminism.cpp decides which of these a load gets, and in which order.
	void DisableLocalBackdoor();     // StartState.cpp
	void ResetRandomState();         // StartState.cpp
}  // namespace TasDeterminism
