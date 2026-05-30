Vector VR::GetAimRenderCameraDelta() const
{
    if (!m_IsThirdPersonCamera)
        return m_ThirdPersonViewOrigin - m_SetupOrigin;

    // 1:1 roomscale decoupled camera already makes controller/viewmodel positions live in the
    // current VR camera anchor space. Adding the third-person render-center delta again makes
    // aim/debug/D3D lines drift away from the physical controller until ResetPosition recenters.
    if (m_Roomscale1To1Movement && m_Roomscale1To1DecoupleCamera && !m_ForceNonVRServerMovement)
        return Vector{ 0.0f, 0.0f, 0.0f };

    return m_ThirdPersonRenderCenter - m_SetupOrigin;
}

bool VR::ShouldUseRoomscale1To1ServerMove() const
{
    const bool hasServerEntityMove =
        m_Game &&
        m_Game->m_EngineTraceServer &&
        m_Game->m_Offsets &&
        m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.address &&
        m_Game->m_Offsets->CBaseEntity_SetOrigin_Server.address;

    return m_IsVREnabled &&
        m_Roomscale1To1Movement &&
        m_Roomscale1To1ServerMove &&
        Hooks::s_ServerUnderstandsVR &&
        !m_ForceNonVRServerMovement &&
        hasServerEntityMove;
}

namespace
{
    struct Roomscale1To1ServerVisualState
    {
        VR* owner = nullptr;
        Vector pendingServerVisualWorldDelta = {};
        Vector pendingServerVisualCorrectionWorld = {};
        bool pendingServerVisualCorrectionWorldValid = false;
    };

    Roomscale1To1ServerVisualState& GetRoomscale1To1ServerVisualState(VR* owner)
    {
        static Roomscale1To1ServerVisualState state;
        if (state.owner != owner)
        {
            state = {};
            state.owner = owner;
        }
        return state;
    }

    bool IsFiniteRoomscalePlanarVector(const Vector& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    void NormalizeRoomscalePendingCorrection(Vector& correction, bool& valid)
    {
        correction.z = 0.0f;
        const float len = correction.Length();
        if (!std::isfinite(len) || len <= 0.01f)
        {
            correction = {};
            valid = false;
            return;
        }

        valid = true;
    }
}

void VR::QueueRoomscale1To1ServerMoveDelta(const Vector& worldDelta, const Vector& visualWorldDelta)
{
    Vector planarDelta(worldDelta.x, worldDelta.y, 0.0f);
    Vector planarVisualDelta(visualWorldDelta.x, visualWorldDelta.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarDelta) || !IsFiniteRoomscalePlanarVector(planarVisualDelta))
        return;

    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);

    // The VR pose can update faster than server usercmd processing. Keep only the newest
    // direct server move request, but cancel the visual displacement that belonged to any
    // overwritten request. Otherwise the HMD camera continues forward while the player
    // entity never receives that discarded physical step.
    if (m_Roomscale1To1PendingServerWorldDeltaValid)
    {
        visualState.pendingServerVisualCorrectionWorld -= visualState.pendingServerVisualWorldDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
    }

    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;

    const float requestedLen = planarDelta.Length();
    if (!std::isfinite(requestedLen) || requestedLen <= 0.01f)
    {
        visualState.pendingServerVisualCorrectionWorld -= planarVisualDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
        return;
    }

    m_Roomscale1To1PendingServerWorldDelta = planarDelta;
    visualState.pendingServerVisualWorldDelta = planarVisualDelta;

    const float maxServerStep =
        std::max(1.0f, std::max(0.0f, m_Roomscale1To1MaxStepMeters) * std::max(1.0f, std::fabs(m_VRScale)));
    const float len = m_Roomscale1To1PendingServerWorldDelta.Length();
    if (std::isfinite(len) && len > maxServerStep && len > 0.0001f)
        m_Roomscale1To1PendingServerWorldDelta *= (maxServerStep / len);

    m_Roomscale1To1PendingServerWorldDeltaValid = true;
}

bool VR::ConsumeRoomscale1To1ServerMoveDelta(Vector& outWorldDelta, Vector& outVisualWorldDelta)
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    if (!m_Roomscale1To1PendingServerWorldDeltaValid)
        return false;

    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    outWorldDelta = m_Roomscale1To1PendingServerWorldDelta;
    outVisualWorldDelta = visualState.pendingServerVisualWorldDelta;
    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
    return outWorldDelta.Length() > 0.01f;
}

void VR::QueueRoomscale1To1ServerVisualCorrection(const Vector& worldCorrection)
{
    Vector planarCorrection(worldCorrection.x, worldCorrection.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarCorrection))
        return;

    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    visualState.pendingServerVisualCorrectionWorld += planarCorrection;
    NormalizeRoomscalePendingCorrection(
        visualState.pendingServerVisualCorrectionWorld,
        visualState.pendingServerVisualCorrectionWorldValid);
}

