void VR::ApplyRoomscale1To1Move(CUserCmd* cmd, float inputSampleTime, bool controlLocomotionActive)
{
    const Vector cur = m_HmdPosCorrectedPrev;

    auto resetReference = [&]()
        {
            m_Roomscale1To1PrevCorrectedAbs = cur;
            m_Roomscale1To1PrevValid = true;
        };

    if (!cmd || !m_Roomscale1To1Movement)
    {
        m_Roomscale1To1PrevValid = false;
        return;
    }

    if (m_Roomscale1To1DisableWhileThumbstick && controlLocomotionActive)
    {
        resetReference();
        return;
    }

    if (!m_Roomscale1To1PrevValid)
    {
        resetReference();
        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
        {
            Game::logMsg("[VR][1to1][cmdmove] init cmd=%d tick=%d hmd=(%.3f %.3f %.3f)",
                cmd->command_number, cmd->tick_count, cur.x, cur.y, cur.z);
        }
        return;
    }

    Vector deltaM = cur - m_Roomscale1To1PrevCorrectedAbs;
    deltaM.z = 0.0f;
    m_Roomscale1To1PrevCorrectedAbs = cur;

    const float lenM = std::sqrt((deltaM.x * deltaM.x) + (deltaM.y * deltaM.y));
    const float noiseDeadzoneM = std::clamp(m_Roomscale1To1MinApplyMeters, 0.0f, 0.005f);
    if (lenM <= noiseDeadzoneM)
        return;

    const float maxStepM = std::max(0.0f, m_Roomscale1To1MaxStepMeters);
    if (maxStepM > 0.0f && lenM > maxStepM)
    {
        const float s = maxStepM / lenM;
        deltaM.x *= s;
        deltaM.y *= s;
    }

    float dt = inputSampleTime;
    if (dt <= 0.0001f)
        dt = 1.0f / 30.0f;
    dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 15.0f);

    Vector roomWorldVelocity(deltaM.x * m_VRScale / dt, deltaM.y * m_VRScale / dt, 0.0f);
    const float roomSpeed = std::sqrt((roomWorldVelocity.x * roomWorldVelocity.x) + (roomWorldVelocity.y * roomWorldVelocity.y));
    const float maxCmdSpeed = m_AdjustingViewmodel ? 25.0f : 250.0f;
    if (roomSpeed > maxCmdSpeed && roomSpeed > 0.0001f)
    {
        const float s = maxCmdSpeed / roomSpeed;
        roomWorldVelocity.x *= s;
        roomWorldVelocity.y *= s;
    }

    QAngle cmdYawOnly(0.0f, cmd->viewangles.y, 0.0f);
    Vector cmdForward, cmdRight, cmdUp;
    QAngle::AngleVectors(cmdYawOnly, &cmdForward, &cmdRight, &cmdUp);

    Vector worldMove = (cmdForward * cmd->forwardmove) + (cmdRight * cmd->sidemove);
    worldMove = worldMove + roomWorldVelocity;

    cmd->forwardmove = DotProduct(worldMove, cmdForward);
    cmd->sidemove = DotProduct(worldMove, cmdRight);

    constexpr int kIN_FORWARD = (1 << 3);
    constexpr int kIN_BACK = (1 << 4);
    constexpr int kIN_MOVELEFT = (1 << 9);
    constexpr int kIN_MOVERIGHT = (1 << 10);
    if (cmd->forwardmove > 0.5f)      cmd->buttons |= kIN_FORWARD;
    else if (cmd->forwardmove < -0.5f) cmd->buttons |= kIN_BACK;
    if (cmd->sidemove > 0.5f)         cmd->buttons |= kIN_MOVERIGHT;
    else if (cmd->sidemove < -0.5f)   cmd->buttons |= kIN_MOVELEFT;

    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][cmdmove] cmd=%d tick=%d dt=%.4f dM=(%.3f %.3f) vel=(%.1f %.1f) move=(%.1f %.1f)",
            cmd->command_number, cmd->tick_count, dt,
            deltaM.x, deltaM.y,
            roomWorldVelocity.x, roomWorldVelocity.y,
            cmd->forwardmove, cmd->sidemove);
    }
}

void VR::OnPredictionRunCommand(CUserCmd* cmd)
{
    (void)cmd;
}

void VR::SendVirtualKey(WORD virtualKey)
{
    SendVirtualKeyDown(virtualKey);
    SendVirtualKeyUp(virtualKey);
}

void VR::SendVirtualKeyDown(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendVirtualKeyUp(WORD virtualKey)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));
}

void VR::SendFunctionKey(WORD virtualKey)
{
    SendVirtualKey(virtualKey);
}

