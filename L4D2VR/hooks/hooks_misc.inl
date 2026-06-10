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

    inline bool TryGetStudioHdrFromDrawState(void* drawState, const uint8_t*& outStudioHdr)
    {
        outStudioHdr = nullptr;
        if (!drawState)
            return false;

        void* studioHdr = nullptr;
        if (!SafeRead(drawState, studioHdr) || !studioHdr)
            return false;

        outStudioHdr = reinterpret_cast<const uint8_t*>(studioHdr);
        return true;
    }

    inline bool TryGetBoneTableLayout(void* drawState, int& outNumBones, int& outBoneIndex, int& outNumBonesOffset)
    {
        outNumBones = 0;
        outBoneIndex = 0;
        outNumBonesOffset = 0;

        const uint8_t* studioHdr = nullptr;
        if (!TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        int studioLength = 0;
        SafeRead(studioHdr + 0x4C, studioLength);

        static const int kNumBoneOffsets[] = { 0x9C, 0xA0, 0x98, 0x94, 0xA4, 0xA8, 0x90, 0x8C, 0xB0 };
        for (int off : kNumBoneOffsets)
        {
            int n = 0;
            int boneIndex = 0;
            if (!SafeRead(studioHdr + off, n) || !SafeRead(studioHdr + off + 4, boneIndex))
                continue;
            if (n <= 0 || n > 512 || boneIndex <= 0 || boneIndex > 0x200000)
                continue;
            if (studioLength > 0 && boneIndex >= studioLength)
                continue;

            outNumBones = n;
            outBoneIndex = boneIndex;
            outNumBonesOffset = off;
            return true;
        }
        return false;
    }

    inline bool TryReadCStringSafe(const char* ptr, std::string& out, size_t maxLen = 96)
    {
        out.clear();
        if (!ptr)
            return false;

        for (size_t i = 0; i < maxLen; ++i)
        {
            char c = '\0';
            if (!SafeRead(ptr + i, c))
                return false;
            if (c == '\0')
                return !out.empty();
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 32 || uc > 126)
                return false;
            out.push_back(c);
        }
        return false;
    }

    inline std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    inline bool TryGetBoneNameAtStride(
        const uint8_t* studioHdr,
        int studioLength,
        int boneIndex,
        int numBones,
        int stride,
        int bone,
        std::string& outName,
        int& outParent)
    {
        outName.clear();
        outParent = -1;
        if (!studioHdr || bone < 0 || bone >= numBones || stride <= 0)
            return false;

        const size_t boneOffset = static_cast<size_t>(boneIndex) +
            (static_cast<size_t>(stride) * static_cast<size_t>(bone));
        if (studioLength > 0 && boneOffset + 8u > static_cast<size_t>(studioLength))
            return false;

        const uint8_t* boneBase = studioHdr + boneOffset;
        int nameOffset = 0;
        int parent = -1;
        if (!SafeRead(boneBase + 0, nameOffset) || !SafeRead(boneBase + 4, parent))
            return false;
        if (nameOffset <= 0)
            return false;
        if (parent < -1 || parent >= numBones)
            return false;

        // mstudiobone_t::sznameindex is relative to the current bone structure.
        // Workshop replacement models can place the string table well beyond 64 KiB.
        // The previous fixed 0x10000 limit rejected valid bone names for most weapons,
        // while the M16 happened to keep a small readable subset close enough to pass.
        const size_t nameAddressOffset = boneOffset + static_cast<size_t>(nameOffset);
        if (studioLength > 0)
        {
            if (nameAddressOffset >= static_cast<size_t>(studioLength))
                return false;
        }
        else if (nameOffset > 0x400000)
        {
            return false;
        }

        std::string name;
        if (!TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), name))
            return false;

        outName = name;
        outParent = parent;
        return true;
    }

    inline bool TryCollectBoneNamesFromDrawState(
        void* drawState,
        std::vector<std::string>& outNames,
        std::vector<int>& outParents,
        int& outNumBones,
        int& outBoneIndex,
        int& outStride,
        int& outNumBonesOffset)
    {
        outNames.clear();
        outParents.clear();
        outNumBones = 0;
        outBoneIndex = 0;
        outStride = 0;
        outNumBonesOffset = 0;

        const uint8_t* studioHdr = nullptr;
        if (!TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;
        if (!TryGetBoneTableLayout(drawState, outNumBones, outBoneIndex, outNumBonesOffset))
            return false;

        int studioLength = 0;
        SafeRead(studioHdr + 0x4C, studioLength);

        static const int kStrideCandidates[] = { 216, 224, 208, 200, 192, 184, 176, 232, 240, 256 };
        int bestStride = 0;
        int bestScore = -1;
        std::vector<std::string> bestNames;
        std::vector<int> bestParents;

        for (int stride : kStrideCandidates)
        {
            std::vector<std::string> names(static_cast<size_t>(outNumBones));
            std::vector<int> parents(static_cast<size_t>(outNumBones), -1);
            int validNames = 0;
            int semanticHits = 0;

            for (int bone = 0; bone < outNumBones; ++bone)
            {
                std::string name;
                int parent = -1;
                if (!TryGetBoneNameAtStride(studioHdr, studioLength, outBoneIndex, outNumBones, stride, bone, name, parent))
                    continue;

                names[static_cast<size_t>(bone)] = name;
                parents[static_cast<size_t>(bone)] = parent;
                ++validNames;

                const std::string lower = ToLowerAscii(name);
                if (lower.find("finger") != std::string::npos ||
                    lower.find("hand") != std::string::npos ||
                    lower.find("wrist") != std::string::npos ||
                    lower.find("clip") != std::string::npos ||
                    lower.find("weapon") != std::string::npos ||
                    lower.find("valvebiped") != std::string::npos ||
                    lower.find("bip01") != std::string::npos)
                {
                    semanticHits += 4;
                }
            }

            const int score = validNames * 2 + semanticHits;
            if (score > bestScore)
            {
                bestScore = score;
                bestStride = stride;
                bestNames.swap(names);
                bestParents.swap(parents);
            }
        }

        if (bestStride <= 0 || bestScore < 6)
            return false;

        outStride = bestStride;
        outNames.swap(bestNames);
        outParents.swap(bestParents);
        return true;
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

    inline bool HooksModelNameIsArmsOrHands(const std::string& modelName)
    {
        return
            (modelName.find("models/weapons/arms/") != std::string::npos) ||
            (modelName.find("/arms/") != std::string::npos) ||
            (modelName.find("v_arms") != std::string::npos) ||
            (modelName.find("models/weapons/hands/") != std::string::npos) ||
            (modelName.find("/hands/") != std::string::npos) ||
            (modelName.find("v_hands") != std::string::npos);
    }

    inline bool HooksModelNameIsViewmodel(const std::string& modelName)
    {
        return
            (modelName.find("models/weapons/v_") != std::string::npos) ||
            (modelName.find("/v_models/") != std::string::npos) ||
            (modelName.find("models/v_models/") != std::string::npos) ||
            (modelName.find("models/weapons/melee/v_") != std::string::npos) ||
            (modelName.find("models/weapons/melee/") != std::string::npos && modelName.find("/v_") != std::string::npos) ||
            (modelName.find("/melee/v_") != std::string::npos) ||
            HooksModelNameIsArmsOrHands(modelName);
    }

    inline void MaybeLogVrHandsViewmodelBoneProbe(
        void* drawState,
        const std::string& modelName,
        int entityIndex,
        const char* className,
        bool hasCustomBones)
    {
        if (!drawState || modelName.empty())
            return;

        static std::mutex s_probeMutex;
        static std::unordered_map<std::string, bool> s_probeLoggedByModel;
        {
            std::lock_guard<std::mutex> lock(s_probeMutex);
            if (s_probeLoggedByModel.find(modelName) != s_probeLoggedByModel.end())
                return;
            s_probeLoggedByModel.emplace(modelName, true);
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        const bool ok = vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);

        Game::logMsg(
            "[VR][Hands][VMProbe] model=\"%s\" ent=%d class=%s customBones=%d ok=%d bones=%d boneIndex=0x%X numBonesOff=0x%X stride=%d",
            modelName.c_str(),
            entityIndex,
            (className && *className) ? className : "<null>",
            hasCustomBones ? 1 : 0,
            ok ? 1 : 0,
            numBones,
            boneIndex,
            numBonesOffset,
            stride);

        if (!ok)
            return;

        std::string chunk;
        int chunkIndex = 0;
        auto flushChunk = [&]()
            {
                if (chunk.empty())
                    return;
                Game::logMsg("[VR][Hands][VMProbe] bones[%d] model=\"%s\" %s", chunkIndex++, modelName.c_str(), chunk.c_str());
                chunk.clear();
            };

        for (int i = 0; i < numBones && i < static_cast<int>(boneNames.size()); ++i)
        {
            if (boneNames[static_cast<size_t>(i)].empty())
                continue;

            char item[256]{};
            std::snprintf(
                item,
                sizeof(item),
                "%d:p%d:%s; ",
                i,
                (i < static_cast<int>(boneParents.size())) ? boneParents[static_cast<size_t>(i)] : -1,
                boneNames[static_cast<size_t>(i)].c_str());

            if (chunk.size() + std::strlen(item) > 850)
                flushChunk();
            chunk += item;
        }
        flushChunk();
    }

    inline bool MuzzleNameEndsWithToken(const std::string& lower, const char* token)
    {
        const size_t len = std::strlen(token);
        if (lower.size() < len)
            return false;
        if (lower.compare(lower.size() - len, len, token) != 0)
            return false;
        if (lower.size() == len)
            return true;
        const char prev = lower[lower.size() - len - 1];
        return prev == '.' || prev == '_' || prev == ':' || prev == '/' || prev == '\\';
    }

    inline int MuzzlePointNameScore(const std::string& lower)
    {
        if (lower.empty())
            return 0;

        if (lower == "attach_muzzle" || MuzzleNameEndsWithToken(lower, "attach_muzzle"))
            return 120;
        if (lower == "muzzle" || MuzzleNameEndsWithToken(lower, "muzzle"))
            return 110;
        if (lower == "muzzlesmoke" || lower == "muzzle_smoke" || lower == "muzzle smoke" ||
            lower.find("muzzlesmoke") != std::string::npos ||
            lower.find("muzzle_smoke") != std::string::npos)
        {
            return 100;
        }
        if (lower == "flash" || MuzzleNameEndsWithToken(lower, "flash"))
            return 90;
        if (lower.find("muzzle") != std::string::npos &&
            lower.find("shell") == std::string::npos &&
            lower.find("eject") == std::string::npos)
        {
            return 70;
        }
        return 0;
    }

    inline Vector MuzzleMatrixColumn(const vr_vm_stabilize::Mat3x4& matrix, int column)
    {
        return Vector(matrix.m[0][column], matrix.m[1][column], matrix.m[2][column]);
    }

    inline bool BuildMuzzleAnglesFromMatrix(
        const ModelRenderInfo_t& drawInfo,
        const vr_vm_stabilize::Mat3x4& matrix,
        QAngle& outAngles)
    {
        Vector referenceForward{};
        QAngle::AngleVectors(drawInfo.angles, &referenceForward, nullptr, nullptr);
        if (!referenceForward.IsZero())
            VectorNormalize(referenceForward);

        Vector bestForward{};
        float bestDot = -FLT_MAX;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis = MuzzleMatrixColumn(matrix, column);
            if (axis.IsZero())
                continue;
            VectorNormalize(axis);

            const Vector candidates[2] = { axis, axis * -1.0f };
            for (const Vector& candidate : candidates)
            {
                float score = 0.0f;
                if (!referenceForward.IsZero())
                    score = DotProduct(candidate, referenceForward);
                else if (column == 0)
                    score = 0.5f;

                if (score > bestDot)
                {
                    bestDot = score;
                    bestForward = candidate;
                }
            }
        }

        if (bestForward.IsZero())
            return false;

        if (!referenceForward.IsZero() && bestDot < 0.8660254f)
            QAngle::AngleVectors(drawInfo.angles, &bestForward, nullptr, nullptr);

        QAngle::VectorAngles(bestForward, outAngles);
        NormalizeAndClampViewAngles(outAngles);
        return std::isfinite(outAngles.x) && std::isfinite(outAngles.y) && std::isfinite(outAngles.z);
    }

    struct MuzzleSmokeAttachmentInfo
    {
        bool parsed = false;
        bool found = false;
        int attachment = -1;
        int localBone = -1;
        int numAttachments = 0;
        int attachmentIndex = 0;
        std::string name;
        vr_vm_stabilize::Mat3x4 local{};
    };

    inline bool TryResolveMuzzleSmokeAttachment(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        int numBones,
        MuzzleSmokeAttachmentInfo& outInfo)
    {
        outInfo = {};
        if (!drawState || modelName.empty() || numBones <= 0)
            return false;

        const std::string key = vr_vm_stabilize::ToLowerAscii(modelName);
        static std::mutex s_mutex;
        static std::unordered_map<std::string, MuzzleSmokeAttachmentInfo> s_cachedByModel;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_cachedByModel.find(key);
            if (it != s_cachedByModel.end())
            {
                outInfo = it->second;
                return outInfo.parsed;
            }
        }

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr))
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numAttachments = 0;
        int attachmentIndex = 0;
        if (!vr_vm_stabilize::SafeRead(studioHdr + 0xF0, numAttachments) ||
            !vr_vm_stabilize::SafeRead(studioHdr + 0xF4, attachmentIndex))
        {
            return false;
        }
        if (numAttachments <= 0 || numAttachments > 256 || attachmentIndex <= 0 || attachmentIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (attachmentIndex >= studioLength ||
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(numAttachments) * 92u > static_cast<size_t>(studioLength)))
        {
            return false;
        }

        outInfo.parsed = true;
        outInfo.numAttachments = numAttachments;
        outInfo.attachmentIndex = attachmentIndex;

        static constexpr int kAttachmentStride = 92; // mstudioattachment_t in L4D2: name, flags, localbone, matrix3x4, unused[8].
        for (int attachment = 0; attachment < numAttachments; ++attachment)
        {
            const size_t attachmentOffset =
                static_cast<size_t>(attachmentIndex) + static_cast<size_t>(attachment) * kAttachmentStride;
            const uint8_t* attachmentBase = studioHdr + attachmentOffset;

            int nameOffset = 0;
            int localBone = -1;
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 0, nameOffset) ||
                !vr_vm_stabilize::SafeRead(attachmentBase + 8, localBone))
            {
                continue;
            }
            if (nameOffset <= 0 || localBone < 0 || localBone >= numBones)
                continue;

            const size_t nameAddressOffset = attachmentOffset + static_cast<size_t>(nameOffset);
            if (studioLength > 0 && nameAddressOffset >= static_cast<size_t>(studioLength))
                continue;

            std::string name;
            if (!vr_vm_stabilize::TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), name))
                continue;

            const int score = MuzzlePointNameScore(vr_vm_stabilize::ToLowerAscii(name));
            if (score <= 0)
                continue;

            vr_vm_stabilize::Mat3x4 local{};
            if (!vr_vm_stabilize::SafeRead(attachmentBase + 12, local))
                continue;

            const int oldScore = MuzzlePointNameScore(vr_vm_stabilize::ToLowerAscii(outInfo.name));
            if (!outInfo.found || score > oldScore)
            {
                outInfo.found = true;
                outInfo.attachment = attachment;
                outInfo.localBone = localBone;
                outInfo.name = name;
                outInfo.local = local;
            }
        }

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_cachedByModel[key] = outInfo;
        }

        if (vr && vr->m_BulletVisualsUseMuzzleSmoke)
        {
            if (outInfo.found)
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] attachment model=%s attachment=%d name=%s localBone=%d attachments=%d table=0x%X",
                    modelName.c_str(),
                    outInfo.attachment,
                    outInfo.name.c_str(),
                    outInfo.localBone,
                    outInfo.numAttachments,
                    outInfo.attachmentIndex);
            }
            else
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] no attachment model=%s attachments=%d table=0x%X",
                    modelName.c_str(),
                    outInfo.numAttachments,
                    outInfo.attachmentIndex);
            }
        }

        return outInfo.parsed;
    }

    inline int ResolveMuzzleSmokeBoneIndex(
        VR* vr,
        void* drawState,
        const std::string& modelName)
    {
        if (!drawState || modelName.empty())
            return -1;

        const std::string key = vr_vm_stabilize::ToLowerAscii(modelName);
        static std::mutex s_mutex;
        static std::unordered_map<std::string, int> s_cachedBoneByModel;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            auto it = s_cachedBoneByModel.find(key);
            if (it != s_cachedBoneByModel.end())
                return it->second;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        const bool ok = vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset);

        int resolved = -1;
        int resolvedScore = 0;
        if (ok)
        {
            for (int bone = 0; bone < numBones && bone < static_cast<int>(boneNames.size()); ++bone)
            {
                const std::string lower = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
                const int score = MuzzlePointNameScore(lower);
                if (score > resolvedScore)
                {
                    resolved = bone;
                    resolvedScore = score;
                }
            }
        }

        if (ok)
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_cachedBoneByModel[key] = resolved;
        }

        if (vr && vr->m_BulletVisualsUseMuzzleSmoke)
        {
            if (resolved >= 0)
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] bone model=%s bone=%d name=%s score=%d bones=%d stride=%d",
                    modelName.c_str(),
                    resolved,
                    (resolved < static_cast<int>(boneNames.size())) ? boneNames[static_cast<size_t>(resolved)].c_str() : "<unknown>",
                    resolvedScore,
                    numBones,
                    stride);
            }
            else
            {
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] not found model=%s ok=%d bones=%d stride=%d",
                    modelName.c_str(),
                    ok ? 1 : 0,
                    numBones,
                    stride);
            }
        }
        return resolved;
    }

    inline void PublishViewmodelMuzzleSmokePose(
        VR* vr,
        const Vector& origin,
        const QAngle& angles)
    {
        if (!vr)
            return;

        uint32_t seq = vr->m_ViewmodelMuzzleSmokePoseSeq.load(std::memory_order_relaxed);
        if (seq & 1u)
            ++seq;
        const uint32_t odd = seq + 1u;
        const uint32_t even = odd + 1u;

        vr->m_ViewmodelMuzzleSmokePoseSeq.store(odd, std::memory_order_release);
        vr->m_ViewmodelMuzzleSmokePosX.store(origin.x, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePosY.store(origin.y, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePosZ.store(origin.z, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngX.store(angles.x, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngY.store(angles.y, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeAngZ.store(angles.z, std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePoseTickMs.store(GetTickCount(), std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokeRenderFrameSeq.store(vr->m_RenderFrameSeq.load(std::memory_order_relaxed), std::memory_order_relaxed);
        vr->m_ViewmodelMuzzleSmokePoseSeq.store(even, std::memory_order_release);
    }

    inline void MaybeCaptureViewmodelMuzzleSmokePose(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& drawInfo,
        const void* pBonesToWorldFinal)
    {
        if (!vr || !vr->m_IsVREnabled || !vr->m_BulletVisualsUseMuzzleSmoke || !pBonesToWorldFinal)
            return;
        if (modelName.empty() || !HooksModelNameIsViewmodel(modelName) || HooksModelNameIsArmsOrHands(modelName))
            return;

        int numBones = 0;
        if (!vr_vm_stabilize::TryGetNumBonesFromDrawState(drawState, numBones) || numBones <= 0)
            return;

        const auto* bones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pBonesToWorldFinal);

        vr_vm_stabilize::Mat3x4 muzzle{};
        const char* poseSource = "bone";
        const int muzzleBone = ResolveMuzzleSmokeBoneIndex(vr, drawState, modelName);
        if (muzzleBone >= 0 && muzzleBone < numBones)
        {
            if (!vr_vm_stabilize::SafeRead(bones + muzzleBone, muzzle))
                return;
            poseSource = "bone";
        }
        else
        {
            MuzzleSmokeAttachmentInfo attachmentInfo{};
            if (!TryResolveMuzzleSmokeAttachment(vr, drawState, modelName, numBones, attachmentInfo) ||
                !attachmentInfo.found ||
                attachmentInfo.localBone < 0 ||
                attachmentInfo.localBone >= numBones)
            {
                return;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(bones + attachmentInfo.localBone, boneWorld))
                return;
            vr_vm_stabilize::Mul(boneWorld, attachmentInfo.local, muzzle);
            poseSource = "attachment";
        }

        Vector origin = vr_vm_stabilize::GetOrigin(muzzle);
        QAngle angles{};
        if (!BuildMuzzleAnglesFromMatrix(drawInfo, muzzle, angles))
            return;
        if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
            !std::isfinite(angles.x) || !std::isfinite(angles.y) || !std::isfinite(angles.z))
        {
            return;
        }

        PublishViewmodelMuzzleSmokePose(vr, origin, angles);

        static std::mutex s_captureLogMutex;
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastCaptureLogByModel;
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(s_captureLogMutex);
            auto& last = s_lastCaptureLogByModel[modelName];
            if (last.time_since_epoch().count() == 0 || now - last > std::chrono::seconds(2))
            {
                last = now;
                Game::logMsg(
                    "[VR][FX][muzzlesmoke] capture source=%s model=%s origin=(%.2f %.2f %.2f) angles=(%.2f %.2f %.2f)",
                    poseSource,
                    modelName.c_str(),
                    origin.x, origin.y, origin.z,
                    angles.x, angles.y, angles.z);
            }
        }
    }

    inline VrHandMatrix4 HooksMat3x4ToVrHandMatrix(const vr_vm_stabilize::Mat3x4& source)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
                VrHandMath::Set(out, row, column, source.m[row][column]);
        }
        return out;
    }

    inline vr_vm_stabilize::Mat3x4 HooksVrHandMatrixToMat3x4(const VrHandMatrix4& source)
    {
        vr_vm_stabilize::Mat3x4 out{};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 4; ++column)
                out.m[row][column] = VrHandMath::Get(source, row, column);
        }
        return out;
    }

    inline VrHandMatrix4 HooksStripVrHandMatrixScale(const VrHandMatrix4& source)
    {
        VrHandMatrix4 out = source;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis(
                VrHandMath::Get(out, 0, column),
                VrHandMath::Get(out, 1, column),
                VrHandMath::Get(out, 2, column));
            const float length = axis.Length();
            if (!(length > 0.000001f))
                continue;
            axis *= (1.0f / length);
            VrHandMath::Set(out, 0, column, axis.x);
            VrHandMath::Set(out, 1, column, axis.y);
            VrHandMath::Set(out, 2, column, axis.z);
        }
        return out;
    }

    inline bool ManualReloadNameContains(const std::string& value, const char* needle)
    {
        return needle && *needle && value.find(needle) != std::string::npos;
    }

    inline bool ManualReloadNameEndsWith(const std::string& value, const char* suffix)
    {
        if (!suffix || !*suffix)
            return false;
        const size_t suffixLength = std::strlen(suffix);
        return value.size() >= suffixLength &&
            value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
    }

    inline bool ManualReloadNameHasLooseMagToken(const std::string& name)
    {
        size_t pos = 0;
        while ((pos = name.find("mag", pos)) != std::string::npos)
        {
            const bool prefixOk =
                pos == 0 ||
                name[pos - 1] == '_' ||
                name[pos - 1] == '.' ||
                name[pos - 1] == '-' ||
                name[pos - 1] == ':';
            const size_t after = pos + 3;
            const bool suffixOk =
                after >= name.size() ||
                name[after] == '_' ||
                name[after] == '.' ||
                name[after] == '-' ||
                std::isdigit(static_cast<unsigned char>(name[after])) != 0;
            if (prefixOk && suffixOk)
                return true;
            pos = after;
        }
        return false;
    }

    inline bool ManualReloadNameIsLegacyValveBipedClip(const std::string& name)
    {
        return name == "valvebiped.weapon_clip" ||
            name == "valvebiped.weapon_magazine" ||
            name == "weapon_clip" ||
            name == "weapon_magazine";
    }

    inline int ScoreManualReloadMagazineBoneName(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        const bool hasClip = ManualReloadNameContains(name, "clip");
        const bool hasMagazine = ManualReloadNameContains(name, "magazine");
        const bool hasLooseMag = ManualReloadNameHasLooseMagToken(name);

        // Ignore controls, helpers and visual children. We need the root bone that
        // moves the whole detachable magazine.
        if (ManualReloadNameContains(name, "release") ||
            ManualReloadNameContains(name, "realease") ||
            ManualReloadNameContains(name, "button") ||
            ManualReloadNameContains(name, "trigger") ||
            ManualReloadNameContains(name, "bullet") ||
            ManualReloadNameContains(name, "round") ||
            ManualReloadNameContains(name, "shell") ||
            (ManualReloadNameContains(name, "ammo") && !hasClip && !hasMagazine && !hasLooseMag) ||
            ManualReloadNameContains(name, "helper") ||
            ManualReloadNameContains(name, "attach"))
        {
            return 0;
        }

        if (!hasClip && !hasMagazine && !hasLooseMag)
            return 0;

        // Replacement viewmodels frequently retain ValveBiped.weapon_clip as a
        // compatibility helper while the visible magazine mesh is weighted to a
        // custom bone such as Magazine_Main, Magazine or j_mag1. Keep the legacy
        // helper as a fallback, but never let it beat an explicit custom bone.
        if (ManualReloadNameIsLegacyValveBipedClip(name))
            return 500;

        int score = 200;
        if (name.rfind("v_weapon.", 0) == 0 || name.rfind("v_weapon_", 0) == 0)
            score += 1600;
        else if (ManualReloadNameContains(name, "v_weapon"))
            score += 1300;
        else if (ManualReloadNameContains(name, "weapon"))
            score += 650;

        if (ManualReloadNameContains(name, "magazine_main") ||
            ManualReloadNameContains(name, "magazine.main") ||
            ManualReloadNameContains(name, "magazine-main"))
        {
            score += 1350;
        }
        else if (hasMagazine)
        {
            score += 1000;
        }
        else if (hasLooseMag)
        {
            score += 900;
        }
        else if (hasClip)
        {
            score += 700;
        }

        if (ManualReloadNameEndsWith(name, "_clip") || ManualReloadNameEndsWith(name, ".clip"))
            score += 300;
        else if (ManualReloadNameEndsWith(name, "_magazine") || ManualReloadNameEndsWith(name, ".magazine"))
            score += 280;

        return score;
    }

    inline int FindManualReloadMagazineBone(
        const std::string& modelName,
        const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreManualReloadMagazineBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, bool> s_loggedModels;
            std::lock_guard<std::mutex> lock(s_logMutex);
            if (s_loggedModels.emplace(modelName, true).second)
            {
                Game::logMsg(
                    "[VR][ManualReload] name candidate model=%s bone=%d name=%s score=%d",
                    modelName.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline Vector HooksTransformPoint(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        return Vector(
            matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z + matrix.m[0][3],
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z + matrix.m[1][3],
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z + matrix.m[2][3]);
    }

    inline Vector HooksTransformVector(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        return Vector(
            matrix.m[0][0] * value.x + matrix.m[0][1] * value.y + matrix.m[0][2] * value.z,
            matrix.m[1][0] * value.x + matrix.m[1][1] * value.y + matrix.m[1][2] * value.z,
            matrix.m[2][0] * value.x + matrix.m[2][1] * value.y + matrix.m[2][2] * value.z);
    }

    inline Vector HooksInverseTransformPoint(const vr_vm_stabilize::Mat3x4& matrix, const Vector& value)
    {
        const Vector delta(
            value.x - matrix.m[0][3],
            value.y - matrix.m[1][3],
            value.z - matrix.m[2][3]);
        return Vector(
            delta.x * matrix.m[0][0] + delta.y * matrix.m[1][0] + delta.z * matrix.m[2][0],
            delta.x * matrix.m[0][1] + delta.y * matrix.m[1][1] + delta.z * matrix.m[2][1],
            delta.x * matrix.m[0][2] + delta.y * matrix.m[1][2] + delta.z * matrix.m[2][2]);
    }

    inline int FindMagazineBoxBone(const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreManualReloadMagazineBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }
        return bestBone;
    }

    inline bool MagazineInteractionWeaponIdIsShotgun(int weaponId)
    {
        return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PUMPSHOTGUN) ||
            weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::SHOTGUN_CHROME) ||
            weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::AUTOSHOTGUN) ||
            weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::SPAS);
    }

    inline bool MagazineInteractionWeaponIdIsHandgun(int weaponId)
    {
        return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL) ||
            weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::MAGNUM);
    }

    inline int MagazineInteractionInferWeaponIdFromViewmodelModelName(const std::string& lowerModel)
    {
        if (lowerModel.empty())
            return 0;

        auto has = [&](const char* token) -> bool
        {
            return token && *token && lowerModel.find(token) != std::string::npos;
        };

        if (has("shotgun_chrome") || has("chrome_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SHOTGUN_CHROME);
        if (has("pumpshotgun") || has("pump_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::PUMPSHOTGUN);
        if (has("autoshotgun") || has("auto_shotgun"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AUTOSHOTGUN);
        if (has("shotgun_spas") || has("v_shotgun_spas") || has("spas"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SPAS);

        if (has("desert_eagle") || has("pistol_magnum"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MAGNUM);
        if (has("dual_pistol") || has("dual_pistola") ||
            has("v_pistol.mdl") || has("v_pistola.mdl") || has("v_pistolb.mdl"))
        {
            return static_cast<int>(C_WeaponCSBase::WeaponID::PISTOL);
        }

        if (has("smg_mp5") || has("mp5"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MP5);
        if (has("silenced_smg") || has("smg_silenced"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::MAC10);
        if (has("v_smg.mdl") || has("smg_uzi"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::UZI);

        if (has("rifle_ak47") || has("ak47"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AK47);
        if (has("desert_rifle") || has("rifle_desert"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SCAR);
        if (has("sg552"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SG552);
        if (has("v_rifle.mdl"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::M16A1);

        if (has("sniper_military") || has("military_sniper"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SNIPER_MILITARY);
        if (has("huntingrifle") || has("hunting_rifle"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::HUNTING_RIFLE);
        if (has("snip_awp") || has("awp"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::AWP);
        if (has("snip_scout") || has("scout"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::SCOUT);

        if (has("m60") || has("machinegun_m60"))
            return static_cast<int>(C_WeaponCSBase::WeaponID::M60);

        return 0;
    }

    inline int ScoreMagazineInteractionShotgunShellBoneName(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        if (ManualReloadNameContains(name, "finger") ||
            ManualReloadNameContains(name, "hand") ||
            ManualReloadNameContains(name, "bip01") ||
            ManualReloadNameContains(name, "attach") ||
            ManualReloadNameContains(name, "muzzle") ||
            ManualReloadNameContains(name, "eject") ||
            ManualReloadNameContains(name, "release") ||
            ManualReloadNameContains(name, "realease") ||
            ManualReloadNameContains(name, "magrel") ||
            ManualReloadNameContains(name, "button") ||
            ManualReloadNameContains(name, "trigger") ||
            ManualReloadNameContains(name, "safety") ||
            ManualReloadNameContains(name, "bolt") ||
            ManualReloadNameContains(name, "slide") ||
            ManualReloadNameContains(name, "charger") ||
            ManualReloadNameContains(name, "handle") ||
            ManualReloadNameContains(name, "barrel") ||
            ManualReloadNameContains(name, "stock") ||
            ManualReloadNameContains(name, "hammer"))
        {
            return 0;
        }

        const bool hasWeapon = ManualReloadNameContains(name, "weapon");
        const bool hasClip = ManualReloadNameContains(name, "clip");
        const bool hasBullet = ManualReloadNameContains(name, "bullet");
        const bool hasRound = ManualReloadNameContains(name, "round");
        const bool hasShell = ManualReloadNameContains(name, "shell");
        const bool hasAmmo = ManualReloadNameContains(name, "ammo");
        if (!hasClip && !hasBullet && !hasRound && !hasShell && !hasAmmo)
            return 0;

        int score = 300;
        if (name == "valvebiped.weapon_clip_bullets" ||
            name == "weapon_clip_bullets")
        {
            score += 3200;
        }
        else if (name == "valvebiped.weapon_clip" ||
            name == "weapon_clip")
        {
            score += 3000;
        }
        else if (ManualReloadNameContains(name, "weapon_clip"))
        {
            score += 2600;
        }
        else if (hasClip)
        {
            score += 1800;
        }

        if (hasWeapon)
            score += 550;
        if (hasBullet || hasRound || hasShell)
            score += 350;
        if (hasAmmo)
            score += 120;
        return score;
    }

    inline int FindMagazineInteractionShotgunShellBone(
        const std::string& modelName,
        const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionShotgunShellBoneName(boneNames[static_cast<size_t>(bone)]);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, int> s_loggedBoneByModel;
            std::lock_guard<std::mutex> lock(s_logMutex);
            auto it = s_loggedBoneByModel.find(modelName);
            if (it == s_loggedBoneByModel.end() || it->second != bestBone)
            {
                s_loggedBoneByModel[modelName] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBox] shotgun shell bone candidate model=%s bone=%d name=%s score=%d",
                    modelName.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline bool MagazineInteractionShotgunShellBoneUsesStockProfileAxes(const std::string& lowerName)
    {
        return lowerName == "valvebiped.weapon_clip_bullets" ||
            lowerName == "weapon_clip_bullets" ||
            lowerName == "valvebiped.weapon_clip" ||
            lowerName == "weapon_clip" ||
            ManualReloadNameContains(lowerName, "weapon_clip");
    }

    inline int FindMagazineInteractionShotgunStableAnchorBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int shellBone)
    {
        const int numBones = static_cast<int>(boneNames.size());
        if (numBones <= 0)
            return -1;

        auto isExactBone = [&](int bone, const char* a, const char* b) -> bool
        {
            if (bone < 0 || bone >= numBones)
                return false;
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            return lowerName == a || lowerName == b;
        };

        auto findExact = [&](const char* a, const char* b) -> int
        {
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (isExactBone(bone, a, b))
                    return bone;
            }
            return -1;
        };

        // The shotgun shell/clip bones can be animated by the native reload/pump layers.
        // Use the main weapon bone as the stable anchor for cached capture offsets.
        int anchor = findExact("valvebiped.weapon_bone", "weapon_bone");
        if (anchor >= 0)
            return anchor;

        if (shellBone >= 0 && shellBone < static_cast<int>(boneParents.size()))
        {
            int current = shellBone;
            for (int guard = 0; guard < static_cast<int>(boneParents.size()); ++guard)
            {
                const int parent = boneParents[static_cast<size_t>(current)];
                if (parent < 0 || parent >= static_cast<int>(boneParents.size()) || parent == current)
                    break;
                if (isExactBone(parent, "valvebiped.weapon_bone", "weapon_bone"))
                    return parent;
                current = parent;
            }
        }

        anchor = findExact("valvebiped.weapon_clip", "weapon_clip");
        if (anchor >= 0)
            return anchor;

        return -1;
    }

    inline int FindMagazineBoxLegacyClipBone(const std::vector<std::string>& boneNames)
    {
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (ManualReloadNameIsLegacyValveBipedClip(lowerName))
                return bone;
        }
        return -1;
    }

    inline bool MagazineBoxBoneCanProvideParentBasis(const std::string& lowerName)
    {
        if (lowerName.empty())
            return false;

        if (ManualReloadNameContains(lowerName, "finger") ||
            ManualReloadNameContains(lowerName, "hand") ||
            ManualReloadNameContains(lowerName, "bip01") ||
            ManualReloadNameContains(lowerName, "wrist") ||
            ManualReloadNameContains(lowerName, "forearm") ||
            ManualReloadNameContains(lowerName, "upperarm") ||
            ManualReloadNameContains(lowerName, "clavicle") ||
            ManualReloadNameContains(lowerName, "spine") ||
            ManualReloadNameContains(lowerName, "camera") ||
            ManualReloadNameContains(lowerName, "attach") ||
            ManualReloadNameContains(lowerName, "muzzle") ||
            ManualReloadNameContains(lowerName, "shell") ||
            ManualReloadNameContains(lowerName, "trigger") ||
            ManualReloadNameContains(lowerName, "release") ||
            ManualReloadNameContains(lowerName, "realease") ||
            ManualReloadNameContains(lowerName, "safety") ||
            ManualReloadNameContains(lowerName, "bolt") ||
            ManualReloadNameContains(lowerName, "slide") ||
            ManualReloadNameContains(lowerName, "charger") ||
            ManualReloadNameContains(lowerName, "hammer") ||
            ManualReloadNameContains(lowerName, "clip") ||
            ManualReloadNameContains(lowerName, "magazine") ||
            ManualReloadNameHasLooseMagToken(lowerName))
        {
            return false;
        }

        return true;
    }

    inline bool MagazineBoxBoneCanProvideOffsetAnchor(const std::string& lowerName)
    {
        if (lowerName.empty())
            return false;

        if (ManualReloadNameContains(lowerName, "finger") ||
            ManualReloadNameContains(lowerName, "hand") ||
            ManualReloadNameContains(lowerName, "bip01") ||
            ManualReloadNameContains(lowerName, "wrist") ||
            ManualReloadNameContains(lowerName, "forearm") ||
            ManualReloadNameContains(lowerName, "upperarm") ||
            ManualReloadNameContains(lowerName, "clavicle") ||
            ManualReloadNameContains(lowerName, "spine") ||
            ManualReloadNameContains(lowerName, "camera") ||
            ManualReloadNameContains(lowerName, "attach") ||
            ManualReloadNameContains(lowerName, "muzzle") ||
            ManualReloadNameContains(lowerName, "shell") ||
            ManualReloadNameContains(lowerName, "trigger") ||
            ManualReloadNameContains(lowerName, "release") ||
            ManualReloadNameContains(lowerName, "realease") ||
            ManualReloadNameContains(lowerName, "safety") ||
            ManualReloadNameContains(lowerName, "bolt") ||
            ManualReloadNameContains(lowerName, "slide") ||
            ManualReloadNameContains(lowerName, "charger") ||
            ManualReloadNameContains(lowerName, "hammer"))
        {
            return false;
        }

        return true;
    }

    inline int FindMagazineBoxDirectOffsetAnchorBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int magazineBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (magazineBone < 0 || magazineBone >= numBones)
            return -1;

        const int parent = boneParents[static_cast<size_t>(magazineBone)];
        if (parent < 0 || parent >= numBones || parent == magazineBone)
            return -1;

        const std::string lowerName =
            (parent < static_cast<int>(boneNames.size()))
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(parent)])
            : std::string();
        return MagazineBoxBoneCanProvideOffsetAnchor(lowerName) ? parent : -1;
    }

    inline int FindMagazineBoxParentBasisBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int magazineBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (magazineBone < 0 || magazineBone >= numBones)
            return -1;

        int current = magazineBone;
        for (int guard = 0; guard < numBones; ++guard)
        {
            const int parent = boneParents[static_cast<size_t>(current)];
            if (parent < 0 || parent >= numBones || parent == current)
                break;

            const std::string lowerName =
                (parent < static_cast<int>(boneNames.size()))
                ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(parent)])
                : std::string();
            if (MagazineBoxBoneCanProvideParentBasis(lowerName))
                return parent;

            current = parent;
        }
        return -1;
    }

    inline int ScoreMagazineInteractionBoltBoneName(const std::string& rawName, int weaponId)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return 0;

        if (MagazineInteractionWeaponIdIsShotgun(weaponId) &&
            (name == "weapon_charger_slide" ||
                name == "valvebiped.weapon_charger_slide"))
        {
            return 5600;
        }

        if (MagazineInteractionWeaponIdIsHandgun(weaponId) &&
            (name == "weapon_charger_slide" ||
                name == "valvebiped.weapon_charger_slide" ||
                name == "weapon_slide" ||
                name == "valvebiped.weapon_slide" ||
                name == "v_weapon.slide" ||
                name == "v_weapon_slide" ||
                name == "slide"))
        {
            return 5600;
        }

        if (MagazineInteractionWeaponIdIsHandgun(weaponId) &&
            ManualReloadNameContains(name, "slide") &&
            (ManualReloadNameContains(name, "weapon") || name == "slide"))
        {
            return 5200;
        }

        if (ManualReloadNameContains(name, "finger") ||
            ManualReloadNameContains(name, "hand") ||
            ManualReloadNameContains(name, "bip01") ||
            ManualReloadNameContains(name, "attach") ||
            ManualReloadNameContains(name, "muzzle") ||
            ManualReloadNameContains(name, "eject") ||
            ManualReloadNameContains(name, "release") ||
            ManualReloadNameContains(name, "realease") ||
            ManualReloadNameContains(name, "magrel") ||
            ManualReloadNameContains(name, "button") ||
            ManualReloadNameContains(name, "trigger") ||
            ManualReloadNameContains(name, "safety") ||
            ManualReloadNameContains(name, "barrel") ||
            ManualReloadNameContains(name, "stock") ||
            ManualReloadNameContains(name, "hammer") ||
            ManualReloadNameContains(name, "bullet") ||
            ManualReloadNameContains(name, "round") ||
            ManualReloadNameContains(name, "shell") ||
            ManualReloadNameContains(name, "ammo") ||
            ManualReloadNameContains(name, "clip") ||
            ManualReloadNameContains(name, "magazine") ||
            ManualReloadNameHasLooseMagToken(name))
        {
            return 0;
        }

        int score = 0;
        if (ManualReloadNameContains(name, "bolt_handle") ||
            ManualReloadNameContains(name, "bolt.handle") ||
            ManualReloadNameContains(name, "bolt-handle"))
        {
            score += 3200;
        }
        if (ManualReloadNameContains(name, "bolt"))
            score += 3000;
        if (ManualReloadNameEndsWith(name, "_bolt") || ManualReloadNameEndsWith(name, ".bolt"))
            score += 650;
        if (ManualReloadNameContains(name, "charging_handle") ||
            ManualReloadNameContains(name, "charging.handle") ||
            ManualReloadNameContains(name, "charging-handle") ||
            ManualReloadNameContains(name, "charge_handle") ||
            ManualReloadNameContains(name, "charge.handle") ||
            ManualReloadNameContains(name, "charge-handle"))
        {
            score += 1600;
        }
        if (ManualReloadNameContains(name, "slide"))
            score += 1100;
        if (ManualReloadNameContains(name, "charging") ||
            ManualReloadNameContains(name, "charger") ||
            ManualReloadNameContains(name, "charge"))
        {
            score += 850;
        }
        if (ManualReloadNameContains(name, "handle"))
            score += (score > 0) ? 350 : 250;
        if (ManualReloadNameContains(name, "cock"))
            score += 650;

        if (score <= 0)
            return 0;

        if (name.rfind("v_weapon.", 0) == 0 || name.rfind("v_weapon_", 0) == 0)
            score += 600;
        else if (ManualReloadNameContains(name, "weapon"))
            score += 250;

        if (ManualReloadNameEndsWith(name, "_slide") || ManualReloadNameEndsWith(name, ".slide"))
            score += 220;
        if (ManualReloadNameEndsWith(name, "_handle") || ManualReloadNameEndsWith(name, ".handle"))
            score += 120;

        return score;
    }

    inline int FindMagazineInteractionBoltBone(
        int weaponId,
        const std::string& modelName,
        const std::vector<std::string>& boneNames)
    {
        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const int score = ScoreMagazineInteractionBoltBoneName(
                boneNames[static_cast<size_t>(bone)],
                weaponId);
            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0)
        {
            static std::mutex s_logMutex;
            static std::unordered_map<std::string, int> s_loggedBoneByModel;
            std::lock_guard<std::mutex> lock(s_logMutex);
            auto it = s_loggedBoneByModel.find(modelName);
            if (it == s_loggedBoneByModel.end() || it->second != bestBone)
            {
                s_loggedBoneByModel[modelName] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBolt] name candidate model=%s weaponId=%d bone=%d name=%s score=%d",
                    modelName.c_str(),
                    weaponId,
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline Vector HooksNormalizeVector(const Vector& value, const Vector& fallback)
    {
        const float length = value.Length();
        if (!(length > 0.000001f))
            return fallback;
        return value * (1.0f / length);
    }

    inline int ResolveMagazineInteractionWeaponIdForConfig(const VR* vr)
    {
        if (!vr)
            return 0;

        if (vr->m_MagazineInteractionWeaponId > 0)
            return vr->m_MagazineInteractionWeaponId;

        return vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
    }

    inline Vector ResolveMagazineInteractionBoltPullAxisLocal(
        const VR* vr,
        bool& outUsedOverride)
    {
        outUsedOverride = false;
        if (!vr)
            return Vector(0.0f, 1.0f, 0.0f);

        const int weaponId = ResolveMagazineInteractionWeaponIdForConfig(vr);
        if (weaponId > 0)
        {
            const auto axisIt = vr->m_MagazineInteractionBoltPullAxisLocalOverrides.find(weaponId);
            if (axisIt != vr->m_MagazineInteractionBoltPullAxisLocalOverrides.end())
            {
                outUsedOverride = true;
                return axisIt->second;
            }
        }

        return vr->m_MagazineInteractionBoltPullAxisLocal;
    }

    inline Vector BuildMagazineInteractionBoltPullAxisWorld(
        VR* vr,
        const std::string& modelName,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        const std::vector<int>& boneParents,
        int boltBone,
        const vr_vm_stabilize::Mat3x4& boltWorld,
        const void* pModelToWorld)
    {
        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        bool usedAxisOverride = false;
        Vector configuredLocalAxis = ResolveMagazineInteractionBoltPullAxisLocal(vr, usedAxisOverride);
        const bool legacyM16Axis =
            !usedAxisOverride &&
            lowerModel.find("models/v_models/v_rifle.mdl") != std::string::npos &&
            std::fabs(configuredLocalAxis.x + 1.0f) < 0.001f &&
            std::fabs(configuredLocalAxis.y) < 0.001f &&
            std::fabs(configuredLocalAxis.z) < 0.001f;
        if (legacyM16Axis)
            configuredLocalAxis = Vector(0.0f, 1.0f, 0.0f);

        const Vector localAxis = HooksNormalizeVector(
            configuredLocalAxis,
            Vector(0.0f, 1.0f, 0.0f));

        auto logAxis = [&](const char* source, const Vector& axis)
        {
            static std::mutex s_axisLogMutex;
            static std::unordered_set<std::string> s_loggedAxisSources;
            char key[256];
            std::snprintf(
                key,
                sizeof(key),
                "%s|%s|%.2f,%.2f,%.2f|%d|%d",
                lowerModel.c_str(),
                source ? source : "unknown",
                localAxis.x,
                localAxis.y,
                localAxis.z,
                legacyM16Axis ? 1 : 0,
                usedAxisOverride ? 1 : 0);
            {
                std::lock_guard<std::mutex> lock(s_axisLogMutex);
                if (!s_loggedAxisSources.insert(key).second)
                    return;
            }
            Game::logMsg(
                "[VR][MagazineBolt] pull axis model=%s source=%s local=(%.2f %.2f %.2f) axis=(%.3f %.3f %.3f)%s%s",
                modelName.c_str(),
                source ? source : "unknown",
                localAxis.x,
                localAxis.y,
                localAxis.z,
                axis.x,
                axis.y,
                axis.z,
                legacyM16Axis ? " legacyM16Axis=1" : "",
                usedAxisOverride ? " override=1" : "");
        };

        auto axisFromMatrix = [&](const vr_vm_stabilize::Mat3x4& matrix) -> Vector
            {
                Vector axis = HooksTransformVector(matrix, localAxis);
                axis = HooksNormalizeVector(axis, Vector(0.0f, 0.0f, 0.0f));
                return (axis.Length() > 0.0001f) ? axis : Vector(0.0f, 0.0f, 0.0f);
            };

        if (sourceBones &&
            boltBone >= 0 &&
            boltBone < numBones &&
            static_cast<int>(boneParents.size()) >= numBones)
        {
            const int parentBone = boneParents[static_cast<size_t>(boltBone)];
            if (parentBone >= 0 && parentBone < numBones && parentBone != boltBone)
            {
                vr_vm_stabilize::Mat3x4 parentWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + parentBone, parentWorld))
                {
                    const Vector axis = axisFromMatrix(parentWorld);
                    if (axis.Length() > 0.0001f)
                    {
                        logAxis("bolt-parent-local", axis);
                        return axis;
                    }
                }
            }
        }

        if (pModelToWorld)
        {
            vr_vm_stabilize::Mat3x4 modelWorld{};
            if (vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld),
                modelWorld))
            {
                const Vector axis = axisFromMatrix(modelWorld);
                if (axis.Length() > 0.0001f)
                {
                    logAxis("model-local", axis);
                    return axis;
                }
            }
        }

        const Vector fallbackAxis = axisFromMatrix(boltWorld);
        if (fallbackAxis.Length() > 0.0001f)
        {
            logAxis("bolt-local", fallbackAxis);
            return fallbackAxis;
        }
        return Vector(0.0f, 1.0f, 0.0f);
    }

    inline bool HooksBoneIsDescendantOf(
        const std::vector<int>& boneParents,
        int bone,
        int ancestorBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (bone < 0 || bone >= numBones || ancestorBone < 0 || ancestorBone >= numBones)
            return false;

        int current = bone;
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            if (current == ancestorBone)
                return true;
            current = boneParents[static_cast<size_t>(current)];
        }
        return false;
    }

    inline int HooksBoneDescendantDepth(
        const std::vector<int>& boneParents,
        int bone,
        int ancestorBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (bone < 0 || bone >= numBones || ancestorBone < 0 || ancestorBone >= numBones)
            return -1;

        int current = bone;
        for (int depth = 0; depth < numBones && current >= 0 && current < numBones; ++depth)
        {
            if (current == ancestorBone)
                return depth;
            current = boneParents[static_cast<size_t>(current)];
        }
        return -1;
    }

    inline bool HooksMagazineBoxCanSampleBoneName(const std::string& lowerName, int descendantDepth)
    {
        if (lowerName.empty())
            return false;

        if (ManualReloadNameContains(lowerName, "finger") ||
            ManualReloadNameContains(lowerName, "hand") ||
            ManualReloadNameContains(lowerName, "bip01") ||
            ManualReloadNameContains(lowerName, "attach") ||
            ManualReloadNameContains(lowerName, "muzzle") ||
            ManualReloadNameContains(lowerName, "eject") ||
            ManualReloadNameContains(lowerName, "release") ||
            ManualReloadNameContains(lowerName, "realease") ||
            ManualReloadNameContains(lowerName, "magrel") ||
            ManualReloadNameContains(lowerName, "button") ||
            ManualReloadNameContains(lowerName, "trigger") ||
            ManualReloadNameContains(lowerName, "safety") ||
            ManualReloadNameContains(lowerName, "bolt") ||
            ManualReloadNameContains(lowerName, "slide") ||
            ManualReloadNameContains(lowerName, "charger") ||
            ManualReloadNameContains(lowerName, "handle") ||
            ManualReloadNameContains(lowerName, "barrel") ||
            ManualReloadNameContains(lowerName, "stock") ||
            ManualReloadNameContains(lowerName, "hammer"))
        {
            return false;
        }

        if (ScoreManualReloadMagazineBoneName(lowerName) > 0)
            return true;

        const bool hasClipOrMag =
            ManualReloadNameContains(lowerName, "clip") ||
            ManualReloadNameContains(lowerName, "mag");
        const bool hasAmmoPart =
            ManualReloadNameContains(lowerName, "bullet") ||
            ManualReloadNameContains(lowerName, "round") ||
            ManualReloadNameContains(lowerName, "ammo");
        return descendantDepth <= 2 && hasClipOrMag && hasAmmoPart;
    }

    inline int HooksDominantAxis(const Vector& value)
    {
        const float ax = std::fabs(value.x);
        const float ay = std::fabs(value.y);
        const float az = std::fabs(value.z);
        if (ay >= ax && ay >= az)
            return 1;
        if (az >= ax && az >= ay)
            return 2;
        return 0;
    }

    inline float HooksVectorComponent(const Vector& value, int axis)
    {
        switch (std::clamp(axis, 0, 2))
        {
        case 0:
            return value.x;
        case 1:
            return value.y;
        default:
            return value.z;
        }
    }

    inline Vector HooksMatrixAxis(const vr_vm_stabilize::Mat3x4& matrix, int axis)
    {
        const int clampedAxis = std::clamp(axis, 0, 2);
        return Vector(
            matrix.m[0][clampedAxis],
            matrix.m[1][clampedAxis],
            matrix.m[2][clampedAxis]);
    }

    inline void HooksSetMatrixAxis(vr_vm_stabilize::Mat3x4& matrix, int axis, const Vector& value)
    {
        const int clampedAxis = std::clamp(axis, 0, 2);
        matrix.m[0][clampedAxis] = value.x;
        matrix.m[1][clampedAxis] = value.y;
        matrix.m[2][clampedAxis] = value.z;
    }

    inline bool HooksProjectBasisReference(
        const Vector& candidate,
        const Vector& lockedAxis,
        Vector& outReference)
    {
        Vector projected = candidate - lockedAxis * DotProduct(candidate, lockedAxis);
        projected = HooksNormalizeVector(projected, Vector(0.0f, 0.0f, 0.0f));
        if (projected.Length() <= 0.0001f)
            return false;

        outReference = projected;
        return true;
    }

    inline bool BuildMagazineBoxBasisFromParentOffset(
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const vr_vm_stabilize::Mat3x4& parentWorld,
        const void* pModelToWorld,
        const Vector& insertionAxisLocal,
        int lengthAxis,
        vr_vm_stabilize::Mat3x4& outWorld)
    {
        const int clampedLengthAxis = std::clamp(lengthAxis, 0, 2);
        Vector extractionAxis = vr_vm_stabilize::GetOrigin(magazineWorld) -
            vr_vm_stabilize::GetOrigin(parentWorld);
        extractionAxis = HooksNormalizeVector(extractionAxis, Vector(0.0f, 0.0f, 0.0f));
        if (extractionAxis.Length() <= 0.0001f)
            return false;

        float localSign = HooksVectorComponent(insertionAxisLocal, clampedLengthAxis);
        if (std::fabs(localSign) <= 0.0001f)
            localSign = (clampedLengthAxis == 1) ? -1.0f : 1.0f;
        const Vector lockedAxis = extractionAxis * ((localSign < 0.0f) ? -1.0f : 1.0f);

        vr_vm_stabilize::Mat3x4 modelWorld{};
        const bool hasModelWorld =
            pModelToWorld &&
            vr_vm_stabilize::SafeRead(reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld), modelWorld);

        Vector referenceAxis(0.0f, 0.0f, 0.0f);
        const int preferredReferenceAxis = (clampedLengthAxis == 0) ? 1 : 0;
        const int axisOrder[3] =
        {
            preferredReferenceAxis,
            (preferredReferenceAxis + 1) % 3,
            (preferredReferenceAxis + 2) % 3
        };

        auto tryMatrixAxes = [&](const vr_vm_stabilize::Mat3x4& matrix) -> bool
        {
            for (int axis : axisOrder)
            {
                if (axis == clampedLengthAxis)
                    continue;
                if (HooksProjectBasisReference(HooksMatrixAxis(matrix, axis), lockedAxis, referenceAxis))
                    return true;
            }
            return false;
        };

        if (!(hasModelWorld && tryMatrixAxes(modelWorld)) &&
            !tryMatrixAxes(parentWorld) &&
            !tryMatrixAxes(magazineWorld))
        {
            const Vector fallbackAxes[3] =
            {
                Vector(0.0f, 0.0f, 1.0f),
                Vector(0.0f, 1.0f, 0.0f),
                Vector(1.0f, 0.0f, 0.0f)
            };
            bool foundFallback = false;
            for (const Vector& fallbackAxis : fallbackAxes)
            {
                if (HooksProjectBasisReference(fallbackAxis, lockedAxis, referenceAxis))
                {
                    foundFallback = true;
                    break;
                }
            }
            if (!foundFallback)
                return false;
        }

        Vector axisX(1.0f, 0.0f, 0.0f);
        Vector axisY(0.0f, 1.0f, 0.0f);
        Vector axisZ(0.0f, 0.0f, 1.0f);
        if (clampedLengthAxis == 0)
        {
            axisX = lockedAxis;
            axisY = referenceAxis;
            axisZ = HooksNormalizeVector(CrossProduct(axisX, axisY), Vector(0.0f, 0.0f, 0.0f));
            axisY = HooksNormalizeVector(CrossProduct(axisZ, axisX), Vector(0.0f, 0.0f, 0.0f));
        }
        else if (clampedLengthAxis == 1)
        {
            axisY = lockedAxis;
            axisX = referenceAxis;
            axisZ = HooksNormalizeVector(CrossProduct(axisX, axisY), Vector(0.0f, 0.0f, 0.0f));
            axisX = HooksNormalizeVector(CrossProduct(axisY, axisZ), Vector(0.0f, 0.0f, 0.0f));
        }
        else
        {
            axisZ = lockedAxis;
            axisX = referenceAxis;
            axisY = HooksNormalizeVector(CrossProduct(axisZ, axisX), Vector(0.0f, 0.0f, 0.0f));
            axisX = HooksNormalizeVector(CrossProduct(axisY, axisZ), Vector(0.0f, 0.0f, 0.0f));
        }

        if (axisX.Length() <= 0.0001f || axisY.Length() <= 0.0001f || axisZ.Length() <= 0.0001f)
            return false;

        outWorld = magazineWorld;
        HooksSetMatrixAxis(outWorld, 0, axisX);
        HooksSetMatrixAxis(outWorld, 1, axisY);
        HooksSetMatrixAxis(outWorld, 2, axisZ);
        return true;
    }

    inline bool TryGetOfficialMagazineBoxProfile(
        const std::string& lowerModel,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 1;
        outLengthAxis = 1;

        auto useProfile = [&](const char* modelPath, const Vector& mins, const Vector& maxs, int axis) -> bool
        {
            if (lowerModel.find(modelPath) == std::string::npos)
                return false;
            outMins = mins - padding;
            outMaxs = maxs + padding;
            outLengthAxis = axis;
            return true;
        };

        if (useProfile("models/v_models/v_pistol.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_pistola.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_pistolb.mdl", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistola.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistol.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_dual_pistols.mdl", Vector(-0.66f, -4.66f, -0.42f), Vector(0.72f, 1.35f, 1.99f), 1)) return true;
        if (useProfile("models/v_models/v_desert_eagle.mdl", Vector(-0.83f, -5.93f, -0.98f), Vector(0.56f, 0.59f, 3.11f), 1)) return true;
        if (useProfile("models/v_models/v_pistol_magnum.mdl", Vector(-0.83f, -5.93f, -0.98f), Vector(0.56f, 0.59f, 3.11f), 1)) return true;
        if (useProfile("models/v_models/v_pistol", Vector(-0.59f, -5.66f, -0.31f), Vector(0.59f, 0.80f, 2.25f), 1)) return true;

        if (useProfile("models/v_models/v_smg.mdl", Vector(-1.17f, -8.29f, -0.86f), Vector(1.17f, 0.09f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_silenced_smg.mdl", Vector(-0.68f, -7.70f, -0.79f), Vector(0.52f, 1.05f, 0.78f), 1)) return true;
        if (useProfile("models/v_models/v_smg_mp5.mdl", Vector(-0.75f, -8.80f, -1.05f), Vector(0.75f, 1.10f, 1.05f), 1)) return true;

        if (useProfile("models/v_models/v_rifle.mdl", Vector(-0.90f, -8.40f, -1.10f), Vector(0.90f, 1.25f, 1.15f), 1)) return true;
        if (useProfile("models/v_models/v_rifle_ak47.mdl", Vector(-0.65f, -9.53f, -0.69f), Vector(0.63f, 1.34f, 7.11f), 1)) return true;
        if (useProfile("models/v_models/v_desert_rifle.mdl", Vector(-1.81f, -0.73f, -4.73f), Vector(1.69f, 0.72f, 1.61f), 2)) return true;
        if (useProfile("models/v_models/v_rif_sg552.mdl", Vector(-1.05f, -8.70f, -1.30f), Vector(1.05f, 1.25f, 1.30f), 1)) return true;

        if (useProfile("models/v_models/v_huntingrifle.mdl", Vector(-1.17f, -3.11f, -0.86f), Vector(1.17f, 2.23f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_sniper_military.mdl", Vector(-0.79f, -5.30f, -0.09f), Vector(0.86f, 2.80f, 5.01f), 1)) return true;
        if (useProfile("models/v_models/v_snip_scout.mdl", Vector(-0.96f, -1.84f, -1.21f), Vector(0.47f, 0.18f, 4.64f), 2)) return true;
        if (useProfile("models/v_models/v_snip_awp.mdl", Vector(-0.66f, -2.56f, -0.97f), Vector(0.67f, 0.92f, 2.37f), 1)) return true;

        if (useProfile("models/v_models/v_pumpshotgun.mdl", Vector(-1.17f, -2.05f, -0.09f), Vector(1.17f, 3.30f, 2.30f), 1)) return true;
        if (useProfile("models/v_models/v_shotgun_chrome.mdl", Vector(-1.17f, -2.05f, -0.09f), Vector(1.17f, 3.30f, 2.30f), 1)) return true;
        if (useProfile("models/v_models/v_autoshotgun.mdl", Vector(-1.17f, -8.29f, -0.86f), Vector(1.17f, 0.09f, 0.86f), 1)) return true;
        if (useProfile("models/v_models/v_shotgun_spas.mdl", Vector(-0.61f, -0.96f, -1.50f), Vector(0.62f, 0.80f, 1.57f), 2)) return true;

        if (useProfile("models/v_models/v_m60.mdl", Vector(-4.20f, -3.20f, -6.40f), Vector(4.20f, 5.40f, 1.60f), 2)) return true;
        if (useProfile("models/v_models/v_grenade_launcher.mdl", Vector(-1.34f, -0.96f, -0.24f), Vector(1.03f, 1.43f, 5.60f), 2)) return true;

        return false;
    }

    inline bool MagazineBoxOfficialProfileExists(const std::string& lowerModel)
    {
        Vector mins;
        Vector maxs;
        int sampleCount = 0;
        int lengthAxis = 0;
        return TryGetOfficialMagazineBoxProfile(
            lowerModel,
            Vector(0.0f, 0.0f, 0.0f),
            mins,
            maxs,
            sampleCount,
            lengthAxis);
    }

    inline int FindMagazineBoxOfficialProfileFallbackBone(
        const std::string& lowerModel,
        const std::vector<std::string>& boneNames)
    {
        if (!MagazineBoxOfficialProfileExists(lowerModel))
            return -1;

        auto canUseFallbackName = [&](const std::string& lowerName)
            {
                if (lowerName.empty())
                    return false;

                if (ManualReloadNameContains(lowerName, "finger") ||
                    ManualReloadNameContains(lowerName, "hand") ||
                    ManualReloadNameContains(lowerName, "bip01") ||
                    ManualReloadNameContains(lowerName, "attach") ||
                    ManualReloadNameContains(lowerName, "muzzle") ||
                    ManualReloadNameContains(lowerName, "eject") ||
                    ManualReloadNameContains(lowerName, "release") ||
                    ManualReloadNameContains(lowerName, "realease") ||
                    ManualReloadNameContains(lowerName, "magrel") ||
                    ManualReloadNameContains(lowerName, "button") ||
                    ManualReloadNameContains(lowerName, "trigger") ||
                    ManualReloadNameContains(lowerName, "safety") ||
                    ManualReloadNameContains(lowerName, "bolt") ||
                    ManualReloadNameContains(lowerName, "slide") ||
                    ManualReloadNameContains(lowerName, "charger") ||
                    ManualReloadNameContains(lowerName, "handle") ||
                    ManualReloadNameContains(lowerName, "barrel") ||
                    ManualReloadNameContains(lowerName, "stock") ||
                    ManualReloadNameContains(lowerName, "hammer") ||
                    ManualReloadNameContains(lowerName, "bullet") ||
                    ManualReloadNameContains(lowerName, "round") ||
                    ManualReloadNameContains(lowerName, "shell"))
                {
                    return false;
                }

                return ManualReloadNameContains(lowerName, "clip") ||
                    ManualReloadNameContains(lowerName, "magazine") ||
                    ManualReloadNameHasLooseMagToken(lowerName);
            };

        int bestBone = -1;
        int bestScore = 0;
        for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
        {
            const std::string lowerName = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]);
            if (!canUseFallbackName(lowerName))
                continue;

            int score = 250;
            if (lowerName == "valvebiped.weapon_clip" ||
                lowerName == "valvebiped.weapon_magazine" ||
                lowerName == "weapon_clip" ||
                lowerName == "weapon_magazine")
            {
                score += 1800;
            }
            else if (lowerName == "valvebiped.clip" ||
                lowerName == "valvebiped.magazine" ||
                lowerName == "clip" ||
                lowerName == "mag" ||
                lowerName == "magazine")
            {
                score += 1400;
            }

            if (ManualReloadNameContains(lowerName, "weapon"))
                score += 500;
            if (ManualReloadNameContains(lowerName, "magazine"))
                score += 450;
            if (ManualReloadNameHasLooseMagToken(lowerName))
                score += 350;
            if (ManualReloadNameContains(lowerName, "clip"))
                score += 300;
            if (ManualReloadNameContains(lowerName, "ammo"))
                score -= 100;

            if (score > bestScore)
            {
                bestScore = score;
                bestBone = bone;
            }
        }

        if (bestBone >= 0)
        {
            static std::mutex s_fallbackLogMutex;
            static std::unordered_map<std::string, int> s_loggedFallbackByModel;
            std::lock_guard<std::mutex> lock(s_fallbackLogMutex);
            auto it = s_loggedFallbackByModel.find(lowerModel);
            if (it == s_loggedFallbackByModel.end() || it->second != bestBone)
            {
                s_loggedFallbackByModel[lowerModel] = bestBone;
                Game::logMsg(
                    "[VR][MagazineBox] official-profile fallback magazine bone model=%s bone=%d name=%s score=%d",
                    lowerModel.c_str(),
                    bestBone,
                    boneNames[static_cast<size_t>(bestBone)].c_str(),
                    bestScore);
            }
        }

        return bestBone;
    }

    inline bool HooksVectorComponentsAreFinite(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    inline void HooksAccumulateBounds(Vector& mins, Vector& maxs, const Vector& value)
    {
        mins.x = std::min(mins.x, value.x);
        mins.y = std::min(mins.y, value.y);
        mins.z = std::min(mins.z, value.z);
        maxs.x = std::max(maxs.x, value.x);
        maxs.y = std::max(maxs.y, value.y);
        maxs.z = std::max(maxs.z, value.z);
    }

    inline bool HooksMagazineBoxCanUseHitboxBone(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        int hitboxBone,
        int magazineBone,
        const std::string& lowerHitboxName)
    {
        const int depth = HooksBoneDescendantDepth(boneParents, hitboxBone, magazineBone);
        if (depth < 0 || depth > 6)
            return false;
        if (depth == 0)
            return true;

        const std::string lowerBoneName =
            hitboxBone < static_cast<int>(boneNames.size())
            ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(hitboxBone)])
            : std::string();
        if (HooksMagazineBoxCanSampleBoneName(lowerBoneName, depth))
            return true;
        if (!lowerHitboxName.empty() && HooksMagazineBoxCanSampleBoneName(lowerHitboxName, depth))
            return true;

        return false;
    }

    inline bool BuildMagazineBoxLocalBoundsFromHitboxes(
        void* drawState,
        int requestedHitboxSet,
        int numBonesOffset,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int magazineBone,
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const Vector& padding,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 0;
        outLengthAxis = 0;
        if (!drawState || !sourceBones || numBones <= 0 || magazineBone < 0 || magazineBone >= numBones)
            return false;

        const uint8_t* studioHdr = nullptr;
        if (!vr_vm_stabilize::TryGetStudioHdrFromDrawState(drawState, studioHdr) || !studioHdr)
            return false;

        int studioLength = 0;
        vr_vm_stabilize::SafeRead(studioHdr + 0x4C, studioLength);

        int numHitboxSets = 0;
        int hitboxSetIndex = 0;
        const int hitboxSetsOffset = (numBonesOffset > 0) ? (numBonesOffset + 16) : 0xAC;
        const int hitboxSetIndexOffset = hitboxSetsOffset + 4;
        if (!vr_vm_stabilize::SafeRead(studioHdr + hitboxSetsOffset, numHitboxSets) ||
            !vr_vm_stabilize::SafeRead(studioHdr + hitboxSetIndexOffset, hitboxSetIndex))
        {
            return false;
        }
        if (numHitboxSets <= 0 || numHitboxSets > 64 || hitboxSetIndex <= 0 || hitboxSetIndex > 0x200000)
            return false;
        if (studioLength > 0 &&
            (hitboxSetIndex >= studioLength ||
                hitboxSetIndex + numHitboxSets * 12 > studioLength))
        {
            return false;
        }

        const int firstSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? requestedHitboxSet : 0;
        const int endSet =
            (requestedHitboxSet >= 0 && requestedHitboxSet < numHitboxSets) ? (requestedHitboxSet + 1) : numHitboxSets;

        static constexpr int kHitboxSetStride = 12;
        static constexpr int kHitboxStrideCandidates[] = { 68, 72, 64, 80, 88 };
        int bestCount = 0;
        int bestAxis = 0;
        Vector bestMins(0.0f, 0.0f, 0.0f);
        Vector bestMaxs(0.0f, 0.0f, 0.0f);

        for (int stride : kHitboxStrideCandidates)
        {
            int sampleCount = 0;
            Vector boundsMins(FLT_MAX, FLT_MAX, FLT_MAX);
            Vector boundsMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);

            for (int set = firstSet; set < endSet; ++set)
            {
                const size_t setOffset = static_cast<size_t>(hitboxSetIndex) +
                    static_cast<size_t>(set) * static_cast<size_t>(kHitboxSetStride);
                if (studioLength > 0 && setOffset + kHitboxSetStride > static_cast<size_t>(studioLength))
                    continue;

                const uint8_t* setBase = studioHdr + setOffset;
                int numHitboxes = 0;
                int hitboxIndex = 0;
                if (!vr_vm_stabilize::SafeRead(setBase + 4, numHitboxes) ||
                    !vr_vm_stabilize::SafeRead(setBase + 8, hitboxIndex))
                {
                    continue;
                }
                if (numHitboxes <= 0 || numHitboxes > 512 || hitboxIndex <= 0 || hitboxIndex > 0x200000)
                    continue;

                for (int hitbox = 0; hitbox < numHitboxes; ++hitbox)
                {
                    const size_t hitboxOffset = setOffset + static_cast<size_t>(hitboxIndex) +
                        static_cast<size_t>(hitbox) * static_cast<size_t>(stride);
                    if (studioLength > 0 && hitboxOffset + 32 > static_cast<size_t>(studioLength))
                        continue;

                    const uint8_t* hitboxBase = studioHdr + hitboxOffset;
                    int hitboxBone = -1;
                    Vector bbMin;
                    Vector bbMax;
                    int nameOffset = 0;
                    if (!vr_vm_stabilize::SafeRead(hitboxBase + 0, hitboxBone) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 8, bbMin) ||
                        !vr_vm_stabilize::SafeRead(hitboxBase + 20, bbMax))
                    {
                        continue;
                    }
                    vr_vm_stabilize::SafeRead(hitboxBase + 32, nameOffset);
                    if (hitboxBone < 0 || hitboxBone >= numBones)
                        continue;
                    if (!HooksVectorComponentsAreFinite(bbMin) || !HooksVectorComponentsAreFinite(bbMax))
                        continue;
                    if (bbMax.x <= bbMin.x || bbMax.y <= bbMin.y || bbMax.z <= bbMin.z)
                        continue;
                    const Vector span = bbMax - bbMin;
                    if (span.x > 128.0f || span.y > 128.0f || span.z > 128.0f)
                        continue;

                    std::string lowerHitboxName;
                    if (nameOffset > 0 && nameOffset < 0x10000)
                    {
                        std::string hitboxName;
                        const size_t nameAddressOffset = hitboxOffset + static_cast<size_t>(nameOffset);
                        if ((studioLength <= 0 || nameAddressOffset < static_cast<size_t>(studioLength)) &&
                            vr_vm_stabilize::TryReadCStringSafe(reinterpret_cast<const char*>(studioHdr + nameAddressOffset), hitboxName))
                        {
                            lowerHitboxName = vr_vm_stabilize::ToLowerAscii(hitboxName);
                        }
                    }

                    if (!HooksMagazineBoxCanUseHitboxBone(boneNames, boneParents, hitboxBone, magazineBone, lowerHitboxName))
                        continue;

                    vr_vm_stabilize::Mat3x4 hitboxBoneWorld{};
                    if (!vr_vm_stabilize::SafeRead(sourceBones + hitboxBone, hitboxBoneWorld))
                        continue;

                    for (int z = 0; z <= 1; ++z)
                    {
                        for (int y = 0; y <= 1; ++y)
                        {
                            for (int x = 0; x <= 1; ++x)
                            {
                                const Vector hitboxLocal(
                                    x ? bbMax.x : bbMin.x,
                                    y ? bbMax.y : bbMin.y,
                                    z ? bbMax.z : bbMin.z);
                                const Vector world = HooksTransformPoint(hitboxBoneWorld, hitboxLocal);
                                const Vector magazineLocal = HooksInverseTransformPoint(magazineWorld, world);
                                HooksAccumulateBounds(boundsMins, boundsMaxs, magazineLocal);
                            }
                        }
                    }
                    ++sampleCount;
                }
            }

            if (sampleCount <= 0 || boundsMins.x == FLT_MAX)
                continue;

            const Vector range = boundsMaxs - boundsMins;
            if (range.x <= 0.001f || range.y <= 0.001f || range.z <= 0.001f)
                continue;

            int axis = 0;
            if (range.y > range[axis])
                axis = 1;
            if (range.z > range[axis])
                axis = 2;

            if (sampleCount > bestCount || (sampleCount == bestCount && range[axis] > (bestMaxs - bestMins)[bestAxis]))
            {
                bestCount = sampleCount;
                bestAxis = axis;
                bestMins = boundsMins;
                bestMaxs = boundsMaxs;
            }
        }

        if (bestCount <= 0)
            return false;

        outMins = bestMins - padding;
        outMaxs = bestMaxs + padding;
        outSampleCount = bestCount;
        outLengthAxis = bestAxis;
        return true;
    }

    inline bool BuildMagazineBoxLocalBoundsFromBoneSamples(
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int magazineBone,
        const vr_vm_stabilize::Mat3x4& magazineWorld,
        const Vector& fallbackHalf,
        const Vector& padding,
        const Vector& insertionAxisLocal,
        Vector& outMins,
        Vector& outMaxs,
        int& outSampleCount,
        int& outLengthAxis)
    {
        outSampleCount = 0;
        outLengthAxis = HooksDominantAxis(insertionAxisLocal);
        if (!sourceBones || numBones <= 0 || magazineBone < 0 || magazineBone >= numBones)
            return false;

        Vector sampleMins(FLT_MAX, FLT_MAX, FLT_MAX);
        Vector sampleMaxs(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int depth = HooksBoneDescendantDepth(boneParents, bone, magazineBone);
            if (depth < 0 || depth > 6)
                continue;

            if (depth > 0)
            {
                const std::string lowerBoneName =
                    bone < static_cast<int>(boneNames.size())
                    ? vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)])
                    : std::string();
                if (!HooksMagazineBoxCanSampleBoneName(lowerBoneName, depth))
                    continue;
            }

            vr_vm_stabilize::Mat3x4 boneWorld{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, boneWorld))
                continue;

            const Vector local = HooksInverseTransformPoint(magazineWorld, vr_vm_stabilize::GetOrigin(boneWorld));
            sampleMins.x = std::min(sampleMins.x, local.x);
            sampleMins.y = std::min(sampleMins.y, local.y);
            sampleMins.z = std::min(sampleMins.z, local.z);
            sampleMaxs.x = std::max(sampleMaxs.x, local.x);
            sampleMaxs.y = std::max(sampleMaxs.y, local.y);
            sampleMaxs.z = std::max(sampleMaxs.z, local.z);
            ++outSampleCount;
        }

        if (outSampleCount < 2)
            return false;

        const float ranges[3] = {
            sampleMaxs.x - sampleMins.x,
            sampleMaxs.y - sampleMins.y,
            sampleMaxs.z - sampleMins.z
        };
        if (ranges[0] > 64.0f || ranges[1] > 64.0f || ranges[2] > 64.0f)
            return false;

        int dominantAxis = 0;
        if (ranges[1] > ranges[dominantAxis])
            dominantAxis = 1;
        if (ranges[2] > ranges[dominantAxis])
            dominantAxis = 2;

        const float minUsefulSpan = std::max(0.005f * 43.2f, 0.002f * std::max(1.0f, fallbackHalf.Length()));
        const int preferredAxis = HooksDominantAxis(insertionAxisLocal);
        outLengthAxis = (ranges[preferredAxis] >= minUsefulSpan) ? preferredAxis : dominantAxis;
        if (ranges[outLengthAxis] < minUsefulSpan)
            return false;

        outMins = Vector(0.0f, 0.0f, 0.0f);
        outMaxs = Vector(0.0f, 0.0f, 0.0f);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float center = (sampleMins[axis] + sampleMaxs[axis]) * 0.5f;
            const float sampledHalf = ranges[axis] * 0.5f;
            float half = fallbackHalf[axis] + padding[axis];
            if (axis == outLengthAxis)
                half = std::max(sampledHalf + std::max(padding[axis], fallbackHalf[axis] * 0.5f), fallbackHalf[axis] * 0.65f);
            else if (ranges[axis] >= minUsefulSpan)
                half = std::max(sampledHalf + padding[axis], fallbackHalf[axis] + padding[axis]);

            outMins[axis] = center - half;
            outMaxs[axis] = center + half;
        }

        return true;
    }

    inline void DrawMagazineBoxSolidQuad(
        IVDebugOverlay* overlay,
        const Vector& a,
        const Vector& b,
        const Vector& c,
        const Vector& d,
        int r,
        int g,
        int bColor,
        int alpha,
        bool noDepthTest,
        float duration)
    {
        overlay->AddTriangleOverlay(a, b, c, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, c, d, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, c, b, r, g, bColor, alpha, noDepthTest, duration);
        overlay->AddTriangleOverlay(a, d, c, r, g, bColor, alpha, noDepthTest, duration);
    }

    inline void DrawMagazineBoxSolidObb(
        IVDebugOverlay* overlay,
        const vr_vm_stabilize::Mat3x4& world,
        const Vector& mins,
        const Vector& maxs,
        int r,
        int g,
        int bColor,
        int alpha,
        bool noDepthTest,
        float duration)
    {
        if (!overlay)
            return;

        Vector corners[8];
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = 0; y <= 1; ++y)
            {
                for (int x = 0; x <= 1; ++x)
                {
                    const int index = x | (y << 1) | (z << 2);
                    const Vector local(
                        x ? maxs.x : mins.x,
                        y ? maxs.y : mins.y,
                        z ? maxs.z : mins.z);
                    corners[index] = HooksTransformPoint(world, local);
                }
            }
        }

        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[1], corners[3], corners[2], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[4], corners[6], corners[7], corners[5], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[4], corners[5], corners[1], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[2], corners[3], corners[7], corners[6], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[0], corners[2], corners[6], corners[4], r, g, bColor, alpha, noDepthTest, duration);
        DrawMagazineBoxSolidQuad(overlay, corners[1], corners[5], corners[7], corners[3], r, g, bColor, alpha, noDepthTest, duration);
    }

    inline int FindConfiguredViewmodelBoneOverride(
        const char* logTag,
        const char* role,
        const char* configName,
        int weaponId,
        const std::unordered_map<int, std::vector<std::string>>& overrides,
        const std::string& overrideSpec,
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        std::string& outConfiguredName)
    {
        outConfiguredName.clear();
        if (weaponId <= 0)
            return -1;

        const auto overrideIt = overrides.find(weaponId);
        if (overrideIt == overrides.end() || overrideIt->second.empty())
            return -1;

        for (const std::string& requestedName : overrideIt->second)
        {
            const std::string requestedLower = vr_vm_stabilize::ToLowerAscii(requestedName);
            if (requestedLower.empty())
                continue;

            for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
            {
                if (vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]) != requestedLower)
                    continue;

                outConfiguredName = requestedName;

                static std::mutex s_overrideLogMutex;
                static std::unordered_set<std::string> s_loggedMatches;
                const std::string logKey =
                    std::string(logTag ? logTag : "VR") + "|match|" +
                    (role ? role : "bone") + "|" +
                    std::to_string(weaponId) + "|" + modelName + "|" + requestedLower;
                bool shouldLog = false;
                {
                    std::lock_guard<std::mutex> lock(s_overrideLogMutex);
                    shouldLog = s_loggedMatches.insert(logKey).second;
                }
                if (shouldLog)
                {
                    Game::logMsg(
                        "[VR][%s] configured %s bone override matched source=%s weaponId=%d model=%s bone=%d name=%s",
                        logTag ? logTag : "Unknown",
                        role ? role : "viewmodel",
                        configName ? configName : "unknown",
                        weaponId,
                        modelName.c_str(),
                        bone,
                        boneNames[static_cast<size_t>(bone)].c_str());
                }
                return bone;
            }
        }

        static std::mutex s_overrideMissLogMutex;
        static std::unordered_set<std::string> s_loggedMisses;
        const std::string missKey =
            std::string(logTag ? logTag : "VR") + "|miss|" +
            (role ? role : "bone") + "|" +
            std::to_string(weaponId) + "|" + modelName + "|" + overrideSpec;
        bool shouldLogMiss = false;
        {
            std::lock_guard<std::mutex> lock(s_overrideMissLogMutex);
            shouldLogMiss = s_loggedMisses.insert(missKey).second;
        }
        if (shouldLogMiss)
        {
            Game::logMsg(
                "[VR][%s] configured %s bone override not found; falling back to automatic detection source=%s weaponId=%d model=%s spec=%s",
                logTag ? logTag : "Unknown",
                role ? role : "viewmodel",
                configName ? configName : "unknown",
                weaponId,
                modelName.c_str(),
                overrideSpec.c_str());
        }
        return -1;
    }

    inline void DrawCurrentWeaponMagazineBox(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        int entityIndex,
        int hitboxSet,
        const void* pModelToWorld,
        const void* pCustomBoneToWorld)
    {
        if (!vr || !drawState || !pCustomBoneToWorld || modelName.empty())
        {
            return;
        }

        const bool wantsMagazineBox = vr->m_MagazineInteractionEnabled || vr->m_MagazineBoxDebugEnabled;
        if (!wantsMagazineBox)
            return;
        constexpr bool drawDebugBox = false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel) || HooksModelNameIsArmsOrHands(lowerModel))
            return;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }

        const int currentMagazineInteractionWeaponId =
            vr->m_MagazineInteractionCurrentWeaponId.load(std::memory_order_relaxed);
        const int inferredModelWeaponId = MagazineInteractionInferWeaponIdFromViewmodelModelName(lowerModel);
        const int magazineInteractionWeaponId = inferredModelWeaponId > 0
            ? inferredModelWeaponId
            : currentMagazineInteractionWeaponId > 0
                ? currentMagazineInteractionWeaponId
                : vr->m_MagazineInteractionWeaponId;
        if (inferredModelWeaponId > 0 &&
            currentMagazineInteractionWeaponId > 0 &&
            inferredModelWeaponId != currentMagazineInteractionWeaponId)
        {
            static std::mutex s_weaponIdOverrideLogMutex;
            static std::unordered_set<std::string> s_loggedWeaponIdOverrides;
            const std::string logKey =
                lowerModel + "|" +
                std::to_string(currentMagazineInteractionWeaponId) + "|" +
                std::to_string(inferredModelWeaponId);
            bool shouldLog = false;
            {
                std::lock_guard<std::mutex> lock(s_weaponIdOverrideLogMutex);
                shouldLog = s_loggedWeaponIdOverrides.insert(logKey).second;
            }
            if (shouldLog)
            {
                Game::logMsg(
                    "[VR][MagazineBox] viewmodel model weaponId overrides stale current weaponId model=%s currentWeaponId=%d modelWeaponId=%d",
                    lowerModel.c_str(),
                    currentMagazineInteractionWeaponId,
                    inferredModelWeaponId);
            }
        }
        std::string configuredMagazineBoneName;
        int magazineBone = FindConfiguredViewmodelBoneOverride(
            "MagazineInteraction",
            "magazine",
            "ManualReloadMagazineBoneOverrides",
            magazineInteractionWeaponId,
            vr->m_ManualReloadMagazineBoneOverrides,
            vr->m_ManualReloadMagazineBoneOverridesSpec,
            modelName,
            boneNames,
            configuredMagazineBoneName);
        if (magazineBone < 0 && MagazineInteractionWeaponIdIsShotgun(magazineInteractionWeaponId))
            magazineBone = FindMagazineInteractionShotgunShellBone(lowerModel, boneNames);
        if (magazineBone < 0)
            magazineBone = FindMagazineBoxBone(boneNames);
        if (magazineBone < 0)
            magazineBone = FindMagazineBoxOfficialProfileFallbackBone(lowerModel, boneNames);
        if (magazineBone < 0 || magazineBone >= numBones)
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        vr_vm_stabilize::Mat3x4 magazineWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + magazineBone, magazineWorld))
            return;

        const std::string magazineBoneName =
            (magazineBone < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(magazineBone)].empty())
            ? boneNames[static_cast<size_t>(magazineBone)]
            : std::string();
        const std::string lowerMagazineBoneName = vr_vm_stabilize::ToLowerAscii(magazineBoneName);
        const bool magazineInteractionIsShotgun =
            MagazineInteractionWeaponIdIsShotgun(magazineInteractionWeaponId);
        const bool magazineBoneUsesStockProfileAxes =
            ManualReloadNameIsLegacyValveBipedClip(lowerMagazineBoneName) ||
            (magazineInteractionIsShotgun &&
                MagazineInteractionShotgunShellBoneUsesStockProfileAxes(lowerMagazineBoneName));

        const uint32_t frameSeq = vr->m_RenderFrameSeq.load(std::memory_order_relaxed);
        if (frameSeq != 0)
        {
            static std::mutex s_drawMutex;
            static std::unordered_map<std::string, uint32_t> s_lastDrawSeq;
            const std::string key = std::to_string(entityIndex) + "|" + lowerModel;
            std::lock_guard<std::mutex> lock(s_drawMutex);
            uint32_t& lastSeq = s_lastDrawSeq[key];
            if (lastSeq == frameSeq)
                return;
            lastSeq = frameSeq;
        }

        const Vector fallbackHalf(
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.x) * vr->m_VRScale,
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.y) * vr->m_VRScale,
            std::max(0.001f, vr->m_MagazineBoxDebugFallbackHalfExtentsMeters.z) * vr->m_VRScale);
        const Vector padding(
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.x) * vr->m_VRScale,
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.y) * vr->m_VRScale,
            std::max(0.0f, vr->m_MagazineBoxDebugPaddingMeters.z) * vr->m_VRScale);

        const Vector insertionAxisLocal = HooksNormalizeVector(
            vr->m_ManualReloadMagazineInsertionAxisLocal,
            Vector(0.0f, -1.0f, 0.0f));
        const float magazineLengthHalf = std::max(fallbackHalf.x, std::max(fallbackHalf.y, fallbackHalf.z));
        int sampleCount = 0;
        int lengthAxis = HooksDominantAxis(insertionAxisLocal);
        Vector mins;
        Vector maxs;
        const char* boundsSource = "fallback";
        auto useFallbackBounds = [&]()
        {
            sampleCount = 1;
            lengthAxis = HooksDominantAxis(insertionAxisLocal);
            const Vector centerLocal = insertionAxisLocal * magazineLengthHalf;
            mins = centerLocal - fallbackHalf - padding;
            maxs = centerLocal + fallbackHalf + padding;
            boundsSource = "fallback";
        };
        if (magazineBoneUsesStockProfileAxes &&
            TryGetOfficialMagazineBoxProfile(
            lowerModel,
            padding,
            mins,
            maxs,
            sampleCount,
            lengthAxis))
        {
            boundsSource = "official";
        }
        else if (BuildMagazineBoxLocalBoundsFromHitboxes(
            drawState,
            hitboxSet,
            numBonesOffset,
            boneNames,
            boneParents,
            sourceBones,
            numBones,
            magazineBone,
            magazineWorld,
            padding,
            mins,
            maxs,
            sampleCount,
            lengthAxis))
        {
            boundsSource = "hitbox";
        }
        else if (BuildMagazineBoxLocalBoundsFromBoneSamples(
            boneNames,
            boneParents,
            sourceBones,
            numBones,
            magazineBone,
            magazineWorld,
            fallbackHalf,
            padding,
            insertionAxisLocal,
            mins,
            maxs,
            sampleCount,
            lengthAxis))
        {
            boundsSource = "bones";
        }
        else
        {
            useFallbackBounds();
        }

        if (std::strcmp(boundsSource, "official") != 0)
        {
            const Vector span = maxs - mins;
            const int clampedLengthAxis = std::clamp(lengthAxis, 0, 2);
            const float maxAllowed[3] =
            {
                std::max(fallbackHalf.x * 2.4f, ((clampedLengthAxis == 0) ? 0.18f : 0.035f) * vr->m_VRScale),
                std::max(fallbackHalf.y * 2.4f, ((clampedLengthAxis == 1) ? 0.18f : 0.035f) * vr->m_VRScale),
                std::max(fallbackHalf.z * 2.4f, ((clampedLengthAxis == 2) ? 0.18f : 0.035f) * vr->m_VRScale)
            };
            if (span.x > maxAllowed[0] || span.y > maxAllowed[1] || span.z > maxAllowed[2])
            {
                const char* rejectedSource = boundsSource;
                useFallbackBounds();
                boundsSource = "fallback-clamped";
                static std::mutex s_clampLogMutex;
                static std::unordered_map<std::string, bool> s_clampLoggedModels;
                std::lock_guard<std::mutex> lock(s_clampLogMutex);
                if (s_clampLoggedModels.emplace(lowerModel, true).second)
                {
                    Game::logMsg(
                        "[VR][MagazineBox] rejected oversized %s bounds model=%s span=(%.2f %.2f %.2f) allowed=(%.2f %.2f %.2f)",
                        rejectedSource,
                        lowerModel.c_str(),
                        span.x,
                        span.y,
                        span.z,
                        maxAllowed[0],
                        maxAllowed[1],
                        maxAllowed[2]);
                }
            }
        }

        vr_vm_stabilize::Mat3x4 boxWorld = magazineWorld;
        int basisBone = magazineBone;
        const char* basisSource = "magazine-bone";
        const bool officialCustomMagazineBone =
            std::strcmp(boundsSource, "official") == 0 &&
            !magazineBoneUsesStockProfileAxes;
        if (officialCustomMagazineBone)
        {
            const int directOffsetAnchorBone = FindMagazineBoxDirectOffsetAnchorBone(
                boneNames,
                boneParents,
                magazineBone);
            if (directOffsetAnchorBone >= 0 && directOffsetAnchorBone < numBones)
            {
                vr_vm_stabilize::Mat3x4 anchorWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + directOffsetAnchorBone, anchorWorld))
                {
                    vr_vm_stabilize::Mat3x4 directOffsetBasisWorld{};
                    if (BuildMagazineBoxBasisFromParentOffset(
                        magazineWorld,
                        anchorWorld,
                        pModelToWorld,
                        insertionAxisLocal,
                        lengthAxis,
                        directOffsetBasisWorld))
                    {
                        boxWorld = directOffsetBasisWorld;
                        basisBone = directOffsetAnchorBone;
                        basisSource = "direct-parent-offset-axis";
                    }
                }
            }

            if (basisBone == magazineBone)
            {
                const int parentBasisBone = FindMagazineBoxParentBasisBone(boneNames, boneParents, magazineBone);
                if (parentBasisBone >= 0 && parentBasisBone < numBones)
                {
                    vr_vm_stabilize::Mat3x4 parentWorld{};
                    if (vr_vm_stabilize::SafeRead(sourceBones + parentBasisBone, parentWorld))
                    {
                        vr_vm_stabilize::Mat3x4 parentOffsetBasisWorld{};
                        if (BuildMagazineBoxBasisFromParentOffset(
                            magazineWorld,
                            parentWorld,
                            pModelToWorld,
                            insertionAxisLocal,
                            lengthAxis,
                            parentOffsetBasisWorld))
                        {
                            boxWorld = parentOffsetBasisWorld;
                            basisBone = parentBasisBone;
                            basisSource = "parent-offset-axis";
                        }
                        else
                        {
                            boxWorld = parentWorld;
                            boxWorld.m[0][3] = magazineWorld.m[0][3];
                            boxWorld.m[1][3] = magazineWorld.m[1][3];
                            boxWorld.m[2][3] = magazineWorld.m[2][3];
                            basisBone = parentBasisBone;
                            basisSource = "parent-axes";
                        }
                    }
                }
            }

            if (basisBone == magazineBone)
            {
                const int legacyClipBasisBone = FindMagazineBoxLegacyClipBone(boneNames);
                if (legacyClipBasisBone >= 0 &&
                    legacyClipBasisBone < numBones &&
                    legacyClipBasisBone != magazineBone)
                {
                    vr_vm_stabilize::Mat3x4 legacyClipWorld{};
                    if (vr_vm_stabilize::SafeRead(sourceBones + legacyClipBasisBone, legacyClipWorld))
                    {
                        boxWorld = legacyClipWorld;
                        boxWorld.m[0][3] = magazineWorld.m[0][3];
                        boxWorld.m[1][3] = magazineWorld.m[1][3];
                        boxWorld.m[2][3] = magazineWorld.m[2][3];
                        basisBone = legacyClipBasisBone;
                        basisSource = "legacy-clip-axes";
                    }
                }
            }
        }

        vr_vm_stabilize::Mat3x4 modelWorld{};
        bool modelBasisValid =
            pModelToWorld != nullptr &&
            vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pModelToWorld),
                modelWorld);

        if (magazineInteractionIsShotgun)
        {
            const int stableAnchorBone = FindMagazineInteractionShotgunStableAnchorBone(
                boneNames,
                boneParents,
                magazineBone);
            if (stableAnchorBone >= 0 && stableAnchorBone < numBones)
            {
                vr_vm_stabilize::Mat3x4 stableAnchorWorld{};
                if (vr_vm_stabilize::SafeRead(sourceBones + stableAnchorBone, stableAnchorWorld))
                {
                    modelWorld = stableAnchorWorld;
                    modelBasisValid = true;

                    static std::mutex s_shotgunAnchorLogMutex;
                    static std::unordered_set<std::string> s_loggedShotgunAnchors;
                    const std::string stableAnchorName =
                        (stableAnchorBone < static_cast<int>(boneNames.size()))
                        ? boneNames[static_cast<size_t>(stableAnchorBone)]
                        : std::string();
                    const std::string logKey =
                        lowerModel + "|" +
                        std::to_string(magazineBone) + "|" +
                        std::to_string(stableAnchorBone);
                    bool shouldLog = false;
                    {
                        std::lock_guard<std::mutex> lock(s_shotgunAnchorLogMutex);
                        shouldLog = s_loggedShotgunAnchors.insert(logKey).second;
                    }
                    if (shouldLog)
                    {
                        Game::logMsg(
                            "[VR][MagazineBox] shotgun stable socket anchor model=%s shellBone=%d shellName=%s anchorBone=%d anchorName=%s",
                            lowerModel.c_str(),
                            magazineBone,
                            magazineBoneName.c_str(),
                            stableAnchorBone,
                            stableAnchorName.c_str());
                    }
                }
            }
        }

        vr->PublishMagazineInteractionBox(
            Vector(boxWorld.m[0][3], boxWorld.m[1][3], boxWorld.m[2][3]),
            Vector(boxWorld.m[0][0], boxWorld.m[1][0], boxWorld.m[2][0]),
            Vector(boxWorld.m[0][1], boxWorld.m[1][1], boxWorld.m[2][1]),
            Vector(boxWorld.m[0][2], boxWorld.m[1][2], boxWorld.m[2][2]),
            mins,
            maxs,
            frameSeq,
            entityIndex,
            magazineBone,
            modelName.c_str(),
            modelBasisValid,
            Vector(modelWorld.m[0][3], modelWorld.m[1][3], modelWorld.m[2][3]),
            Vector(modelWorld.m[0][0], modelWorld.m[1][0], modelWorld.m[2][0]),
            Vector(modelWorld.m[0][1], modelWorld.m[1][1], modelWorld.m[2][1]),
            Vector(modelWorld.m[0][2], modelWorld.m[1][2], modelWorld.m[2][2]));

        std::string configuredBoltBoneName;
        int boltBone = FindConfiguredViewmodelBoneOverride(
            "MagazineInteraction",
            "bolt",
            "MagazineInteractionBoltBoneOverrides",
            magazineInteractionWeaponId,
            vr->m_MagazineInteractionBoltBoneOverrides,
            vr->m_MagazineInteractionBoltBoneOverridesSpec,
            modelName,
            boneNames,
            configuredBoltBoneName);
        if (boltBone < 0)
            boltBone = FindMagazineInteractionBoltBone(
                magazineInteractionWeaponId,
                lowerModel,
                boneNames);
        if (boltBone >= 0 && boltBone < numBones)
        {
            vr_vm_stabilize::Mat3x4 boltWorld{};
            if (vr_vm_stabilize::SafeRead(sourceBones + boltBone, boltWorld))
            {
                const Vector boltHalf(
                    std::max(0.005f, vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.x) * vr->m_VRScale,
                    std::max(0.005f, vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.y) * vr->m_VRScale,
                    std::max(0.005f, vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.z) * vr->m_VRScale);
                const Vector boltLocalOffset(
                    vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.x * vr->m_VRScale,
                    vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.y * vr->m_VRScale,
                    vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.z * vr->m_VRScale);
                const Vector pullAxisWorld = BuildMagazineInteractionBoltPullAxisWorld(
                    vr,
                    modelName,
                    sourceBones,
                    numBones,
                    boneParents,
                    boltBone,
                    boltWorld,
                    pModelToWorld);
                vr->PublishMagazineInteractionBoltBox(
                    Vector(boltWorld.m[0][3], boltWorld.m[1][3], boltWorld.m[2][3]),
                    Vector(boltWorld.m[0][0], boltWorld.m[1][0], boltWorld.m[2][0]),
                    Vector(boltWorld.m[0][1], boltWorld.m[1][1], boltWorld.m[2][1]),
                    Vector(boltWorld.m[0][2], boltWorld.m[1][2], boltWorld.m[2][2]),
                    boltLocalOffset - boltHalf,
                    boltLocalOffset + boltHalf,
                    pullAxisWorld,
                    frameSeq,
                    entityIndex,
                    boltBone,
                    modelName.c_str());

                {
                    static std::mutex s_boltBoxLogMutex;
                    static std::unordered_set<std::string> s_loggedBoltBoxes;
                    const char* boltBoneName =
                        (static_cast<size_t>(boltBone) < boneNames.size())
                        ? boneNames[static_cast<size_t>(boltBone)].c_str()
                        : "";
                    char key[256] = {};
                    std::snprintf(
                        key,
                        sizeof(key),
                        "%s|%d|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f",
                        lowerModel.c_str(),
                        boltBone,
                        vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.x,
                        vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.y,
                        vr->m_MagazineInteractionBoltBoxHalfExtentsMeters.z,
                        vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.x,
                        vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.y,
                        vr->m_MagazineInteractionBoltBoxLocalOffsetMeters.z);
                    bool shouldLog = false;
                    {
                        std::lock_guard<std::mutex> lock(s_boltBoxLogMutex);
                        shouldLog = s_loggedBoltBoxes.insert(key).second;
                    }
                    if (shouldLog)
                    {
                        Game::logMsg(
                            "[VR][MagazineBolt] drawing bolt box model=%s bone=%d name=%s mins=(%.2f %.2f %.2f) maxs=(%.2f %.2f %.2f) pullAxis=(%.3f %.3f %.3f)",
                            modelName.c_str(),
                            boltBone,
                            boltBoneName,
                            boltLocalOffset.x - boltHalf.x,
                            boltLocalOffset.y - boltHalf.y,
                            boltLocalOffset.z - boltHalf.z,
                            boltLocalOffset.x + boltHalf.x,
                            boltLocalOffset.y + boltHalf.y,
                            boltLocalOffset.z + boltHalf.z,
                            pullAxisWorld.x,
                            pullAxisWorld.y,
                            pullAxisWorld.z);
                    }
                }
            }
        }

        if (drawDebugBox)
        {
            const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.0f);
            constexpr int kR = 255;
            constexpr int kG = 72;
            constexpr int kB = 24;
            constexpr int kA = 220;
            constexpr bool kNoDepthTest = true;
            IVDebugOverlay* overlay = vr->m_Game->m_DebugOverlay;
            DrawMagazineBoxSolidObb(overlay, boxWorld, mins, maxs, kR, kG, kB, kA, kNoDepthTest, duration);
        }

        static std::mutex s_logMutex;
        static std::unordered_map<std::string, std::string> s_loggedSignatureByModel;
        {
            std::lock_guard<std::mutex> lock(s_logMutex);
            const std::string logSignature =
                std::to_string(magazineBone) + "|" +
                std::to_string(basisBone) + "|" +
                (basisSource ? basisSource : "unknown") + "|" +
                (boundsSource ? boundsSource : "unknown");
            auto it = s_loggedSignatureByModel.find(lowerModel);
            if (it == s_loggedSignatureByModel.end() || it->second != logSignature)
            {
                s_loggedSignatureByModel[lowerModel] = logSignature;
                const char* boneName =
                    !magazineBoneName.empty()
                    ? magazineBoneName.c_str()
                    : "<unnamed>";
                const char* basisName =
                    (basisBone >= 0 &&
                        basisBone < static_cast<int>(boneNames.size()) &&
                        !boneNames[static_cast<size_t>(basisBone)].empty())
                    ? boneNames[static_cast<size_t>(basisBone)].c_str()
                    : "<unnamed>";
                Game::logMsg(
                    "[VR][MagazineBox] drawing solid magazine box model=%s bone=%d name=%s basisBone=%d basisName=%s basis=%s source=%s samples=%d axis=%d mins=(%.2f %.2f %.2f) maxs=(%.2f %.2f %.2f)",
                    modelName.c_str(),
                    magazineBone,
                    boneName,
                    basisBone,
                    basisName,
                    basisSource,
                    boundsSource,
                    sampleCount,
                    lengthAxis,
                    mins.x,
                    mins.y,
                    mins.z,
                    maxs.x,
                    maxs.y,
                    maxs.z);
            }
        }
    }

    inline int FindManualReloadConfiguredMagazineBone(
        const VR* vr,
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        std::string& outConfiguredName)
    {
        outConfiguredName.clear();
        if (!vr || vr->m_ManualReloadWeaponId <= 0)
            return -1;

        const auto overrideIt = vr->m_ManualReloadMagazineBoneOverrides.find(vr->m_ManualReloadWeaponId);
        if (overrideIt == vr->m_ManualReloadMagazineBoneOverrides.end() || overrideIt->second.empty())
            return -1;

        for (const std::string& requestedName : overrideIt->second)
        {
            const std::string requestedLower = vr_vm_stabilize::ToLowerAscii(requestedName);
            if (requestedLower.empty())
                continue;

            for (int bone = 0; bone < static_cast<int>(boneNames.size()); ++bone)
            {
                if (vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(bone)]) != requestedLower)
                    continue;

                outConfiguredName = requestedName;
                Game::logMsg(
                    "[VR][ManualReload] configured magazine bone override matched weaponId=%d model=%s bone=%d name=%s",
                    vr->m_ManualReloadWeaponId,
                    modelName.c_str(),
                    bone,
                    boneNames[static_cast<size_t>(bone)].c_str());
                return bone;
            }
        }

        static std::mutex s_overrideMissLogMutex;
        static std::unordered_map<std::string, bool> s_overrideMissLogged;
        const std::string logKey = std::to_string(vr->m_ManualReloadWeaponId) + "|" + modelName + "|" + vr->m_ManualReloadMagazineBoneOverridesSpec;
        {
            std::lock_guard<std::mutex> lock(s_overrideMissLogMutex);
            if (s_overrideMissLogged.emplace(logKey, true).second)
            {
                Game::logMsg(
                    "[VR][ManualReload] configured magazine bone override not found; falling back to automatic detection weaponId=%d model=%s spec=%s",
                    vr->m_ManualReloadWeaponId,
                    modelName.c_str(),
                    vr->m_ManualReloadMagazineBoneOverridesSpec.c_str());
            }
        }
        return -1;
    }

    inline bool ManualReloadModelMatchesLockedMagazine(const VR* vr, const std::string& modelName)
    {
        if (!vr || vr->m_ManualReloadMagazineBoneIndex < 0 || vr->m_ManualReloadMagazineModelName.empty())
            return false;
        return vr_vm_stabilize::ToLowerAscii(vr->m_ManualReloadMagazineModelName) ==
            vr_vm_stabilize::ToLowerAscii(modelName);
    }

    inline int GetManualReloadLockedMagazineBone(const VR* vr, const std::string& modelName, int numBones)
    {
        if (!ManualReloadModelMatchesLockedMagazine(vr, modelName))
            return -1;
        const int bone = vr->m_ManualReloadMagazineBoneIndex;
        return (bone >= 0 && bone < numBones) ? bone : -1;
    }

    inline int GetManualReloadLockedMagazineMotionBone(const VR* vr, const std::string& modelName, int numBones)
    {
        if (!ManualReloadModelMatchesLockedMagazine(vr, modelName))
            return -1;
        const int motionBone = vr->m_ManualReloadMagazineMotionBoneIndex;
        if (motionBone >= 0 && motionBone < numBones)
            return motionBone;
        return GetManualReloadLockedMagazineBone(vr, modelName, numBones);
    }

    inline bool ShouldDrawManualReloadNativeMagazine(const VR* vr)
    {
        // The detached Source-material copy exists only while the fresh magazine is held.
        // Once insertion completes, the native magazine is restored immediately.
        return vr && vr->m_ManualReloadState == ManualReloadState::HoldingFreshMagazine;
    }

    struct MagazineInteractionDetachedMagazinePoseCache
    {
        VR* owner = nullptr;
        std::string modelName;
        int clipBone = -1;
        int numBones = 0;
        std::vector<vr_vm_stabilize::Mat3x4> sourceBones;

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            clipBone = -1;
            numBones = 0;
            sourceBones.clear();
        }
    };

    inline MagazineInteractionDetachedMagazinePoseCache& GetMagazineInteractionDetachedMagazinePoseCache()
    {
        static MagazineInteractionDetachedMagazinePoseCache cache;
        return cache;
    }

    inline void LogMagazineInteractionDetachedDrawSkip(const char* reason, const std::string& modelName = std::string())
    {
        const std::string reasonText = reason ? reason : "unknown";
        if (reasonText == "not-viewmodel" || reasonText == "model-mismatch")
            return;

        static std::mutex s_skipLogMutex;
        static std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_lastSkipLog;
        const auto now = std::chrono::steady_clock::now();
        const std::string key = reasonText + "|" + modelName;
        std::lock_guard<std::mutex> lock(s_skipLogMutex);
        auto& last = s_lastSkipLog[key];
        if (last.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(now - last).count() < 1.0f)
        {
            return;
        }
        last = now;
        Game::logMsg(
            "[VR][MagazineInteraction] detached magazine draw skipped reason=%s model=%s",
            reasonText.c_str(),
            modelName.empty() ? "<none>" : modelName.c_str());
    }

    inline bool BuildDetachedSourceMagazineBones(
        const char* logPrefix,
        const std::string& modelName,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        int clipBone,
        const VrHandMatrix4& targetMagazineWorld,
        const vr_vm_stabilize::Mat3x4* cachedMagazineBones,
        uint32_t renderFrameSeq,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!sourceBones)
            return false;
        if (numBones <= 0 || numBones > 512 || clipBone < 0 || clipBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
            return false;

        const vr_vm_stabilize::Mat3x4* magazineSourceBones = cachedMagazineBones ? cachedMagazineBones : sourceBones;
        vr_vm_stabilize::Mat3x4 originalClipWorld{};
        if (!vr_vm_stabilize::SafeRead(magazineSourceBones + clipBone, originalClipWorld))
            return false;

        vr_vm_stabilize::Mat3x4 inverseOriginalClip{};
        vr_vm_stabilize::Mat3x4 targetClipWorld = HooksVrHandMatrixToMat3x4(
            HooksStripVrHandMatrixScale(targetMagazineWorld));
        vr_vm_stabilize::Mat3x4 targetDelta{};
        vr_vm_stabilize::InvertTR(originalClipWorld, inverseOriginalClip);
        vr_vm_stabilize::Mul(targetClipWorld, inverseOriginalClip, targetDelta);

        uint32_t seqEven = renderFrameSeq & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* isolatedBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!isolatedBones)
            return false;

        auto isClipOrDescendant = [&](int bone)
            {
                int current = bone;
                for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                {
                    if (current == clipBone)
                        return true;
                    current = boneParents[static_cast<size_t>(current)];
                }
                return false;
            };

        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;

            if (isClipOrDescendant(bone))
            {
                if (!vr_vm_stabilize::SafeRead(magazineSourceBones + bone, source))
                    return false;
                vr_vm_stabilize::Mul(targetDelta, source, isolatedBones[bone]);
            }
            else
            {
                isolatedBones[bone] = source;
                isolatedBones[bone].m[0][3] += 100000.0f;
                isolatedBones[bone].m[1][3] += 100000.0f;
                isolatedBones[bone].m[2][3] += 100000.0f;
            }
        }

        static std::mutex s_detachedSourceMagazineLogMutex;
        static std::unordered_set<std::string> s_detachedSourceMagazineLoggedModels;
        {
            std::lock_guard<std::mutex> lock(s_detachedSourceMagazineLogMutex);
            const std::string key = std::string(logPrefix ? logPrefix : "Magazine") + "|" + modelName + "|" + std::to_string(clipBone);
            if (s_detachedSourceMagazineLoggedModels.emplace(key).second)
            {
                Game::logMsg(
                    "[VR][%s] drawing detached magazine through Source viewmodel shader model=%s clipBone=%d cached=%d",
                    logPrefix ? logPrefix : "Magazine",
                    modelName.c_str(),
                    clipBone,
                    cachedMagazineBones ? 1 : 0);
            }
        }

        outBones = isolatedBones;
        return true;
    }

    inline bool BuildManualReloadNativeMagazineBones(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const void* pCustomBoneToWorld,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!vr || !ShouldDrawManualReloadNativeMagazine(vr) || !drawState || !pCustomBoneToWorld)
            return false;
        if (!vr->m_Game || vr->m_Game->GetMatQueueMode() != 0)
            return false;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel))
            return false;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return false;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return false;

        const int clipBone = GetManualReloadLockedMagazineBone(vr, modelName, numBones);
        if (clipBone < 0)
            return false;

        VrHandMatrix4 targetMagazineWorld{};
        if (!vr->GetManualReloadMagazineWorld(targetMagazineWorld))
            return false;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        return BuildDetachedSourceMagazineBones(
            "ManualReload",
            modelName,
            boneParents,
            sourceBones,
            numBones,
            clipBone,
            targetMagazineWorld,
            nullptr,
            vr->m_RenderFrameSeq.load(std::memory_order_relaxed),
            outBones);
    }

    inline bool BuildMagazineInteractionDetachedMagazineBones(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const void* pCustomBoneToWorld,
        vr_vm_stabilize::Mat3x4*& outBones)
    {
        outBones = nullptr;
        if (!vr || !vr->ShouldDrawMagazineInteractionDetachedMagazine() || !drawState || !pCustomBoneToWorld)
        {
            if (vr && vr->IsMagazineInteractionManualActive())
                LogMagazineInteractionDetachedDrawSkip("inactive-or-missing-draw-input", modelName);
            return false;
        }
        if (!vr->m_Game || vr->m_Game->GetMatQueueMode() != 0)
        {
            LogMagazineInteractionDetachedDrawSkip("mat-queue-not-single-thread", modelName);
            return false;
        }

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel))
        {
            LogMagazineInteractionDetachedDrawSkip("not-viewmodel", modelName);
            return false;
        }
        if (vr->m_MagazineInteractionMagazineModelName.empty() ||
            vr_vm_stabilize::ToLowerAscii(vr->m_MagazineInteractionMagazineModelName) != lowerModel)
        {
            LogMagazineInteractionDetachedDrawSkip("model-mismatch", modelName);
            return false;
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            LogMagazineInteractionDetachedDrawSkip("collect-bones-failed", modelName);
            return false;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneParents.size()) < numBones)
        {
            LogMagazineInteractionDetachedDrawSkip("invalid-bone-count", modelName);
            return false;
        }

        const int clipBone = vr->m_MagazineInteractionMagazineBoneIndex;
        if (clipBone < 0 || clipBone >= numBones)
        {
            LogMagazineInteractionDetachedDrawSkip("invalid-clip-bone", modelName);
            return false;
        }

        MagazineInteractionDetachedMagazinePoseCache& poseCache = GetMagazineInteractionDetachedMagazinePoseCache();
        const bool cacheMatches =
            poseCache.owner == vr &&
            poseCache.modelName == lowerModel &&
            poseCache.clipBone == clipBone &&
            poseCache.numBones == numBones &&
            static_cast<int>(poseCache.sourceBones.size()) == numBones;
        const bool refreshPoseCache =
            vr->m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine &&
            !vr->m_MagazineInteractionReloadTriggered;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        if (refreshPoseCache || !cacheMatches)
        {
            std::vector<vr_vm_stabilize::Mat3x4> capturedBones(static_cast<size_t>(numBones));
            bool captured = true;
            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!vr_vm_stabilize::SafeRead(sourceBones + bone, capturedBones[static_cast<size_t>(bone)]))
                {
                    captured = false;
                    break;
                }
            }
            if (captured)
            {
                poseCache.owner = vr;
                poseCache.modelName = lowerModel;
                poseCache.clipBone = clipBone;
                poseCache.numBones = numBones;
                poseCache.sourceBones.swap(capturedBones);
            }
            else if (!cacheMatches)
            {
                poseCache.Reset();
            }
        }

        const bool useCachedMagazinePose =
            poseCache.owner == vr &&
            poseCache.modelName == lowerModel &&
            poseCache.clipBone == clipBone &&
            poseCache.numBones == numBones &&
            static_cast<int>(poseCache.sourceBones.size()) == numBones;
        const vr_vm_stabilize::Mat3x4* magazineSourceBones = useCachedMagazinePose
            ? poseCache.sourceBones.data()
            : sourceBones;

        VrHandMatrix4 targetMagazineWorld{};
        if (!vr->GetMagazineInteractionDetachedMagazineWorld(targetMagazineWorld))
        {
            LogMagazineInteractionDetachedDrawSkip("missing-target-world", modelName);
            return false;
        }

        const char* logPrefix =
            (vr->m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
            ? "MagazineInteractionFresh"
            : "MagazineInteraction";
        const bool built = BuildDetachedSourceMagazineBones(
            logPrefix,
            modelName,
            boneParents,
            sourceBones,
            numBones,
            clipBone,
            targetMagazineWorld,
            useCachedMagazinePose ? magazineSourceBones : nullptr,
            vr->m_RenderFrameSeq.load(std::memory_order_relaxed),
            outBones);
        if (!built)
            LogMagazineInteractionDetachedDrawSkip("build-source-magazine-failed", modelName);
        return built;
    }

    inline void MaybeCaptureVrHandsVmPose(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& modelInfo,
        const void* pCustomBoneToWorld)
    {
        if (!vr || !vr->m_VrHandsEnabled || !vr->m_VrHandsRightUseViewmodelPose)
            return;
        if (!drawState || !pCustomBoneToWorld || !HooksModelNameIsArmsOrHands(modelName))
            return;

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        std::vector<VrHandMatrix4> boneWorldMatrices(static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return;
            boneWorldMatrices[static_cast<size_t>(bone)] = HooksMat3x4ToVrHandMatrix(source);
        }

        vr_vm_stabilize::Mat3x4 modelWorld{};
        vr_vm_stabilize::BuildFromOrgAngles(modelInfo.origin, modelInfo.angles, modelWorld);
        VrHandVmPose::Capture(
            modelName.c_str(),
            boneNames,
            boneParents,
            HooksMat3x4ToVrHandMatrix(modelWorld),
            boneWorldMatrices);
    }

    struct ManualReloadTailPoseSample
    {
        float timeSeconds = 0.0f;
        std::vector<vr_vm_stabilize::Mat3x4> localBones;
    };

    struct ManualReloadFrozenViewmodelPoseEntry
    {
        std::string modelName;
        int numBones = 0;
        int rootBone = -1;
        bool valid = false;
        bool hardLockModelAnchorValid = false;
        vr_vm_stabilize::Mat3x4 hardLockModelAnchor{};
        std::vector<vr_vm_stabilize::Mat3x4> frozenLocalBones;
        std::vector<ManualReloadTailPoseSample> tailSamples;
        std::chrono::steady_clock::time_point tailCaptureStarted{};
        std::chrono::steady_clock::time_point tailLastSample{};
    };

    struct ManualReloadFrozenViewmodelPoseCache
    {
        VR* owner = nullptr;
        std::unordered_map<std::string, ManualReloadFrozenViewmodelPoseEntry> models;

        void Reset()
        {
            owner = nullptr;
            models.clear();
        }
    };

    inline ManualReloadFrozenViewmodelPoseCache& GetManualReloadFrozenViewmodelPoseCache()
    {
        static ManualReloadFrozenViewmodelPoseCache cache;
        return cache;
    }

    inline bool IsManualReloadViewmodelVisualPauseState(const VR* vr)
    {
        return vr &&
            (vr->m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab ||
                vr->m_ManualReloadState == ManualReloadState::HoldingFreshMagazine ||
                vr->m_ManualReloadState == ManualReloadState::AwaitingNativePostInsertBoundary);
    }

    inline bool IsManualReloadViewmodelVisualReplayState(const VR* vr)
    {
        return vr && vr->m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithMagazine;
    }

    inline bool TryGetManualReloadModelAnchor(
        const ModelRenderInfo_t& info,
        vr_vm_stabilize::Mat3x4& outAnchor)
    {
        if (info.pModelToWorld &&
            vr_vm_stabilize::SafeRead(
                reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(info.pModelToWorld),
                outAnchor))
        {
            return true;
        }

        vr_vm_stabilize::BuildFromOrgAngles(info.origin, info.angles, outAnchor);
        return true;
    }

    inline int FindManualReloadTopLevelBone(const std::vector<int>& boneParents, int preferredBone)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (numBones <= 0)
            return -1;

        int rootBone = (preferredBone >= 0 && preferredBone < numBones) ? preferredBone : 0;
        for (int guard = 0; guard < numBones; ++guard)
        {
            const int parent = boneParents[static_cast<size_t>(rootBone)];
            if (parent < 0 || parent >= numBones || parent == rootBone)
                break;
            rootBone = parent;
        }
        return rootBone;
    }

    inline bool BuildManualReloadLocalBones(
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones,
        std::vector<vr_vm_stabilize::Mat3x4>& outLocalBones)
    {
        if (!sourceBones || numBones <= 0)
            return false;

        vr_vm_stabilize::Mat3x4 inverseAnchor{};
        vr_vm_stabilize::InvertTR(modelAnchor, inverseAnchor);
        outLocalBones.resize(static_cast<size_t>(numBones));
        for (int bone = 0; bone < numBones; ++bone)
        {
            vr_vm_stabilize::Mat3x4 source{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, source))
                return false;
            vr_vm_stabilize::Mul(inverseAnchor, source, outLocalBones[static_cast<size_t>(bone)]);
        }
        return true;
    }

    struct ManualReloadMagazineResolverCache
    {
        VR* owner = nullptr;
        std::string modelName;
        int numBones = 0;
        std::chrono::steady_clock::time_point reloadStarted{};
        std::vector<vr_vm_stabilize::Mat3x4> initialLocalBones;
        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int fallbackCandidateBone = -1;
        int fallbackCandidateConfirmations = 0;

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            numBones = 0;
            reloadStarted = {};
            initialLocalBones.clear();
            boneNames.clear();
            boneParents.clear();
            fallbackCandidateBone = -1;
            fallbackCandidateConfirmations = 0;
        }
    };

    inline ManualReloadMagazineResolverCache& GetManualReloadMagazineResolverCache()
    {
        static ManualReloadMagazineResolverCache cache;
        return cache;
    }

    inline bool ManualReloadNameLooksLikeBodyOrNonMagazinePart(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return false;

        return
            ManualReloadNameContains(name, "valvebiped") ||
            ManualReloadNameContains(name, "bip01") ||
            ManualReloadNameContains(name, "finger") ||
            ManualReloadNameContains(name, "hand") ||
            ManualReloadNameContains(name, "wrist") ||
            ManualReloadNameContains(name, "forearm") ||
            ManualReloadNameContains(name, "upperarm") ||
            ManualReloadNameContains(name, "clavicle") ||
            ManualReloadNameContains(name, "spine") ||
            ManualReloadNameContains(name, "camera") ||
            ManualReloadNameContains(name, "attach") ||
            ManualReloadNameContains(name, "muzzle") ||
            ManualReloadNameContains(name, "shell") ||
            ManualReloadNameContains(name, "trigger") ||
            ManualReloadNameContains(name, "release") ||
            ManualReloadNameContains(name, "realease") ||
            ManualReloadNameContains(name, "safety") ||
            ManualReloadNameContains(name, "bolt") ||
            ManualReloadNameContains(name, "slide") ||
            ManualReloadNameContains(name, "charger") ||
            ManualReloadNameContains(name, "hammer") ||
            ManualReloadNameContains(name, "root") ||
            ManualReloadNameContains(name, "skeleton") ||
            name == "j_gun" ||
            name == "gun" ||
            name == "base" ||
            name == "def_c_base" ||
            name == "jc_def_c_base";
    }

    inline bool ManualReloadNameLooksLikeGenericWeaponRoot(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty())
            return false;

        return name == "j_gun" ||
            name == "gun" ||
            name == "root" ||
            name == "skeleton" ||
            name == "base" ||
            name == "def_c_base" ||
            name == "jc_def_c_base" ||
            ManualReloadNameContains(name, "gun_root") ||
            ManualReloadNameContains(name, "propgun") ||
            ManualReloadNameEndsWith(name, "_root") ||
            ManualReloadNameEndsWith(name, ".root") ||
            ManualReloadNameEndsWith(name, "_base") ||
            ManualReloadNameEndsWith(name, ".base");
    }

    inline bool ManualReloadBoneCanBeMotionFallback(
        int bone,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents)
    {
        const int numBones = static_cast<int>(boneParents.size());
        if (bone < 0 || bone >= numBones)
            return false;

        const std::string boneName =
            (bone < static_cast<int>(boneNames.size()))
            ? boneNames[static_cast<size_t>(bone)]
            : std::string();
        if (ManualReloadNameLooksLikeBodyOrNonMagazinePart(boneName) ||
            ManualReloadNameLooksLikeGenericWeaponRoot(boneName))
        {
            return false;
        }

        const int parent = boneParents[static_cast<size_t>(bone)];
        // Some replacement models expose the detachable part as a top-level custom bone.
        // Allow that case as long as the bone itself is not a body/gun root.
        if (parent < 0 || parent >= numBones)
            return true;

        int current = parent;
        for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
        {
            if (current < static_cast<int>(boneNames.size()) &&
                ManualReloadNameLooksLikeBodyOrNonMagazinePart(boneNames[static_cast<size_t>(current)]))
            {
                // A custom part below a replacement-model weapon root such as j_gun,
                // Gun_Root or def_c_base is valid. Body/camera ancestry is not.
                const std::string lower = vr_vm_stabilize::ToLowerAscii(boneNames[static_cast<size_t>(current)]);
                if (ManualReloadNameLooksLikeGenericWeaponRoot(lower))
                    break;
                return false;
            }
            const int next = boneParents[static_cast<size_t>(current)];
            if (next == current)
                break;
            current = next;
        }
        return true;
    }

    inline float ManualReloadLocalBoneMovedMeters(
        const vr_vm_stabilize::Mat3x4& initialLocal,
        const vr_vm_stabilize::Mat3x4& currentLocal,
        float unitsPerMeter)
    {
        const Vector initial = vr_vm_stabilize::GetOrigin(initialLocal);
        const Vector current = vr_vm_stabilize::GetOrigin(currentLocal);
        return (current - initial).Length() / std::max(0.001f, unitsPerMeter);
    }

    inline float ManualReloadLocalBoneDistanceMeters(
        const vr_vm_stabilize::Mat3x4& local,
        float unitsPerMeter)
    {
        return vr_vm_stabilize::GetOrigin(local).Length() / std::max(0.001f, unitsPerMeter);
    }

    inline bool ManualReloadConfirmFallbackCandidate(
        ManualReloadMagazineResolverCache& cache,
        int bone)
    {
        if (bone < 0)
            return false;
        if (cache.fallbackCandidateBone != bone)
        {
            cache.fallbackCandidateBone = bone;
            cache.fallbackCandidateConfirmations = 1;
            return false;
        }
        ++cache.fallbackCandidateConfirmations;
        return cache.fallbackCandidateConfirmations >= 3;
    }

    inline bool ManualReloadFallbackMatchesPreviousModelChoice(
        const std::string& modelName,
        int visualBone)
    {
        static std::mutex s_mutex;
        static std::unordered_map<std::string, int> s_previousFallbackBoneByModel;
        std::lock_guard<std::mutex> lock(s_mutex);
        const std::string key = vr_vm_stabilize::ToLowerAscii(modelName);
        auto it = s_previousFallbackBoneByModel.find(key);
        if (it == s_previousFallbackBoneByModel.end())
        {
            s_previousFallbackBoneByModel.emplace(key, visualBone);
            return true;
        }
        if (it->second == visualBone)
            return true;

        Game::logMsg(
            "[VR][ManualReload] rejected unstable unnamed fallback model=%s previousBone=%d newBone=%d; add ManualReloadMagazineBoneOverrides for this replacement model",
            modelName.c_str(),
            it->second,
            visualBone);
        return false;
    }

    inline bool LockManualReloadMagazineBone(
        VR* vr,
        const std::string& modelName,
        int visualBone,
        int motionBone,
        const std::vector<std::string>& boneNames,
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const vr_vm_stabilize::Mat3x4& initialMagazineLocal,
        const vr_vm_stabilize::Mat3x4& initialMotionProbeLocal,
        const char* reason,
        int visualScore,
        float motionMovedMeters)
    {
        if (!vr || visualBone < 0 || motionBone < 0)
            return false;

        constexpr float kMaximumNamedMotionMeters = 1.50f;
        constexpr float kMaximumUnnamedFallbackMotionMeters = 0.75f;
        constexpr float kMaximumSocketFromModelAnchorMeters = 1.50f;
        constexpr float kMaximumSocketFromCameraMeters = 4.00f;
        const bool unnamedFallback = visualScore <= 0;
        const float maximumMotionMeters = unnamedFallback
            ? kMaximumUnnamedFallbackMotionMeters
            : kMaximumNamedMotionMeters;
        const float socketFromModelAnchorMeters = ManualReloadLocalBoneDistanceMeters(
            initialMagazineLocal,
            vr->m_VRScale);

        vr_vm_stabilize::Mat3x4 socketWorld{};
        vr_vm_stabilize::Mul(modelAnchor, initialMagazineLocal, socketWorld);
        const float socketFromCameraMeters =
            (vr_vm_stabilize::GetOrigin(socketWorld) - vr->m_CameraAnchor).Length() /
            std::max(0.001f, vr->m_VRScale);

        if (!std::isfinite(motionMovedMeters) ||
            !std::isfinite(socketFromModelAnchorMeters) ||
            !std::isfinite(socketFromCameraMeters) ||
            motionMovedMeters > maximumMotionMeters ||
            socketFromModelAnchorMeters > kMaximumSocketFromModelAnchorMeters ||
            socketFromCameraMeters > kMaximumSocketFromCameraMeters)
        {
            Game::logMsg(
                "[VR][ManualReload] rejected implausible magazine candidate model=%s visualBone=%d motionBone=%d visualScore=%d motionMoved=%.3fm socketFromModel=%.3fm socketFromCamera=%.3fm",
                modelName.c_str(),
                visualBone,
                motionBone,
                visualScore,
                motionMovedMeters,
                socketFromModelAnchorMeters,
                socketFromCameraMeters);
            return false;
        }

        if (unnamedFallback && !ManualReloadFallbackMatchesPreviousModelChoice(modelName, visualBone))
            return false;

        vr->m_ManualReloadMagazineModelName = modelName;
        vr->m_ManualReloadMagazineBoneIndex = visualBone;
        vr->m_ManualReloadMagazineMotionBoneIndex = motionBone;
        vr->m_ManualReloadSocketLocal = HooksMat3x4ToVrHandMatrix(initialMagazineLocal);
        vr->m_ManualReloadSocketWorld = VrHandMath::Multiply(
            HooksMat3x4ToVrHandMatrix(modelAnchor),
            vr->m_ManualReloadSocketLocal);
        vr->m_ManualReloadSocketValid = true;
        vr->m_ManualReloadMotionProbeLocal = HooksMat3x4ToVrHandMatrix(initialMotionProbeLocal);
        vr->m_ManualReloadMotionProbeValid = true;

        const char* visualName =
            (visualBone < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(visualBone)].empty())
            ? boneNames[static_cast<size_t>(visualBone)].c_str()
            : "<unnamed>";
        const char* motionName =
            (motionBone < static_cast<int>(boneNames.size()) && !boneNames[static_cast<size_t>(motionBone)].empty())
            ? boneNames[static_cast<size_t>(motionBone)].c_str()
            : "<unnamed>";
        Game::logMsg(
            "[VR][ManualReload] locked detachable magazine model=%s visualBone=%d visualName=%s motionBone=%d motionName=%s reason=%s visualScore=%d motionMoved=%.3fm socketFromCamera=%.3fm",
            modelName.c_str(),
            visualBone,
            visualName,
            motionBone,
            motionName,
            reason ? reason : "unknown",
            visualScore,
            motionMovedMeters,
            socketFromCameraMeters);
        return true;
    }

    inline bool ManualReloadNameIsStrongCustomMagazineCandidate(const std::string& rawName)
    {
        const std::string name = vr_vm_stabilize::ToLowerAscii(rawName);
        if (name.empty() || ManualReloadNameIsLegacyValveBipedClip(name))
            return false;

        // These are explicit replacement-model magazine roots. If one exists, it is
        // the preferred visual root even when a legacy helper is the actual animation driver.
        return ScoreManualReloadMagazineBoneName(rawName) >= 900;
    }

    inline int ResolveManualReloadMagazineBone(
        VR* vr,
        const std::string& modelName,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones)
    {
        if (!vr || !sourceBones || numBones <= 0)
            return -1;

        const int lockedBone = GetManualReloadLockedMagazineBone(vr, modelName, numBones);
        if (lockedBone >= 0)
            return lockedBone;

        if (vr->m_ManualReloadState != ManualReloadState::WatchingNativeClipRemoval ||
            HooksModelNameIsArmsOrHands(vr_vm_stabilize::ToLowerAscii(modelName)))
        {
            return -1;
        }

        ManualReloadMagazineResolverCache& cache = GetManualReloadMagazineResolverCache();
        const bool needReset =
            cache.owner != vr ||
            cache.modelName != modelName ||
            cache.numBones != numBones ||
            cache.reloadStarted != vr->m_ManualReloadStarted;
        if (needReset)
        {
            cache.Reset();
            cache.owner = vr;
            cache.modelName = modelName;
            cache.numBones = numBones;
            cache.reloadStarted = vr->m_ManualReloadStarted;
            cache.boneNames = boneNames;
            cache.boneParents = boneParents;
            if (!BuildManualReloadLocalBones(modelAnchor, sourceBones, numBones, cache.initialLocalBones))
            {
                cache.Reset();
                return -1;
            }
        }

        // A configured exact bone is the strongest visual hint. Automatic detection remains
        // the default and is used when the current weapon has no override or the requested
        // name does not exist in the active replacement viewmodel.
        std::string configuredVisualName;
        const int configuredVisualBone = FindManualReloadConfiguredMagazineBone(
            vr,
            modelName,
            boneNames,
            configuredVisualName);

        // Name is only a visual hint. Workshop replacements frequently separate the
        // visible magazine root from the helper bone that actually moves during reload.
        const int strongestNameHintBone = FindManualReloadMagazineBone(modelName, boneNames);
        const bool hasStrongCustomNameHint =
            strongestNameHintBone >= 0 &&
            strongestNameHintBone < static_cast<int>(boneNames.size()) &&
            ManualReloadNameIsStrongCustomMagazineCandidate(
                boneNames[static_cast<size_t>(strongestNameHintBone)]);

        std::vector<vr_vm_stabilize::Mat3x4> currentLocalBones;
        if (!BuildManualReloadLocalBones(modelAnchor, sourceBones, numBones, currentLocalBones))
            return -1;

        const float elapsedSeconds =
            (vr->m_ManualReloadStarted.time_since_epoch().count() != 0)
            ? std::chrono::duration<float>(std::chrono::steady_clock::now() - vr->m_ManualReloadStarted).count()
            : 0.0f;
        const float leaveThreshold = std::max(0.02f, vr->m_ManualReloadNativeClipLeaveDistanceMeters);
        constexpr float kMinimumUsefulMotionMeters = 0.012f;
        constexpr float kMinimumUnnamedFallbackMotionMeters = 0.025f;
        constexpr float kMaximumUnnamedFallbackMotionMeters = 0.75f;

        struct Candidate
        {
            int bone = -1;
            int nameScore = 0;
            float movedMeters = 0.0f;
            bool strongCustomName = false;
            bool canMotionFallback = false;
        };

        auto betterByNameThenMotion = [](const Candidate& candidate, const Candidate& best)
            {
                if (candidate.bone < 0)
                    return false;
                if (best.bone < 0)
                    return true;
                if (candidate.nameScore != best.nameScore)
                    return candidate.nameScore > best.nameScore;
                return candidate.movedMeters > best.movedMeters;
            };
        auto betterByMotion = [](const Candidate& candidate, const Candidate& best)
            {
                if (candidate.bone < 0)
                    return false;
                if (best.bone < 0)
                    return true;
                if (std::fabs(candidate.movedMeters - best.movedMeters) > 0.0005f)
                    return candidate.movedMeters > best.movedMeters;
                return candidate.nameScore > best.nameScore;
            };

        Candidate strongestMovingVisual;
        Candidate strongestVisualPastLeave;
        Candidate bestConfiguredDriver;
        Candidate bestNamedDriver;
        Candidate bestMotionFallback;
        for (int bone = 0; bone < numBones; ++bone)
        {
            const int namedScore =
                (bone < static_cast<int>(boneNames.size()))
                ? ScoreManualReloadMagazineBoneName(boneNames[static_cast<size_t>(bone)])
                : 0;
            const bool strongCustomName =
                namedScore > 0 &&
                bone < static_cast<int>(boneNames.size()) &&
                ManualReloadNameIsStrongCustomMagazineCandidate(boneNames[static_cast<size_t>(bone)]);
            const bool isConfiguredVisual = bone == configuredVisualBone;
            const bool canMotionFallback = ManualReloadBoneCanBeMotionFallback(bone, boneNames, boneParents);
            if (namedScore <= 0 && !canMotionFallback && !isConfiguredVisual)
                continue;

            const float movedMeters = ManualReloadLocalBoneMovedMeters(
                cache.initialLocalBones[static_cast<size_t>(bone)],
                currentLocalBones[static_cast<size_t>(bone)],
                vr->m_VRScale);
            if (movedMeters < kMinimumUsefulMotionMeters)
                continue;

            Candidate candidate;
            candidate.bone = bone;
            candidate.nameScore = namedScore;
            candidate.movedMeters = movedMeters;
            candidate.strongCustomName = strongCustomName;
            candidate.canMotionFallback = canMotionFallback;

            if (isConfiguredVisual && betterByMotion(candidate, bestConfiguredDriver))
                bestConfiguredDriver = candidate;
            if (namedScore > 0 && betterByMotion(candidate, bestNamedDriver))
                bestNamedDriver = candidate;
            if (canMotionFallback &&
                movedMeters >= kMinimumUnnamedFallbackMotionMeters &&
                movedMeters <= kMaximumUnnamedFallbackMotionMeters &&
                betterByMotion(candidate, bestMotionFallback))
            {
                bestMotionFallback = candidate;
            }
            if (strongCustomName && betterByNameThenMotion(candidate, strongestMovingVisual))
                strongestMovingVisual = candidate;
            if (strongCustomName && movedMeters >= leaveThreshold && betterByNameThenMotion(candidate, strongestVisualPastLeave))
                strongestVisualPastLeave = candidate;
        }

        Candidate motionProbe;
        const char* reason = nullptr;
        if (bestConfiguredDriver.bone >= 0 && bestConfiguredDriver.movedMeters >= leaveThreshold)
        {
            motionProbe = bestConfiguredDriver;
            reason = "configured-motion-probe";
        }
        else if (bestNamedDriver.bone >= 0 && bestNamedDriver.movedMeters >= leaveThreshold)
        {
            motionProbe = bestNamedDriver;
            reason = "named-motion-probe";
        }
        else if (bestMotionFallback.bone >= 0 && bestMotionFallback.movedMeters >= leaveThreshold && elapsedSeconds >= 0.25f)
        {
            motionProbe = bestMotionFallback;
            reason = "motion-fallback-probe";
        }
        else if (elapsedSeconds >= 2.20f)
        {
            // Near the end of the animation, allow a named/configured helper with a small
            // but meaningful movement. Never promote an unnamed late-small fallback: it is
            // too easy to mistake a decorative part or root jitter for a detachable magazine.
            if (betterByMotion(bestConfiguredDriver, motionProbe))
                motionProbe = bestConfiguredDriver;
            if (betterByMotion(bestNamedDriver, motionProbe))
                motionProbe = bestNamedDriver;
            if (motionProbe.bone >= 0 && motionProbe.movedMeters >= kMinimumUsefulMotionMeters)
                reason = "late-small-named-motion-probe";
        }

        if (motionProbe.bone < 0)
            return -1;

        Candidate visual;
        if (configuredVisualBone >= 0)
        {
            visual.bone = configuredVisualBone;
            visual.nameScore = 100000;
            visual.movedMeters = ManualReloadLocalBoneMovedMeters(
                cache.initialLocalBones[static_cast<size_t>(configuredVisualBone)],
                currentLocalBones[static_cast<size_t>(configuredVisualBone)],
                vr->m_VRScale);
            visual.strongCustomName = true;
        }
        else if (strongestVisualPastLeave.bone >= 0)
        {
            visual = strongestVisualPastLeave;
        }
        else if (hasStrongCustomNameHint)
        {
            visual.bone = strongestNameHintBone;
            visual.nameScore = ScoreManualReloadMagazineBoneName(boneNames[static_cast<size_t>(strongestNameHintBone)]);
            visual.movedMeters = ManualReloadLocalBoneMovedMeters(
                cache.initialLocalBones[static_cast<size_t>(strongestNameHintBone)],
                currentLocalBones[static_cast<size_t>(strongestNameHintBone)],
                vr->m_VRScale);
            visual.strongCustomName = true;
        }
        else if (strongestMovingVisual.bone >= 0)
        {
            visual = strongestMovingVisual;
        }
        else
        {
            visual = motionProbe;
        }

        if (visual.nameScore <= 0 && !ManualReloadConfirmFallbackCandidate(cache, visual.bone))
        {
            return -1;
        }

        // Climb only through another explicitly magazine-named parent. Never climb into
        // a generic gun parent such as v_weapon.M4A1_s_Parent, otherwise the detached
        // second Source pass would redraw the entire firearm instead of just the magazine.
        int resolvedVisualBone = visual.bone;
        int resolvedVisualScore = visual.nameScore;
        int parent = boneParents[static_cast<size_t>(resolvedVisualBone)];
        for (int guard = 0; guard < numBones && parent >= 0 && parent < numBones; ++guard)
        {
            const int parentNameScore =
                (parent < static_cast<int>(boneNames.size()))
                ? ScoreManualReloadMagazineBoneName(boneNames[static_cast<size_t>(parent)])
                : 0;
            if (parentNameScore <= 0)
                break;

            resolvedVisualBone = parent;
            resolvedVisualScore = std::max(resolvedVisualScore, parentNameScore);
            parent = boneParents[static_cast<size_t>(resolvedVisualBone)];
        }

        if (!LockManualReloadMagazineBone(
            vr,
            modelName,
            resolvedVisualBone,
            motionProbe.bone,
            boneNames,
            modelAnchor,
            cache.initialLocalBones[static_cast<size_t>(resolvedVisualBone)],
            cache.initialLocalBones[static_cast<size_t>(motionProbe.bone)],
            reason,
            resolvedVisualScore,
            motionProbe.movedMeters))
        {
            return -1;
        }
        return resolvedVisualBone;
    }

    inline bool ManualReloadLocalBonesDiffer(
        const std::vector<vr_vm_stabilize::Mat3x4>& a,
        const std::vector<vr_vm_stabilize::Mat3x4>& b)
    {
        if (a.size() != b.size())
            return true;

        // Local matrices remove controller/camera motion. A small threshold is enough
        // to detect actual Source animation progress while avoiding duplicate stereo draws.
        constexpr float kDifferenceThreshold = 0.0005f;
        for (size_t bone = 0; bone < a.size(); ++bone)
        {
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    if (std::fabs(a[bone].m[row][column] - b[bone].m[row][column]) > kDifferenceThreshold)
                        return true;
                }
            }
        }
        return false;
    }

    inline void CaptureManualReloadTailPose(
        VR* vr,
        ManualReloadFrozenViewmodelPoseEntry& entry,
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const vr_vm_stabilize::Mat3x4* sourceBones,
        int numBones)
    {
        if (!vr || !sourceBones || numBones <= 0 || vr->m_ManualReloadTailCaptureComplete)
            return;

        const auto now = std::chrono::steady_clock::now();
        if (entry.tailCaptureStarted.time_since_epoch().count() == 0)
            entry.tailCaptureStarted = now;

        const float elapsed = std::chrono::duration<float>(now - entry.tailCaptureStarted).count();
        constexpr float kManualReloadTailCaptureSafetyLimitSeconds = 8.0f;
        constexpr size_t kManualReloadTailCaptureSafetyLimitSamples = 720;
        if (elapsed > kManualReloadTailCaptureSafetyLimitSeconds ||
            entry.tailSamples.size() >= kManualReloadTailCaptureSafetyLimitSamples)
        {
            if (!vr->m_ManualReloadTailCaptureComplete)
            {
                vr->m_ManualReloadTailCaptureComplete = true;
                Game::logMsg(
                    "[VR][ManualReload] hidden native reload tail capture reached safety limit duration=%.3fs samples=%zu",
                    elapsed,
                    entry.tailSamples.size());
            }
            return;
        }

        if (entry.tailLastSample.time_since_epoch().count() != 0 &&
            std::chrono::duration<float>(now - entry.tailLastSample).count() < (1.0f / 90.0f))
        {
            return;
        }

        std::vector<vr_vm_stabilize::Mat3x4> localBones;
        if (!BuildManualReloadLocalBones(modelAnchor, sourceBones, numBones, localBones))
            return;

        if (!entry.tailSamples.empty() &&
            !ManualReloadLocalBonesDiffer(localBones, entry.tailSamples.back().localBones))
        {
            return;
        }

        ManualReloadTailPoseSample sample;
        sample.timeSeconds = elapsed;
        sample.localBones.swap(localBones);
        entry.tailSamples.push_back(sample);
        entry.tailLastSample = now;
        vr->m_ManualReloadVisualResumeDurationSeconds = std::max(
            vr->m_ManualReloadVisualResumeDurationSeconds,
            elapsed);
    }

    inline const std::vector<vr_vm_stabilize::Mat3x4>* SelectManualReloadReplayLocalBones(
        const ManualReloadFrozenViewmodelPoseEntry& entry,
        float elapsedSeconds,
        float replayStartOffsetSeconds)
    {
        if (entry.tailSamples.empty())
            return &entry.frozenLocalBones;

        const float targetSeconds = std::max(0.0f, replayStartOffsetSeconds + elapsedSeconds);
        // Select the first captured pose at or after the target time. In particular, the
        // first replayed frame must already be past Source's native magazine insertion.
        // Selecting the previous sample would visibly replay the insertion transition once more.
        for (const ManualReloadTailPoseSample& sample : entry.tailSamples)
        {
            if (sample.timeSeconds >= targetSeconds)
                return &sample.localBones;
        }
        return &entry.tailSamples.back().localBones;
    }

    inline void ApplyManualReloadLocalPose(
        const vr_vm_stabilize::Mat3x4& modelAnchor,
        const std::vector<vr_vm_stabilize::Mat3x4>& localBones,
        vr_vm_stabilize::Mat3x4* outBones,
        int numBones)
    {
        if (!outBones || static_cast<int>(localBones.size()) != numBones)
            return;

        for (int bone = 0; bone < numBones; ++bone)
            vr_vm_stabilize::Mul(modelAnchor, localBones[static_cast<size_t>(bone)], outBones[bone]);
    }

    inline void SnapManualReloadNativeMagazineToSocket(
        VR* vr,
        const std::vector<int>& boneParents,
        int clipBone,
        vr_vm_stabilize::Mat3x4* bones,
        int numBones)
    {
        if (!vr ||
            (vr->m_ManualReloadState != ManualReloadState::AwaitingNativePostInsertBoundary &&
                vr->m_ManualReloadState != ManualReloadState::ResumingNativeReloadWithMagazine) ||
            !vr->m_ManualReloadSocketValid || !bones || clipBone < 0 || clipBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }

        const vr_vm_stabilize::Mat3x4 targetClipWorld = HooksVrHandMatrixToMat3x4(
            HooksStripVrHandMatrixScale(vr->m_ManualReloadSocketWorld));
        vr_vm_stabilize::Mat3x4 inverseCurrentClip{};
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::InvertTR(bones[clipBone], inverseCurrentClip);
        vr_vm_stabilize::Mul(targetClipWorld, inverseCurrentClip, delta);

        auto isClipOrDescendant = [&](int bone)
            {
                int current = bone;
                for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                {
                    if (current == clipBone)
                        return true;
                    current = boneParents[static_cast<size_t>(current)];
                }
                return false;
            };

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!isClipOrDescendant(bone))
                continue;
            vr_vm_stabilize::Mat3x4 moved{};
            vr_vm_stabilize::Mul(delta, bones[bone], moved);
            bones[bone] = moved;
        }
    }

    inline void ApplyMagazineInteractionBoltPose(
        VR* vr,
        const std::vector<int>& boneParents,
        int boltBone,
        vr_vm_stabilize::Mat3x4* bones,
        int numBones)
    {
        if (!vr || !vr->ShouldMoveMagazineInteractionBolt() || !bones ||
            boltBone < 0 || boltBone >= numBones ||
            static_cast<int>(boneParents.size()) < numBones)
        {
            return;
        }

        VrHandMatrix4 targetBoltWorld{};
        if (!vr->GetMagazineInteractionBoltWorld(targetBoltWorld))
            return;

        const vr_vm_stabilize::Mat3x4 targetBolt =
            HooksVrHandMatrixToMat3x4(HooksStripVrHandMatrixScale(targetBoltWorld));
        vr_vm_stabilize::Mat3x4 inverseCurrentBolt{};
        vr_vm_stabilize::InvertTR(bones[boltBone], inverseCurrentBolt);
        vr_vm_stabilize::Mat3x4 delta{};
        vr_vm_stabilize::Mul(targetBolt, inverseCurrentBolt, delta);

        auto isBoltOrDescendant = [&](int bone)
            {
                int current = bone;
                for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                {
                    if (current == boltBone)
                        return true;
                    current = boneParents[static_cast<size_t>(current)];
                }
                return false;
            };

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!isBoltOrDescendant(bone))
                continue;

            vr_vm_stabilize::Mat3x4 moved{};
            vr_vm_stabilize::Mul(delta, bones[bone], moved);
            bones[bone] = moved;
        }
    }

    inline void ApplyManualReloadViewmodelOverride(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& info,
        void* viewmodelEntity,
        void*& pCustomBoneToWorld)
    {
        ManualReloadFrozenViewmodelPoseCache& frozenCache = GetManualReloadFrozenViewmodelPoseCache();

        const bool manualReloadActive = vr && vr->IsManualReloadActive();
        const bool magazineInteractionActive = vr && vr->IsMagazineInteractionManualActive();
        if (!vr || (!manualReloadActive && !magazineInteractionActive))
        {
            if (!vr || frozenCache.owner == vr)
                frozenCache.Reset();
            ManualReloadMagazineResolverCache& resolverCache = GetManualReloadMagazineResolverCache();
            if (!vr || resolverCache.owner == vr)
                resolverCache.Reset();
            return;
        }
        if (!drawState || !pCustomBoneToWorld)
            return;

        const std::string lowerModel = vr_vm_stabilize::ToLowerAscii(modelName);
        if (!HooksModelNameIsViewmodel(lowerModel))
            return;
        std::string magazineInteractionLockedModel;
        const bool magazineInteractionOnly = magazineInteractionActive && !manualReloadActive;
        const bool isArmsOrHandsModel = HooksModelNameIsArmsOrHands(lowerModel);
        if (magazineInteractionOnly)
        {
            magazineInteractionLockedModel =
                vr_vm_stabilize::ToLowerAscii(vr->m_MagazineInteractionMagazineModelName);
            if ((magazineInteractionLockedModel.empty() || magazineInteractionLockedModel != lowerModel) &&
                !isArmsOrHandsModel)
            {
                return;
            }
        }

        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        int numBones = 0;
        int boneIndex = 0;
        int stride = 0;
        int numBonesOffset = 0;
        if (!vr_vm_stabilize::TryCollectBoneNamesFromDrawState(
            drawState,
            boneNames,
            boneParents,
            numBones,
            boneIndex,
            stride,
            numBonesOffset))
        {
            return;
        }
        if (numBones <= 0 || numBones > 512 || static_cast<int>(boneNames.size()) < numBones)
            return;

        vr_vm_stabilize::Mat3x4 modelAnchor{};
        if (!TryGetManualReloadModelAnchor(info, modelAnchor))
            return;

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        int clipBone = -1;
        if (magazineInteractionActive)
        {
            const std::string lockedModel = magazineInteractionLockedModel.empty()
                ? vr_vm_stabilize::ToLowerAscii(vr->m_MagazineInteractionMagazineModelName)
                : magazineInteractionLockedModel;
            if (!lockedModel.empty() && lockedModel == lowerModel)
            {
                const int lockedBone = vr->m_MagazineInteractionMagazineBoneIndex;
                if (lockedBone >= 0 && lockedBone < numBones)
                    clipBone = lockedBone;
            }
        }
        if (clipBone < 0 && manualReloadActive)
        {
            clipBone = GetManualReloadLockedMagazineBone(vr, modelName, numBones);
        }
        if (clipBone < 0 && manualReloadActive)
        {
            clipBone = ResolveManualReloadMagazineBone(
                vr,
                modelName,
                boneNames,
                boneParents,
                modelAnchor,
                sourceBones,
                numBones);
        }

        int magazineInteractionBoltBone = -1;
        if (magazineInteractionActive && vr->m_MagazineInteractionBoltRestValid)
        {
            const std::string boltModel = vr_vm_stabilize::ToLowerAscii(
                !vr->m_MagazineInteractionBoltRestBox.modelName.empty()
                ? vr->m_MagazineInteractionBoltRestBox.modelName
                : vr->m_MagazineInteractionMagazineModelName);
            if (!boltModel.empty() && boltModel == lowerModel)
            {
                const int lockedBolt = vr->m_MagazineInteractionBoltRestBox.boneIndex;
                if (lockedBolt >= 0 && lockedBolt < numBones)
                    magazineInteractionBoltBone = lockedBolt;
            }
        }

        if (manualReloadActive && clipBone >= 0 && viewmodelEntity)
        {
            const int motionBone = GetManualReloadLockedMagazineMotionBone(vr, modelName, numBones);
            vr_vm_stabilize::Mat3x4 clip{};
            vr_vm_stabilize::Mat3x4 motionProbe{};
            if (!vr_vm_stabilize::SafeRead(sourceBones + clipBone, clip))
                return;
            if (motionBone < 0 || !vr_vm_stabilize::SafeRead(sourceBones + motionBone, motionProbe))
                motionProbe = clip;

            vr->OnManualReloadViewmodelPose(
                modelName.c_str(),
                viewmodelEntity,
                info.entity_index,
                HooksMat3x4ToVrHandMatrix(modelAnchor),
                HooksMat3x4ToVrHandMatrix(clip),
                HooksMat3x4ToVrHandMatrix(motionProbe));
        }

        const bool visuallyPauseViewmodel = manualReloadActive
            ? IsManualReloadViewmodelVisualPauseState(vr)
            : vr->ShouldFreezeMagazineInteractionViewmodel();
        const bool visuallyReplayViewmodel = manualReloadActive
            ? IsManualReloadViewmodelVisualReplayState(vr)
            : false;
        const bool magazineInteractionHardLockViewmodel =
            magazineInteractionActive &&
            vr->m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt;
        const bool hideNativeClip = (manualReloadActive
            ? vr->ShouldHideManualReloadNativeClip()
            : vr->ShouldHideMagazineInteractionNativeClip()) && clipBone >= 0;
        if (!visuallyPauseViewmodel && !visuallyReplayViewmodel && !hideNativeClip)
        {
            if (frozenCache.owner == vr)
            {
                if (manualReloadActive && vr->m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval)
                    frozenCache.Reset();
                else
                    frozenCache.models.erase(lowerModel);
            }
            return;
        }

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
        if (seqEven == 0)
            seqEven = (static_cast<uint32_t>(GetTickCount()) << 1u) | 2u;
        vr_vm_stabilize::Mat3x4* copiedBones = vr_vm_stabilize::AllocStableBones(numBones, seqEven);
        if (!copiedBones)
            return;

        for (int bone = 0; bone < numBones; ++bone)
        {
            if (!vr_vm_stabilize::SafeRead(sourceBones + bone, copiedBones[bone]))
                return;
        }

        if (visuallyPauseViewmodel || visuallyReplayViewmodel)
        {
            if (frozenCache.owner != vr)
            {
                frozenCache.Reset();
                frozenCache.owner = vr;
            }

            const int rootBone = FindManualReloadTopLevelBone(boneParents, clipBone);
            ManualReloadFrozenViewmodelPoseEntry& frozenPose = frozenCache.models[lowerModel];
            const bool needCapture =
                !frozenPose.valid ||
                frozenPose.modelName != modelName ||
                frozenPose.numBones != numBones ||
                frozenPose.rootBone != rootBone ||
                static_cast<int>(frozenPose.frozenLocalBones.size()) != numBones;

            if (needCapture)
            {
                frozenPose = {};
                frozenPose.modelName = modelName;
                frozenPose.numBones = numBones;
                frozenPose.rootBone = rootBone;
                frozenPose.valid = BuildManualReloadLocalBones(
                    modelAnchor,
                    sourceBones,
                    numBones,
                    frozenPose.frozenLocalBones);
                if (frozenPose.valid)
                {
                    Game::logMsg(
                        "[VR][ManualReload] cached frozen viewmodel pose model=%s bones=%d root=%d",
                        modelName.c_str(),
                        numBones,
                        rootBone);
                }
            }

            if (frozenPose.valid)
            {
                if (magazineInteractionHardLockViewmodel &&
                    !frozenPose.hardLockModelAnchorValid)
                {
                    frozenPose.hardLockModelAnchor = modelAnchor;
                    frozenPose.hardLockModelAnchorValid = true;
                    Game::logMsg(
                        "[VR][MagazineInteraction] hard-locked viewmodel anchor while bolt is held model=%s",
                        modelName.c_str());
                }
                else if (!magazineInteractionHardLockViewmodel)
                {
                    frozenPose.hardLockModelAnchorValid = false;
                }

                if (visuallyPauseViewmodel)
                {
                    if (manualReloadActive)
                        CaptureManualReloadTailPose(vr, frozenPose, modelAnchor, sourceBones, numBones);
                    const vr_vm_stabilize::Mat3x4& anchor =
                        (magazineInteractionHardLockViewmodel && frozenPose.hardLockModelAnchorValid)
                        ? frozenPose.hardLockModelAnchor
                        : modelAnchor;
                    ApplyManualReloadLocalPose(anchor, frozenPose.frozenLocalBones, copiedBones, numBones);
                }
                else
                {
                    const auto now = std::chrono::steady_clock::now();
                    const float elapsedSeconds = std::chrono::duration<float>(
                        now - vr->m_ManualReloadResumeStarted).count();
                    const float replayStartOffsetSeconds = vr->m_ManualReloadVisualReplayStartOffsetSeconds;
                    const auto* replayLocalBones = SelectManualReloadReplayLocalBones(
                        frozenPose,
                        elapsedSeconds,
                        replayStartOffsetSeconds);
                    if (replayLocalBones)
                        ApplyManualReloadLocalPose(modelAnchor, *replayLocalBones, copiedBones, numBones);
                }
            }
        }

        if (manualReloadActive)
            SnapManualReloadNativeMagazineToSocket(vr, boneParents, clipBone, copiedBones, numBones);
        if (magazineInteractionActive)
            ApplyMagazineInteractionBoltPose(vr, boneParents, magazineInteractionBoltBone, copiedBones, numBones);

        if (magazineInteractionActive && hideNativeClip)
        {
            DrawCurrentWeaponMagazineBox(
                vr,
                drawState,
                modelName,
                info.entity_index,
                info.hitboxset,
                info.pModelToWorld,
                copiedBones);
        }

        if (hideNativeClip)
        {
            auto isClipOrDescendant = [&](int bone)
                {
                    int current = bone;
                    for (int guard = 0; guard < numBones && current >= 0 && current < numBones; ++guard)
                    {
                        if (current == clipBone)
                            return true;
                        current = boneParents[static_cast<size_t>(current)];
                    }
                    return false;
                };

            for (int bone = 0; bone < numBones; ++bone)
            {
                if (!isClipOrDescendant(bone))
                    continue;
                copiedBones[bone].m[0][3] += 100000.0f;
                copiedBones[bone].m[1][3] += 100000.0f;
                copiedBones[bone].m[2][3] += 100000.0f;
            }
        }

        pCustomBoneToWorld = copiedBones;
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

	if (!m_VR || !ecx || !IsLocalServerActiveWeapon(ecx))
		return;

	Server_WeaponCSBase* weapon = reinterpret_cast<Server_WeaponCSBase*>(ecx);
	int weaponId = 0;
#ifdef _MSC_VER
	__try
	{
		weaponId = weapon->GetWeaponID();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return;
	}
#else
	weaponId = weapon->GetWeaponID();
#endif

	m_VR->TryApplyMagazineInteractionShotgunServerReloadAbort(ecx, weaponId);
}

