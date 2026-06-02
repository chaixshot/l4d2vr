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
namespace VrHandVmPose { struct Snapshot; }

class VrHandSystem
{
public:
    VrHandSystem();
    ~VrHandSystem();

    VrHandSystem(const VrHandSystem&) = delete;
    VrHandSystem& operator=(const VrHandSystem&) = delete;

    bool DrawForEye(
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
        VrHandDrawPass drawPass);

    bool ClearViewmodelOcclusionStencil(IDirect3DDevice9* device);
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
        bool skeletonInitialized = false;
    };

    bool EnsureAssetsLoaded(bool debugLog);
    bool EnsureInitialized(vr::IVRInput* input, bool rightUseViewmodelPose, bool debugLog);
    bool ResolveSteamVrAssetPath(const char* fileName, std::string& outPath) const;
    void UpdatePoses(vr::IVRInput* input, bool motionRangeWithoutController, bool rightUseViewmodelPose, bool debugLog);
    bool DrawControllerlessTestPose(
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
        VrHandDrawPass drawPass);
    bool BuildRightViewmodelPalette(const VrHandVmPose::Snapshot& snapshot, std::vector<VrHandMatrixRows3x4>& outPalette);
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
    bool m_DebugRightViewmodelPoseLogged = false;
    bool m_DebugRightViewmodelPoseMissingLogged = false;
    bool m_RightViewmodelPoseWasEnabled = false;
    bool m_RightViewmodelPalmWorldValid = false;
    bool m_RightViewmodelAnchorValid = false;
    std::string m_RightViewmodelPoseModel;
    std::string m_RightViewmodelAnchorModel;
    VrHandMatrix4 m_RightViewmodelPalmWorld{};
    VrHandMatrix4 m_RightViewmodelPalmToGloveWorld{};
    float m_RightViewmodelAnchorVrScale = 0.0f;
    float m_RightViewmodelAnchorModelScale = 0.0f;
    Vector m_RightViewmodelAnchorPoseOffsetMeters = { 0.0f, 0.0f, 0.0f };
    Vector m_RightViewmodelAnchorPoseRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
};