VMatrix VR::VMatrixFromHmdMatrix(const vr::HmdMatrix34_t& hmdMat)
{
    // VMatrix has a different implicit coordinate system than HmdMatrix34_t, but this function does not convert between them
    VMatrix vMat(
        hmdMat.m[0][0], hmdMat.m[1][0], hmdMat.m[2][0], 0.0f,
        hmdMat.m[0][1], hmdMat.m[1][1], hmdMat.m[2][1], 0.0f,
        hmdMat.m[0][2], hmdMat.m[1][2], hmdMat.m[2][2], 0.0f,
        hmdMat.m[0][3], hmdMat.m[1][3], hmdMat.m[2][3], 1.0f
    );

    return vMat;
}

vr::HmdMatrix34_t VR::VMatrixToHmdMatrix(const VMatrix& vMat)
{
    vr::HmdMatrix34_t hmdMat = { 0 };

    hmdMat.m[0][0] = vMat.m[0][0];
    hmdMat.m[1][0] = vMat.m[0][1];
    hmdMat.m[2][0] = vMat.m[0][2];

    hmdMat.m[0][1] = vMat.m[1][0];
    hmdMat.m[1][1] = vMat.m[1][1];
    hmdMat.m[2][1] = vMat.m[1][2];

    hmdMat.m[0][2] = vMat.m[2][0];
    hmdMat.m[1][2] = vMat.m[2][1];
    hmdMat.m[2][2] = vMat.m[2][2];

    hmdMat.m[0][3] = vMat.m[3][0];
    hmdMat.m[1][3] = vMat.m[3][1];
    hmdMat.m[2][3] = vMat.m[3][2];

    return hmdMat;
}

vr::HmdMatrix34_t VR::GetControllerTipMatrix(vr::ETrackedControllerRole controllerRole)
{
    vr::VRInputValueHandle_t inputValue = vr::k_ulInvalidInputValueHandle;

    if (controllerRole == vr::TrackedControllerRole_RightHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/right", &inputValue);
    }
    else if (controllerRole == vr::TrackedControllerRole_LeftHand)
    {
        m_Input->GetInputSourceHandle("/user/hand/left", &inputValue);
    }

    if (inputValue != vr::k_ulInvalidInputValueHandle)
    {
        char buffer[vr::k_unMaxPropertyStringSize];

        m_System->GetStringTrackedDeviceProperty(vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(controllerRole), vr::Prop_RenderModelName_String,
            buffer, vr::k_unMaxPropertyStringSize);

        vr::RenderModel_ControllerMode_State_t controllerState = { 0 };
        vr::RenderModel_ComponentState_t componentState = { 0 };

        if (vr::VRRenderModels()->GetComponentStateForDevicePath(buffer, vr::k_pch_Controller_Component_Tip, inputValue, &controllerState, &componentState))
        {
            return componentState.mTrackingToComponentLocal;
        }
    }

    // Not a hand controller role or tip lookup failed, return identity
    const vr::HmdMatrix34_t identity =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f
    };

    return identity;
}

bool VR::CheckOverlayIntersectionForController(vr::VROverlayHandle_t overlayHandle, vr::ETrackedControllerRole controllerRole)
{
    vr::TrackedDeviceIndex_t deviceIndex = m_System->GetTrackedDeviceIndexForControllerRole(controllerRole);

    if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
        return false;

    vr::TrackedDevicePose_t& controllerPose = m_Poses[deviceIndex];

    if (!controllerPose.bPoseIsValid)
        return false;

    VMatrix controllerVMatrix = VMatrixFromHmdMatrix(controllerPose.mDeviceToAbsoluteTracking);
    VMatrix tipVMatrix = VMatrixFromHmdMatrix(GetControllerTipMatrix(controllerRole));
    tipVMatrix.MatrixMul(controllerVMatrix, controllerVMatrix);

    vr::VROverlayIntersectionParams_t  params = { 0 };
    vr::VROverlayIntersectionResults_t results = { 0 };

    params.eOrigin = vr::VRCompositor()->GetTrackingSpace();
    params.vSource = { controllerVMatrix.m[3][0],  controllerVMatrix.m[3][1],  controllerVMatrix.m[3][2] };
    params.vDirection = { -controllerVMatrix.m[2][0], -controllerVMatrix.m[2][1], -controllerVMatrix.m[2][2] };

    return m_Overlay->ComputeOverlayIntersection(overlayHandle, &params, &results);
}


namespace vr_render_snapshot_cache_roomscale
{
    struct HandCache
    {
        uint32_t seq = 0;
        Vector leftPos{ 0.0f, 0.0f, 0.0f };
        QAngle leftAng{ 0.0f, 0.0f, 0.0f };
        Vector rightPos{ 0.0f, 0.0f, 0.0f };
        QAngle rightAng{ 0.0f, 0.0f, 0.0f };
        Vector vmPos{ 0.0f, 0.0f, 0.0f };
        QAngle vmAng{ 0.0f, 0.0f, 0.0f };
    };