int Hooks::dGetPrimaryAttackActivity(void* ecx, void* edx, void* meleeInfo)
{
	return hkGetPrimaryAttackActivity.fOriginal(ecx, meleeInfo);
}

namespace
{
	thread_local bool g_ServerUseControllerAimOverride = false;
	thread_local void* g_ServerUseControllerAimPlayer = nullptr;
	thread_local Vector g_ServerUseControllerAimOrigin = { 0.0f, 0.0f, 0.0f };
	thread_local QAngle g_ServerUseControllerAimAngles = { 0.0f, 0.0f, 0.0f };

	static inline bool IsFiniteVector3(const Vector& v)
	{
		return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
	}

	static bool TryBuildServerUseControllerPose(void* player, Vector& origin, QAngle& angles)
	{
		if (!Hooks::m_Game || !player)
			return false;

		const int playerIndex = Hooks::m_Game->m_CurrentUsercmdID;
		if (!Hooks::m_Game->IsValidPlayerIndex(playerIndex))
			return false;

		if (Hooks::m_Game->m_CurrentUsercmdPlayer &&
			reinterpret_cast<void*>(Hooks::m_Game->m_CurrentUsercmdPlayer) != player)
			return false;

		const Player& vrPlayer = Hooks::m_Game->m_PlayersVRInfo[playerIndex];
		if (!vrPlayer.isUsingVR)
			return false;

		origin = vrPlayer.controllerPos;
		angles = vrPlayer.controllerAngle;
		NormalizeAndClampViewAngles(angles);

		return IsFiniteVector3(origin) && IsFiniteViewAngle(angles);
	}

