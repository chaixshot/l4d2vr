// ------------------------------------------------------------
// Multicore viewmodel stabilization helpers
//
// In Source, viewmodels are often drawn with pCustomBoneToWorld (bone matrices in world space).
// In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model.
//
// For queued rendering (mat_queue_mode!=0) we must instead apply a delta transform to the
// custom bone matrices for the draw call, so the viewmodel uses the controller-anchored pose
// sampled on the render thread (no shared-state writes, no tearing).
// ------------------------------------------------------------
namespace vr_vm_stabilize
{
    struct Mat3x4
    {
        float m[3][4];
    };

    template <typename T>
    inline bool SafeRead(const void* p, T& out)
    {
#if defined(_MSC_VER)
        __try
        {
            out = *reinterpret_cast<const T*>(p);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        // Non-MSVC builds are not expected for this project.
        // Keep it simple: attempt the read.
        out = *reinterpret_cast<const T*>(p);
        return true;
#endif
    }

    inline Vector GetOrigin(const Mat3x4& a)
    {
        return Vector(a.m[0][3], a.m[1][3], a.m[2][3]);
    }

    inline void BuildFromOrgAngles(const Vector& origin, const QAngle& ang, Mat3x4& out)
    {
        Vector f, r, u;
        QAngle::AngleVectors(ang, &f, &r, &u);

        out.m[0][0] = f.x; out.m[0][1] = r.x; out.m[0][2] = u.x; out.m[0][3] = origin.x;
        out.m[1][0] = f.y; out.m[1][1] = r.y; out.m[1][2] = u.y; out.m[1][3] = origin.y;
        out.m[2][0] = f.z; out.m[2][1] = r.z; out.m[2][2] = u.z; out.m[2][3] = origin.z;
    }

    // Invert a rigid transform (rotation + translation only)
    inline void InvertTR(const Mat3x4& in, Mat3x4& out)
    {
        // Transpose rotation
        out.m[0][0] = in.m[0][0]; out.m[0][1] = in.m[1][0]; out.m[0][2] = in.m[2][0];
        out.m[1][0] = in.m[0][1]; out.m[1][1] = in.m[1][1]; out.m[1][2] = in.m[2][1];
        out.m[2][0] = in.m[0][2]; out.m[2][1] = in.m[1][2]; out.m[2][2] = in.m[2][2];

        const float tx = -in.m[0][3];
        const float ty = -in.m[1][3];
        const float tz = -in.m[2][3];

        out.m[0][3] = tx * out.m[0][0] + ty * out.m[0][1] + tz * out.m[0][2];
        out.m[1][3] = tx * out.m[1][0] + ty * out.m[1][1] + tz * out.m[1][2];
        out.m[2][3] = tx * out.m[2][0] + ty * out.m[2][1] + tz * out.m[2][2];
    }

    inline void Mul(const Mat3x4& a, const Mat3x4& b, Mat3x4& out)
    {
        // Rotation
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                out.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            }
        }
        // Translation
        out.m[0][3] = a.m[0][0] * b.m[0][3] + a.m[0][1] * b.m[1][3] + a.m[0][2] * b.m[2][3] + a.m[0][3];
        out.m[1][3] = a.m[1][0] * b.m[0][3] + a.m[1][1] * b.m[1][3] + a.m[1][2] * b.m[2][3] + a.m[1][3];
        out.m[2][3] = a.m[2][0] * b.m[0][3] + a.m[2][1] * b.m[1][3] + a.m[2][2] * b.m[2][3] + a.m[2][3];
    }

    inline void ApplyDelta(const Mat3x4& delta, Mat3x4* bones, int numBones)
    {
        for (int i = 0; i < numBones; ++i)
        {
            Mat3x4 tmp{};
            Mul(delta, bones[i], tmp);
            bones[i] = tmp;
        }
    }
    // IMPORTANT for mat_queue_mode!=0:
    // DrawModelExecute may queue commands that reference pCustomBoneToWorld later on another thread.
    // If we pass a pointer to a temporary / thread_local scratch buffer, it can be overwritten before
    // the queued command executes, causing severe ghosting / double images.
    //
    // So we allocate per-draw bone copies from a small ring of per-frame slots. Each slot is kept
    // alive for kRing frames before being recycled. This makes the pointer stable long enough for
    // the material queue to consume it.
    struct BoneRingSlot
    {
        uint64_t frame = 0;
        std::vector<Mat3x4*> blocks;
    };

    inline Mat3x4* AllocStableBones(int numBones, uint32_t seqEven)
    {
        if (numBones <= 0 || numBones > 512)
            return nullptr;

        static constexpr uint32_t kRing = 64;
        static BoneRingSlot s_slots[kRing];
        static std::mutex s_mu;

        const uint64_t frame = (uint64_t)(seqEven >> 1);
        const uint32_t slot = (uint32_t)(frame % kRing);

        std::lock_guard<std::mutex> lock(s_mu);
        BoneRingSlot& s = s_slots[slot];
        if (s.frame != frame)
        {
            for (Mat3x4* p : s.blocks)
                delete[] p;
            s.blocks.clear();
            s.frame = frame;
        }

        Mat3x4* p = nullptr;
        try { p = new Mat3x4[(size_t)numBones]; } catch (...) { p = nullptr; }
        if (!p)
            return nullptr;

        s.blocks.push_back(p);
        return p;
    }
    // DrawModelState_t is opaque here, but in Source the first pointer is typically studiohdr_t*.
    // We avoid hard-crashing by SEH-guarding reads and probing common studiohdr_t offsets for numbones.
    inline bool TryGetNumBonesFromDrawState(void* drawState, int& outBones)
    {
        if (!drawState)
            return false;

        void* studioHdr = nullptr;
        if (!SafeRead(drawState, studioHdr) || !studioHdr)
            return false;

        // Common studiohdr_t::numbones offsets across Source branches.
        static const int kOffsets[] = { 0x9C, 0xA0, 0x98, 0x94, 0xA4, 0xA8, 0x90, 0x8C, 0xB0 };
        for (int off : kOffsets)
        {
            int n = 0;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(studioHdr) + off;
            if (SafeRead(p, n) && n > 0 && n <= 512)
            {
                outBones = n;
                return true;
            }
        }
        return false;
    }
}

namespace
{
    std::mutex s_TrackedConVarTraceMutex;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_TrackedConVarTraceLastLog;