void VR::ApplyRoomscale1To1VisualWorldCorrection(const Vector& worldCorrection)
{
    Vector planarCorrection(worldCorrection.x, worldCorrection.y, 0.0f);
    if (!IsFiniteRoomscalePlanarVector(planarCorrection))
        return;

    const float correctionLen = planarCorrection.Length();
    if (!std::isfinite(correctionLen) || correctionLen <= 0.01f || std::fabs(m_VRScale) <= 0.001f)
        return;

    const Vector correctionLocal = planarCorrection / m_VRScale;
    m_HmdPosCorrectedPrev += correctionLocal;
    if (m_Roomscale1To1PrevValid)
        m_Roomscale1To1PrevCorrectedAbs += correctionLocal;

    // A roomscale collision correction is a coordinate rebase, not tracked-head motion.
    // Shift the smoothed/cached values together so head smoothing does not repay the
    // rejected movement over several rendered frames and cached aim/controller positions
    // remain aligned until the next tracking update.
    if (m_HeadSmoothingInitialized)
        m_HmdPosSmoothed += correctionLocal;
    m_HmdPosLocalInWorld += planarCorrection;
    m_HmdPosAbs += planarCorrection;
    m_HmdPosAbsPrev += planarCorrection;
    m_SetupOriginToHMD += planarCorrection;
    m_LeftControllerPosAbs += planarCorrection;
    m_RightControllerPosAbs += planarCorrection;
}

void VR::ApplyPendingRoomscale1To1ServerVisualCorrection()
{
    Vector worldCorrection{};
    {
        std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
        auto& visualState = GetRoomscale1To1ServerVisualState(this);
        if (!visualState.pendingServerVisualCorrectionWorldValid)
            return;

        worldCorrection = visualState.pendingServerVisualCorrectionWorld;
        visualState.pendingServerVisualCorrectionWorld = {};
        visualState.pendingServerVisualCorrectionWorldValid = false;
    }

    ApplyRoomscale1To1VisualWorldCorrection(worldCorrection);

    if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
    {
        Game::logMsg("[VR][1to1][servermove-visual-correct] world=(%.2f %.2f) len=%.2f",
            worldCorrection.x, worldCorrection.y, worldCorrection.Length());
    }
}

void VR::CancelRoomscale1To1ServerMoveDelta()
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    if (m_Roomscale1To1PendingServerWorldDeltaValid)
    {
        visualState.pendingServerVisualCorrectionWorld -= visualState.pendingServerVisualWorldDelta;
        NormalizeRoomscalePendingCorrection(
            visualState.pendingServerVisualCorrectionWorld,
            visualState.pendingServerVisualCorrectionWorldValid);
    }

    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
}

void VR::ClearRoomscale1To1ServerMoveDelta()
{
    std::lock_guard<std::mutex> lock(m_Roomscale1To1ServerMoveMutex);
    auto& visualState = GetRoomscale1To1ServerVisualState(this);
    m_Roomscale1To1PendingServerWorldDelta = {};
    visualState.pendingServerVisualWorldDelta = {};
    m_Roomscale1To1PendingServerWorldDeltaValid = false;
    visualState.pendingServerVisualCorrectionWorld = {};
    visualState.pendingServerVisualCorrectionWorldValid = false;
}

