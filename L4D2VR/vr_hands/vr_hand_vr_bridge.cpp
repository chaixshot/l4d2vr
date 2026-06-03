#include "vr.h"

#include "game.h"
#include "sdk.h"
#include "vr_hand_system.h"
#include "vr_hand_math.h"

#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>
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

    // The detachable magazine is rendered by repeating the native Source
    // viewmodel draw with every non-clip bone moved out of view. This reuses
    // the weapon model's active materials, shader path and lighting exactly.
    // Do not also draw the old standalone D3D9 GLB visual.
    const VrHandMatrix4* manualReloadMagazineWorldPtr = nullptr;
    const bool manualReloadMagazineUseViewmodelLayer = false;

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
        m_ManualReloadMagazineGlbPath,
        manualReloadMagazineWorldPtr,
        manualReloadMagazineUseViewmodelLayer,
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

namespace
{
    constexpr int kManualReloadViewmodelCycleOffset = 0x65C;
    constexpr int kManualReloadViewmodelPlaybackRateOffset = 0x660;
    constexpr int kManualReloadViewmodelSequenceOffset = 0x8A4;

    float ManualReloadVectorLength(const Vector& value)
    {
        return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    }

    Vector ManualReloadNormalize(const Vector& value)
    {
        const float length = ManualReloadVectorLength(value);
        if (!(length > 0.000001f))
            return Vector(0.0f, 0.0f, 1.0f);
        return value * (1.0f / length);
    }

    Vector ManualReloadMatrixTranslation(const VrHandMatrix4& matrix)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
    }

    Vector ManualReloadTransformDirection(const VrHandMatrix4& matrix, const Vector& value)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 0) * value.x + VrHandMath::Get(matrix, 0, 1) * value.y + VrHandMath::Get(matrix, 0, 2) * value.z,
            VrHandMath::Get(matrix, 1, 0) * value.x + VrHandMath::Get(matrix, 1, 1) * value.y + VrHandMath::Get(matrix, 1, 2) * value.z,
            VrHandMath::Get(matrix, 2, 0) * value.x + VrHandMath::Get(matrix, 2, 1) * value.y + VrHandMath::Get(matrix, 2, 2) * value.z);
    }

    VrHandMatrix4 ManualReloadStripScale(const VrHandMatrix4& matrix)
    {
        VrHandMatrix4 out = matrix;
        for (int column = 0; column < 3; ++column)
        {
            Vector axis(
                VrHandMath::Get(out, 0, column),
                VrHandMath::Get(out, 1, column),
                VrHandMath::Get(out, 2, column));
            axis = ManualReloadNormalize(axis);
            VrHandMath::Set(out, 0, column, axis.x);
            VrHandMath::Set(out, 1, column, axis.y);
            VrHandMath::Set(out, 2, column, axis.z);
        }
        VrHandMath::Set(out, 3, 0, 0.0f);
        VrHandMath::Set(out, 3, 1, 0.0f);
        VrHandMath::Set(out, 3, 2, 0.0f);
        VrHandMath::Set(out, 3, 3, 1.0f);
        return out;
    }

    VrHandMatrix4 ManualReloadBuildLocalTransform(
        float sourceUnitsPerMeter,
        float modelScale,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg,
        bool includeModelScale)
    {
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rx = localRotationOffsetDeg.x * kDegToRad;
        const float ry = localRotationOffsetDeg.y * kDegToRad;
        const float rz = localRotationOffsetDeg.z * kDegToRad;
        const float sx = std::sin(rx), cx = std::cos(rx);
        const float sy = std::sin(ry), cy = std::cos(ry);
        const float sz = std::sin(rz), cz = std::cos(rz);

        VrHandMatrix4 local = VrHandMath::Identity();
        VrHandMath::Set(local, 0, 0, cz * cy);
        VrHandMath::Set(local, 0, 1, cz * sy * sx - sz * cx);
        VrHandMath::Set(local, 0, 2, cz * sy * cx + sz * sx);
        VrHandMath::Set(local, 1, 0, sz * cy);
        VrHandMath::Set(local, 1, 1, sz * sy * sx + cz * cx);
        VrHandMath::Set(local, 1, 2, sz * sy * cx - cz * sx);
        VrHandMath::Set(local, 2, 0, -sy);
        VrHandMath::Set(local, 2, 1, cy * sx);
        VrHandMath::Set(local, 2, 2, cy * cx);
        VrHandMath::Set(local, 0, 3, localPositionOffsetMeters.x * sourceUnitsPerMeter);
        VrHandMath::Set(local, 1, 3, localPositionOffsetMeters.y * sourceUnitsPerMeter);
        VrHandMath::Set(local, 2, 3, localPositionOffsetMeters.z * sourceUnitsPerMeter);

        if (!includeModelScale)
            return local;

        VrHandMatrix4 scale = VrHandMath::Identity();
        const float sourceScale = sourceUnitsPerMeter * modelScale;
        VrHandMath::Set(scale, 0, 0, sourceScale);
        VrHandMath::Set(scale, 1, 1, sourceScale);
        VrHandMath::Set(scale, 2, 2, sourceScale);
        return VrHandMath::Multiply(local, scale);
    }

    bool ManualReloadReadViewmodelAnimation(void* entity, int& sequence, float& cycle, float& playbackRate)
    {
        if (!entity)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            const auto* base = reinterpret_cast<const unsigned char*>(entity);
            sequence = *reinterpret_cast<const int*>(base + kManualReloadViewmodelSequenceOffset);
            cycle = *reinterpret_cast<const float*>(base + kManualReloadViewmodelCycleOffset);
            playbackRate = *reinterpret_cast<const float*>(base + kManualReloadViewmodelPlaybackRateOffset);
            return std::isfinite(cycle) && std::isfinite(playbackRate);
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

    bool ManualReloadWriteViewmodelAnimation(void* entity, int sequence, float cycle, float playbackRate)
    {
        if (!entity)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            auto* base = reinterpret_cast<unsigned char*>(entity);
            *reinterpret_cast<int*>(base + kManualReloadViewmodelSequenceOffset) = sequence;
            *reinterpret_cast<float*>(base + kManualReloadViewmodelCycleOffset) = cycle;
            *reinterpret_cast<float*>(base + kManualReloadViewmodelPlaybackRateOffset) = playbackRate;
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#endif
    }

}

