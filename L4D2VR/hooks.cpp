
#include "hooks.h"
#include "game.h"
#include "texture.h"
#include "sdk.h"
#include "sdk_server.h"
#include "vr.h"
#include "trace.h"
#include "offsets.h"
#include <iostream>
#include <cstdint>
#include <string>
#include <cstring>
#include <algorithm> // std::clamp
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <intrin.h>
#include <cstddef>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Normalize Source-style angles:
// - Bring pitch/yaw into [-180, 180] first (avoid -30 becoming 330 and then clamped to 89).
// - Then clamp pitch to [-89, 89].
static inline void NormalizeAndClampViewAngles(QAngle& a)
{
	while (a.x > 180.f) a.x -= 360.f;
	while (a.x < -180.f) a.x += 360.f;
	while (a.y > 180.f) a.y -= 360.f;
	while (a.y < -180.f) a.y += 360.f;
	a.z = 0.f;
	if (a.x > 89.f) a.x = 89.f;
	if (a.x < -89.f) a.x = -89.f;
}

static inline bool IsFiniteViewAngle(const QAngle& a)
{
	return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

static inline QAngle BuildVRAudioListenerAngles(VR* vr, const Vector& fallbackAngles)
{
	QAngle listenerAngles(fallbackAngles.x, fallbackAngles.y, fallbackAngles.z);

	if (vr)
	{
		// Keep Source's audio listener tied to the direction the player regards as "front",
		// not to the visual third-person camera. This is especially important for
		// front-view third-person, where the render camera looks back at the player
		// and would otherwise invert front/back spatial audio.
		Vector vrView = vr->GetViewAngle();
		QAngle vrAngles(vrView.x, vrView.y, vrView.z);
		if (IsFiniteViewAngle(vrAngles))
			listenerAngles = vrAngles;

		if (vr->m_MouseModeEnabled && !vr->m_MouseModeAimFromHmd)
		{
			listenerAngles.x = vr->m_MouseAimInitialized ? vr->m_MouseAimPitchOffset : listenerAngles.x;
			listenerAngles.y = vr->m_RotationOffset;
			listenerAngles.z = 0.0f;
		}
	}

	NormalizeAndClampViewAngles(listenerAngles);
	return listenerAngles;
}

// ------------------------------------------------------------
// Engine third-person camera smoothing
//
// Some camera mods (e.g. slide) switch to an engine-controlled third-person camera
// that updates at tick rate (30/60Hz) while we still render at HMD rate (90Hz+).
// That produces a "feels like 30fps" stutter even when frametime is stable.
//
// We blend from prev tick camera to curr tick camera over an estimated tick interval.
// Only enabled when tick interval > ~16ms (~<60Hz) to avoid adding lag to 90Hz paths.
// ------------------------------------------------------------
static inline float AngleDeltaDeg(float to, float from)
{
	float delta = std::fmod(to - from, 360.0f);
	if (delta > 180.0f) delta -= 360.0f;
	if (delta < -180.0f) delta += 360.0f;
	return delta;
}

static inline float AngleLerpDeg(float a, float b, float t)
{
	return a + AngleDeltaDeg(b, a) * t;
}

// Returns true if the call should be skipped because we ran it too recently.
static inline bool ShouldThrottleLog(std::chrono::steady_clock::time_point& last, float maxHz)
{
	if (maxHz <= 0.0f)
		return false;
	const float minInterval = 1.0f / std::max(1.0f, maxHz);
	const auto now = std::chrono::steady_clock::now();
	if (last.time_since_epoch().count() != 0)
	{
		const float elapsed = std::chrono::duration<float>(now - last).count();
		if (elapsed < minInterval)
			return true;
	}
	last = now;
	return false;
}

static inline const char* DebugTextureName(ITexture* texture)
{
	if (!texture)
		return "<null>";

	__try
	{
		const char* name = texture->GetName();
		return (name && *name) ? name : "<unnamed>";
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return "<bad-texture>";
	}
}

static inline void DebugTextureSize(ITexture* texture, int& width, int& height)
{
	width = 0;
	height = 0;
	if (!texture)
		return;

	__try
	{
		width = texture->GetMappingWidth();
		height = texture->GetMappingHeight();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		width = -1;
		height = -1;
	}
}

static inline void DebugTextureFullSize(ITexture* texture, int& mapW, int& mapH, int& actualW, int& actualH)
{
	mapW = 0;
	mapH = 0;
	actualW = 0;
	actualH = 0;
	if (!texture)
		return;

	__try
	{
		mapW = texture->GetMappingWidth();
		mapH = texture->GetMappingHeight();
		actualW = texture->GetActualWidth();
		actualH = texture->GetActualHeight();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		mapW = -1;
		mapH = -1;
		actualW = -1;
		actualH = -1;
	}
}

static inline void DebugRenderContextWindowSize(IMatRenderContext* context, int& width, int& height)
{
	width = 0;
	height = 0;
	if (!context)
		return;

	__try
	{
		context->GetWindowSize(width, height);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		width = -1;
		height = -1;
	}
}

static inline void DebugBackBufferDimensions(IMaterialSystem* materialSystem, int& width, int& height)
{
	width = 0;
	height = 0;
	if (!materialSystem)
		return;

	__try
	{
		materialSystem->GetBackBufferDimensions(width, height);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		width = -1;
		height = -1;
	}
}

struct DebugWindowSearchContext
{
	DWORD processId = 0;
	HWND hwnd = nullptr;
};

static BOOL CALLBACK DebugFindMainWindowProc(HWND hwnd, LPARAM lParam)
{
	auto* ctx = reinterpret_cast<DebugWindowSearchContext*>(lParam);
	if (!ctx || !IsWindow(hwnd))
		return TRUE;

	DWORD windowProcessId = 0;
	GetWindowThreadProcessId(hwnd, &windowProcessId);
	if (windowProcessId != ctx->processId)
		return TRUE;

	if (GetWindow(hwnd, GW_OWNER) != nullptr)
		return TRUE;

	LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
	if ((style & WS_VISIBLE) == 0)
		return TRUE;

	ctx->hwnd = hwnd;
	return FALSE;
}

static inline HWND DebugFindCurrentProcessMainWindow()
{
	DebugWindowSearchContext ctx;
	ctx.processId = GetCurrentProcessId();
	EnumWindows(DebugFindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
	return ctx.hwnd;
}

static inline void DebugClientRectSize(int& width, int& height)
{
	width = 0;
	height = 0;
	HWND hwnd = DebugFindCurrentProcessMainWindow();
	if (!hwnd)
		return;

	RECT rc{};
	if (GetClientRect(hwnd, &rc))
	{
		width = static_cast<int>(rc.right - rc.left);
		height = static_cast<int>(rc.bottom - rc.top);
	}
}

static inline bool DebugGetViewport(void* context, int& x, int& y, int& width, int& height)
{
	x = 0;
	y = 0;
	width = 0;
	height = 0;
	if (!context || !Hooks::hkGetViewport.fOriginal)
		return false;

	__try
	{
		Hooks::hkGetViewport.fOriginal(context, x, y, width, height);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		x = -1;
		y = -1;
		width = -1;
		height = -1;
		return false;
	}
}

static inline ITexture* DebugCurrentRenderTarget(IMatRenderContext* context)
{
	if (!context)
		return nullptr;

	__try
	{
		return context->GetRenderTarget();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static inline bool IsReadableProtection(DWORD protect)
{
	if (protect & PAGE_GUARD)
		return false;

	switch (protect & 0xff)
	{
	case PAGE_READONLY:
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
	case PAGE_EXECUTE_READ:
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return true;
	default:
		return false;
	}
}

static inline bool IsReadableMemoryRange(const void* ptr, size_t bytes)
{
	if (!ptr || bytes == 0)
		return false;

	const uint8_t* p = reinterpret_cast<const uint8_t*>(ptr);
	size_t remaining = bytes;

	while (remaining > 0)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (!VirtualQuery(p, &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		if (!IsReadableProtection(mbi.Protect))
			return false;

		const uintptr_t cur = reinterpret_cast<uintptr_t>(p);
		const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		if (regionEnd <= cur)
			return false;

		const size_t chunk = std::min<size_t>(remaining, size_t(regionEnd - cur));
		p += chunk;
		remaining -= chunk;
	}

	return true;
}

constexpr uintptr_t kClientSayTextHandlerOffset = 0x22BF50;
constexpr uintptr_t kClientSayText2HandlerOffset = 0x22BF80;
constexpr uintptr_t kClientTextMsgHandlerOffset = 0x22BFB0;

struct OldBfRead
{
	const char* m_pDebugName = nullptr;
	bool m_bOverflow = false;
	uint8_t m_Padding5 = 0;
	uint8_t m_Padding6 = 0;
	uint8_t m_Padding7 = 0;
	int m_nDataBits = 0;
	uint32_t m_nDataBytes = 0;
	uint32_t m_nInBufWord = 0;
	int m_nBitsAvail = 0;
	const uint32_t* m_pDataIn = nullptr;
	const uint32_t* m_pBufferEnd = nullptr;
	const uint32_t* m_pData = nullptr;

	void GrabNextDWord(bool overflowImmediately = false)
	{
		if (m_pDataIn == m_pBufferEnd)
		{
			m_nBitsAvail = 1;
			m_nInBufWord = 0;
			++m_pDataIn;
			if (overflowImmediately)
				m_bOverflow = true;
		}
		else if (m_pDataIn > m_pBufferEnd)
		{
			m_bOverflow = true;
			m_nInBufWord = 0;
		}
		else
		{
			m_nInBufWord = *m_pDataIn++;
		}
	}

	uint32_t ReadByte()
	{
		if (m_nBitsAvail >= 8)
		{
			const uint32_t value = m_nInBufWord & 0xffu;
			m_nBitsAvail -= 8;
			if (m_nBitsAvail == 0)
			{
				m_nBitsAvail = 32;
				GrabNextDWord(false);
			}
			else
			{
				m_nInBufWord >>= 8;
			}
			return value;
		}

		const int bitsFromCurrent = std::max(0, m_nBitsAvail);
		uint32_t value = m_nInBufWord;
		const int bitsNeeded = 8 - bitsFromCurrent;

		m_nBitsAvail = 32;
		GrabNextDWord(true);
		if (m_bOverflow)
			return 0;

		const uint32_t mask = (bitsNeeded >= 32) ? 0xffffffffu : ((1u << bitsNeeded) - 1u);
		value |= (m_nInBufWord & mask) << bitsFromCurrent;
		m_nInBufWord >>= bitsNeeded;
		m_nBitsAvail = 32 - bitsNeeded;
		return value & 0xffu;
	}

	bool ReadCString(char* out, size_t outLen)
	{
		if (!out || outLen == 0)
			return false;

		size_t written = 0;
		bool truncated = false;
		out[0] = '\0';

		for (size_t i = 0; i < 2048; ++i)
		{
			const char ch = static_cast<char>(ReadByte());
			if (m_bOverflow)
				break;

			if (ch == '\0')
				break;

			if (written + 1 < outLen)
			{
				out[written++] = ch;
			}
			else
			{
				truncated = true;
			}
		}

		out[(written < outLen) ? written : (outLen - 1)] = '\0';
		return !m_bOverflow && !truncated;
	}
};

static_assert(offsetof(OldBfRead, m_nDataBits) == 0x08, "Unexpected OldBfRead layout");
static_assert(offsetof(OldBfRead, m_pDataIn) == 0x18, "Unexpected OldBfRead layout");
static_assert(offsetof(OldBfRead, m_pData) == 0x20, "Unexpected OldBfRead layout");
static_assert(sizeof(OldBfRead) == 0x24, "Unexpected OldBfRead size");

static std::string SanitizeHudMessageField(const char* text)
{
	if (!text)
		return {};

	std::string sanitized;
	sanitized.reserve(std::strlen(text));
	for (const unsigned char ch : std::string(text))
	{
		if (ch == '\r')
			sanitized += "\\r";
		else if (ch == '\n')
			sanitized += "\\n";
		else if (ch == '\t')
			sanitized += "\\t";
		else if (ch < 0x20)
			sanitized += '?';
		else
			sanitized.push_back(static_cast<char>(ch));
	}
	return sanitized;
}

static std::string CompactChatText(const char* text)
{
	std::string compact;
	if (!text)
		return compact;

	compact.reserve(std::strlen(text));
	bool prevSpace = false;
	for (const unsigned char ch : std::string(text))
	{
		if (ch < 0x20)
			continue;

		if (ch == ' ')
		{
			if (!compact.empty() && !prevSpace)
			{
				compact.push_back(' ');
				prevSpace = true;
			}
			continue;
		}

		compact.push_back(static_cast<char>(ch));
		prevSpace = false;
	}

	while (!compact.empty() && compact.front() == ' ')
		compact.erase(compact.begin());
	while (!compact.empty() && compact.back() == ' ')
		compact.pop_back();
	return compact;
}

static bool SnapshotHudMessageReader(const void* msgData, OldBfRead& out)
{
	if (!msgData || !IsReadableMemoryRange(msgData, sizeof(OldBfRead)))
		return false;

	std::memcpy(&out, msgData, sizeof(out));
	if (!out.m_pData || !out.m_pDataIn || !out.m_pBufferEnd)
		return false;

	if (out.m_nDataBits <= 0 || out.m_nDataBits > (1 << 20))
		return false;

	if (out.m_nDataBytes == 0 || out.m_nDataBytes > (1u << 16))
		return false;

	return IsReadableMemoryRange(out.m_pData, out.m_nDataBytes);
}

static void LogHudUserMessagePayload(const char* type, void* msgData)
{
	OldBfRead reader{};
	if (!SnapshotHudMessageReader(msgData, reader))
		return;

	auto forwardHudChatLine = [&](const std::string& speaker, const std::string& text)
		{
			if (Hooks::m_VR && !text.empty())
				Hooks::m_VR->HandleHudChatLine(speaker, text);
		};

	if (_stricmp(type, "SayText") == 0)
	{
		reader.ReadByte(); // sender
		char textBuf[256]{};
		reader.ReadCString(textBuf, sizeof(textBuf));
		if (!reader.m_bOverflow)
			reader.ReadByte(); // wants to chat

		const std::string content = CompactChatText(textBuf);
		if (!content.empty())
		{
			Game::logMsg("[HUDChat] %s", content.c_str());
			forwardHudChatLine({}, content);
		}
		return;
	}

	if (_stricmp(type, "SayText2") == 0)
	{
		reader.ReadByte(); // sender
		reader.ReadByte(); // wants to chat

		char fields[5][256]{};
		for (int i = 0; i < 5 && !reader.m_bOverflow; ++i)
			reader.ReadCString(fields[i], sizeof(fields[i]));

		const std::string name = CompactChatText(fields[1]);
		const std::string content = CompactChatText(fields[2]);
		const std::string extra1 = CompactChatText(fields[3]);
		const std::string extra2 = CompactChatText(fields[4]);
		const std::string token = CompactChatText(fields[0]);

		if (!name.empty() && !content.empty())
		{
			Game::logMsg("[HUDChat] %s: %s", name.c_str(), content.c_str());
			forwardHudChatLine(name, content);
		}
		else if (!content.empty())
		{
			Game::logMsg("[HUDChat] %s", content.c_str());
			forwardHudChatLine({}, content);
		}
		else if (!name.empty() && !extra1.empty())
		{
			Game::logMsg("[HUDChat] %s: %s", name.c_str(), extra1.c_str());
			forwardHudChatLine(name, extra1);
		}
		else if (!name.empty() && !extra2.empty())
		{
			Game::logMsg("[HUDChat] %s: %s", name.c_str(), extra2.c_str());
			forwardHudChatLine(name, extra2);
		}
		else if (!token.empty() && token[0] != '#')
		{
			Game::logMsg("[HUDChat] %s", token.c_str());
			forwardHudChatLine({}, token);
		}
		return;
	}

	if (_stricmp(type, "TextMsg") == 0)
	{
		return;
	}
}

static void TryLogHudUserMessagePayload(const char* type, void* msgData)
{
#if defined(_MSC_VER)
	__try
	{
		LogHudUserMessagePayload(type, msgData);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		Game::logMsg("[HUDChat][%s] decode fault", type ? type : "Unknown");
	}
#else
	LogHudUserMessagePayload(type, msgData);
#endif
}

static inline float SmoothStep01(float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

struct EngineThirdPersonCamSmoother
{
	bool valid = false;
	Vector prevOrigin{ 0,0,0 };
	Vector currOrigin{ 0,0,0 };
	QAngle prevAngles{ 0,0,0 };
	QAngle currAngles{ 0,0,0 };
	std::chrono::steady_clock::time_point lastRawUpdate{};
	std::chrono::steady_clock::time_point blendStart{};
	float tickIntervalSec = (1.0f / 30.0f); // pessimistic default

	void Reset()
	{
		valid = false;
	}

	void PushRaw(const Vector& rawOrigin, const QAngle& rawAngles)
	{
		const auto now = std::chrono::steady_clock::now();

		if (!valid)
		{
			valid = true;
			prevOrigin = currOrigin = rawOrigin;
			prevAngles = currAngles = rawAngles;
			lastRawUpdate = now;
			blendStart = now;
			return;
		}

		// Detect a new tick camera sample.
		const float posDeltaSqr = (rawOrigin - currOrigin).LengthSqr();
		const float angDelta =
			std::fabs(AngleDeltaDeg(rawAngles.x, currAngles.x)) +
			std::fabs(AngleDeltaDeg(rawAngles.y, currAngles.y)) +
			std::fabs(AngleDeltaDeg(rawAngles.z, currAngles.z));
		const bool changed = (posDeltaSqr > (0.25f * 0.25f)) || (angDelta > 0.25f);

		if (changed)
		{
			const float dt = std::chrono::duration<float>(now - lastRawUpdate).count();
			const float clamped = std::clamp(dt, 0.008f, 0.100f);
			tickIntervalSec = (tickIntervalSec * 0.8f) + (clamped * 0.2f);

			prevOrigin = currOrigin;
			prevAngles = currAngles;
			currOrigin = rawOrigin;
			currAngles = rawAngles;
			lastRawUpdate = now;
			blendStart = now;
		}
	}

	bool ShouldSmooth() const
	{
		// Only smooth when camera updates are slower than ~60Hz.
		return valid && (tickIntervalSec > 0.016f);
	}

	void GetSmoothed(Vector& outOrigin, QAngle& outAngles) const
	{
		if (!valid)
		{
			outOrigin = { 0,0,0 };
			outAngles = { 0,0,0 };
			return;
		}

		if (!ShouldSmooth())
		{
			outOrigin = currOrigin;
			outAngles = currAngles;
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		const float tRaw = std::chrono::duration<float>(now - blendStart).count() / std::max(0.001f, tickIntervalSec);
		const float t = SmoothStep01(tRaw);

		outOrigin = prevOrigin + (currOrigin - prevOrigin) * t;
		outAngles.x = AngleLerpDeg(prevAngles.x, currAngles.x, t);
		outAngles.y = AngleLerpDeg(prevAngles.y, currAngles.y, t);
		outAngles.z = AngleLerpDeg(prevAngles.z, currAngles.z, t);
	}
};


// ------------------------------------------------------------
// Third-person render stability helpers (netvars from offsets.txt)
// When the player is pinned / incapacitated / using certain actions,
// the engine can momentarily "snap" between first/third-person.
// We treat those states as "force third-person rendering" to avoid flicker.
// ------------------------------------------------------------
template <typename T>
static inline T ReadNetvar(const void* base, int ofs)
{
	return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + ofs);
}

static inline bool HandleValid(int h)
{
	return (h != 0 && h != -1);
}

// Mouse-mode aiming helpers (mouse+keyboard play; no controllers required)
static inline Vector GetMouseModeGunOriginAbs(const VR* vr)
{
	return vr->m_HmdPosAbs
		+ (vr->m_HmdForward * (vr->m_MouseModeViewmodelAnchorOffset.x * vr->m_VRScale))
		+ (vr->m_HmdRight * (vr->m_MouseModeViewmodelAnchorOffset.y * vr->m_VRScale))
		+ (vr->m_HmdUp * (vr->m_MouseModeViewmodelAnchorOffset.z * vr->m_VRScale));
}

static inline Vector GetMouseModeEyeDir(const VR* vr)
{
	Vector eyeDir{ 0.0f, 0.0f, 0.0f };
	if (vr->m_MouseModeAimFromHmd)
	{
		eyeDir = vr->m_HmdForward;
	}
	else
	{
		const float pitch = std::clamp(vr->m_MouseAimPitchOffset, -89.f, 89.f);
		const float yaw = vr->m_RotationOffset;
		QAngle eyeAng(pitch, yaw, 0.f);
		NormalizeAndClampViewAngles(eyeAng);

		Vector right, up;
		QAngle::AngleVectors(eyeAng, &eyeDir, &right, &up);
	}

	if (!eyeDir.IsZero())
		VectorNormalize(eyeDir);
	return eyeDir;
}

static inline Vector GetMouseModeDefaultTargetAbs(const VR* vr)
{
	Vector eyeDir = GetMouseModeEyeDir(vr);
	const float dist = (vr->m_MouseModeAimConvergeDistance > 0.0f) ? vr->m_MouseModeAimConvergeDistance : 8192.0f;
	return vr->m_HmdPosAbs + eyeDir * dist;
}

static inline bool GetMouseModeAimAnglesToTarget(const VR* vr, const Vector& from, const Vector& target, QAngle& outAngles)
{
	Vector to = target - from;
	if (to.IsZero())
		return false;
	VectorNormalize(to);
	QAngle ang;
	QAngle::VectorAngles(to, ang);
	NormalizeAndClampViewAngles(ang);
	outAngles = ang;
	return true;
}

static inline QAngle GetMouseModeFallbackAimAngles(const VR* vr)
{
	Vector eyeDir = GetMouseModeEyeDir(vr);
	if (eyeDir.IsZero())
	{
		QAngle a(std::clamp(vr->m_MouseAimPitchOffset, -89.f, 89.f), vr->m_RotationOffset, 0.f);
		NormalizeAndClampViewAngles(a);
		return a;
	}
	QAngle a;
	QAngle::VectorAngles(eyeDir, a);
	NormalizeAndClampViewAngles(a);
	return a;
}

// Returns true if the local player is currently pinned/controlled by special infected.
// Used to disable jittery aim line while being dragged / pinned.
static inline bool IsPlayerControlledBySI(const C_BasePlayer* player)
{
	if (!player)
		return false;

	// Smoker
	const int tongueOwner = ReadNetvar<int>(player, 0x1f6c);              // m_tongueOwner
	const bool hangingTongue = ReadNetvar<uint8_t>(player, 0x1f84) != 0;  // m_isHangingFromTongue
	const bool tongue = hangingTongue || HandleValid(tongueOwner);

	// Hunter / Charger / Jockey pins
	const int carryAttacker = ReadNetvar<int>(player, 0x2714);           // m_carryAttacker
	const int pummelAttacker = ReadNetvar<int>(player, 0x2720);           // m_pummelAttacker
	const int pounceAttacker = ReadNetvar<int>(player, 0x272c);           // m_pounceAttacker
	const int jockeyAttacker = ReadNetvar<int>(player, 0x274c);           // m_jockeyAttacker
	const bool pinned = HandleValid(carryAttacker) || HandleValid(pummelAttacker) ||
		HandleValid(pounceAttacker) || HandleValid(jockeyAttacker);

	return tongue || pinned;
}

struct ThirdPersonStateDebug
{
	bool dead = false;
	int lifeState = 0;
	bool goingToDie = false;
	int observerMode = 0;
	int observerTarget = 0;
	bool incap = false;
	bool ledge = false;
	bool hangingTongue = false;
	bool tongue = false;
	bool pinned = false;
	bool doingUseAction = false;
	bool reviving = false;
	bool selfMedkit = false;
	int useActionOwner = 0;
	int useActionTarget = 0;
	int tongueOwner = 0;
	int carryAttacker = 0;
	int pummelAttacker = 0;
	int pounceAttacker = 0;
	int jockeyAttacker = 0;
	int useAction = 0;
	int reviveOwner = 0;
	int reviveTarget = 0;
};

static inline bool ShouldForceThirdPersonByState(const C_BasePlayer* player,
	IClientEntityList* entList,
	const C_BasePlayer* localPlayer,
	ThirdPersonStateDebug* outDbg = nullptr)
{
	if (outDbg)
		*outDbg = ThirdPersonStateDebug{};

	if (!player)
		return false;

	ThirdPersonStateDebug dbg{};

	// When dead / dying / observer transitions happen, the engine camera can flicker
	// between views for a few frames. Force third-person rendering to avoid VR flicker.
	dbg.lifeState = (int)ReadNetvar<uint8_t>(player, 0x147); // m_lifeState
	dbg.goingToDie = ReadNetvar<uint8_t>(player, 0x1fb4) != 0;   // m_isGoingToDie
	dbg.observerMode = ReadNetvar<int>(player, 0x1450);          // m_iObserverMode
	dbg.observerTarget = ReadNetvar<int>(player, 0x1454);        // m_hObserverTarget


	// Offsets are client netvars (see offsets.txt)
	dbg.incap = ReadNetvar<uint8_t>(player, 0x1ea9) != 0;          // m_isIncapacitated
	dbg.ledge = ReadNetvar<uint8_t>(player, 0x25ec) != 0;          // m_isHangingFromLedge
	// IMPORTANT (L4D2):
	// m_isGoingToDie can stay true for "near death / black&white / scripted transitions" while the player is alive
	// (lifeState==0). Using it as "dead-ish" causes third-person latching after revive.
	// Only use lifeState to decide dead/dying here.
	// Typical Source: 0=ALIVE, 1=DYING, 2=DEAD.
	const bool lifeDead = (dbg.lifeState == 2);
	const bool lifeDying = (dbg.lifeState == 1);
	dbg.dead = lifeDead || (lifeDying && !dbg.incap);
	dbg.tongueOwner = ReadNetvar<int>(player, 0x1f6c);             // m_tongueOwner
	dbg.hangingTongue = ReadNetvar<uint8_t>(player, 0x1f84) != 0;  // m_isHangingFromTongue
	dbg.tongue = dbg.hangingTongue || HandleValid(dbg.tongueOwner);

	dbg.carryAttacker = ReadNetvar<int>(player, 0x2714);           // m_carryAttacker
	dbg.pummelAttacker = ReadNetvar<int>(player, 0x2720);          // m_pummelAttacker
	dbg.pounceAttacker = ReadNetvar<int>(player, 0x272c);          // m_pounceAttacker
	dbg.jockeyAttacker = ReadNetvar<int>(player, 0x274c);          // m_jockeyAttacker
	dbg.pinned = HandleValid(dbg.carryAttacker) || HandleValid(dbg.pummelAttacker) ||
		HandleValid(dbg.pounceAttacker) || HandleValid(dbg.jockeyAttacker);

	dbg.useAction = ReadNetvar<int>(player, 0x1ba8);               // m_iCurrentUseAction
	dbg.doingUseAction = (dbg.useAction != 0);
	// Distinguish "being treated by teammate" vs "self-heal".
   // We ONLY force third-person for self-heal; teammate-treatment should NOT force third-person
   // (it causes flicker and is disorienting in VR).
	dbg.useActionOwner = ReadNetvar<int>(player, 0x1ba4);          // m_useActionOwner
	dbg.useActionTarget = ReadNetvar<int>(player, 0x1ba0);         // m_useActionTarget
	if (dbg.useAction == 1 && entList && localPlayer && HandleValid(dbg.useActionOwner) && HandleValid(dbg.useActionTarget))
	{
		auto* ownerEnt = (C_BaseEntity*)entList->GetClientEntityFromHandle(dbg.useActionOwner);
		auto* targetEnt = (C_BaseEntity*)entList->GetClientEntityFromHandle(dbg.useActionTarget);
		dbg.selfMedkit = (ownerEnt == localPlayer) && (targetEnt == localPlayer);
	}
	dbg.reviveOwner = ReadNetvar<int>(player, 0x1f88);             // m_reviveOwner
	dbg.reviveTarget = ReadNetvar<int>(player, 0x1f8c);            // m_reviveTarget
	dbg.reviving = HandleValid(dbg.reviveOwner) || HandleValid(dbg.reviveTarget);

	if (outDbg)
		*outDbg = dbg;

	// NOTE: user request:
	// - "倒地" (incapacitated) 不强制第三人称
	// Keep other pinned/use/tongue states.
	// Keep other pinned/tongue states. Only self-heal forces third-person from useAction.
	const bool observer = (dbg.observerMode != 0) && (dbg.dead || HandleValid(dbg.observerTarget));
	// NOTE: m_iObserverMode can be transiently non-zero during revive/incap camera transitions.
	// Guard it with either a dead-ish state or a valid observer target to avoid false third-person latching.
	// NOTE: do NOT force 3P for generic useAction; it includes teammate revive/assistance/interaction and can latch 3P.
	return dbg.dead || observer || dbg.ledge || dbg.tongue || dbg.pinned || dbg.selfMedkit;
}

#include "hooks/hooks_init.inl"
#include "hooks/hooks_render.inl"
#include "hooks/hooks_createmove.inl"
#include "hooks/hooks_combat_network.inl"
#include "hooks/hooks_misc.inl"