    inline HandCache& TLS()
    {
        static thread_local HandCache cache{};
        return cache;
    }

    inline bool Refresh(const VR* vr, HandCache& c)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const uint32_t s1 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u))
                continue;

            const float rpx = vr->m_RenderRightControllerPosAbsX.load(std::memory_order_relaxed);
            const float rpy = vr->m_RenderRightControllerPosAbsY.load(std::memory_order_relaxed);
            const float rpz = vr->m_RenderRightControllerPosAbsZ.load(std::memory_order_relaxed);

            const float rax = vr->m_RenderRightControllerAngAbsX.load(std::memory_order_relaxed);
            const float ray = vr->m_RenderRightControllerAngAbsY.load(std::memory_order_relaxed);
            const float raz = vr->m_RenderRightControllerAngAbsZ.load(std::memory_order_relaxed);

            const float lpx = vr->m_RenderLeftControllerPosAbsX.load(std::memory_order_relaxed);
            const float lpy = vr->m_RenderLeftControllerPosAbsY.load(std::memory_order_relaxed);
            const float lpz = vr->m_RenderLeftControllerPosAbsZ.load(std::memory_order_relaxed);

            const float lax = vr->m_RenderLeftControllerAngAbsX.load(std::memory_order_relaxed);
            const float lay = vr->m_RenderLeftControllerAngAbsY.load(std::memory_order_relaxed);
            const float laz = vr->m_RenderLeftControllerAngAbsZ.load(std::memory_order_relaxed);

            const float vpx = vr->m_RenderRecommendedViewmodelPosX.load(std::memory_order_relaxed);
            const float vpy = vr->m_RenderRecommendedViewmodelPosY.load(std::memory_order_relaxed);
            const float vpz = vr->m_RenderRecommendedViewmodelPosZ.load(std::memory_order_relaxed);

            const float vax = vr->m_RenderRecommendedViewmodelAngX.load(std::memory_order_relaxed);
            const float vay = vr->m_RenderRecommendedViewmodelAngY.load(std::memory_order_relaxed);
            const float vaz = vr->m_RenderRecommendedViewmodelAngZ.load(std::memory_order_relaxed);

            const uint32_t s2 = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
            if (s1 == s2 && !(s2 & 1u))
            {
                c.seq = s2;
                c.leftPos = Vector(lpx, lpy, lpz);
                c.leftAng = QAngle(lax, lay, laz);
                c.rightPos = Vector(rpx, rpy, rpz);
                c.rightAng = QAngle(rax, ray, raz);
                c.vmPos = Vector(vpx, vpy, vpz);
                c.vmAng = QAngle(vax, vay, vaz);
                return true;
            }
        }
        return false;
    }

    inline bool Get(const VR* vr, HandCache& c)
    {
        if (!VR::t_UseRenderFrameSnapshot)
            return false;

        const uint32_t s = vr->m_RenderFrameSeq.load(std::memory_order_acquire);
        if (s != 0 && !(s & 1u) && s == c.seq)
            return true;

        return Refresh(vr, c);
    }
}

QAngle VR::GetLeftControllerAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.leftAng;
    }

    return m_LeftControllerAngAbs;
}


Vector VR::GetLeftControllerAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.leftPos;
    }

    return m_LeftControllerPosAbs;
}


QAngle VR::GetRightControllerAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.rightAng;
    }

    return m_RightControllerAngAbs;
}


Vector VR::GetRightControllerAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.rightPos;
    }

    return m_RightControllerPosAbs;
}


Vector VR::GetRecommendedViewmodelAbsPos()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.vmPos;
    }

    Vector viewmodelPos = GetRightControllerAbsPos();
    if (m_MouseModeEnabled)
    {
        const Vector& anchor = IsMouseModeScopeActive() ? m_MouseModeScopedViewmodelAnchorOffset : m_MouseModeViewmodelAnchorOffset;
        viewmodelPos = m_HmdPosAbs
            + (m_HmdForward * (anchor.x * m_VRScale))
            + (m_HmdRight * (anchor.y * m_VRScale))
            + (m_HmdUp * (anchor.z * m_VRScale));
    }
    viewmodelPos -= m_ViewmodelForward * m_ViewmodelPosOffset.x;
    viewmodelPos -= m_ViewmodelRight * m_ViewmodelPosOffset.y;
    viewmodelPos -= m_ViewmodelUp * m_ViewmodelPosOffset.z;

    return viewmodelPos;
}