bool VR::IsManualReloadActive() const
{
    return m_ManualReloadState != ManualReloadState::Idle;
}

bool VR::IsManualReloadBlockingFire() const
{
    return m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab ||
        m_ManualReloadState == ManualReloadState::HoldingFreshMagazine ||
        m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithGlbMagazine;
}

bool VR::ShouldHideManualReloadNativeClip() const
{
    return m_ManualReloadHideNativeClip;
}

void VR::BeginManualReload(C_BasePlayer* localPlayer)
{
    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
        return;

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || weapon->GetWeaponID() != C_WeaponCSBase::WeaponID::M16A1)
        return;

    CancelManualReload();
    m_ManualReloadWeapon = weapon;
    m_ManualReloadState = ManualReloadState::WatchingNativeClipRemoval;
    m_ManualReloadStarted = std::chrono::steady_clock::now();
    Game::logMsg("[VR][ManualReload] begin weapon=M16A1 clipBone=v_weapon.M4A1_Clip");
}

void VR::CancelManualReload()
{
    if (m_ManualReloadViewmodelFrozen && m_ManualReloadViewmodelEntity)
    {
        const float playbackRate = (m_ManualReloadOriginalPlaybackRate > 0.0001f) ? m_ManualReloadOriginalPlaybackRate : 1.0f;
        ManualReloadWriteViewmodelAnimation(
            m_ManualReloadViewmodelEntity,
            m_ManualReloadFrozenSequence,
            m_ManualReloadFrozenCycle,
            playbackRate);
    }

    if (m_ManualReloadMouseTestReloadPulseOwned && m_Game)
    {
        m_Game->ClientCmd_Unrestricted("-reload");
        m_ManualReloadMouseTestReloadPulseOwned = false;
    }

    m_ManualReloadState = ManualReloadState::Idle;
    m_ManualReloadWeapon = nullptr;
    m_ManualReloadViewmodelEntity = nullptr;
    m_ManualReloadSocketValid = false;
    m_ManualReloadHideNativeClip = false;
    m_ManualReloadMagazineInsertionArmed = false;
    m_ManualReloadFrozenSequence = -1;
    m_ManualReloadFrozenCycle = 0.0f;
    m_ManualReloadOriginalPlaybackRate = 1.0f;
    m_ManualReloadViewmodelFrozen = false;
    m_ManualReloadStarted = {};
    m_ManualReloadResumeStarted = {};
    m_ManualReloadMouseTestMagazineLocalOffsetMeters = { 0.0f, 0.0f, 0.0f };
    m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
    m_ManualReloadMouseTestLastUpdate = {};
}

