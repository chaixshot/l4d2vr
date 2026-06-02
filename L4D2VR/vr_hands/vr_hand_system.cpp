#include "vr_hand_system.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_math.h"

#include <Windows.h>
#include <d3d9.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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

    for (int handIndex = 0; handIndex < static_cast<int>(m_Hands.size()); ++handIndex)
    {
        const HandState& hand = m_Hands[static_cast<size_t>(handIndex)];
        if (!hand.asset.IsValid() || hand.asset.jointNames.empty())
            continue;

        std::vector<VrHandMatrixRows3x4> bindPalette(hand.asset.jointNames.size(), VrHandMath::ToRows3x4(VrHandMath::Identity()));
        const VrHandMatrix4 world = VrHandMath::BuildControllerWorld(positions[handIndex], viewAngles, sourceUnitsPerMeter);
        const VrHandMatrix4 wvp = VrHandMath::Multiply(projection, VrHandMath::Multiply(camera, world));
        std::string error;
        if (!m_Renderer.Draw(device, handIndex, hand.asset, bindPalette, world, wvp, sceneLightScale, error))
            ReportErrorOnce(error);
    }

    if (debugLog && !m_DebugTestPoseLogged)
    {
        m_DebugTestPoseLogged = true;
        Game::logMsg("[VR][Hands] drawing mouse-mode controllerless test pose");
    }
}

void VrHandSystem::OnDeviceLost()
{
    m_Renderer.OnDeviceLost();
}