void VR::ApplyRoomscale1To1Move(CUserCmd* cmd, float inputSampleTime, bool controlLocomotionActive)
{
    const Vector cur = m_HmdPosCorrectedPrev;

    auto resetReference = [&]()
        {
            m_Roomscale1To1PrevCorrectedAbs = cur;
            m_Roomscale1To1PrevValid = true;
        };

    m_Roomscale1To1PendingVisualWorldDelta = {};
    m_Roomscale1To1PendingVisualWorldDeltaValid = false;

    if (!cmd || !m_Roomscale1To1Movement)
    {
        m_Roomscale1To1PrevValid = false;
        CancelRoomscale1To1ServerMoveDelta();
        m_Roomscale1To1StandingHmdZValid = false;
        m_Roomscale1To1PhysicalCrouchActive = false;
        return;
    }

    constexpr int kIN_DUCK = (1 << 2);
    if (m_Roomscale1To1PhysicalCrouch)
    {
        if (!m_Roomscale1To1StandingHmdZValid)
        {
            m_Roomscale1To1StandingHmdZ = cur.z;
            m_Roomscale1To1StandingHmdZValid = true;
            m_Roomscale1To1PhysicalCrouchActive = false;
        }

        if (!m_Roomscale1To1PhysicalCrouchActive && cur.z > m_Roomscale1To1StandingHmdZ)
            m_Roomscale1To1StandingHmdZ = cur.z;

        const float enterM = std::clamp(m_Roomscale1To1CrouchEnterMeters, 0.05f, 1.0f);
        const float exitM = std::clamp(m_Roomscale1To1CrouchExitMeters, 0.0f, enterM);
        const float crouchDepthM = m_Roomscale1To1StandingHmdZ - cur.z;
        if (!m_Roomscale1To1PhysicalCrouchActive && crouchDepthM >= enterM)
            m_Roomscale1To1PhysicalCrouchActive = true;
        else if (m_Roomscale1To1PhysicalCrouchActive && crouchDepthM <= exitM)
            m_Roomscale1To1PhysicalCrouchActive = false;

        if (m_Roomscale1To1PhysicalCrouchActive)
            cmd->buttons |= kIN_DUCK;
    }
    else
    {
        m_Roomscale1To1StandingHmdZValid = false;
        m_Roomscale1To1PhysicalCrouchActive = false;
    }

    if (m_Roomscale1To1DisableWhileThumbstick && controlLocomotionActive)
    {
        resetReference();
        CancelRoomscale1To1ServerMoveDelta();
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

    const float movementScale = std::clamp(m_Roomscale1To1MovementScale, 0.0f, 4.0f);

    const float maxStepM = std::max(0.0f, m_Roomscale1To1MaxStepMeters);
    if (maxStepM > 0.0f && lenM > maxStepM)
    {
        const float s = maxStepM / lenM;
        deltaM.x *= s;
        deltaM.y *= s;
    }

    const Vector gameDeltaM(deltaM.x * movementScale, deltaM.y * movementScale, 0.0f);

    // The physical HMD pose has already moved the camera by deltaM. When the configured
    // roomscale multiplier is not 1.0, immediately rebase the visual camera by the
    // remaining scaled displacement. Server/client movement reconciliation must then
    // compare against the scaled visual delta, otherwise the player entity can move
    // farther or shorter than the rendered camera until ResetPosition is used.
    const Vector visualScaleCorrectionM = gameDeltaM - deltaM;
    const Vector visualScaleCorrectionWorld(
        visualScaleCorrectionM.x * m_VRScale,
        visualScaleCorrectionM.y * m_VRScale,
        0.0f);
    ApplyRoomscale1To1VisualWorldCorrection(visualScaleCorrectionWorld);

    m_Roomscale1To1PendingVisualWorldDelta = Vector(gameDeltaM.x * m_VRScale, gameDeltaM.y * m_VRScale, 0.0f);
    m_Roomscale1To1PendingVisualWorldDeltaValid =
        m_Roomscale1To1PendingVisualWorldDelta.Length() > 0.01f;

    if (ShouldUseRoomscale1To1ServerMove())
    {
        Vector serverWorldDelta(gameDeltaM.x * m_VRScale, gameDeltaM.y * m_VRScale, 0.0f);
        QueueRoomscale1To1ServerMoveDelta(serverWorldDelta, m_Roomscale1To1PendingVisualWorldDelta);
        m_Roomscale1To1PendingVisualWorldDelta = {};
        m_Roomscale1To1PendingVisualWorldDeltaValid = false;

        if (m_Roomscale1To1DebugLog && !ShouldThrottle(m_Roomscale1To1DebugLastEncode, m_Roomscale1To1DebugLogHz))
        {
            Game::logMsg("[VR][1to1][servermove-queue] cmd=%d tick=%d scale=%.3f hmdM=(%.3f %.3f) gameM=(%.3f %.3f) world=(%.1f %.1f)",
                cmd->command_number, cmd->tick_count,
                movementScale,
                deltaM.x, deltaM.y,
                gameDeltaM.x, gameDeltaM.y,
                serverWorldDelta.x, serverWorldDelta.y);
        }
        return;
    }

    if (movementScale <= 0.0001f)
        return;

    float dt = inputSampleTime;
    if (dt <= 0.0001f)
        dt = 1.0f / 30.0f;
    dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 15.0f);

    Vector roomWorldVelocity(gameDeltaM.x * m_VRScale / dt, gameDeltaM.y * m_VRScale / dt, 0.0f);
    const float roomSpeed = std::sqrt((roomWorldVelocity.x * roomWorldVelocity.x) + (roomWorldVelocity.y * roomWorldVelocity.y));
    const float maxCmdSpeed = (m_AdjustingViewmodel || m_AdjustingScope) ? 25.0f : 250.0f;
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
        Game::logMsg("[VR][1to1][cmdmove] cmd=%d tick=%d dt=%.4f scale=%.3f hmdM=(%.3f %.3f) gameM=(%.3f %.3f) vel=(%.1f %.1f) move=(%.1f %.1f)",
            cmd->command_number, cmd->tick_count, dt,
            movementScale,
            deltaM.x, deltaM.y,
            gameDeltaM.x, gameDeltaM.y,
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