    C_BaseEntity* HooksSafeGetClientEntity(Game* game, int entityIndex)
    {
        if (!game || !game->m_ClientEntityList || entityIndex <= 0 || entityIndex > 2048)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return game->GetClientEntity(entityIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return game->GetClientEntity(entityIndex);
#endif
    }

    const char* HooksSafeGetNetworkClassName(Game* game, C_BaseEntity* entity)
    {
        if (!game || !entity)
            return nullptr;
#ifdef _MSC_VER
        __try
        {
            return game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
#else
        return game->GetNetworkClassName(reinterpret_cast<uintptr_t*>(entity));
#endif
    }

    inline bool ShouldThrottleTrackedConVarTrace(const std::string& key, float maxHz = 5.0f)
    {
        if (key.empty() || maxHz <= 0.0f)
            return false;

        const auto now = std::chrono::steady_clock::now();
        const auto minInterval =
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(1.0f / maxHz));

        std::lock_guard<std::mutex> lock(s_TrackedConVarTraceMutex);
        auto& last = s_TrackedConVarTraceLastLog[key];
        if (last.time_since_epoch().count() != 0 && now - last < minInterval)
            return true;

        last = now;
        return false;
    }

    inline std::string DescribeCallerAddress(const void* address)
    {
        if (!address)
            return "unknown";

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0 || !mbi.AllocationBase)
            return "unknown";

        char path[MAX_PATH] = {};
        const DWORD pathLen = GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH);
        const char* moduleName = (pathLen > 0) ? path : "unknown";
        if (pathLen > 0)
        {
            const char* slash = std::strrchr(path, '\\');
            if (slash && slash[1] != '\0')
                moduleName = slash + 1;
        }

        char buffer[MAX_PATH + 64] = {};
        const uintptr_t offset =
            reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        sprintf_s(buffer, "%s+0x%IX", moduleName, offset);
        return buffer;
    }

    inline void TraceTrackedConVarWrite(
        void* convar,
        const char* requestedValue,
        const char* hookPath,
        const void* callerAddress,
        bool isIConVarThis,
        bool blocked)
    {
        if (!Hooks::m_Game || !Hooks::m_VR || Game::HasConVarWritePermit() || !Hooks::m_VR->m_LocalVScriptConvarsLogEnabled)
            return;

        const char* name = isIConVarThis
            ? Hooks::m_Game->GetConVarNameFromIConVarPointer(convar)
            : Hooks::m_Game->GetConVarNameFromPointer(convar);
        if (!name || !*name)
            return;

        std::string expectedValue;
        if (!Hooks::m_VR->TryGetTrackedProtectedConvarValue(name, expectedValue))
            return;

        std::string throttleKey = std::string(hookPath ? hookPath : "ConVarTrace") + "|" + name;
        if (ShouldThrottleTrackedConVarTrace(throttleKey))
            return;

        const std::string beforeValue = Hooks::m_Game->GetConVarString(name);
        const std::string caller = DescribeCallerAddress(callerAddress);
        Game::logMsg(
            "[VR][LocalVScriptConvars] TraceWrite path=%s name=%s before='%s' expected='%s' requested='%s' caller=%s blocked=%d",
            hookPath ? hookPath : "unknown",
            name,
            beforeValue.c_str(),
            expectedValue.c_str(),
            requestedValue ? requestedValue : "",
            caller.c_str(),
            blocked ? 1 : 0);
    }

    inline bool ShouldBlockLockedConVarWrite(void* convar, const char* requestedValue)
    {
        if (!Hooks::m_Game || !Hooks::m_VR || Game::HasConVarWritePermit())
            return false;

        const char* name = Hooks::m_Game->GetConVarNameFromIConVarPointer(convar);
        if (!name || !*name)
            return false;

        return Hooks::m_VR->ShouldBlockExternalProtectedConvarWrite(
            name,
            requestedValue ? requestedValue : "");
    }
}

void Hooks::dAdjustEngineViewport(int& x, int& y, int& width, int& height)
{
	hkAdjustEngineViewport.fOriginal(x, y, width, height);
}

void Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
	hkViewport.fOriginal(ecx, x, y, width, height);
}

void Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
	hkGetViewport.fOriginal(ecx, x, y, width, height);
}

int Hooks::dTestMeleeSwingCollisionClient(void* ecx, void* edx, Vector const& vec)
{
	const int result = hkTestMeleeSwingCollisionClient.fOriginal(ecx, vec);
	NotifyLocalMeleeCollisionHaptics(false, ecx, result, -1, -1);
	return result;
}

int Hooks::dTestMeleeSwingCollisionServer(void* ecx, void* edx, Vector const& vec)
{
	Server_WeaponCSBase* weapon = reinterpret_cast<Server_WeaponCSBase*>(ecx);
	const int entitiesHitBefore = weapon ? weapon->entitiesHitThisSwing : -1;
	const int result = hkTestMeleeSwingCollisionServer.fOriginal(ecx, vec);
	const int entitiesHitAfter = weapon ? weapon->entitiesHitThisSwing : entitiesHitBefore;
	NotifyLocalMeleeCollisionHaptics(true, ecx, result, entitiesHitBefore, entitiesHitAfter);
	return result;
}

void Hooks::dDoMeleeSwingServer(void* ecx, void* edx)
{
	return hkDoMeleeSwingServer.fOriginal(ecx);
}

void Hooks::dStartMeleeSwingServer(void* ecx, void* edx, void* player, bool a3)
{
	return hkStartMeleeSwingServer.fOriginal(ecx, player, a3);
}

int Hooks::dPrimaryAttackServer(void* ecx, void* edx)
{
	return hkPrimaryAttackServer.fOriginal(ecx);
}

void Hooks::dItemPostFrameServer(void* ecx, void* edx)
{
	hkItemPostFrameServer.fOriginal(ecx);
}

int Hooks::dGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

Vector* Hooks::dEyePosition(void* ecx, void* edx, Vector* eyePos)
{
	Vector* result = hkEyePosition.fOriginal(ecx, eyePos);

	if (m_Game->m_PerformingMelee)
	{
		int i = m_Game->m_CurrentUsercmdID;
		if (m_Game->IsValidPlayerIndex(i))
		{
			*result = m_Game->m_PlayersVRInfo[i].controllerPos;
		}
	}

	return result;
}

void Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
	if (m_Game->m_SwitchedWeapons)
		m_Game->m_CachedArmsModel = false;

	bool hideArms = m_Game->m_IsMeleeWeaponActive || m_VR->m_HideArms;

	void* pBonesToWorldFinal = pCustomBoneToWorld;

	// Per-draw origin/angles override (used for multicore viewmodel stabilization).
	// We never write into shared entity state here; we only override the ModelRenderInfo_t
	// passed down to the renderer for this draw call (frame-stable, avoids queued-thread tearing).
	ModelRenderInfo_t drawInfo = info;
	const ModelRenderInfo_t* pDrawInfo = &info;

	std::string modelName;
	if (info.pModel)
	{
		modelName = m_Game->m_ModelInfo->GetModelName(info.pModel);
		// In desktop-mirror overlay hide mode, special-infected arrows are cached
		// from the RenderView hook at a fixed stereo-pass point. Never scan from
		// DrawModelExecute in that mode: under mat_queue_mode 2 this hook can run
		// once per model and would multiply client-entity-list scan requests.
		const bool desktopMirrorOverlayHideActiveEarly =
			m_VR->m_DesktopMirrorHidePluginOverlays && m_VR->m_DesktopMirrorEnabled;
		if (!desktopMirrorOverlayHideActiveEarly)
			m_VR->ScanSpecialInfectedEntitiesFromClientList();

		const C_BaseEntity* entity = nullptr;
		if (m_Game->m_ClientEntityList && info.entity_index > 0 && info.entity_index <= 2048)
		{
			entity = HooksSafeGetClientEntity(m_Game, info.entity_index);
		}
		bool isPlayerClass = false;
		const char* className = nullptr;
		if (entity)
		{
			className = HooksSafeGetNetworkClassName(m_Game, const_cast<C_BaseEntity*>(entity));
			isPlayerClass = className && (std::strcmp(className, "CTerrorPlayer") == 0 || std::strcmp(className, "C_TerrorPlayer") == 0);
		}
		// A server SetOrigin teleport can leave one queued first-person viewmodel draw
		// produced against the pre-teleport anchor. Drop that short transition window
		// instead of rendering a weapon model that flashes once and disappears.
		const bool teleportSuppressibleViewmodel =
			(className && (std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0)) ||
			(modelName.find("models/weapons/v_") != std::string::npos) ||
			(modelName.find("/v_models/") != std::string::npos) ||
			(modelName.find("models/v_models/") != std::string::npos) ||
			(modelName.find("models/weapons/melee/v_") != std::string::npos) ||
			(modelName.find("/melee/v_") != std::string::npos) ||
			(modelName.find("models/weapons/arms/") != std::string::npos) ||
			(modelName.find("/arms/") != std::string::npos) ||
			(modelName.find("v_arms") != std::string::npos) ||
			(modelName.find("models/weapons/hands/") != std::string::npos) ||
			(modelName.find("/hands/") != std::string::npos) ||
			(modelName.find("v_hands") != std::string::npos);
		if (teleportSuppressibleViewmodel && m_VR->ShouldSuppressTeleportViewmodelRender())
			return;

		const bool suppressDesktopMirrorPluginOverlays =
			m_VR->m_DesktopMirrorCleanRenderingPass && m_VR->m_DesktopMirrorHidePluginOverlays;
		const bool desktopMirrorOverlayHideActive =
			m_VR->m_DesktopMirrorHidePluginOverlays && m_VR->m_DesktopMirrorEnabled;
		const bool singlePassDesktopMirrorPluginOverlays = false;
		const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
		if (queueMode == 0 &&
			!suppressDesktopMirrorPluginOverlays &&
			(info.entity_index == -1 || (info.entity_index > 0 && info.entity_index <= 2048)))
		{
			m_VR->DrawItemModelLabel(info.entity_index, modelName, info.origin, entity, className);
		}
		// Scope RTT pass: optionally hide the local player model so scoped view isn't blocked by your own head/body.
		if (m_VR->m_ScopeRenderingPass && m_VR->m_ScopeHideLocalPlayerModelInScope && isPlayerClass && m_Game->m_EngineClient)
		{
			const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
			if (info.entity_index == localPlayerIndex)
				return;
		}


// --- Multicore viewmodel stabilization (first-person viewmodel ghosting fix) ---
// In queued rendering (mat_queue_mode!=0), viewmodels are frequently submitted with custom bone matrices.
// In that case, overriding ModelRenderInfo_t.origin/angles does NOT move the model (it stays "head-locked").
// So we apply a rigid delta to the bone matrices for this draw call, based on our controller-anchored target.
if (m_VR->m_IsVREnabled && queueMode == 2 && (m_VR->m_QueuedViewmodelStabilize || m_VR->m_ViewmodelDisableMoveBob))
{
	const bool isViewmodelClass = className &&
		(std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
	const bool isArmsOrHandsModel =
		(modelName.find("models/weapons/arms/") != std::string::npos) ||
		(modelName.find("/arms/") != std::string::npos) ||
		(modelName.find("v_arms") != std::string::npos) ||
		(modelName.find("models/weapons/hands/") != std::string::npos) ||
		(modelName.find("/hands/") != std::string::npos) ||
		(modelName.find("v_hands") != std::string::npos);
	const bool isViewmodelModel =
		(modelName.find("models/weapons/v_") != std::string::npos) ||
		(modelName.find("/v_models/") != std::string::npos) ||
		(modelName.find("models/v_models/") != std::string::npos) ||

		// L4D2 melee viewmodels often live under models/weapons/melee/...
		(modelName.find("models/weapons/melee/v_") != std::string::npos) ||
		(modelName.find("models/weapons/melee/") != std::string::npos && modelName.find("/v_") != std::string::npos) ||
		(modelName.find("/melee/v_") != std::string::npos) ||

		// Arms/hands are frequently separate models from the gun.
		isArmsOrHandsModel;


	if (isViewmodelClass || isViewmodelModel)
	{
		struct RenderSnapshotTLSGuard
		{
			bool prev = false;
			RenderSnapshotTLSGuard()
			{
				prev = VR::t_UseRenderFrameSnapshot;
				VR::t_UseRenderFrameSnapshot = true;
			}
			~RenderSnapshotTLSGuard()
			{
				VR::t_UseRenderFrameSnapshot = prev;
			}
		} tlsGuard;

		const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos();
		const QAngle targetAngles = m_VR->GetRecommendedViewmodelAbsAngle();

		// Always override origin/angles for lighting/etc (even if bones are used).
		drawInfo = info;
		drawInfo.origin = targetOrigin;
		drawInfo.angles = targetAngles;
		pDrawInfo = &drawInfo;

		bool appliedBoneDelta = false;
		int numBones = 0;

		if (pCustomBoneToWorld)
		{
			if (vr_vm_stabilize::TryGetNumBonesFromDrawState(state, numBones) && numBones > 0)
			{
				uint32_t seqEven = m_VR->m_RenderFrameSeq.load(std::memory_order_acquire);
				seqEven &= ~1u;
				if (seqEven == 0)
					seqEven = 2;

				vr_vm_stabilize::Mat3x4* bonesCopy = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
				if (bonesCopy)
				{
					memcpy(bonesCopy, pCustomBoneToWorld, (size_t)numBones * sizeof(vr_vm_stabilize::Mat3x4));

					// NOTE:
					// pCustomBoneToWorld is already in WORLD space. However, bone[0] is NOT guaranteed
					// to be at the entity origin (studio root can have a built-in offset). Using bone[0]
					// as the reference will mis-anchor the whole model (often looks like it's still HMD-bound).
					//
					// Correct approach: treat the bones as (EntityToWorld * BoneLocal). Recover BoneLocal
					// via inverse(EntityToWorld), then re-apply with TargetEntityToWorld.
					vr_vm_stabilize::Mat3x4 origEntity{};
					vr_vm_stabilize::BuildFromOrgAngles(info.origin, info.angles, origEntity);
					vr_vm_stabilize::Mat3x4 origInv{};
					vr_vm_stabilize::InvertTR(origEntity, origInv);
					vr_vm_stabilize::Mat3x4 targetEntity{};
					vr_vm_stabilize::BuildFromOrgAngles(targetOrigin, targetAngles, targetEntity);
					vr_vm_stabilize::Mat3x4 delta{};
					vr_vm_stabilize::Mul(targetEntity, origInv, delta);

					bool splitApplied = false;
					if (m_VR->m_SplitArmsToControllers && isArmsOrHandsModel && numBones > 8 && !m_VR->m_MouseModeEnabled)
					{
						const Vector leftCtrlPos = m_VR->GetLeftControllerAbsPos();
						const QAngle leftCtrlAng = m_VR->GetLeftControllerAbsAngle();

						Vector leftForward{}, leftRight{}, leftUp{};
						QAngle::AngleVectors(leftCtrlAng, &leftForward, &leftRight, &leftUp);

						leftForward = VectorRotate(leftForward, leftRight, -45.0f);
						leftUp = VectorRotate(leftUp, leftRight, -45.0f);

						leftForward = VectorRotate(leftForward, leftUp, m_VR->m_ViewmodelAngOffset.y);
						leftRight = VectorRotate(leftRight, leftUp, m_VR->m_ViewmodelAngOffset.y);
						leftForward = VectorRotate(leftForward, leftRight, m_VR->m_ViewmodelAngOffset.x);
						leftUp = VectorRotate(leftUp, leftRight, m_VR->m_ViewmodelAngOffset.x);
						leftRight = VectorRotate(leftRight, leftForward, m_VR->m_ViewmodelAngOffset.z);
						leftUp = VectorRotate(leftUp, leftForward, m_VR->m_ViewmodelAngOffset.z);

						Vector leftVmPos = leftCtrlPos
							- (leftForward * m_VR->m_ViewmodelPosOffset.x)
							- (leftRight * m_VR->m_ViewmodelPosOffset.y)
							- (leftUp * m_VR->m_ViewmodelPosOffset.z);

						QAngle leftVmAng{};
						QAngle::VectorAngles(leftForward, leftUp, leftVmAng);

						vr_vm_stabilize::Mat3x4 leftTargetEntity{};
						vr_vm_stabilize::BuildFromOrgAngles(leftVmPos, leftVmAng, leftTargetEntity);

						vr_vm_stabilize::Mat3x4 leftDelta{};
						vr_vm_stabilize::Mul(leftTargetEntity, origInv, leftDelta);

						std::vector<float> localY((size_t)numBones, 0.0f);
						Vector posSum{ 0.0f, 0.0f, 0.0f };
						Vector negSum{ 0.0f, 0.0f, 0.0f };
						int posCount = 0;
						int negCount = 0;
						float minY = 1e9f;
						float maxY = -1e9f;

						const auto* srcBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
						for (int i = 0; i < numBones; ++i)
						{
							vr_vm_stabilize::Mat3x4 localBone{};
							vr_vm_stabilize::Mul(origInv, srcBones[i], localBone);
							const float y = localBone.m[1][3];
							localY[(size_t)i] = y;
							minY = std::min(minY, y);
							maxY = std::max(maxY, y);

							const Vector worldPos = vr_vm_stabilize::GetOrigin(srcBones[i]);
							if (y > 1.0f)
							{
								posSum += worldPos;
								++posCount;
							}
							else if (y < -1.0f)
							{
								negSum += worldPos;
								++negCount;
							}
						}

						if (posCount > 0 && negCount > 0 && (maxY - minY) > 4.0f)
						{
							const Vector rightCtrlPos = m_VR->GetRightControllerAbsPos();
							const Vector posAvg = posSum / (float)posCount;
							const Vector negAvg = negSum / (float)negCount;
							const bool positiveYIsRight = (posAvg - rightCtrlPos).LengthSqr() <= (negAvg - rightCtrlPos).LengthSqr();
							const float deadZone = std::max(1.0f, (maxY - minY) * 0.08f);

							for (int i = 0; i < numBones; ++i)
							{
								const float y = localY[(size_t)i];
								const bool isCenter = std::fabs(y) <= deadZone;
								const bool useRightDelta = isCenter || ((y > 0.0f) == positiveYIsRight);
								vr_vm_stabilize::Mat3x4 tmp{};
								vr_vm_stabilize::Mul(useRightDelta ? delta : leftDelta, bonesCopy[i], tmp);
								bonesCopy[i] = tmp;
							}
							splitApplied = true;
						}
					}

					if (!splitApplied)
						vr_vm_stabilize::ApplyDelta(delta, bonesCopy, numBones);

					pBonesToWorldFinal = bonesCopy;
					appliedBoneDelta = true;
				}
			}
		}

		if (m_VR->m_QueuedViewmodelStabilizeDebugLog)
		{
			static thread_local std::chrono::steady_clock::time_point s_last{};
			if (!ShouldThrottleLog(s_last, m_VR->m_QueuedViewmodelStabilizeDebugLogHz))
			{
				const uint32_t seq = m_VR->m_RenderFrameSeq.load(std::memory_order_relaxed);
				const uint32_t tid = (uint32_t)GetCurrentThreadId();
										Vector root0 = info.origin;
										Vector root1 = targetOrigin;
										if (pCustomBoneToWorld)
										{
											vr_vm_stabilize::Mat3x4 r0{};
											if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
												root0 = vr_vm_stabilize::GetOrigin(r0);
										}
										if (appliedBoneDelta && pBonesToWorldFinal)
										{
											root1 = vr_vm_stabilize::GetOrigin(reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal)[0]);
										}

										const Vector eyeO = m_VR->m_HmdPosAbs;
										const Vector rcO = m_VR->GetRightControllerAbsPos();
										const float dTgtRc = (targetOrigin - rcO).Length();
                                        const Vector entDelta = targetOrigin - info.origin;
                                        Vector bone0Off(0.0f, 0.0f, 0.0f);
                                        if (pCustomBoneToWorld)
                                        {
                                            vr_vm_stabilize::Mat3x4 r0{};
                                            if (vr_vm_stabilize::SafeRead(pCustomBoneToWorld, r0))
                                                bone0Off = vr_vm_stabilize::GetOrigin(r0) - info.origin;
                                        }

										Game::logMsg(
										"[VR][VM][draw] tid=%u qmode=%d seq=%u ent=%d model=\"%s\" customBones=%d bones=%d applied=%d slot=%u root0=(%.2f %.2f %.2f) root1=(%.2f %.2f %.2f) eyeO=(%.2f %.2f %.2f) rcO=(%.2f %.2f %.2f) dTgtRc=%.2f entD=(%.2f %.2f %.2f) bone0Off=(%.2f %.2f %.2f) origO=(%.2f %.2f %.2f) origA=(%.2f %.2f %.2f) tgtO=(%.2f %.2f %.2f) tgtA=(%.2f %.2f %.2f)"
										,
										tid, queueMode, seq, info.entity_index, modelName.c_str(),
										(pCustomBoneToWorld != nullptr) ? 1 : 0,
										numBones,
										appliedBoneDelta ? 1 : 0,
										(uint32_t)((seq >> 1) % 64),
										root0.x, root0.y, root0.z,
										root1.x, root1.y, root1.z,
										eyeO.x, eyeO.y, eyeO.z,
										rcO.x, rcO.y, rcO.z,
										dTgtRc,
                                        entDelta.x, entDelta.y, entDelta.z,
                                        bone0Off.x, bone0Off.y, bone0Off.z,
										info.origin.x, info.origin.y, info.origin.z,
										info.angles.x, info.angles.y, info.angles.z,
										targetOrigin.x, targetOrigin.y, targetOrigin.z,
										targetAngles.x, targetAngles.y, targetAngles.z);
			}
		}
	}
}

		const VR::SpecialInfectedType entityInfectedType =
			entity ? m_VR->GetSpecialInfectedType(entity) : VR::SpecialInfectedType::None;
		const VR::SpecialInfectedType modelInfectedType = m_VR->GetSpecialInfectedTypeFromModel(modelName);
		const bool useWitchModelFallback =
			modelInfectedType == VR::SpecialInfectedType::Witch &&
			entityInfectedType == VR::SpecialInfectedType::None;

		if (!suppressDesktopMirrorPluginOverlays && !desktopMirrorOverlayHideActive && useWitchModelFallback)
		{
			if (m_VR->m_SpecialInfectedArrowDebugLog && m_VR->m_SpecialInfectedArrowDebugLogHz > 0.0f)
			{
				static std::unordered_map<int, std::chrono::steady_clock::time_point> s_lastWitchModelDebugLog;
				const int debugKey = info.entity_index > 0 ? info.entity_index : -1;
				bool doDebugLog = true;
				auto& last = s_lastWitchModelDebugLog[debugKey];
				const auto now = std::chrono::steady_clock::now();
				if (last.time_since_epoch().count() != 0)
				{
					const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedArrowDebugLogHz);
					const float elapsed = std::chrono::duration<float>(now - last).count();
					if (elapsed >= 0.0f && elapsed < minInterval)
						doDebugLog = false;
				}
				if (doDebugLog)
				{
					last = now;
					Game::logMsg(
						"[VR][SIArrow][model] idx=%d class=%s model=\"%s\" type=%d origin=(%.1f %.1f %.1f)",
						info.entity_index,
						(className && *className) ? className : "<null>",
						modelName.c_str(),
						static_cast<int>(modelInfectedType),
						info.origin.x,
						info.origin.y,
						info.origin.z);
				}
			}

			m_VR->RefreshSpecialInfectedPreWarning(info.origin, modelInfectedType, info.entity_index, false);

			bool doOverlay = true;
			if (!singlePassDesktopMirrorPluginOverlays && info.entity_index > 0 && m_VR->m_SpecialInfectedOverlayMaxHz > 0.0f)
			{
				auto& last = m_VR->m_LastSpecialInfectedOverlayTime[info.entity_index];
				const auto now = std::chrono::steady_clock::now();
				if (last.time_since_epoch().count() != 0)
				{
					const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedOverlayMaxHz);
					const float elapsed = std::chrono::duration<float>(now - last).count();
					if (elapsed >= 0.0f && elapsed < minInterval)
						doOverlay = false;
				}
				if (doOverlay)
					last = now;
			}

			if (doOverlay)
			{
				if (m_VR->m_RearMirrorEnabled && m_VR->m_RearMirrorShowOnlyOnSpecialWarning
					&& m_VR->m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
				{
					Vector to = info.origin - m_VR->m_HmdPosAbs;
					to.z = 0.0f;
					const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
					if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
					{
						Vector fwd = m_VR->m_HmdForward;
						fwd.z = 0.0f;
						if (VectorNormalize(fwd) == 0.0f)
							fwd = { 1.0f, 0.0f, 0.0f };
						VectorNormalize(to);
						if (DotProduct(to, fwd) < 0.0f)
							m_VR->NotifyRearMirrorSpecialWarning();
						m_VR->m_RearMirrorSawSpecialThisPass = true;
					}
				}

				m_VR->DrawSpecialInfectedArrow(info.origin, modelInfectedType);
			}
		}

		if (!suppressDesktopMirrorPluginOverlays && !desktopMirrorOverlayHideActive && entity && entityInfectedType != VR::SpecialInfectedType::None)
		{
			if (m_VR->IsEntityAlive(entity))
			{
				// 1) 高优先级：自瞄/目标刷新不要被 Overlay 节流影响（否则锁定会飘）
				// RefreshSpecialInfectedPreWarning 内部会用到 Trace 缓存（TraceMaxHz），所以这里高频调用不会把 CPU 打爆。
				m_VR->RefreshSpecialInfectedPreWarning(info.origin, entityInfectedType, info.entity_index, isPlayerClass);

				// Rear mirror pop-up: if enabled, show the mirror briefly when a special infected is behind you
				// within the configured warning distance. This detection runs on the main render pass so the
				// mirror can wake up without relying on the mirror RTT pass.
				if (m_VR->m_RearMirrorEnabled && m_VR->m_RearMirrorShowOnlyOnSpecialWarning
					&& m_VR->m_RearMirrorSpecialShowHoldSeconds > 0.0f && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
				{
					Vector to = info.origin - m_VR->m_HmdPosAbs;
					to.z = 0.0f;
					const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
					if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
					{
						Vector fwd = m_VR->m_HmdForward;
						fwd.z = 0.0f;
						if (VectorNormalize(fwd) == 0.0f)
							fwd = { 1.0f, 0.0f, 0.0f };
						VectorNormalize(to);
						// Behind = more likely you want the rear mirror.
						if (DotProduct(to, fwd) < 0.0f)
							m_VR->NotifyRearMirrorSpecialWarning();
					}
				}

				// 2) 低优先级：视觉 Overlay（箭头/盲区提示）继续按实体节流，避免 dDrawModelExecute 多次调用导致尖峰
				bool doOverlay = true;
				if (!singlePassDesktopMirrorPluginOverlays && info.entity_index > 0 && m_VR->m_SpecialInfectedOverlayMaxHz > 0.0f)
				{
					auto& last = m_VR->m_LastSpecialInfectedOverlayTime[info.entity_index];
					const auto now = std::chrono::steady_clock::now();
					if (last.time_since_epoch().count() != 0)
					{
						const float minInterval = 1.0f / std::max(1.0f, m_VR->m_SpecialInfectedOverlayMaxHz);
						const float elapsed = std::chrono::duration<float>(now - last).count();
						if (elapsed < minInterval)
							doOverlay = false;
					}
					if (doOverlay)
						last = now;
				}

				if (doOverlay)
				{
					// Rear-mirror hint: if this special-infected arrow is being rendered during the rear-mirror RTT pass
					// and within the configured distance, enlarge the mirror overlay.
					if (m_VR->m_RearMirrorRenderingPass && m_VR->m_RearMirrorSpecialWarningDistance > 0.0f)
					{
						Vector to = info.origin - m_VR->m_HmdPosAbs;
						to.z = 0.0f;
						const float maxD = m_VR->m_RearMirrorSpecialWarningDistance;
						if (!to.IsZero() && to.LengthSqr() <= (maxD * maxD))
							m_VR->m_RearMirrorSawSpecialThisPass = true;
					}
					if (entityInfectedType != VR::SpecialInfectedType::Tank
						&& entityInfectedType != VR::SpecialInfectedType::Witch
						&& entityInfectedType != VR::SpecialInfectedType::Charger)
					{
						m_VR->RefreshSpecialInfectedBlindSpotWarning(info.origin);
					}
					m_VR->DrawSpecialInfectedArrow(info.origin, entityInfectedType);
				}
			}
		}
	}

	if (info.pModel && hideArms && !m_Game->m_CachedArmsModel)
	{
		if (modelName.find("/arms/") != std::string::npos)
		{
			m_Game->m_ArmsMaterial = m_Game->m_MaterialSystem->FindMaterial(modelName.c_str(), "Model textures");
			m_Game->m_ArmsModel = info.pModel;
			m_Game->m_CachedArmsModel = true;
		}
	}

	if (info.pModel && info.pModel == m_Game->m_ArmsModel && hideArms)
	{
		m_Game->m_ArmsMaterial->SetMaterialVarFlag(MATERIAL_VAR_NO_DRAW, true);
		m_Game->m_ModelRender->ForcedMaterialOverride(m_Game->m_ArmsMaterial);
		hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, pBonesToWorldFinal);
		m_Game->m_ModelRender->ForcedMaterialOverride(NULL);
		return;
	}

	hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, pBonesToWorldFinal);
}