bool VR::GetManualReloadMagazineWorld(VrHandMatrix4& outWorld) const
{
    if (!m_ManualReloadEnabled || !m_ManualReloadSocketValid)
        return false;

    if (m_ManualReloadState == ManualReloadState::HoldingFreshMagazine)
    {
        if (m_ManualReloadMouseTestMode && m_MouseModeEnabled)
        {
            const VrHandMatrix4 socketCorrection = ManualReloadBuildLocalTransform(
                m_VRScale,
                1.0f,
                m_ManualReloadMagazineSocketOffsetMeters,
                m_ManualReloadMagazineSocketRotationOffsetDeg,
                false);
            const VrHandMatrix4 local = ManualReloadBuildLocalTransform(
                m_VRScale,
                std::clamp(m_ManualReloadMagazineModelScale, 0.01f, 20.0f),
                m_ManualReloadMouseTestMagazineLocalOffsetMeters,
                m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg,
                true);
            outWorld = VrHandMath::Multiply(
                VrHandMath::Multiply(m_ManualReloadSocketWorld, socketCorrection),
                local);
            return true;
        }

        outWorld = VrHandMath::BuildControllerWorld(
            m_LeftControllerPosAbs,
            m_LeftControllerAngAbs,
            m_VRScale,
            std::clamp(m_ManualReloadMagazineModelScale, 0.01f, 20.0f),
            m_ManualReloadMagazineHandOffsetMeters,
            m_ManualReloadMagazineHandRotationOffsetDeg);
        return true;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithGlbMagazine)
    {
        const VrHandMatrix4 local = ManualReloadBuildLocalTransform(
            m_VRScale,
            std::clamp(m_ManualReloadMagazineModelScale, 0.01f, 20.0f),
            m_ManualReloadMagazineSocketOffsetMeters,
            m_ManualReloadMagazineSocketRotationOffsetDeg,
            true);
        outWorld = VrHandMath::Multiply(m_ManualReloadSocketWorld, local);
        return true;
    }

    return false;
}

