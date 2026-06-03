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

        // Ignore controls, helpers and visual children. We need the root bone that
        // moves the whole detachable magazine.
        if (ManualReloadNameContains(name, "release") ||
            ManualReloadNameContains(name, "realease") ||
            ManualReloadNameContains(name, "button") ||
            ManualReloadNameContains(name, "trigger") ||
            ManualReloadNameContains(name, "bullet") ||
            ManualReloadNameContains(name, "round") ||
            ManualReloadNameContains(name, "shell") ||
            ManualReloadNameContains(name, "ammo") ||
            ManualReloadNameContains(name, "helper") ||
            ManualReloadNameContains(name, "attach"))
        {
            return 0;
        }

        const bool hasClip = ManualReloadNameContains(name, "clip");
        const bool hasMagazine = ManualReloadNameContains(name, "magazine");
        const bool hasLooseMag = ManualReloadNameHasLooseMagToken(name);
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
        targetMagazineWorld = HooksStripVrHandMatrixScale(targetMagazineWorld);

        const auto* sourceBones = reinterpret_cast<const vr_vm_stabilize::Mat3x4*>(pCustomBoneToWorld);
        vr_vm_stabilize::Mat3x4 originalClipWorld{};
        if (!vr_vm_stabilize::SafeRead(sourceBones + clipBone, originalClipWorld))
            return false;

        vr_vm_stabilize::Mat3x4 inverseOriginalClip{};
        vr_vm_stabilize::Mat3x4 targetClipWorld = HooksVrHandMatrixToMat3x4(targetMagazineWorld);
        vr_vm_stabilize::Mat3x4 targetDelta{};
        vr_vm_stabilize::InvertTR(originalClipWorld, inverseOriginalClip);
        vr_vm_stabilize::Mul(targetClipWorld, inverseOriginalClip, targetDelta);

        uint32_t seqEven = vr->m_RenderFrameSeq.load(std::memory_order_relaxed) & ~1u;
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

        static std::mutex s_sourceMagazineLogMutex;
        static std::unordered_map<std::string, bool> s_sourceMagazineLoggedModels;
        {
            std::lock_guard<std::mutex> lock(s_sourceMagazineLogMutex);
            if (s_sourceMagazineLoggedModels.emplace(modelName, true).second)
            {
                Game::logMsg(
                    "[VR][ManualReload] drawing detached magazine through Source viewmodel shader model=%s clipBone=%d",
                    modelName.c_str(),
                    clipBone);
            }
        }

        outBones = isolatedBones;
        return true;
    }

    inline void MaybeCaptureVrHandsVmPose(
        VR* vr,
        void* drawState,
        const std::string& modelName,
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

        VrHandVmPose::Capture(modelName.c_str(), boneNames, boneParents, boneWorldMatrices);
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
                vr->m_ManualReloadState == ManualReloadState::HoldingFreshMagazine);
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

        void Reset()
        {
            owner = nullptr;
            modelName.clear();
            numBones = 0;
            reloadStarted = {};
            initialLocalBones.clear();
            boneNames.clear();
            boneParents.clear();
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

    inline void LockManualReloadMagazineBone(
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
            return;

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
            "[VR][ManualReload] locked detachable magazine model=%s visualBone=%d visualName=%s motionBone=%d motionName=%s reason=%s visualScore=%d motionMoved=%.3fm",
            modelName.c_str(),
            visualBone,
            visualName,
            motionBone,
            motionName,
            reason ? reason : "unknown",
            visualScore,
            motionMovedMeters);
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
            if (canMotionFallback && betterByMotion(candidate, bestMotionFallback))
                bestMotionFallback = candidate;
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
            // Some replacement animations only move the helper by a few centimeters.
            // Near the end of the reload, accept the best meaningful motion instead of
            // discarding the entire manual-reload path after a hard 5 cm requirement.
            if (betterByMotion(bestConfiguredDriver, motionProbe))
                motionProbe = bestConfiguredDriver;
            if (betterByMotion(bestNamedDriver, motionProbe))
                motionProbe = bestNamedDriver;
            if (betterByMotion(bestMotionFallback, motionProbe))
                motionProbe = bestMotionFallback;
            if (motionProbe.bone >= 0 && motionProbe.movedMeters >= kMinimumUsefulMotionMeters)
                reason = "late-small-motion-probe";
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

        LockManualReloadMagazineBone(
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
            motionProbe.movedMeters);
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
        if (elapsed > 3.0f || entry.tailSamples.size() >= 240)
            return;

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
        float elapsedSeconds)
    {
        if (entry.tailSamples.empty())
            return &entry.frozenLocalBones;

        const ManualReloadTailPoseSample* selected = &entry.tailSamples.front();
        for (const ManualReloadTailPoseSample& sample : entry.tailSamples)
        {
            if (sample.timeSeconds > elapsedSeconds)
                break;
            selected = &sample;
        }
        return &selected->localBones;
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

    inline void ApplyManualReloadViewmodelOverride(
        VR* vr,
        void* drawState,
        const std::string& modelName,
        const ModelRenderInfo_t& info,
        void* viewmodelEntity,
        void*& pCustomBoneToWorld)
    {
        ManualReloadFrozenViewmodelPoseCache& frozenCache = GetManualReloadFrozenViewmodelPoseCache();

        if (!vr || !vr->IsManualReloadActive())
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
        int clipBone = GetManualReloadLockedMagazineBone(vr, modelName, numBones);
        if (clipBone < 0)
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

        if (clipBone >= 0 && viewmodelEntity)
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
                HooksMat3x4ToVrHandMatrix(modelAnchor),
                HooksMat3x4ToVrHandMatrix(clip),
                HooksMat3x4ToVrHandMatrix(motionProbe));
        }

        const bool visuallyPauseViewmodel = IsManualReloadViewmodelVisualPauseState(vr);
        const bool visuallyReplayViewmodel = IsManualReloadViewmodelVisualReplayState(vr);
        const bool hideNativeClip = vr->ShouldHideManualReloadNativeClip() && clipBone >= 0;
        if (!visuallyPauseViewmodel && !visuallyReplayViewmodel && !hideNativeClip)
        {
            if (frozenCache.owner == vr)
            {
                if (vr->m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval)
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
                if (visuallyPauseViewmodel)
                {
                    CaptureManualReloadTailPose(vr, frozenPose, modelAnchor, sourceBones, numBones);
                    ApplyManualReloadLocalPose(modelAnchor, frozenPose.frozenLocalBones, copiedBones, numBones);
                }
                else
                {
                    const float elapsedSeconds = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - vr->m_ManualReloadResumeStarted).count();
                    const auto* replayLocalBones = SelectManualReloadReplayLocalBones(frozenPose, elapsedSeconds);
                    if (replayLocalBones)
                        ApplyManualReloadLocalPose(modelAnchor, *replayLocalBones, copiedBones, numBones);
                }
            }
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
	vr_vm_stabilize::Mat3x4* manualReloadNativeMagazineBones = nullptr;
	bool drawManualReloadNativeMagazine = false;

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

		drawManualReloadNativeMagazine = BuildManualReloadNativeMagazineBones(
			m_VR,
			state,
			modelName,
			pBonesToWorldFinal,
			manualReloadNativeMagazineBones);

		ApplyManualReloadViewmodelOverride(
			m_VR,
			state,
			modelName,
			info,
			const_cast<C_BaseEntity*>(entity),
			pBonesToWorldFinal);
	}

	// Capture the exact arm matrices submitted to Source. In queued rendering
	// pBonesToWorldFinal contains the same stabilization delta as the visible gun,
	// so the standalone right glove follows controller rotation and HMD movement.
	MaybeCaptureVrHandsVmPose(m_VR, state, modelName, pBonesToWorldFinal);

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
