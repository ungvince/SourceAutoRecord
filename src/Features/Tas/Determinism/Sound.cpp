#include "Features/Tas/Determinism/Internal.hpp"

#include "Event.hpp"
#include "Hook.hpp"
#include "Interface.hpp"
#include "Modules/Console.hpp"
#include "Modules/Engine.hpp"
#include "Modules/Server.hpp"
#include "Offsets.hpp"
#include "Utils.hpp"
#include "Utils/Memory.hpp"
#include "Utils/SDK.hpp"

#include <chrono>
#include <set>
#include <thread>

using namespace TasDeterminism;

// Puts the DSP processors, the wave data cache and the sound mixers back to what they are after the engine starts.
// For example, a mix layer left on by the previous session (ducking) changes which sounds are audible.
void TasDeterminism::FlushSoundSystems() {
	engine->ExecuteCommand("dsp_reload; snd_async_flush; snd_soundmixer_flush", true);
}

// Sound operator entry matches are timed blocks ("do not start this sound again for some time"). A load does not
// clear them, so blocks of earlier sessions can mute sounds in the next one. They are freed here.
// resetSoundEntry repeats what the engine does inline in CSosEntryMatchList::Update when a block expires. ON_INIT
// scans for that exact code, so a game update that changes it keeps the cvar at 0 instead of corrupting memory.
static constexpr int ENTRY_MATCH_SLOTS = 32;  // the list is a 32-bit mask of free slots, then a pointer per slot
static uint8_t *g_sosSystem, *g_sosSystemCreated;
static void resetSoundEntry(uint8_t *e) {
	e[0] = 0x0a;
	*(uint16_t *)(e + 0x40) = 0x0a00;
	*(uint16_t *)(e + 0x81) = 0;
	*(int32_t *)(e + 0x84) = -1;
	e[0x88] = 0;
	*(int32_t *)(e + 0x8c) = -1;
	e[0x90] = 0;
	*(int32_t *)(e + 0x94) = -1;
	*(uint32_t *)(e + 0x98) = 0x100;
	*(float *)(e + 0x9c) = -1.0f;
	*(float *)(e + 0xa0) = -1.0f;
}
static void freeSoundEntryMatches() {
	if (!(*g_sosSystemCreated & 1)) return;
	uint32_t *mask = (uint32_t *)(g_sosSystem + Offsets::CSosOperatorSystem_entryMatchList);
	auto slot = (uint8_t **)(mask + 1);
	for (int i = 0; i < ENTRY_MATCH_SLOTS; ++i) {
		if (*mask & (1u << i) || !slot[i]) continue;
		resetSoundEntry(slot[i]);
		*mask |= 1u << i;
	}
}
ON_INIT {
	if (!Supported()) return;
	// CSosOperatorRandom::Execute calls the operator system's getter right after Plat_FloatTime. The getter is
	// movzx eax, byte [created]; test al, al; jz +13; mov eax, system; ret
	auto call = Memory::Scan(engine->Name(), Offsets::CSosOperatorRandom_Execute_FloatTime, Offsets::CSosOperatorRandom_Execute_GetSoundOperatorSystemOff);
	auto getter = call ? Memory::Read<uint8_t *>(call) : nullptr;
	bool isGetter = getter && getter[0] == 0x0F && getter[1] == 0xB6 && getter[2] == 0x05 && getter[Offsets::GetSoundOperatorSystem_systemOff - 1] == 0xB8;
	if (!isGetter || !Memory::Scan(engine->Name(), Offsets::CSosEntryMatchList_Update_freeEntrySig)) return Missing("the sound operator system");
	g_sosSystem = Memory::Deref<uint8_t *>((uintptr_t)getter + Offsets::GetSoundOperatorSystem_systemOff);
	g_sosSystemCreated = Memory::Deref<uint8_t *>((uintptr_t)getter + Offsets::GetSoundOperatorSystem_createdOff);
}

// The sound listener is only moved by rendered frames. Loads are not rendered, so the first sounds of a session are
// placed relative to the last view of the previous one. After a fresh launch the listener is at zero.
namespace {
	struct AudioState_t {
		Vector m_Origin;
		QAngle m_Angles;
		bool m_bIsUnderwater;
	};
}  // namespace
static void resetSoundListener() {
	using _SetAudioState = void(__rescall *)(void *thisptr, const AudioState_t &state);
	auto SetAudioState = engine->engineClient->Original<_SetAudioState>(Offsets::SetAudioState);
	SetAudioState(engine->engineClient->ThisPtr(), AudioState_t{});
}

void TasDeterminism::ResetSoundState() {
	freeSoundEntryMatches();
	resetSoundListener();
}