void VR::UpdateManualReloadMouseTestKeyboard(C_BasePlayer* localPlayer)
{
    auto keyDown = [](int virtualKey) -> bool
        {
            return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        };

    const bool reloadKeyDown = keyDown(VK_F6);
    const bool grabKeyDown = keyDown(VK_F7);
    const bool dropKeyDown = keyDown(VK_DELETE);
    const bool resetKeyDown = keyDown(VK_HOME);
    const bool cancelKeyDown = keyDown(VK_F9);

    const bool reloadJustPressed = reloadKeyDown && !m_ManualReloadMouseTestReloadKeyDownPrev;
    const bool grabJustPressed = grabKeyDown && !m_ManualReloadMouseTestGrabKeyDownPrev;
    const bool dropJustPressed = dropKeyDown && !m_ManualReloadMouseTestDropKeyDownPrev;
    const bool resetJustPressed = resetKeyDown && !m_ManualReloadMouseTestResetKeyDownPrev;
    const bool cancelJustPressed = cancelKeyDown && !m_ManualReloadMouseTestCancelKeyDownPrev;

    m_ManualReloadMouseTestReloadKeyDownPrev = reloadKeyDown;
    m_ManualReloadMouseTestGrabKeyDownPrev = grabKeyDown;
    m_ManualReloadMouseTestDropKeyDownPrev = dropKeyDown;
    m_ManualReloadMouseTestResetKeyDownPrev = resetKeyDown;
    m_ManualReloadMouseTestCancelKeyDownPrev = cancelKeyDown;

    // Keep the synthetic reload pulse alive for one ProcessInput frame so Source sees
    // a normal button press rather than +reload and -reload in the same command batch.
    if (m_ManualReloadMouseTestReloadPulseOwned && m_Game)
    {
        m_Game->ClientCmd_Unrestricted("-reload");
        m_ManualReloadMouseTestReloadPulseOwned = false;
    }

    if (!m_ManualReloadEnabled || !m_ManualReloadMouseTestMode || !m_MouseModeEnabled)
    {
        m_ManualReloadMouseTestLastUpdate = {};
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = 0.0f;
    if (m_ManualReloadMouseTestLastUpdate.time_since_epoch().count() != 0)
    {
        deltaSeconds = std::chrono::duration<float>(now - m_ManualReloadMouseTestLastUpdate).count();
        deltaSeconds = std::clamp(deltaSeconds, 0.0f, 0.10f);
    }
    m_ManualReloadMouseTestLastUpdate = now;

    if (cancelJustPressed)
    {
        if (IsManualReloadActive())
            Game::logMsg("[VR][ManualReload][MouseTest] canceled by F9");
        CancelManualReload();
        return;
    }

    if (!IsManualReloadActive())
    {
        if (!reloadJustPressed || !localPlayer || !m_Game)
            return;

        BeginManualReload(localPlayer);
        if (!IsManualReloadActive())
        {
            Game::logMsg("[VR][ManualReload][MouseTest] F6 ignored: equip M16A1 and keep single-threaded rendering enabled");
            return;
        }

        m_Game->ClientCmd_Unrestricted("+reload");
        m_ManualReloadMouseTestReloadPulseOwned = true;
        Game::logMsg("[VR][ManualReload][MouseTest] F6 started native reload; after pause press F7, then hold PageDown to push the GLB magazine into the socket");
        Game::logMsg("[VR][ManualReload][MouseTest] controls: F7=grab  Home=reset alignment  arrows=lateral move  PageUp/PageDown=pull/push  numpad 8/2 4/6 7/9=rotate  Delete=drop  F9=cancel");
        return;
    }

    const Vector insertionAxis = ManualReloadNormalize(m_ManualReloadMagazineInsertionAxisLocal);
    auto resetMagazineToGuideStart = [&]()
        {
            m_ManualReloadMouseTestMagazineLocalOffsetMeters = insertionAxis * m_ManualReloadMagazineGuideStartDepthMeters;
            m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg = { 0.0f, 0.0f, 0.0f };
            m_ManualReloadMagazineInsertionArmed = false;
        };

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab)
    {
        if (grabJustPressed)
        {
            resetMagazineToGuideStart();
            m_ManualReloadState = ManualReloadState::HoldingFreshMagazine;
            VrHandMatrix4 previewWorld{};
            if (GetManualReloadMagazineWorld(previewWorld))
            {
                const Vector socketWorld = ManualReloadMatrixTranslation(m_ManualReloadSocketWorld);
                const Vector guideWorld = ManualReloadMatrixTranslation(previewWorld);
                Game::logMsg(
                    "[VR][ManualReload][MouseTest] F7 spawned aligned GLB magazine socket=(%.2f %.2f %.2f) guide=(%.2f %.2f %.2f); hold PageDown to insert",
                    socketWorld.x, socketWorld.y, socketWorld.z,
                    guideWorld.x, guideWorld.y, guideWorld.z);
            }
            else
            {
                Game::logMsg("[VR][ManualReload][MouseTest] F7 spawned aligned GLB magazine at guide start; hold PageDown to insert");
            }
        }
        return;
    }

    if (m_ManualReloadState != ManualReloadState::HoldingFreshMagazine)
        return;

    if (dropJustPressed)
    {
        m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
        m_ManualReloadMagazineInsertionArmed = false;
        Game::logMsg("[VR][ManualReload][MouseTest] GLB magazine dropped; press F7 to spawn another one");
        return;
    }

    if (resetJustPressed)
    {
        resetMagazineToGuideStart();
        Game::logMsg("[VR][ManualReload][MouseTest] GLB magazine reset to aligned guide start");
    }

    const float moveStep = 0.12f * deltaSeconds;
    if (keyDown(VK_LEFT))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.x -= moveStep;
    if (keyDown(VK_RIGHT))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.x += moveStep;
    if (keyDown(VK_UP))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.y += moveStep;
    if (keyDown(VK_DOWN))
        m_ManualReloadMouseTestMagazineLocalOffsetMeters.y -= moveStep;
    if (keyDown(VK_PRIOR)) // PageUp: pull away from the socket.
        m_ManualReloadMouseTestMagazineLocalOffsetMeters += insertionAxis * moveStep;
    if (keyDown(VK_NEXT)) // PageDown: push into the socket.
        m_ManualReloadMouseTestMagazineLocalOffsetMeters -= insertionAxis * moveStep;

    const float rotationStep = 90.0f * deltaSeconds;
    if (keyDown(VK_NUMPAD8))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.x += rotationStep;
    if (keyDown(VK_NUMPAD2))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.x -= rotationStep;
    if (keyDown(VK_NUMPAD6))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.y += rotationStep;
    if (keyDown(VK_NUMPAD4))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.y -= rotationStep;
    if (keyDown(VK_NUMPAD9))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.z += rotationStep;
    if (keyDown(VK_NUMPAD7))
        m_ManualReloadMouseTestMagazineLocalRotationOffsetDeg.z -= rotationStep;
}