	class ScopedServerUseControllerAimOverride
	{
	public:
		ScopedServerUseControllerAimOverride(void* player, const Vector& origin, const QAngle& angles)
			: m_prevActive(g_ServerUseControllerAimOverride),
			m_prevPlayer(g_ServerUseControllerAimPlayer),
			m_prevOrigin(g_ServerUseControllerAimOrigin),
			m_prevAngles(g_ServerUseControllerAimAngles)
		{
			g_ServerUseControllerAimOverride = true;
			g_ServerUseControllerAimPlayer = player;
			g_ServerUseControllerAimOrigin = origin;
			g_ServerUseControllerAimAngles = angles;
		}

		~ScopedServerUseControllerAimOverride()
		{
			g_ServerUseControllerAimOverride = m_prevActive;
			g_ServerUseControllerAimPlayer = m_prevPlayer;
			g_ServerUseControllerAimOrigin = m_prevOrigin;
			g_ServerUseControllerAimAngles = m_prevAngles;
		}

	private:
		bool m_prevActive;
		void* m_prevPlayer;
		Vector m_prevOrigin;
		QAngle m_prevAngles;
	};
}

Vector* Hooks::dEyePosition(void* ecx, void* edx, Vector* eyePos)
{
	if (eyePos && g_ServerUseControllerAimOverride && ecx == g_ServerUseControllerAimPlayer)
	{
		*eyePos = g_ServerUseControllerAimOrigin;
		return eyePos;
	}

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

Vector* Hooks::dServerPlayerEyePosition(void* ecx, void* edx, Vector* eyePos)
{
	if (eyePos && g_ServerUseControllerAimOverride && ecx == g_ServerUseControllerAimPlayer)
	{
		*eyePos = g_ServerUseControllerAimOrigin;
		return eyePos;
	}

	Vector* result = hkServerPlayerEyePosition.fOriginal(ecx, eyePos);
	return result;
}

const QAngle* Hooks::dServerPlayerEyeAngles(void* ecx, void* edx)
{
	if (g_ServerUseControllerAimOverride && ecx == g_ServerUseControllerAimPlayer)
		return &g_ServerUseControllerAimAngles;

	return hkServerPlayerEyeAngles.fOriginal(ecx);
}

Server_BaseEntity* Hooks::dFindUseEntity(void* ecx, void* edx, float radius, float dotLimit, float defaultDotLimit, void* traceResult, void* extra)
{
	Vector controllerOrigin;
	QAngle controllerAngles;
	if (TryBuildServerUseControllerPose(ecx, controllerOrigin, controllerAngles))
	{
		ScopedServerUseControllerAimOverride useAim(ecx, controllerOrigin, controllerAngles);
		return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
	}

	return hkFindUseEntity.fOriginal(ecx, radius, dotLimit, defaultDotLimit, traceResult, extra);
}

void Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
	if (m_Game->m_SwitchedWeapons)
		m_Game->m_CachedArmsModel = false;

	bool hideArms = m_Game->m_IsMeleeWeaponActive || m_VR->m_HideArms;

	void* pBonesToWorldFinal = pCustomBoneToWorld;
	vr_vm_stabilize::Mat3x4* manualReloadNativeMagazineBones = nullptr;
	vr_vm_stabilize::Mat3x4* magazineInteractionDetachedMagazineBones = nullptr;
	bool drawManualReloadNativeMagazine = false;
	bool drawMagazineInteractionDetachedMagazine = false;

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
		const bool isViewmodelClassForProbe = className &&
			(std::strcmp(className, "CBaseViewModel") == 0 || std::strcmp(className, "C_BaseViewModel") == 0);
		if (m_VR->m_VrHandsDebugLog && (isViewmodelClassForProbe || HooksModelNameIsViewmodel(modelName)))
		{
			MaybeLogVrHandsViewmodelBoneProbe(
				state,
				modelName,
				info.entity_index,
				className,
				pCustomBoneToWorld != nullptr);
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
		if (teleportSuppressibleViewmodel)
			m_VR->DrawVrHandsWorldDepthMaskBeforeViewmodel();

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
if (m_VR->m_IsVREnabled && queueMode == 2 &&
	(m_VR->m_QueuedViewmodelStabilize ||
	 m_VR->m_ViewmodelDisableMoveBob ||
	 (!m_VR->m_MouseModeEnabled && m_VR->m_VrHandsRightUseViewmodelPose)))
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

		drawManualReloadNativeMagazine = BuildManualReloadNativeMagazineBones(
			m_VR,
			state,
			modelName,
			pBonesToWorldFinal,
			manualReloadNativeMagazineBones);
		drawMagazineInteractionDetachedMagazine = BuildMagazineInteractionDetachedMagazineBones(
			m_VR,
			state,
			modelName,
			pBonesToWorldFinal,
			magazineInteractionDetachedMagazineBones);

		ApplyManualReloadViewmodelOverride(
			m_VR,
			state,
			modelName,
			info,
			const_cast<C_BaseEntity*>(entity),
			pBonesToWorldFinal);

		if (!m_VR || !m_VR->ShouldHideMagazineInteractionNativeClip())
		{
			DrawCurrentWeaponMagazineBox(
				m_VR,
				state,
				modelName,
				info.entity_index,
				info.hitboxset,
				info.pModelToWorld,
				pBonesToWorldFinal);
		}
	}

	// Capture the exact arm matrices submitted to Source. In queued rendering
	// pBonesToWorldFinal contains the same stabilization delta as the visible gun,
	// so the standalone right glove follows controller rotation and HMD movement.
	MaybeCaptureViewmodelMuzzleSmokePose(m_VR, state, modelName, *pDrawInfo, pBonesToWorldFinal);
	MaybeCaptureVrHandsVmPose(m_VR, state, modelName, *pDrawInfo, pBonesToWorldFinal);

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

	// Draw the detached/new magazine as a second pass of the original weapon
	// viewmodel. Every non-clip bone is moved out of view, so Source applies the
	// same active material, shader, skin, lighting and post-processing as the gun.
	if (drawManualReloadNativeMagazine && manualReloadNativeMagazineBones)
		hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, manualReloadNativeMagazineBones);
	if (drawMagazineInteractionDetachedMagazine && magazineInteractionDetachedMagazineBones)
		hkDrawModelExecute.fOriginal(ecx, state, *pDrawInfo, magazineInteractionDetachedMagazineBones);
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
            m_VR->MarkQueuedHudFresh();
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
            {
                hkVgui_Paint.fOriginal(ecx, fullHudMode);
                m_VR->m_NativeDesktopHudPainted.store(true, std::memory_order_release);
            }
        }
        else
        {
            m_VR->m_RenderedHud.store(false, std::memory_order_release);
            m_VR->ClearQueuedHudFresh();
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


void Hooks::dEmitSoundAttenuation(
    void* ecx,
    void* edx,
    void* filter,
    int entIndex,
    int channel,
    const char* sample,
    float volume,
    float attenuation,
    int flags,
    int pitch,
    const Vector* origin,
    const Vector* direction,
    void* origins,
    bool updatePositions,
    float soundTime,
    int speakerEntity)
{
    if (m_VR && m_VR->CaptureMagazineInteractionSound(entIndex, sample, volume, flags, pitch))
        return;
    if (m_VR && m_VR->CaptureManualReloadSound(entIndex, sample, volume, flags, pitch))
        return;

    hkEmitSoundAttenuation.fOriginal(
        ecx,
        filter,
        entIndex,
        channel,
        sample,
        volume,
        attenuation,
        flags,
        pitch,
        origin,
        direction,
        origins,
        updatePositions,
        soundTime,
        speakerEntity);
}

void Hooks::dEmitSoundLevel(
    void* ecx,
    void* edx,
    void* filter,
    int entIndex,
    int channel,
    const char* sample,
    float volume,
    int soundLevel,
    int flags,
    int pitch,
    const Vector* origin,
    const Vector* direction,
    void* origins,
    bool updatePositions,
    float soundTime,
    int speakerEntity)
{
    if (m_VR && m_VR->CaptureMagazineInteractionSound(entIndex, sample, volume, flags, pitch))
        return;
    if (m_VR && m_VR->CaptureManualReloadSound(entIndex, sample, volume, flags, pitch))
        return;

    hkEmitSoundLevel.fOriginal(
        ecx,
        filter,
        entIndex,
        channel,
        sample,
        volume,
        soundLevel,
        flags,
        pitch,
        origin,
        direction,
        origins,
        updatePositions,
        soundTime,
        speakerEntity);
}