// Two more things carry over from earlier sessions:
// - the impact sound timer (CPhysicsHook::m_impactSoundTime), which can mute a session's first impact sounds;
// - which waves of a multi-wave sound were played. A sound picks among the waves it has not played yet.
// For the reset load, the timer is 0 and each sound has all its waves available the first time it is used.
static float *g_impactSoundTime;
// By wave list address. Accepted limitation: a list freed and allocated again at the same address during the session
// is not reset a second time.
static std::set<const void *> g_resetSoundFiles;
static bool g_soundSlotsFresh;
namespace {
	struct SoundFile {
		unsigned short symbol;
		uint8_t gender;
		uint8_t available;
	};
}  // namespace
extern Hook g_EnsureAvailableSlotsForGender_Hook;
DECL_DETOUR_T(void, EnsureAvailableSlotsForGender, SoundFile *pSounds, int count, int gender) {
	if (Enabled() && g_soundSlotsFresh && g_resetSoundFiles.insert(pSounds).second) {
		for (int i = 0; i < count; ++i) pSounds[i].available = 1;
	}
	g_EnsureAvailableSlotsForGender_Hook.Disable();
	EnsureAvailableSlotsForGender(thisptr, pSounds, count, gender);
	g_EnsureAvailableSlotsForGender_Hook.Enable();
}
Hook g_EnsureAvailableSlotsForGender_Hook(EnsureAvailableSlotsForGender_Hook);
void TasDeterminism::SoundSessionEnd() {
	// Prerun restarts are meant to behave like a player's own restarts after a fresh launch, so they keep the timer
	// and the waves played since the reset load.
	if (PrerunRestartPending()) return;
	bool fresh = ResetLoadPending();
	if (fresh) *g_impactSoundTime = 0.0f;
	g_soundSlotsFresh = fresh;
	g_resetSoundFiles.clear();
}
// The timer advances during the load, so it is set to 0 again when the reset session starts, as on a first playback
void TasDeterminism::SoundSessionStart() {
	if (InResetSession()) *g_impactSoundTime = 0.0f;
}
ON_INIT {
	if (!Supported()) return;
	auto PhysFrame = Memory::Scan(server->Name(), Offsets::PhysFrame);
	EnsureAvailableSlotsForGender = Memory::Scan<decltype(EnsureAvailableSlotsForGender)>(MODULE("soundemittersystem"), Offsets::EnsureAvailableSlotsForGender);
	if (!PhysFrame || !EnsureAvailableSlotsForGender) return Missing("the sound slots");
	g_impactSoundTime = Memory::Deref<float *>(PhysFrame + Offsets::PhysFrame_m_bPausedOff) - 1;  // m_impactSoundTime is the float before m_bPaused
	g_EnsureAvailableSlotsForGender_Hook.SetFunc(EnsureAvailableSlotsForGender);
}

// Sound data is read by a job thread, so the frame on which a sound becomes ready depends on disk and thread timing.
// With the cvar on, the load request (always made on the main thread) only returns once the data has been read.
static constexpr int SOUND_LOAD_TIMEOUT_SECS = 10;
static Interface *g_waveDataCache;
static bool(__rescall *IsDataLoadCompleted)(void *, void *, bool *, bool *);
DECL_DETOUR_T(void *, AsyncLoadCache, const char *file, int size, int start, int flag) {
	void *h = AsyncLoadCache(thisptr, file, size, start, flag);
	if (!Enabled() || !h) return h;
	auto t0 = std::chrono::steady_clock::now();
	for (;;) {
		// stop waiting when the data is there, and also when the handle is not valid or the file is missing
		bool valid = false, missing = false;
		if (IsDataLoadCompleted(thisptr, h, &valid, &missing) || !valid || missing) break;
		if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(SOUND_LOAD_TIMEOUT_SECS)) {
			console->Warning("sar_tas_determinism: sound %s did not load in %d s\n", file, SOUND_LOAD_TIMEOUT_SECS);
			break;
		}
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}
	return h;
}
ON_INIT {
	if (!Supported()) return;
	auto wavedatacache = Memory::Scan(engine->Name(), Offsets::wavedatacacheSig, Offsets::wavedatacacheOff);
	if (wavedatacache) g_waveDataCache = Interface::Create(Memory::DerefDeref<void *>(wavedatacache));
	if (!g_waveDataCache) return Missing("the wave data cache");
	g_waveDataCache->Hook(AsyncLoadCache_Hook, AsyncLoadCache, Offsets::AsyncLoadCache);
	IsDataLoadCompleted = g_waveDataCache->Original<decltype(IsDataLoadCompleted)>(Offsets::IsDataLoadCompleted);
}
void TasDeterminism::UnhookSound() {
	Interface::Delete(g_waveDataCache);
	g_waveDataCache = nullptr;
}