void VR::UpdateManualReload(C_BasePlayer* localPlayer, bool leftGripDown, bool leftGripJustPressed)
{
    if (!IsManualReloadActive())
        return;

    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
    {
        CancelManualReload();
        return;
    }

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || weapon != m_ManualReloadWeapon || weapon->GetWeaponID() != C_WeaponCSBase::WeaponID::M16A1)
    {
        CancelManualReload();
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval &&
        m_ManualReloadStarted.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_ManualReloadStarted).count() >= 3.0f)
    {
        Game::logMsg("[VR][ManualReload] native clip did not leave weapon; manual reload canceled");
        CancelManualReload();
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab)
    {
        if (m_ManualReloadMouseTestMode && m_MouseModeEnabled)
            return;

        Vector bodyForward = m_HmdForward;
        bodyForward.z = 0.0f;
        bodyForward = ManualReloadNormalize(bodyForward);
        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyRight(
            bodyForward.y * worldUp.z - bodyForward.z * worldUp.y,
            bodyForward.z * worldUp.x - bodyForward.x * worldUp.z,
            bodyForward.x * worldUp.y - bodyForward.y * worldUp.x);
        bodyRight = ManualReloadNormalize(bodyRight);
        const Vector bodyOrigin = m_CameraAnchor
            + bodyForward * (m_InventoryBodyOriginOffset.x * m_VRScale)
            + bodyRight * (m_InventoryBodyOriginOffset.y * m_VRScale)
            + worldUp * (m_InventoryBodyOriginOffset.z * m_VRScale);
        const Vector pouch = bodyOrigin
            + bodyForward * (m_InventoryLeftWaistOffset.x * m_VRScale)
            + bodyRight * (m_InventoryLeftWaistOffset.y * m_VRScale)
            + worldUp * (m_InventoryLeftWaistOffset.z * m_VRScale);

        if (leftGripJustPressed && ManualReloadVectorLength(m_LeftControllerPosAbs - pouch) <= m_ManualReloadMagazineGrabRangeMeters * m_VRScale)
        {
            m_ManualReloadState = ManualReloadState::HoldingFreshMagazine;
            m_ManualReloadMagazineInsertionArmed = false;
            Game::logMsg("[VR][ManualReload] fresh GLB magazine grabbed from left waist");
        }
        return;
    }

    if (m_ManualReloadState != ManualReloadState::HoldingFreshMagazine)
        return;

    if (!leftGripDown)
    {
        m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
        m_ManualReloadMagazineInsertionArmed = false;
        return;
    }

    VrHandMatrix4 heldWorld{};
    if (!GetManualReloadMagazineWorld(heldWorld))
        return;

    const VrHandMatrix4 socketCorrection = ManualReloadBuildLocalTransform(
        m_VRScale,
        1.0f,
        m_ManualReloadMagazineSocketOffsetMeters,
        m_ManualReloadMagazineSocketRotationOffsetDeg,
        false);
    const VrHandMatrix4 socketRigid = VrHandMath::Multiply(m_ManualReloadSocketWorld, socketCorrection);
    VrHandMatrix4 inverseSocket{};
    if (!VrHandMath::Invert4x4(socketRigid, inverseSocket))
        return;

    const VrHandMatrix4 heldRigid = ManualReloadStripScale(heldWorld);
    const VrHandMatrix4 magazineInSocket = VrHandMath::Multiply(inverseSocket, heldRigid);
    const Vector localPositionMeters = ManualReloadMatrixTranslation(magazineInSocket) * (1.0f / std::max(0.001f, m_VRScale));
    const Vector insertionAxis = ManualReloadNormalize(m_ManualReloadMagazineInsertionAxisLocal);
    const float axial = VrHandMath::Dot(localPositionMeters, insertionAxis);
    const Vector lateralVector = localPositionMeters - insertionAxis * axial;
    const float lateral = ManualReloadVectorLength(lateralVector);

    const Vector heldAxis = ManualReloadNormalize(ManualReloadTransformDirection(magazineInSocket, insertionAxis));
    const float dot = std::clamp(VrHandMath::Dot(heldAxis, insertionAxis), -1.0f, 1.0f);
    const float angleDeg = std::acos(dot) * (180.0f / 3.14159265358979323846f);
    const bool aligned = lateral <= m_ManualReloadMagazineCaptureRadiusMeters &&
        angleDeg <= m_ManualReloadMagazineCaptureAngleDeg;

    if (aligned && axial >= m_ManualReloadMagazineFullInsertDepthMeters && axial <= m_ManualReloadMagazineGuideStartDepthMeters)
        m_ManualReloadMagazineInsertionArmed = true;

    if (m_ManualReloadMagazineInsertionArmed && aligned &&
        axial <= m_ManualReloadMagazineFullInsertDepthMeters && axial >= -0.02f)
    {
        m_ManualReloadState = ManualReloadState::ResumingNativeReloadWithGlbMagazine;
        m_ManualReloadResumeStarted = std::chrono::steady_clock::now();
        m_ManualReloadMagazineInsertionArmed = false;
        if (m_ManualReloadViewmodelFrozen && m_ManualReloadViewmodelEntity)
        {
            const float playbackRate = (m_ManualReloadOriginalPlaybackRate > 0.0001f) ? m_ManualReloadOriginalPlaybackRate : 1.0f;
            ManualReloadWriteViewmodelAnimation(
                m_ManualReloadViewmodelEntity,
                m_ManualReloadFrozenSequence,
                m_ManualReloadFrozenCycle,
                playbackRate);
            m_ManualReloadViewmodelFrozen = false;
        }
        Game::logMsg("[VR][ManualReload] GLB magazine inserted; native reload resumed");
    }
}

