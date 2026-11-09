#pragma once

struct TasPlaybackInfo;

// sar_tas_determinism: every TAS plays like the first playback after launching the game (docs/determinism.md).
// The cvar can only be 1 in Portal 2 on Linux. While it is 0, the functions below change nothing in the game.
namespace TasDeterminism {
	bool Enabled();  // sar_tas_determinism is 1

	// TasPlayer
	bool OnActivate(const TasPlaybackInfo &info);  // false: the script is refused
	void OnStop();
	bool OnSessionStart();   // true: another load runs before the script starts
	bool InStartSequence();  // a TAS started with the cvar on is active and has not reached its first tick
}  // namespace TasDeterminism
