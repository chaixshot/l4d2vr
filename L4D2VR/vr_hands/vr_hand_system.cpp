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
    struct VrHandsVec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct VrHandsVmFingerSource
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

    struct VrHandsGloveFingerChain
    {
        const char* joint0 = nullptr;
        const char* joint1 = nullptr;
        const char* joint2 = nullptr;
    };

    constexpr std::uint32_t kVrHandsVmPoseMaxAgeMs = 500u;
    constexpr float kVrHandsVmCurlDeadZoneRadians = 0.08f;

    int FindVrHandsNameIndex(const std::vector<std::string>& names, const char* name)
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

    VrHandMatrix4 BuildVrHandsBindLocalMatrix(const VrHandMeshAsset& asset, int joint)
    {
        const int parent = asset.jointParents[static_cast<size_t>(joint)];
        if (parent >= 0 && parent < static_cast<int>(asset.inverseBindMatrices.size()))
        {
            return VrHandMath::Multiply(
                asset.inverseBindMatrices[static_cast<size_t>(parent)],
                asset.bindMatrices[static_cast<size_t>(joint)]);
        }
        return asset.bindMatrices[static_cast<size_t>(joint)];
    }

    VrHandsVec3 VrHandsMatrixTranslation(const VrHandMatrix4& matrix)
    {
        return
        {
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3)
        };
    }

    VrHandsVec3 VrHandsSubtract(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return { left.x - right.x, left.y - right.y, left.z - right.z };
    }

    float VrHandsDot(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    float VrHandsAngleBetween(const VrHandsVec3& left, const VrHandsVec3& right)
    {
        const float leftLenSq = VrHandsDot(left, left);
        const float rightLenSq = VrHandsDot(right, right);
        if (!(leftLenSq > 0.000001f) || !(rightLenSq > 0.000001f))
            return 0.0f;
        const float invLen = 1.0f / std::sqrt(leftLenSq * rightLenSq);
        return std::acos(std::clamp(VrHandsDot(left, right) * invLen, -1.0f, 1.0f));
    }

    VrHandMatrix4 MakeVrHandsLocalZRotation(float radians)
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

    bool GetVrHandsVmBonePositionInPalmSpace(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& palmInverse,
        const char* boneName,
        VrHandsVec3& outPosition)
    {
        const int bone = FindVrHandsNameIndex(snapshot.boneNames, boneName);
        if (bone < 0 || bone >= static_cast<int>(snapshot.boneWorldMatrices.size()))
            return false;
        outPosition = VrHandsMatrixTranslation(VrHandMath::Multiply(
            palmInverse,
            snapshot.boneWorldMatrices[static_cast<size_t>(bone)]));
        return true;
    }

    bool ComputeVrHandsVmFingerCurl(
        const VrHandVmPose::Snapshot& snapshot,
        const VrHandMatrix4& palmInverse,
        const VrHandsVmFingerSource& source,
        std::array<float, 3>& outCurl)
    {
        VrHandsVec3 palm{};
        VrHandsVec3 bone0{};
        VrHandsVec3 bone1{};
        VrHandsVec3 bone2{};
        if (!GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone0, bone0) ||
            !GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone1, bone1) ||
            !GetVrHandsVmBonePositionInPalmSpace(snapshot, palmInverse, source.bone2, bone2))
        {
            return false;
        }

        const VrHandsVec3 segment0 = VrHandsSubtract(bone0, palm);
        const VrHandsVec3 segment1 = VrHandsSubtract(bone1, bone0);
        const VrHandsVec3 segment2 = VrHandsSubtract(bone2, bone1);
        const float proximal = std::max(0.0f, VrHandsAngleBetween(segment0, segment1) - kVrHandsVmCurlDeadZoneRadians);
        const float middle = std::max(0.0f, VrHandsAngleBetween(segment1, segment2) - kVrHandsVmCurlDeadZoneRadians);
        outCurl[0] = std::clamp(proximal * source.proximalScale, 0.0f, source.maxProximal);
        outCurl[1] = std::clamp(middle * source.middleScale, 0.0f, source.maxMiddle);
        outCurl[2] = std::clamp(middle * source.distalScale, 0.0f, source.maxDistal);
        return true;
    }

    bool VrHandsSameVector(const Vector& left, const Vector& right)
    {
        return left.x == right.x && left.y == right.y && left.z == right.z;
    }
}