// Returns true if the engine RT being pushed looks like the HUD/VGUI render target.
// This is a heuristic (names + dimensions) to avoid hijacking other offscreen passes.
static bool IsHudRenderTarget(ITexture* texture, ITexture* hudTexture)
{
    if (!texture)
        return false;

    const char* name = texture->GetName();
    if (name && *name)
    {
        auto ciFind = [](const char* haystack, const char* needle) -> bool
            {
                const size_t nLen = strlen(needle);
                for (const char* p = haystack; *p; ++p)
                {
                    if (_strnicmp(p, needle, nLen) == 0)
                        return true;
                }
                return false;
            };

        // Exclude obvious non-HUD targets
        if (ciFind(name, "backbuffer") || ciFind(name, "left") || ciFind(name, "right") ||
            ciFind(name, "blank") || ciFind(name, "scope") || ciFind(name, "rearmirror"))
            return false;

        if (ciFind(name, "vgui") || ciFind(name, "hud"))
            return true;
    }

    // Fallback: match the HUD texture size
    if (hudTexture)
    {
        const int hudW = hudTexture->GetMappingWidth();
        const int hudH = hudTexture->GetMappingHeight();
        if (hudW > 0 && hudH > 0)
        {
            if (texture->GetMappingWidth() == hudW && texture->GetMappingHeight() == hudH)
                return true;
        }
    }

    return false;
}

void Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (m_VR->m_RenderPipelineDebugLog)
    {
        static thread_local std::chrono::steady_clock::time_point s_lastPushRtLog{};
        if (!ShouldThrottleLog(s_lastPushRtLog, m_VR->m_RenderPipelineDebugLogHz))
        {
            int texMapW = 0;
            int texMapH = 0;
            int texActualW = 0;
            int texActualH = 0;
            DebugTextureFullSize(pTexture, texMapW, texMapH, texActualW, texActualH);

            ITexture* hudTexture = nullptr;
            {
                std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
                hudTexture = m_VR->m_HUDTexture;
            }
            int hudMapW = 0;
            int hudMapH = 0;
            int hudActualW = 0;
            int hudActualH = 0;
            DebugTextureFullSize(hudTexture, hudMapW, hudMapH, hudActualW, hudActualH);

            IMatRenderContext* ctx = m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
            int windowW = 0;
            int windowH = 0;
            int backBufferW = 0;
            int backBufferH = 0;
            int clientW = 0;
            int clientH = 0;
            int curVpX = 0;
            int curVpY = 0;
            int curVpW = 0;
            int curVpH = 0;
            DebugRenderContextWindowSize(ctx, windowW, windowH);
            DebugBackBufferDimensions(m_Game ? m_Game->m_MaterialSystem : nullptr, backBufferW, backBufferH);
            DebugClientRectSize(clientW, clientH);
            DebugGetViewport(ctx, curVpX, curVpY, curVpW, curVpH);

            Game::logMsg("[VR][DesktopHUD][PushRT] tid=%lu q=%d step=%d pushed=%d hudPainted=%d suppress=%d win=%dx%d client=%dx%d bb=%dx%d curVp=%d,%d %dx%d tex=%s(map=%dx%d actual=%dx%d) reqVp=%d,%d %dx%d hudTex=%s(map=%dx%d actual=%dx%d)",
                GetCurrentThreadId(), queueMode,
                static_cast<int>(m_HUDStep), m_PushedHud ? 1 : 0,
                m_VR->m_HudPaintedThisFrame.load(std::memory_order_acquire) ? 1 : 0,
                m_VR->m_SuppressHudCapture ? 1 : 0,
                windowW, windowH, clientW, clientH, backBufferW, backBufferH, curVpX, curVpY, curVpW, curVpH,
                DebugTextureName(pTexture), texMapW, texMapH, texActualW, texActualH,
                nViewX, nViewY, nViewW, nViewH,
                DebugTextureName(hudTexture), hudMapW, hudMapH, hudActualW, hudActualH);
        }
    }

    // Extra offscreen passes (scope/rear-mirror RTT) must not hijack HUD capture
    if (m_VR->m_SuppressHudCapture)
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);

    if (queueMode != 0)
    {
        // Queued/multicore path: the Pop->IsSplitScreen->PrePush->Push sequence
        // isn't reliable, so never attempt RT hijack here.
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Single-threaded path (mat_queue_mode 0): use state machine to detect HUD push.
    bool overrideHudRT = (m_HUDStep == HUDPushStep::ReadyToOverride) &&
        !m_VR->m_HudPaintedThisFrame.load(std::memory_order_relaxed);

    if (overrideHudRT)
    {
        std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
        if (!m_VR->m_HUDTexture || !IsHudRenderTarget(pTexture, m_VR->m_HUDTexture))
            overrideHudRT = false;
    }

    if (!overrideHudRT)
    {
        m_PushedHud = false;
        m_HUDStep = HUDPushStep::None;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    ITexture* hudTexture = nullptr;
    {
        std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
        hudTexture = m_VR->m_HUDTexture;
    }

    if (!hudTexture)
    {
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    IMatRenderContext* renderContext = m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
    if (!renderContext)
    {
        m_VR->HandleMissingRenderContext("Hooks::dPushRenderTargetAndViewport");
        m_HUDStep = HUDPushStep::None;
        m_PushedHud = false;
        return hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    }

    // Clear depth/stencil first, then push RT and clear color to transparent.
    renderContext->ClearBuffers(false, true, true);
    hkPushRenderTargetAndViewport.fOriginal(ecx, hudTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
    renderContext->OverrideAlphaWriteEnable(true, true);
    renderContext->ClearColor4ub(0, 0, 0, 0);
    renderContext->ClearBuffers(true, false);

    if (m_VR->m_RenderPipelineDebugLog)
    {
        int vpX = 0;
        int vpY = 0;
        int vpW = 0;
        int vpH = 0;
        DebugGetViewport(renderContext, vpX, vpY, vpW, vpH);
        int hudMapW = 0;
        int hudMapH = 0;
        int hudActualW = 0;
        int hudActualH = 0;
        DebugTextureFullSize(hudTexture, hudMapW, hudMapH, hudActualW, hudActualH);
        Game::logMsg("[VR][DesktopHUD][PushOverride] tid=%lu requestedVp=%d,%d %dx%d actualVp=%d,%d %dx%d hudTex=%s(map=%dx%d actual=%dx%d)",
            GetCurrentThreadId(), nViewX, nViewY, nViewW, nViewH, vpX, vpY, vpW, vpH,
            DebugTextureName(hudTexture), hudMapW, hudMapH, hudActualW, hudActualH);
    }

    m_PushedHud = true;
    m_HUDStep = HUDPushStep::None;
}

void Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkPopRenderTargetAndViewport.fOriginal(ecx);

    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    m_HUDStep = (queueMode == 0) ? HUDPushStep::AfterPop : HUDPushStep::None;

    if (m_PushedHud)
    {
        IMatRenderContext* renderContext = m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
        if (renderContext)
        {
            renderContext->OverrideAlphaWriteEnable(false, true);
            renderContext->ClearColor4ub(0, 0, 0, 255);
        }
    }

    hkPopRenderTargetAndViewport.fOriginal(ecx);
    m_PushedHud = false;
}

void Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    if (!m_VR->m_CreatedVRTextures.load(std::memory_order_acquire))
        return hkVgui_Paint.fOriginal(ecx, mode);

    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    const bool isPaused = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsPaused();
    const bool cursorVisible = (m_Game && m_Game->m_VguiSurface) ? m_Game->m_VguiSurface->IsCursorVisible() : false;
    const bool focusedInGameVgui = inGame && (isPaused || cursorVisible);
    const bool gameplayHudRequested = inGame && m_VR->IsGameplayHudRequested();

    auto BuildFullHudPaintMode = [&](int paintMode)
        {
            int fullHudMode = PAINT_UIPANELS | PAINT_INGAMEPANELS;
            if (cursorVisible)
                fullHudMode |= PAINT_CURSOR;
            return paintMode | fullHudMode;
        };

    // Extra offscreen passes such as scope / rear mirror should not recurse through
    // the VGUI capture path. The selected desktop-mirror clean pass is the one
    // exception: when the gameplay HUD is requested, let Source paint VGUI directly
    // into desktopMirrorClean0 so spectators see the same HUD state without placing
    // the HUD inside the VR eye textures.
    if (m_VR->m_SuppressHudCapture)
    {
        if (!inGame)
            return hkVgui_Paint.fOriginal(ecx, mode);

        if (m_VR->m_DesktopMirrorCleanRenderingPass && (focusedInGameVgui || gameplayHudRequested))
            return hkVgui_Paint.fOriginal(ecx, BuildFullHudPaintMode(mode));

        return;
    }

    auto IsPaintingToNativeBackBuffer = [&]() -> bool
        {
            IMatRenderContext* ctx = m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
            if (!ctx)
                return false;

            // In Source's material system a null render target represents the current backbuffer.
            // Only allow native desktop VGUI in that case. If this is an eye RT, HUD RT, scope RT,
            // mirror RT, water RT, etc., capture to m_HUDTexture only and do not draw into that target.
            return ctx->GetRenderTarget() == nullptr;
        };

    auto PaintToHudOnce = [&](int paintMode)
        {
            bool expected = false;
            if (!m_VR->m_HudPaintedThisFrame.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return;

            IMatRenderContext* ctx = m_Game && m_Game->m_MaterialSystem ? m_Game->m_MaterialSystem->GetRenderContext() : nullptr;
            if (!ctx)
            {
                m_VR->HandleMissingRenderContext("Hooks::dVGui_Paint");
                return;
            }

            ITexture* hudTexture = nullptr;
            {
                std::lock_guard<TextureStateMutex> lock(m_VR->m_TextureMutex);
                hudTexture = m_VR->m_HUDTexture;
            }
            if (!hudTexture)
                return;

            ITexture* prevTarget = ctx->GetRenderTarget();
            int hudMapW = hudTexture->GetMappingWidth();
            int hudMapH = hudTexture->GetMappingHeight();
            if (hudMapW <= 0)
                hudMapW = 1;
            if (hudMapH <= 0)
                hudMapH = 1;

            int oldX = 0;
            int oldY = 0;
            int oldW = 0;
            int oldH = 0;
            const bool canRestoreViewport = hkGetViewport.fOriginal && hkViewport.fOriginal &&
                DebugGetViewport(ctx, oldX, oldY, oldW, oldH);

            ctx->SetRenderTarget(hudTexture);
            if (hkViewport.fOriginal)
                hkViewport.fOriginal(ctx, 0, 0, hudMapW, hudMapH);

            ctx->OverrideAlphaWriteEnable(true, true);
            ctx->ClearColor4ub(0, 0, 0, isPaused ? 255 : 0);
            ctx->ClearBuffers(true, false, false);

            hkVgui_Paint.fOriginal(ecx, paintMode);
            m_VR->DrawKillIndicators(ctx, hudTexture);

            ctx->OverrideAlphaWriteEnable(false, true);
            ctx->SetRenderTarget(prevTarget);
            if (canRestoreViewport)
                hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);

            m_VR->m_RenderedHud.store(true, std::memory_order_release);
        };

    if (inGame)
    {
        // In VR gameplay, capture is allowed only while the HUD is actually meant to be visible:
        // focused UI, HudAlwaysVisible=true, or the off-hand lift request. Every other gameplay
        // state is a hard capture stop. When the current target is the native backbuffer, paint
        // there too so desktop spectators match the requested HUD state.
        if (focusedInGameVgui || gameplayHudRequested)
        {
            const int fullHudMode = BuildFullHudPaintMode(mode);
            PaintToHudOnce(fullHudMode);

            // HudAlwaysVisible/lift/menu must also be visible on the desktop when Source is
            // currently painting the native backbuffer. This does not weaken the capture-stop
            // rule: the branch is reached only while the HUD is explicitly requested.
            if (IsPaintingToNativeBackBuffer())
                hkVgui_Paint.fOriginal(ecx, fullHudMode);
        }
        else
        {
            m_VR->m_RenderedHud.store(false, std::memory_order_release);
            m_VR->m_QueuedHudFreshUntil = {};
        }
        return;
    }

    // Main menu / loading screens are not VR gameplay; keep normal desktop VGUI.
    hkVgui_Paint.fOriginal(ecx, mode);
}

//
int Hooks::dIsSplitScreen()
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterPop)
            m_HUDStep = HUDPushStep::AfterIsSplitScreen;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkIsSplitScreen.fOriginal();
}