void VR::OnManualReloadViewmodelPose(
    const char* modelName,
    void* viewmodelEntity,
    const VrHandMatrix4& modelAnchorWorld,
    const VrHandMatrix4& nativeClipWorld)
{
    (void)modelName;
    if (!IsManualReloadActive() || !viewmodelEntity)
        return;

    m_ManualReloadViewmodelEntity = viewmodelEntity;
    VrHandMatrix4 inverseModelAnchor{};
    if (!VrHandMath::Invert4x4(modelAnchorWorld, inverseModelAnchor))
        return;
    const VrHandMatrix4 currentClipLocal = VrHandMath::Multiply(inverseModelAnchor, nativeClipWorld);

    if (!m_ManualReloadSocketValid)
    {
        m_ManualReloadSocketLocal = currentClipLocal;
        m_ManualReloadSocketWorld = VrHandMath::Multiply(modelAnchorWorld, m_ManualReloadSocketLocal);
        m_ManualReloadSocketValid = true;
    }
    else
    {
        m_ManualReloadSocketWorld = VrHandMath::Multiply(modelAnchorWorld, m_ManualReloadSocketLocal);
    }

    int sequence = -1;
    float cycle = 0.0f;
    float playbackRate = 1.0f;
    if (!ManualReloadReadViewmodelAnimation(viewmodelEntity, sequence, cycle, playbackRate))
        return;

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval)
    {
        const Vector current = ManualReloadMatrixTranslation(currentClipLocal);
        const Vector socket = ManualReloadMatrixTranslation(m_ManualReloadSocketLocal);
        const float movedMeters = ManualReloadVectorLength(current - socket) / std::max(0.001f, m_VRScale);
        if (movedMeters >= m_ManualReloadNativeClipLeaveDistanceMeters)
        {
            m_ManualReloadFrozenSequence = sequence;
            m_ManualReloadFrozenCycle = cycle;
            m_ManualReloadOriginalPlaybackRate = playbackRate;
            m_ManualReloadViewmodelFrozen = ManualReloadWriteViewmodelAnimation(viewmodelEntity, sequence, cycle, 0.0f);
            m_ManualReloadHideNativeClip = true;
            m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
            Game::logMsg("[VR][ManualReload] native clip left weapon %.3fm; animation paused", movedMeters);
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab ||
        m_ManualReloadState == ManualReloadState::HoldingFreshMagazine)
    {
        if (m_ManualReloadFrozenSequence >= 0)
        {
            m_ManualReloadViewmodelFrozen = ManualReloadWriteViewmodelAnimation(
                viewmodelEntity,
                m_ManualReloadFrozenSequence,
                m_ManualReloadFrozenCycle,
                0.0f);
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithGlbMagazine)
    {
        const auto now = std::chrono::steady_clock::now();
        const float resumedSeconds = std::chrono::duration<float>(now - m_ManualReloadResumeStarted).count();
        if (resumedSeconds >= 0.05f &&
            (sequence != m_ManualReloadFrozenSequence || cycle >= 0.999f))
        {
            m_ManualReloadHideNativeClip = false;
            m_ManualReloadState = ManualReloadState::Idle;
            m_ManualReloadWeapon = nullptr;
            m_ManualReloadViewmodelEntity = nullptr;
            m_ManualReloadSocketValid = false;
            m_ManualReloadMagazineInsertionArmed = false;
            m_ManualReloadFrozenSequence = -1;
            m_ManualReloadFrozenCycle = 0.0f;
            m_ManualReloadOriginalPlaybackRate = 1.0f;
            m_ManualReloadViewmodelFrozen = false;
            m_ManualReloadStarted = {};
            m_ManualReloadResumeStarted = {};
            Game::logMsg("[VR][ManualReload] native reload animation finished; GLB hidden and native clip restored");
        }
    }
}