QAngle VR::GetRecommendedViewmodelAbsAngle()
{
    if (t_UseRenderFrameSnapshot)
    {
        auto& cache = vr_render_snapshot_cache_roomscale::TLS();
        if (vr_render_snapshot_cache_roomscale::Get(this, cache))
            return cache.vmAng;
    }

    QAngle result{};

    QAngle::VectorAngles(m_ViewmodelForward, m_ViewmodelUp, result);

    return result;
}


void VR::HandleMissingRenderContext(const char* location)
{
    m_CreatedVRTextures.store(false, std::memory_order_release);
    m_RenderedNewFrame.store(false, std::memory_order_release);
    m_RenderedHud.store(false, std::memory_order_release);
}

void VR::FinishFrame()
{
    if (!m_Compositor || !m_CompositorExplicitTiming)
        return;

    if (!m_CompositorNeedsHandoff)
        return;

    m_Compositor->PostPresentHandoff();
    m_CompositorNeedsHandoff = false;
}

void VR::GetMouseModeEyeRay(Vector& eyeDirOut, QAngle* eyeAngOut)
{
    eyeDirOut = { 0.0f, 0.0f, 0.0f };

    // Reset reference when not using HMD-driven aiming (so re-entering re-centers).
    if (!m_MouseModeEnabled || !m_MouseModeAimFromHmd)
        m_MouseModeHmdAimReferenceInitialized = false;

    QAngle eyeAng{ 0.0f, 0.0f, 0.0f };

    if (m_MouseModeAimFromHmd)
    {
        // Capture a reference direction the first frame HMD aiming becomes active.
        if (!m_MouseModeHmdAimReferenceInitialized)
        {
            // IMPORTANT:
            //  - m_HmdAngAbs already includes m_RotationOffset (mouse/body yaw).
            //  - MouseModeHmdAimSensitivity is meant to scale *HMD* deltas only.
            // If we capture/scale the full absolute angle, mouse yaw gets scaled too,
            // which makes MouseModeHmdAimSensitivity < 1 feel like mouse turning is "stuck",
            // and > 1 feel like mouse turning is too fast.
            //
            // So: keep the reference in "head-local" space (yaw with body yaw removed).
            m_MouseModeHmdAimReferenceAng = m_HmdAngAbs;
            m_MouseModeHmdAimReferenceAng.y -= m_RotationOffset;
            // Wrap yaw to [-180, 180]
            m_MouseModeHmdAimReferenceAng.y -= 360.0f * std::floor((m_MouseModeHmdAimReferenceAng.y + 180.0f) / 360.0f);
            m_MouseModeHmdAimReferenceAng.z = 0.0f;
            NormalizeAndClampViewAngles(m_MouseModeHmdAimReferenceAng);
            m_MouseModeHmdAimReferenceInitialized = true;
        }

        const float sens = std::clamp(m_MouseModeHmdAimSensitivity, 0.0f, 3.0f);

        // Scale pitch/yaw deltas around the captured reference.
        // Use "head-local" yaw (absolute yaw minus body yaw) so mouse turning stays 1:1.
        QAngle cur = m_HmdAngAbs;
        cur.y -= m_RotationOffset;
        // Wrap yaw to [-180, 180]
        cur.y -= 360.0f * std::floor((cur.y + 180.0f) / 360.0f);
        cur.z = 0.0f;
        NormalizeAndClampViewAngles(cur);

        auto wrapDelta = [](float deg)
            {
                deg -= 360.0f * std::floor((deg + 180.0f) / 360.0f);
                return deg;
            };

        const float dPitch = wrapDelta(cur.x - m_MouseModeHmdAimReferenceAng.x);
        const float dYaw = wrapDelta(cur.y - m_MouseModeHmdAimReferenceAng.y);

        eyeAng.x = m_MouseModeHmdAimReferenceAng.x + dPitch * sens;
        // Convert back to absolute yaw by re-applying body yaw.
        eyeAng.y = m_RotationOffset + (m_MouseModeHmdAimReferenceAng.y + dYaw * sens);
        eyeAng.z = 0.0f;
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }
    else
    {
        // Default mouse-mode eye ray: mouse pitch + body yaw (m_RotationOffset).
        const float pitch = std::clamp(m_MouseAimPitchOffset, -89.f, 89.f);
        const float yaw = m_RotationOffset;
        eyeAng = QAngle(pitch, yaw, 0.f);
        NormalizeAndClampViewAngles(eyeAng);

        Vector right, up;
        QAngle::AngleVectors(eyeAng, &eyeDirOut, &right, &up);
        if (!eyeDirOut.IsZero())
            VectorNormalize(eyeDirOut);
    }

    if (eyeAngOut)
        *eyeAngOut = eyeAng;
}

