#include "vr.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_system.h"

#include <d3d9.h>

#include <algorithm>
#include <memory>

VR::VR() = default;

VR::~VR() = default;

bool VR::DrawVrHandsForEye(const CViewSetup& view, int eyeIndex, VrHandDrawPass drawPass)
{
    // Raw D3D9 commands issued directly from Source RenderView are safe only in
    // single-threaded rendering. The queued path needs a DXVK-side submission point.
    if (!m_VrHandsEnabled || !m_IsVREnabled || !m_Input || !m_Game || m_Game->GetMatQueueMode() != 0)
        return false;

    IDirect3DSurface9* surface = (eyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (!surface)
        return false;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
        return false;

    if (!m_VrHands)
        m_VrHands = std::make_unique<VrHandSystem>();

    float sceneLightScale = 1.0f;
    if (m_AutoFlashlightHasScreenLuma)
    {
        const float localLuma = std::max(
            m_AutoFlashlightCenterMedianLuma,
            m_AutoFlashlightPeripheralMedianLuma * 0.35f);
        sceneLightScale = std::clamp(localLuma / 110.0f, 0.08f, 1.0f);
    }

    const bool drewAny = m_VrHands->DrawForEye(
        device,
        m_Input,
        view,
        eyeIndex,
        m_VRScale,
        m_VrHandsModelScale,
        m_VrHandsMotionRangeWithoutController,
        m_VrHandsRightUseViewmodelPose,
        m_MouseModeEnabled,
        m_VrHandsDebugLog,
        sceneLightScale,
        m_LeftControllerPosAbs,
        m_LeftControllerAngAbs,
        m_RightControllerPosAbs,
        m_RightControllerAngAbs,
        m_VrHandsLeftPoseOffsetMeters,
        m_VrHandsLeftPoseRotationOffsetDeg,
        m_VrHandsRightPoseOffsetMeters,
        m_VrHandsRightPoseRotationOffsetDeg,
        drawPass);
    device->Release();
    return drewAny;
}

void VR::BeginVrHandsEyeRender(const CViewSetup& view, int eyeIndex)
{
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;
    if (!m_VrHandsEnabled || !m_IsVREnabled || !m_Input || !m_Game || m_Game->GetMatQueueMode() != 0)
        return;

    m_VrHandsActiveEyeView = &view;
    m_VrHandsActiveEyeIndex = eyeIndex;
}

void VR::DrawVrHandsWorldDepthMaskBeforeViewmodel()
{
    if (!m_VrHandsActiveEyeView || m_VrHandsActiveEyeIndex < 0 || m_VrHandsWorldMaskDrawn)
        return;

    IDirect3DSurface9* surface = (m_VrHandsActiveEyeIndex == 0) ? m_D9LeftEyeSurface : m_D9RightEyeSurface;
    if (!surface)
        return;

    IDirect3DDevice9* device = nullptr;
    if (FAILED(surface->GetDevice(&device)) || !device)
        return;

    if (!m_VrHands)
        m_VrHands = std::make_unique<VrHandSystem>();

    const bool stencilReady = m_VrHands->ClearViewmodelOcclusionStencil(device);
    device->Release();
    if (!stencilReady)
        return;

    m_VrHandsWorldMaskDrawn = DrawVrHandsForEye(
        *m_VrHandsActiveEyeView,
        m_VrHandsActiveEyeIndex,
        VrHandDrawPass::WorldVisibilityMask);
}

void VR::FinishVrHandsEyeRender()
{
    const CViewSetup* view = m_VrHandsActiveEyeView;
    const int eyeIndex = m_VrHandsActiveEyeIndex;
    const bool worldMaskDrawn = m_VrHandsWorldMaskDrawn;
    m_VrHandsActiveEyeView = nullptr;
    m_VrHandsActiveEyeIndex = -1;
    m_VrHandsWorldMaskDrawn = false;

    if (!view || eyeIndex < 0)
        return;

    DrawVrHandsForEye(
        *view,
        eyeIndex,
        worldMaskDrawn ? VrHandDrawPass::ViewmodelComposite : VrHandDrawPass::WorldDepth);
}

void VR::ReleaseVrHandsD3DResources()
{
    if (m_VrHands)
        m_VrHands->OnDeviceLost();
}
