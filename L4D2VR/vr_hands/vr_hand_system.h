#pragma once

#include "openvr.h"
#include "vr_hand_asset_loader.h"
#include "vr_hand_renderer_d3d9.h"
#include "vr_hand_skeleton_runtime.h"
#include "vector.h"

#include <array>
#include <cstdint>
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
        const Vector& currentViewmodelPosition,
        const QAngle& currentViewmodelAngles,
        const Vector& leftHandPoseOffsetMeters,
        const Vector& leftHandPoseRotationOffsetDeg,
        const Vector& rightHandPoseOffsetMeters,
        const Vector& rightHandPoseRotationOffsetDeg,
        const std::string& manualReloadMagazineGlbPath,
        const VrHandMatrix4* manualReloadMagazineWorld,
        bool manualReloadMagazineUseViewmodelLayer,
        const VrHandMatrix4* standaloneMagazineBoxWorld,
        const Vector& standaloneMagazineBoxMins,
        const Vector& standaloneMagazineBoxMaxs,
        bool standaloneMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* magazineSocketCaptureBoxWorld,
        const Vector& magazineSocketCaptureBoxMins,
        const Vector& magazineSocketCaptureBoxMaxs,
        bool magazineSocketCaptureBoxUseViewmodelLayer,
        const VrHandMatrix4* currentMagazineBoxWorld,
        const Vector& currentMagazineBoxMins,
        const Vector& currentMagazineBoxMaxs,
        bool currentMagazineBoxUseViewmodelLayer,
        const VrHandMatrix4* currentBoltBoxWorld,
        const Vector& currentBoltBoxMins,
        const Vector& currentBoltBoxMaxs,
        bool currentBoltBoxUseViewmodelLayer,
        bool leftHandMagazineGripPose,
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
    bool ResolveGameAssetPath(const std::string& relativePath, std::string& outPath) const;
    bool EnsureManualReloadMagazineLoaded(const std::string& relativePath, bool debugLog);
    bool DrawManualReloadMagazine(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float sceneLightScale,
        const std::string& relativePath,
        const VrHandMatrix4* world,
        bool useViewmodelLayer,
        VrHandDrawPass drawPass);
    bool EnsureStandaloneMagazineBoxLoaded(
        const Vector& mins,
        const Vector& maxs,
        std::uint32_t fallbackColorArgb,
        const char* debugName,
        bool debugLog);
    bool DrawStandaloneMagazineBox(
        IDirect3DDevice9* device,
        const CViewSetup& view,
        float sceneLightScale,
        const VrHandMatrix4* world,
        const Vector& mins,
        const Vector& maxs,
        std::uint32_t fallbackColorArgb,
        const char* debugName,
        bool useViewmodelLayer,
        VrHandDrawPass drawPass);
    void UpdatePoses(
        vr::IVRInput* input,
        bool motionRangeWithoutController,
        bool rightUseViewmodelPose,
        bool leftHandMagazineGripPose,
        bool debugLog);
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
    bool BuildRightViewmodelWorld(
        float sourceUnitsPerMeter,
        float modelScale,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg,
        bool anchorToCurrentViewmodelRoot,
        const Vector& currentViewmodelPosition,
        const QAngle& currentViewmodelAngles,
        VrHandMatrix4& outWorld) const;
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
    bool m_LeftHandMagazineGripPoseWasEnabled = false;
    bool m_RightViewmodelPalmWorldValid = false;
    bool m_RightViewmodelAnchorValid = false;
    VrHandMeshAsset m_ManualReloadMagazineAsset;
    std::vector<VrHandMatrixRows3x4> m_ManualReloadMagazinePalette;
    std::string m_ManualReloadMagazineLoadedRelativePath;
    bool m_ManualReloadMagazineDrawLogged = false;
    VrHandMeshAsset m_StandaloneMagazineBoxAsset;
    std::vector<VrHandMatrixRows3x4> m_StandaloneMagazineBoxPalette;
    std::string m_StandaloneMagazineBoxKey;
    std::unordered_set<std::string> m_StandaloneMagazineBoxDrawLoggedKeys;
    std::string m_RightViewmodelPoseModel;
    std::string m_RightViewmodelAnchorModel;
    VrHandMatrix4 m_RightViewmodelPalmWorld{};
    VrHandMatrix4 m_RightViewmodelPalmFromModelRoot{};
    bool m_RightViewmodelPalmFromModelRootValid = false;
    VrHandMatrix4 m_RightViewmodelPalmFromGloveWrist{};
    VrHandMatrix4 m_RightViewmodelGloveWristBindInverse{};
};
