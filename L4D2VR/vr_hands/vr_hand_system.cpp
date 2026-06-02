#include "vr_hand_system.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_math.h"
#include "vr_hand_vm_pose.h"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct Vec3f
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct VmFingerSource
    {
        const char* bone0 = nullptr;
        const char* bone1 = nullptr;
        const char* bone2 = nullptr;
        float proximalScale = 0.85f;
        float middleScale = 1.05f;
        float distalScale = 0.70f;
        float maxProximal = 1.15f;
        float maxMiddle = 1.25f;
        float maxDistal = 0.90f;
    };

    struct GloveFingerChain
    {
        const char* meta = nullptr;
        const char* joint0 = nullptr;
        const char* joint1 = nullptr;
        const char* joint2 = nullptr;
        const char* end = nullptr;
    };

    constexpr std::uint32_t kVmPoseMaxAgeMs = 500;
    constexpr float kCurlDeadZoneRadians = 0.08f;

    int FindNameIndex(const std::vector<std::string>& names, const char* name)
    {
        if (!name || !*name)
            return -1;

        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    Vec3f MatrixTranslation(const VrHandMatrix4& matrix)
    {
        return
        {
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3)
        };
    }

    Vec3f Subtract(const Vec3f& left, const Vec3f& right)
    {
        return { left.x - right.x, left.y - right.y, left.z - right.z };
    }

    float Dot(const Vec3f& left, const Vec3f& right)
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    float LengthSq(const Vec3f& value)
    {
        return Dot(value, value);
    }

    float AngleBetween(const Vec3f& left, const Vec3f& right)
    {
        const float leftLenSq = LengthSq(left);
        const float rightLenSq = LengthSq(right);
        if (!(leftLenSq > 0.000001f) || !(rightLenSq > 0.000001f))
            return 0.0f;

        const float invLen = 1.0f / std::sqrt(leftLenSq * rightLenSq);
        const float cosine = std::clamp(Dot(left, right) * invLen, -1.0f, 1.0f);
        return std::acos(cosine);
    }

    VrHandMatrix4 MakeLocalZRotation(float radians)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        const float c = std::cos(radians);
        const float s = std::sin(radians);
        VrHandMath::Set(out, 0, 0, c);
        VrHandMath::Set(out, 0, 1, -s);
        VrHandMath::Set(out, 1, 0, s);
        VrHandMath::Set(out, 1, 1, c);
        return out;
    }

    bool GetVmHandLocalBonePosition(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& wristInverse,
        const char* boneName,
        Vec3f& outPosition)
    {
        const int bone = FindNameIndex(snapshot.boneNames, boneName);
        if (bone < 0 || bone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
            return false;

        const VrHandMatrix4 local =
            VrHandMath::Multiply(wristInverse, snapshot.boneWorldMatrices[static_cast<size_t>(bone)]);
        outPosition = MatrixTranslation(local);
        return true;
    }

    bool ComputeVmFingerCurl(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& wristInverse,
        const VmFingerSource& source,
        std::array<float, 3>& outCurl)
    {
        Vec3f wrist{};
        Vec3f bone0{};
        Vec3f bone1{};
        Vec3f bone2{};
        if (!GetVmHandLocalBonePosition(snapshot, wristInverse, source.bone0, bone0) ||
            !GetVmHandLocalBonePosition(snapshot, wristInverse, source.bone1, bone1) ||
            !GetVmHandLocalBonePosition(snapshot, wristInverse, source.bone2, bone2))
        {
            return false;
        }

        const Vec3f segment0 = Subtract(bone0, wrist);
        const Vec3f segment1 = Subtract(bone1, bone0);
        const Vec3f segment2 = Subtract(bone2, bone1);

        const float proximal = std::max(0.0f, AngleBetween(segment0, segment1) - kCurlDeadZoneRadians);
        const float middle = std::max(0.0f, AngleBetween(segment1, segment2) - kCurlDeadZoneRadians);
        outCurl[0] = std::clamp(proximal * source.proximalScale, 0.0f, source.maxProximal);
        outCurl[1] = std::clamp(middle * source.middleScale, 0.0f, source.maxMiddle);
        outCurl[2] = std::clamp(middle * source.distalScale, 0.0f, source.maxDistal);
        return true;
    }

    int FindAssetJoint(const VrHandMeshAsset& asset, const char* name)
    {
        return FindNameIndex(asset.jointNames, name);
    }

    bool BuildChainIndices(
        const VrHandMeshAsset& asset,
        const GloveFingerChain& chain,
        std::array<int, 5>& outJoints,
        int& outCount)
    {
        outCount = 0;
        outJoints.fill(-1);

        const char* names[] = { chain.meta, chain.joint0, chain.joint1, chain.joint2, chain.end };
        for (const char* name : names)
        {
            if (!name || !*name)
                continue;

            const int joint = FindAssetJoint(asset, name);
            if (joint < 0)
            {
                if (name == chain.end)
                    continue;
                return false;
            }

            outJoints[static_cast<size_t>(outCount++)] = joint;
        }

        return outCount >= 4;
    }

    VrHandMatrix4 BuildBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
    {
        const int parent = asset.jointParents[static_cast<size_t>(joint)];
        if (parent >= 0 && parent < static_cast<int>(asset.inverseBindMatrices.size()))
            return VrHandMath::Multiply(
                asset.inverseBindMatrices[static_cast<size_t>(parent)],
                asset.bindMatrices[static_cast<size_t>(joint)]);
        return asset.bindMatrices[static_cast<size_t>(joint)];
    }

    void ApplyFingerCurl(
        const VrHandMeshAsset& asset,
        const GloveFingerChain& chain,
        const std::array<float, 3>& curl,
        float sideSign,
        std::vector<VrHandMatrix4>& finalModelMatrices,
        std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        std::array<int, 5> joints{};
        int jointCount = 0;
        if (!BuildChainIndices(asset, chain, joints, jointCount))
            return;

        const int firstBendChainIndex = (chain.meta && *chain.meta) ? 1 : 0;
        for (int chainIndex = 0; chainIndex < jointCount; ++chainIndex)
        {
            const int joint = joints[static_cast<size_t>(chainIndex)];
            const int parent = asset.jointParents[static_cast<size_t>(joint)];
            VrHandMatrix4 local = BuildBindLocalMatrix(asset, joint);

            const int curlIndex = chainIndex - firstBendChainIndex;
            if (curlIndex >= 0 && curlIndex < 3)
            {
                const float rotation = curl[static_cast<size_t>(curlIndex)] * sideSign;
                local = VrHandMath::Multiply(local, MakeLocalZRotation(rotation));
            }

            if (parent >= 0 && parent < static_cast<int>(finalModelMatrices.size()))
                finalModelMatrices[static_cast<size_t>(joint)] =
                    VrHandMath::Multiply(finalModelMatrices[static_cast<size_t>(parent)], local);
            else
                finalModelMatrices[static_cast<size_t>(joint)] = local;

            const VrHandMatrix4 skinning = VrHandMath::Multiply(
                finalModelMatrices[static_cast<size_t>(joint)],
                asset.inverseBindMatrices[static_cast<size_t>(joint)]);
            outPalette[static_cast<size_t>(joint)] = VrHandMath::ToRows3x4(skinning);
        }
    }

    bool BuildVmDrivenHandPalette(
        int handIndex,
        const VrHandMeshAsset& asset,
        const VrHandVmPose::Snapshot& snapshot,
        std::vector<VrHandMatrixRows3x4>& outPalette)
    {
        outPalette.clear();
        if (!asset.IsValid() ||
            asset.jointParents.size() != asset.jointNames.size() ||
            asset.bindMatrices.size() != asset.jointNames.size())
        {
            return false;
        }
        if (snapshot.boneWorldMatrices.size() < snapshot.boneNames.size())
            return false;

        static const VmFingerSource kLeftSources[] =
        {
            { "ValveBiped.Bip01_L_Finger0", "ValveBiped.Bip01_L_Finger01", "ValveBiped.Bip01_L_Finger02", 0.45f, 0.75f, 0.50f, 0.75f, 0.90f, 0.65f },
            { "ValveBiped.Bip01_L_Finger1", "ValveBiped.Bip01_L_Finger11", "ValveBiped.Bip01_L_Finger12" },
            { "ValveBiped.Bip01_L_Finger2", "ValveBiped.Bip01_L_Finger21", "ValveBiped.Bip01_L_Finger22" },
            { "ValveBiped.Bip01_L_Finger3", "ValveBiped.Bip01_L_Finger31", "ValveBiped.Bip01_L_Finger32" },
            { "ValveBiped.Bip01_L_Finger4", "ValveBiped.Bip01_L_Finger41", "ValveBiped.Bip01_L_Finger42" },
        };

        static const VmFingerSource kRightSources[] =
        {
            { "ValveBiped.Bip01_R_Finger0", "ValveBiped.Bip01_R_Finger01", "ValveBiped.Bip01_R_Finger02", 0.45f, 0.75f, 0.50f, 0.75f, 0.90f, 0.65f },
            { "ValveBiped.Bip01_R_Finger1", "ValveBiped.Bip01_R_Finger11", "ValveBiped.Bip01_R_Finger12" },
            { "ValveBiped.Bip01_R_Finger2", "ValveBiped.Bip01_R_Finger21", "ValveBiped.Bip01_R_Finger22" },
            { "ValveBiped.Bip01_R_Finger3", "ValveBiped.Bip01_R_Finger31", "ValveBiped.Bip01_R_Finger32" },
            { "ValveBiped.Bip01_R_Finger4", "ValveBiped.Bip01_R_Finger41", "ValveBiped.Bip01_R_Finger42" },
        };

        static const GloveFingerChain kLeftChains[] =
        {
            { nullptr, "finger_thumb_0_l", "finger_thumb_1_l", "finger_thumb_2_l", "finger_thumb_l_end" },
            { "finger_index_meta_l", "finger_index_0_l", "finger_index_1_l", "finger_index_2_l", "finger_index_l_end" },
            { "finger_middle_meta_l", "finger_middle_0_l", "finger_middle_1_l", "finger_middle_2_l", "finger_middle_l_end" },
            { "finger_ring_meta_l", "finger_ring_0_l", "finger_ring_1_l", "finger_ring_2_l", nullptr },
            { "finger_pinky_meta_l", "finger_pinky_0_l", "finger_pinky_1_l", "finger_pinky_2_l", "finger_pinky_l_end" },
        };

        static const GloveFingerChain kRightChains[] =
        {
            { nullptr, "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r", "finger_thumb_r_end" },
            { "finger_index_meta_r", "finger_index_0_r", "finger_index_1_r", "finger_index_2_r", "finger_index_r_end" },
            { "finger_middle_meta_r", "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r", "finger_middle_r_end" },
            { "finger_ring_meta_r", "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r", "finger_ring_r_end" },
            { "finger_pinky_meta_r", "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r", "finger_pinky_r_end" },
        };

        const char* wristBoneName = (handIndex == 0)
            ? "ValveBiped.Bip01_L_Hand"
            : "ValveBiped.Bip01_R_Hand";
        const int wristBone = FindNameIndex(snapshot.boneNames, wristBoneName);
        if (wristBone < 0 || wristBone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
            return false;

        VrHandMatrix4 wristInverse{};
        if (!VrHandMath::Invert4x4(snapshot.boneWorldMatrices[static_cast<size_t>(wristBone)], wristInverse))
            return false;

        outPalette.assign(asset.jointNames.size(), VrHandMath::ToRows3x4(VrHandMath::Identity()));
        std::vector<VrHandMatrix4> finalModelMatrices = asset.bindMatrices;

        const VmFingerSource* sources = (handIndex == 0) ? kLeftSources : kRightSources;
        const GloveFingerChain* chains = (handIndex == 0) ? kLeftChains : kRightChains;
        const size_t fingerCount = 5;
        const float sideSign = (handIndex == 0) ? 1.0f : -1.0f;

        int applied = 0;
        for (size_t finger = 0; finger < fingerCount; ++finger)
        {
            std::array<float, 3> curl{};
            if (!ComputeVmFingerCurl(snapshot, wristInverse, sources[finger], curl))
                continue;

            ApplyFingerCurl(asset, chains[finger], curl, sideSign, finalModelMatrices, outPalette);
            ++applied;
        }

        return applied >= 2;
    }
}

VrHandSystem::VrHandSystem()
{
    m_Hands[0].actionPath = "/actions/main/in/SkeletonLeftHand";
    m_Hands[0].assetFileName = "vr_glove_left_model.glb";
    m_Hands[1].actionPath = "/actions/main/in/SkeletonRightHand";
    m_Hands[1].assetFileName = "vr_glove_right_model.glb";
}

VrHandSystem::~VrHandSystem() = default;

void VrHandSystem::ReportErrorOnce(const std::string& error)
{
    if (error.empty() || !m_ReportedErrors.emplace(error).second)
        return;
    Game::errorMsg(("[VR][Hands] " + error).c_str());
}

bool VrHandSystem::ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const
{
    outPath.clear();

    std::vector<char> buffer(vr::k_unMaxPropertyStringSize, '\0');
    uint32_t required = 0;
    if (!vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
        return false;
    if (required > buffer.size())
    {
        buffer.assign(static_cast<size_t>(required) + 1u, '\0');
        if (!vr::VR_GetRuntimePath(buffer.data(), static_cast<uint32_t>(buffer.size()), &required))
            return false;
    }
    if (buffer.empty() || buffer.front() == '\0')
        return false;

    const std::filesystem::path path = std::filesystem::path(buffer.data()) /
        "resources" / "rendermodels" / "vr_glove" / fileName;
    outPath = path.string();
    return std::filesystem::exists(path);
}

bool VrHandSystem::EnsureAssetsLoaded(bool debugLog)
{
    if (m_AssetsLoaded)
        return true;
    if (m_AssetLoadAttempted)
        return false;
    m_AssetLoadAttempted = true;

    for (HandState& hand : m_Hands)
    {
        std::string assetPath;
        if (!ResolveSteamVrAssetPath(hand.assetFileName, assetPath))
        {
            ReportErrorOnce(std::string("cannot find SteamVR glove asset: ") + hand.assetFileName);
            return false;
        }

        std::string error;
        if (!VrHandAssetLoader::LoadGlb(assetPath, hand.asset, error))
        {
            ReportErrorOnce(error + ": " + assetPath);
            return false;
        }
    }

    m_AssetsLoaded = true;
    if (debugLog)
    {
        Game::logMsg(
            "[VR][Hands] assets loaded left=%u vertices right=%u vertices",
            static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
            static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
    }
    return true;
}

bool VrHandSystem::EnsureInitialized(vr::IVRInput* input, bool debugLog)
{
    if (m_Initialized)
        return true;
    if (m_InitializationAttempted || !input)
        return false;
    if (!EnsureAssetsLoaded(debugLog))
        return false;

    for (HandState& hand : m_Hands)
    {
        if (input->GetActionHandle(hand.actionPath, &hand.action) != vr::VRInputError_None ||
            hand.action == vr::k_ulInvalidActionHandle)
        {
            m_InitializationAttempted = true;
            ReportErrorOnce(std::string("missing SteamVR skeletal action: ") + hand.actionPath);
            return false;
        }

        vr::InputSkeletalActionData_t actionData{};
        const vr::EVRInputError actionDataResult = input->GetSkeletalActionData(hand.action, &actionData, sizeof(actionData));
        if (actionDataResult != vr::VRInputError_None || !actionData.bActive)
        {
            if (debugLog)
            {
                static DWORD lastLogTick = 0;
                const DWORD now = GetTickCount();
                if (now - lastLogTick >= 1000u)
                {
                    lastLogTick = now;
                    Game::logMsg("[VR][Hands] waiting for active skeletal action: %s", hand.actionPath);
                }
            }
            return false;
        }
    }

    m_InitializationAttempted = true;
    for (HandState& hand : m_Hands)
    {
        std::string error;
        if (!hand.skeleton.Initialize(input, hand.action, error))
        {
            ReportErrorOnce(error + ": " + hand.actionPath);
            return false;
        }
    }

    m_Initialized = true;
    if (debugLog && !m_DebugInitializationLogged)
    {
        m_DebugInitializationLogged = true;
        Game::logMsg(
            "[VR][Hands] initialized left=%d joints/%u vertices right=%d joints/%u vertices",
            m_Hands[0].skeleton.JointCount(),
            static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
            m_Hands[1].skeleton.JointCount(),
            static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
    }
    return true;
}

void VrHandSystem::UpdatePoses(vr::IVRInput* input, bool motionRangeWithoutController, bool debugLog)
{
    const vr::EVRSkeletalMotionRange motionRange = motionRangeWithoutController
        ? vr::VRSkeletalMotionRange_WithoutController
        : vr::VRSkeletalMotionRange_WithController;

    for (HandState& hand : m_Hands)
    {
        std::string error;
        hand.paletteValid = false;
        if (!hand.skeleton.Update(input, motionRange, error))
        {
            if (!error.empty())
                ReportErrorOnce(error + ": " + hand.actionPath);
            continue;
        }
        if (!hand.skeleton.BuildSkinningPalette(hand.asset, hand.palette, error))
        {
            if (!error.empty())
                ReportErrorOnce(error + ": " + hand.asset.sourcePath);
            continue;
        }
        hand.paletteValid = true;
    }

    if (debugLog)
    {
        static DWORD lastLogTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastLogTick >= 1000u)
        {
            lastLogTick = now;
            Game::logMsg(
                "[VR][Hands] pose left=%d right=%d motionRange=%s",
                m_Hands[0].paletteValid ? 1 : 0,
                m_Hands[1].paletteValid ? 1 : 0,
                motionRangeWithoutController ? "without-controller" : "with-controller");
        }
    }
}

void VrHandSystem::DrawForEye(
    IDirect3DDevice9* device,
    vr::IVRInput* input,
    const CViewSetup& view,
    int eyeIndex,
    float vrScale,
    float modelScale,
    bool motionRangeWithoutController,
    bool allowControllerlessTestPose,
    bool debugLog,
    float sceneLightScale,
    const Vector& leftControllerPosition,
    const QAngle& leftControllerAngles,
    const Vector& rightControllerPosition,
    const QAngle& rightControllerAngles)
{
    if (!device || !input)
        return;
    if (!EnsureInitialized(input, debugLog))
    {
        if (allowControllerlessTestPose)
            DrawControllerlessTestPose(device, view, vrScale, modelScale, sceneLightScale, debugLog);
        return;
    }

    if (eyeIndex == 0 || (!m_Hands[0].paletteValid && !m_Hands[1].paletteValid))
        UpdatePoses(input, motionRangeWithoutController, debugLog);

    float aspect = view.m_flAspectRatio;
    if (!(aspect > 0.001f) && view.height > 0)
        aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(view.fov, aspect, view.zNear, view.zFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale * std::clamp(modelScale, 0.25f, 4.0f);

    const Vector positions[2] = { leftControllerPosition, rightControllerPosition };
    const QAngle angles[2] = { leftControllerAngles, rightControllerAngles };
    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const HandState& hand = m_Hands[static_cast<size_t>(handIndex)];
        if (!hand.paletteValid)
            continue;

        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(positions[handIndex], angles[handIndex], sourceUnitsPerMeter);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, hand.palette, world, wvp, sceneLightScale, error))
            ReportErrorOnce(error);
    }
}

void VrHandSystem::DrawControllerlessTestPose(
    IDirect3DDevice9* device,
    const CViewSetup& view,
    float vrScale,
    float modelScale,
    float sceneLightScale,
    bool debugLog)
{
    if (!EnsureAssetsLoaded(debugLog))
        return;

    float aspect = view.m_flAspectRatio;
    if (!(aspect > 0.001f) && view.height > 0)
        aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(view.fov, aspect, view.zNear, view.zFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale * std::clamp(modelScale, 0.25f, 4.0f);

    Vector forward{};
    Vector right{};
    Vector up{};
    QAngle viewAngles(view.angles.x, view.angles.y, view.angles.z);
    QAngle::AngleVectors(viewAngles, &forward, &right, &up);

    const Vector base = view.origin + forward * 42.0f - up * 16.0f;
    const Vector positions[2] =
    {
        base - right * 12.0f,
        base + right * 12.0f
    };

    VrHandVmPose::Snapshot vmPose{};
    const bool hasVmPose = VrHandVmPose::GetLatest(vmPose, kVmPoseMaxAgeMs);
    bool usedVmPose = false;

    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const HandState& hand = m_Hands[static_cast<size_t>(handIndex)];
        if (!hand.asset.IsValid() || hand.asset.jointNames.empty())
            continue;

        std::vector<VrHandMatrixRows3x4> bindPalette(hand.asset.jointNames.size(), VrHandMath::ToRows3x4(VrHandMath::Identity()));
        std::vector<VrHandMatrixRows3x4> vmPalette;
        if (hasVmPose && BuildVmDrivenHandPalette(handIndex, hand.asset, vmPose, vmPalette))
        {
            bindPalette = std::move(vmPalette);
            usedVmPose = true;
        }

        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(positions[handIndex], viewAngles, sourceUnitsPerMeter);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, bindPalette, world, wvp, sceneLightScale, error))
            ReportErrorOnce(error);
    }

    if (debugLog && usedVmPose && !m_DebugVmPoseLogged)
    {
        m_DebugVmPoseLogged = true;
        Game::logMsg(
            "[VR][Hands] drawing mouse-mode VM-driven hand pose model=\"%s\" seq=%u",
            vmPose.modelName.c_str(),
            vmPose.sequence);
    }
    else if (debugLog && !usedVmPose && !m_DebugTestPoseLogged)
    {
        m_DebugTestPoseLogged = true;
        Game::logMsg("[VR][Hands] drawing mouse-mode controllerless test pose");
    }
}

void VrHandSystem::OnDeviceLost()
{
    m_Renderer.OnDeviceLost();
}
