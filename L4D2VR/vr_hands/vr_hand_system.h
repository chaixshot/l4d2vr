#pragma once

#include "openvr.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_renderer_d3d9.h"
#include "vr_hand_skeleton_runtime.h"
#include "vector.h"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

class CViewSetup;
struct IDirect3DDevice9;

class VrHandSystem
{
public:
    VrHandSystem();
    ~VrHandSystem();

    VrHandSystem(const VrHandSystem&) = delete;
    VrHandSystem& operator=(const VrHandSystem&) = delete;

    void DrawForEye(
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
        const QAngle& rightControllerAngles);

    void OnDeviceLost();

private:
    struct HandState
    {
        const char* actionPath = nullptr;
        const char* assetFileName = nullptr;
        vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
        VrHandMeshAsset asset;
        VrHandSkeletonRuntime skeleton;
        std::vector<VrHandMatrixRows3x4> palette;
        bool paletteValid = false;
    };

    bool EnsureAssetsLoaded(bool debugLog);
    bool EnsureInitialized(vr::IVRInput* input, bool debugLog);
    bool ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const;
    void UpdatePoses(vr::IVRInput* input, bool motionRangeWithoutController, bool debugLog);
    void DrawControllerlessTestPose(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float vrScale,
        float modelScale,
        float sceneLightScale,
        bool debugLog);
    void ReportErrorOnce(const std::string& error);

    std::array<HandState, 2> m_Hands;
    VrHandRendererD3D9 m_Renderer;
    std::unordered_set<std::string> m_ReportedErrors;
    bool m_AssetLoadAttempted = false;
    bool m_AssetsLoaded = false;
    bool m_InitializationAttempted = false;
    bool m_Initialized = false;
    bool m_DebugInitializationLogged = false;
    bool m_DebugTestPoseLogged = false;
    bool m_DebugVmPoseLogged = false;
};