DWORD* Hooks::dPrePushRenderTarget(void* ecx, void* edx, int a2)
{
    const int queueMode = (m_Game != nullptr) ? m_Game->GetMatQueueMode() : 0;
    if (queueMode == 0)
    {
        if (m_HUDStep == HUDPushStep::AfterIsSplitScreen)
            m_HUDStep = HUDPushStep::ReadyToOverride;
        else
            m_HUDStep = HUDPushStep::None;
    }
    else
    {
        m_HUDStep = HUDPushStep::None;
    }

    return hkPrePushRenderTarget.fOriginal(ecx, a2);
}

void Hooks::dSayText(void* msgData)
{
	TryLogHudUserMessagePayload("SayText", msgData);
	hkSayText.fOriginal(msgData);
}

void Hooks::dSayText2(void* msgData)
{
	TryLogHudUserMessagePayload("SayText2", msgData);
	hkSayText2.fOriginal(msgData);
}

void Hooks::dTextMsg(void* msgData)
{
	TryLogHudUserMessagePayload("TextMsg", msgData);
	hkTextMsg.fOriginal(msgData);
}

void Hooks::dConVarSetValueString(void* ecx, void* edx, const char* value)
{
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, value);
    TraceTrackedConVarWrite(ecx, value, "IConVar::SetValue(string)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarSetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, buffer);
    TraceTrackedConVarWrite(ecx, buffer, "IConVar::SetValue(float)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarSetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    const bool blocked = ShouldBlockLockedConVarWrite(ecx, buffer);
    TraceTrackedConVarWrite(ecx, buffer, "IConVar::SetValue(int)", _ReturnAddress(), true, blocked);
    if (blocked)
        return;

    hkConVarSetValueInt.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueString(void* ecx, void* edx, const char* value)
{
    TraceTrackedConVarWrite(ecx, value, "ConVar::SetValue(string)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::SetValue(float)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarPrimarySetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::SetValue(int)", _ReturnAddress(), false, false);
    hkConVarPrimarySetValueInt.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueString(void* ecx, void* edx, const char* value)
{
    TraceTrackedConVarWrite(ecx, value, "ConVar::InternalSetValue(string)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueString.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueFloat(void* ecx, void* edx, float value)
{
    char buffer[64] = {};
    sprintf_s(buffer, "%.9g", static_cast<double>(value));
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::InternalSetValue(float)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueFloat.fOriginal(ecx, value);
}

void Hooks::dConVarInternalSetValueInt(void* ecx, void* edx, int value)
{
    char buffer[32] = {};
    sprintf_s(buffer, "%d", value);
    TraceTrackedConVarWrite(ecx, buffer, "ConVar::InternalSetValue(int)", _ReturnAddress(), false, false);
    hkConVarInternalSetValueInt.fOriginal(ecx, value);
}