VrHandSystem::VrHandSystem()
{
    m_Hands[0].actionPath = "/actions/base/in/skeleton_lefthand";
    m_Hands[0].assetFileName = "vr_glove_left_model.glb";
    m_Hands[1].actionPath = "/actions/base/in/skeleton_righthand";
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

bool VrHandSystem::EnsureInitialized(vr::IVRInput* input, bool rightUseViewmodelPose, bool debugLog)
{
    if (!input || !EnsureAssetsLoaded(debugLog))
        return false;

    for (size_t handIndex = 0; handIndex < m_Hands.size(); ++handIndex)
    {
        HandState& hand = m_Hands[handIndex];
        if (handIndex == 1u && rightUseViewmodelPose)
            continue;
        if (hand.skeletonInitialized)
            continue;

        if (hand.action == vr::k_ulInvalidActionHandle &&
            (input->GetActionHandle(hand.actionPath, &hand.action) != vr::VRInputError_None ||
                hand.action == vr::k_ulInvalidActionHandle))
        {
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

        std::string error;
        if (!hand.skeleton.Initialize(input, hand.action, error))
        {
            ReportErrorOnce(error + ": " + hand.actionPath);
            return false;
        }
        hand.skeletonInitialized = true;
    }

    m_Initialized = m_Hands[0].skeletonInitialized &&
        (rightUseViewmodelPose || m_Hands[1].skeletonInitialized);
    if (m_Initialized && debugLog && !m_DebugInitializationLogged)
    {
        m_DebugInitializationLogged = true;
        if (rightUseViewmodelPose)
        {
            Game::logMsg(
                "[VR][Hands] initialized left=%d joints/%u vertices right=viewmodel/%u vertices",
                m_Hands[0].skeleton.JointCount(),
                static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
                static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
        }
        else
        {
            Game::logMsg(
                "[VR][Hands] initialized left=%d joints/%u vertices right=%d joints/%u vertices",
                m_Hands[0].skeleton.JointCount(),
                static_cast<unsigned int>(m_Hands[0].asset.vertices.size()),
                m_Hands[1].skeleton.JointCount(),
                static_cast<unsigned int>(m_Hands[1].asset.vertices.size()));
        }
    }
    return m_Initialized;
}

bool VrHandSystem::BuildRightViewmodelPalette(
    const VrHandVmPose::Snapshot& snapshot,
    std::vector<VrHandMatrixRows3x4>& outPalette)
{
    outPalette.clear();
    m_RightViewmodelPalmWorldValid = false;

    HandState& hand = m_Hands[1];
    const VrHandMeshAsset& asset = hand.asset;
    if (!asset.IsValid() || snapshot.boneWorldMatrices.size() < snapshot.boneNames.size())
        return false;

    const int vmPalm = FindVrHandsNameIndex(snapshot.boneNames, "ValveBiped.Bip01_R_Hand");
    if (vmPalm < 0 || vmPalm >= static_cast<int>(snapshot.boneWorldMatrices.size()))
        return false;

    if (m_RightViewmodelPoseModel != snapshot.modelName)
    {
        m_RightViewmodelPoseModel = snapshot.modelName;
        m_RightViewmodelAnchorValid = false;
    }
    m_RightViewmodelPalmWorld = snapshot.boneWorldMatrices[static_cast<size_t>(vmPalm)];
    m_RightViewmodelPalmWorldValid = true;

    VrHandMatrix4 palmInverse{};
    if (!VrHandMath::Invert4x4(m_RightViewmodelPalmWorld, palmInverse))
    {
        m_RightViewmodelPalmWorldValid = false;
        return false;
    }

    std::vector<VrHandMatrix4> localMatrices(asset.jointNames.size(), VrHandMath::Identity());
    for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        localMatrices[joint] = BuildVrHandsBindLocalMatrix(asset, static_cast<int>(joint));

    static const VrHandsVmFingerSource kSources[] =
    {
        { "ValveBiped.Bip01_R_Finger0", "ValveBiped.Bip01_R_Finger01", "ValveBiped.Bip01_R_Finger02", 0.45f, 0.75f, 0.50f, 0.75f, 0.90f, 0.65f },
        { "ValveBiped.Bip01_R_Finger1", "ValveBiped.Bip01_R_Finger11", "ValveBiped.Bip01_R_Finger12" },
        { "ValveBiped.Bip01_R_Finger2", "ValveBiped.Bip01_R_Finger21", "ValveBiped.Bip01_R_Finger22" },
        { "ValveBiped.Bip01_R_Finger3", "ValveBiped.Bip01_R_Finger31", "ValveBiped.Bip01_R_Finger32" },
        { "ValveBiped.Bip01_R_Finger4", "ValveBiped.Bip01_R_Finger41", "ValveBiped.Bip01_R_Finger42" },
    };
    static const VrHandsGloveFingerChain kChains[] =
    {
        { "finger_thumb_0_r", "finger_thumb_1_r", "finger_thumb_2_r" },
        { "finger_index_0_r", "finger_index_1_r", "finger_index_2_r" },
        { "finger_middle_0_r", "finger_middle_1_r", "finger_middle_2_r" },
        { "finger_ring_0_r", "finger_ring_1_r", "finger_ring_2_r" },
        { "finger_pinky_0_r", "finger_pinky_1_r", "finger_pinky_2_r" },
    };

    int appliedFingerCount = 0;
    for (size_t finger = 0; finger < 5u; ++finger)
    {
        std::array<float, 3> curl{};
        if (!ComputeVrHandsVmFingerCurl(snapshot, palmInverse, kSources[finger], curl))
            continue;

        const char* joints[] = { kChains[finger].joint0, kChains[finger].joint1, kChains[finger].joint2 };
        bool applied = true;
        for (size_t segment = 0; segment < 3u; ++segment)
        {
            const int joint = FindVrHandsNameIndex(asset.jointNames, joints[segment]);
            if (joint < 0)
            {
                applied = false;
                break;
            }

            // The right SteamVR glove is already authored as a mirrored right-hand rig.
            // Positive local-Z flexion bends the fingers into the palm. Negating it
            // opens them outwards and produces the inverted grip seen in-game.
            localMatrices[static_cast<size_t>(joint)] = VrHandMath::Multiply(
                localMatrices[static_cast<size_t>(joint)],
                MakeVrHandsLocalZRotation(curl[segment]));
        }
        if (applied)
            ++appliedFingerCount;
    }
    if (appliedFingerCount < 5)
    {
        m_RightViewmodelPalmWorldValid = false;
        return false;
    }

    std::vector<VrHandMatrix4> modelMatrices(asset.jointNames.size(), VrHandMath::Identity());
    std::vector<bool> resolved(asset.jointNames.size(), false);
    size_t unresolved = asset.jointNames.size();
    for (size_t pass = 0; pass < asset.jointNames.size() && unresolved > 0; ++pass)
    {
        bool progressed = false;
        for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
        {
            if (resolved[joint])
                continue;
            const int parent = asset.jointParents[joint];
            if (parent >= 0 && (parent >= static_cast<int>(resolved.size()) || !resolved[static_cast<size_t>(parent)]))
                continue;
            modelMatrices[joint] = (parent >= 0)
                ? VrHandMath::Multiply(modelMatrices[static_cast<size_t>(parent)], localMatrices[joint])
                : localMatrices[joint];
            resolved[joint] = true;
            --unresolved;
            progressed = true;
        }
        if (!progressed)
        {
            m_RightViewmodelPalmWorldValid = false;
            return false;
        }
    }

    outPalette.resize(asset.jointNames.size());
    for (size_t joint = 0; joint < asset.jointNames.size(); ++joint)
    {
        outPalette[joint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(
            modelMatrices[joint],
            asset.inverseBindMatrices[joint]));
    }
    return true;
}

void VrHandSystem::UpdatePoses(
    vr::IVRInput* input,
    bool motionRangeWithoutController,
    bool rightUseViewmodelPose,
    bool debugLog)
{
    if (m_RightViewmodelPoseWasEnabled != rightUseViewmodelPose)
    {
        m_RightViewmodelPoseWasEnabled = rightUseViewmodelPose;
        m_RightViewmodelPalmWorldValid = false;
        m_RightViewmodelAnchorValid = false;
        m_RightViewmodelPoseModel.clear();
        m_DebugRightViewmodelPoseLogged = false;
        m_DebugRightViewmodelPoseMissingLogged = false;
    }

    const vr::EVRSkeletalMotionRange motionRange = motionRangeWithoutController
        ? vr::VRSkeletalMotionRange_WithoutController
        : vr::VRSkeletalMotionRange_WithController;

    for (size_t handIndex = 0; handIndex < m_Hands.size(); ++handIndex)
    {
        HandState& hand = m_Hands[handIndex];
        hand.paletteValid = false;
        if (handIndex == 1u && rightUseViewmodelPose)
            continue;
        if (!hand.skeletonInitialized)
            continue;

        std::string error;
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

    bool hasRightViewmodelPose = false;
    if (rightUseViewmodelPose)
    {
        VrHandVmPose::Snapshot snapshot{};
        if (VrHandVmPose::GetLatest(snapshot, kVrHandsVmPoseMaxAgeMs) &&
            BuildRightViewmodelPalette(snapshot, m_Hands[1].palette))
        {
            m_Hands[1].paletteValid = true;
            hasRightViewmodelPose = true;
            if (debugLog && !m_DebugRightViewmodelPoseLogged)
            {
                m_DebugRightViewmodelPoseLogged = true;
                Game::logMsg(
                    "[VR][Hands] right hand uses viewmodel pose model=\"%s\" seq=%u",
                    snapshot.modelName.c_str(),
                    snapshot.sequence);
            }
        }
        else if (debugLog && !m_DebugRightViewmodelPoseMissingLogged)
        {
            m_DebugRightViewmodelPoseMissingLogged = true;
            Game::logMsg("[VR][Hands] right viewmodel pose enabled; waiting for a valid VM arm snapshot");
        }
    }

    if (debugLog)
    {
        static DWORD lastLogTick = 0;
        const DWORD now = GetTickCount();
        if (now - lastLogTick >= 1000u)
        {
            lastLogTick = now;
            Game::logMsg(
                "[VR][Hands] pose left=%d right=%d rightSource=%s motionRange=%s",
                m_Hands[0].paletteValid ? 1 : 0,
                m_Hands[1].paletteValid ? 1 : 0,
                rightUseViewmodelPose ? (hasRightViewmodelPose ? "viewmodel" : "viewmodel-waiting") : "openvr",
                motionRangeWithoutController ? "without-controller" : "with-controller");
        }
    }
}

bool VrHandSystem::DrawForEye(
    IDirect3DDevice9* device,
    vr::IVRInput* input,
    const CViewSetup& view,
    int eyeIndex,
    float vrScale,
    float modelScale,
    bool motionRangeWithoutController,
    bool rightUseViewmodelPose,
    bool allowControllerlessTestPose,
    bool debugLog,
    float sceneLightScale,
    const Vector& leftControllerPosition,
    const QAngle& leftControllerAngles,
    const Vector& rightControllerPosition,
    const QAngle& rightControllerAngles,
    const Vector& leftHandPoseOffsetMeters,
    const Vector& leftHandPoseRotationOffsetDeg,
    const Vector& rightHandPoseOffsetMeters,
    const Vector& rightHandPoseRotationOffsetDeg,
    VrHandDrawPass drawPass)
{
    if (!device || !input)
        return false;
    if (!EnsureInitialized(input, rightUseViewmodelPose, debugLog))
    {
        if (allowControllerlessTestPose)
            return DrawControllerlessTestPose(
                device,
                view,
                vrScale,
                modelScale,
                rightUseViewmodelPose,
                sceneLightScale,
                debugLog,
                leftHandPoseOffsetMeters,
                leftHandPoseRotationOffsetDeg,
                rightHandPoseOffsetMeters,
                rightHandPoseRotationOffsetDeg,
                drawPass);
        return false;
    }

    if (eyeIndex == 0 || (!m_Hands[0].paletteValid && !m_Hands[1].paletteValid))
        UpdatePoses(input, motionRangeWithoutController, rightUseViewmodelPose, debugLog);

    float aspect = view.m_flAspectRatio;
    if (!(aspect > 0.001f) && view.height > 0)
        aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(view.fov, aspect, view.zNear, view.zFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale;
    const float clampedModelScale = std::clamp(modelScale, 0.25f, 4.0f);

    bool drewAny = false;
    const Vector positions[2] = { leftControllerPosition, rightControllerPosition };
    const QAngle angles[2] = { leftControllerAngles, rightControllerAngles };
    const Vector positionOffsets[2] = { leftHandPoseOffsetMeters, rightHandPoseOffsetMeters };
    const Vector rotationOffsets[2] = { leftHandPoseRotationOffsetDeg, rightHandPoseRotationOffsetDeg };
    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const HandState& hand = m_Hands[static_cast<size_t>(handIndex)];
        if (!hand.paletteValid)
            continue;

        VrHandMatrix4 world = VrHandMath::BuildControllerWorld(
            positions[handIndex],
            angles[handIndex],
            sourceUnitsPerMeter,
            clampedModelScale,
            positionOffsets[handIndex],
            rotationOffsets[handIndex]);

        if (handIndex == 1 && rightUseViewmodelPose)
        {
            if (!m_RightViewmodelPalmWorldValid)
                continue;

            const bool calibrationChanged =
                !m_RightViewmodelAnchorValid ||
                m_RightViewmodelAnchorModel != m_RightViewmodelPoseModel ||
                m_RightViewmodelAnchorVrScale != sourceUnitsPerMeter ||
                m_RightViewmodelAnchorModelScale != clampedModelScale ||
                !VrHandsSameVector(m_RightViewmodelAnchorPoseOffsetMeters, positionOffsets[handIndex]) ||
                !VrHandsSameVector(m_RightViewmodelAnchorPoseRotationOffsetDeg, rotationOffsets[handIndex]);
            if (calibrationChanged)
            {
                VrHandMatrix4 palmInverse{};
                if (!VrHandMath::Invert4x4(m_RightViewmodelPalmWorld, palmInverse))
                    continue;

                // Keep the known-good OpenVR/controller placement as a one-time static
                // calibration, then drive the complete glove world transform from the
                // current VM palm matrix. This binds the glove to the weapon animation
                // without feeding VM translation back through the scaled skinning palette.
                m_RightViewmodelPalmToGloveWorld = VrHandMath::Multiply(palmInverse, world);
                m_RightViewmodelAnchorModel = m_RightViewmodelPoseModel;
                m_RightViewmodelAnchorVrScale = sourceUnitsPerMeter;
                m_RightViewmodelAnchorModelScale = clampedModelScale;
                m_RightViewmodelAnchorPoseOffsetMeters = positionOffsets[handIndex];
                m_RightViewmodelAnchorPoseRotationOffsetDeg = rotationOffsets[handIndex];
                m_RightViewmodelAnchorValid = true;

                if (debugLog)
                {
                    Game::logMsg(
                        "[VR][Hands] calibrated right VM palm anchor model=\"%s\"",
                        m_RightViewmodelAnchorModel.c_str());
                }
            }

            world = VrHandMath::Multiply(m_RightViewmodelPalmWorld, m_RightViewmodelPalmToGloveWorld);
        }

        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, hand.palette, world, wvp, drawPass, sceneLightScale, error))
            ReportErrorOnce(error);
        else
            drewAny = true;
    }
    return drewAny;
}

bool VrHandSystem::DrawControllerlessTestPose(
    IDirect3DDevice9* device,
    const CViewSetup& view,
    float vrScale,
    float modelScale,
    bool rightUseViewmodelPose,
    float sceneLightScale,
    bool debugLog,
    const Vector& leftHandPoseOffsetMeters,
    const Vector& leftHandPoseRotationOffsetDeg,
    const Vector& rightHandPoseOffsetMeters,
    const Vector& rightHandPoseRotationOffsetDeg,
    VrHandDrawPass drawPass)
{
    if (!EnsureAssetsLoaded(debugLog))
        return false;

    float aspect = view.m_flAspectRatio;
    if (!(aspect > 0.001f) && view.height > 0)
        aspect = static_cast<float>(view.width) / static_cast<float>(view.height);
    const VrHandMatrix4 projection = VrHandMath::BuildPerspective(view.fov, aspect, view.zNear, view.zFar);
    const VrHandMatrix4 camera = VrHandMath::BuildSourceView(view.origin, view.angles);
    const float sourceUnitsPerMeter = vrScale;
    const float clampedModelScale = std::clamp(modelScale, 0.25f, 4.0f);

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

    bool drewAny = false;
    const Vector positionOffsets[2] = { leftHandPoseOffsetMeters, rightHandPoseOffsetMeters };
    const Vector rotationOffsets[2] = { leftHandPoseRotationOffsetDeg, rightHandPoseRotationOffsetDeg };

    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const HandState& hand = m_Hands[static_cast<size_t>(handIndex)];
        if (!hand.asset.IsValid() || hand.asset.jointNames.empty())
            continue;

        std::vector<VrHandMatrixRows3x4> bindPalette(hand.asset.jointNames.size(), VrHandMath::ToRows3x4(VrHandMath::Identity()));
        if (handIndex == 1 && rightUseViewmodelPose)
        {
            VrHandVmPose::Snapshot snapshot{};
            if (!VrHandVmPose::GetLatest(snapshot, kVrHandsVmPoseMaxAgeMs) ||
                !BuildRightViewmodelPalette(snapshot, bindPalette))
            {
                continue;
            }
        }

        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(
            positions[handIndex],
            viewAngles,
            sourceUnitsPerMeter,
            clampedModelScale,
            positionOffsets[handIndex],
            rotationOffsets[handIndex]);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, bindPalette, world, wvp, drawPass, sceneLightScale, error))
            ReportErrorOnce(error);
        else
            drewAny = true;
    }

    if (debugLog && !m_DebugTestPoseLogged)
    {
        m_DebugTestPoseLogged = true;
        Game::logMsg("[VR][Hands] drawing mouse-mode controllerless test pose");
    }
    return drewAny;
}

bool VrHandSystem::ClearViewmodelOcclusionStencil(IDirect3DDevice9* device)
{
    std::string error;
    if (m_Renderer.ClearViewmodelOcclusionStencil(device, error))
        return true;
    ReportErrorOnce(error);
    return false;
}

void VrHandSystem::OnDeviceLost()
{
    m_Renderer.OnDeviceLost();
}
