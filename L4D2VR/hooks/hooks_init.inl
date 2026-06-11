bool Hooks::s_ServerUnderstandsVR = false;

Hooks::Hooks(Game* game)
{
	if (MH_Initialize() != MH_OK)
	{
		Game::errorMsg("Failed to init MinHook");
	}

	m_Game = game;
	m_VR = m_Game->m_VR;

	m_HUDStep = HUDPushStep::None;
	m_PushedHud = false;

	initSourceHooks();

	hkGetRenderTarget.enableHook();
	if (hkEndFrame.pTarget)
		hkEndFrame.enableHook();
	hkCalcViewModelView.enableHook();
	hkServerFireTerrorBullets.enableHook();
	hkClientFireTerrorBullets.enableHook();
	hkProcessUsercmds.enableHook();
	hkReadUsercmd.enableHook();
	hkWriteUsercmdDeltaToBuffer.enableHook();
	hkWriteUsercmd.enableHook();
	hkAdjustEngineViewport.enableHook();
	hkViewport.enableHook();
	hkGetViewport.enableHook();
	hkCreateMove.enableHook();
	hkTestMeleeSwingCollisionClient.enableHook();
	hkTestMeleeSwingCollisionServer.enableHook();
	hkDoMeleeSwingServer.enableHook();
	hkStartMeleeSwingServer.enableHook();
	hkPrimaryAttackServer.enableHook();
	hkItemPostFrameServer.enableHook();
	hkGetPrimaryAttackActivity.enableHook();
	hkEyePosition.enableHook();
	if (hkServerPlayerEyePosition.pTarget &&
		hkServerPlayerEyeAngles.pTarget &&
		(hkFindUseEntity.pTarget || hkPlayerUse.pTarget))
	{
		hkServerPlayerEyePosition.enableHook();
		hkServerPlayerEyeAngles.enableHook();
		if (hkFindUseEntity.pTarget)
			hkFindUseEntity.enableHook();
		if (hkPlayerUse.pTarget)
			hkPlayerUse.enableHook();
		Game::logMsg(
			"[VR][Use] installed L4D2 FindUseEntity/PlayerUse controller-pose hooks findUse=%p playerUse=%p",
			hkFindUseEntity.pTarget,
			hkPlayerUse.pTarget);
	}
	if (hkClientFindUseEntity.pTarget &&
		hkClientPlayerEyePosition.pTarget &&
		hkClientPlayerEyeVectors.pTarget)
	{
		hkClientPlayerEyePosition.enableHook();
		hkClientPlayerEyeVectors.enableHook();
		hkClientFindUseEntity.enableHook();
		Game::logMsg(
			"[VR][Use] installed L4D2 client FindUseEntity highlight controller-pose hook findUse=%p eyePos=%p eyeVectors=%p",
			hkClientFindUseEntity.pTarget,
			hkClientPlayerEyePosition.pTarget,
			hkClientPlayerEyeVectors.pTarget);
	}
	hkDrawModelExecute.enableHook();
	hkRenderView.enableHook();
	hkPushRenderTargetAndViewport.enableHook();
	hkPopRenderTargetAndViewport.enableHook();
	hkVgui_Paint.enableHook();
	hkIsSplitScreen.enableHook();
	hkPrePushRenderTarget.enableHook();
	if (hkSayText.pTarget)
		hkSayText.enableHook();
	if (hkSayText2.pTarget)
		hkSayText2.enableHook();
	if (hkTextMsg.pTarget)
		hkTextMsg.enableHook();
	if (hkEmitSoundAttenuation.pTarget)
		hkEmitSoundAttenuation.enableHook();
	if (hkEmitSoundLevel.pTarget)
		hkEmitSoundLevel.enableHook();
	if (hkUpdateLaserSight.pTarget)
		hkUpdateLaserSight.enableHook();
	if (hkUpdateFlashlight.pTarget)
		hkUpdateFlashlight.enableHook();
	if (hkUpdateFlashlightColor.pTarget)
		hkUpdateFlashlightColor.enableHook();
	if (hkConVarSetValueString.pTarget)
		hkConVarSetValueString.enableHook();
	if (hkConVarSetValueFloat.pTarget)
		hkConVarSetValueFloat.enableHook();
	if (hkConVarSetValueInt.pTarget)
		hkConVarSetValueInt.enableHook();
	if (hkConVarPrimarySetValueString.pTarget)
		hkConVarPrimarySetValueString.enableHook();
	if (hkConVarPrimarySetValueFloat.pTarget)
		hkConVarPrimarySetValueFloat.enableHook();
	if (hkConVarPrimarySetValueInt.pTarget)
		hkConVarPrimarySetValueInt.enableHook();
	if (hkConVarInternalSetValueString.pTarget)
		hkConVarInternalSetValueString.enableHook();
	if (hkConVarInternalSetValueFloat.pTarget)
		hkConVarInternalSetValueFloat.enableHook();
	if (hkConVarInternalSetValueInt.pTarget)
		hkConVarInternalSetValueInt.enableHook();
}

Hooks::~Hooks()
{
	if (MH_Uninitialize() != MH_OK)
	{
		Game::errorMsg("Failed to uninitialize MinHook");
	}
}


int Hooks::initSourceHooks()
{
	LPVOID pGetRenderTargetVFunc = (LPVOID)(m_Game->m_Offsets->GetRenderTarget.address);
	hkGetRenderTarget.createHook(pGetRenderTargetVFunc, &dGetRenderTarget);

	if (m_Game->m_MaterialSystem)
	{
		void** materialVTable = *reinterpret_cast<void***>(m_Game->m_MaterialSystem);
		if (materialVTable)
		{
			// IMaterialSystem::EndFrame is vfunc #37 in VMaterialSystem080.
			hkEndFrame.createHook(materialVTable[37], &dEndFrame);
		}
	}

	LPVOID pRenderViewVFunc = (LPVOID)(m_Game->m_Offsets->RenderView.address);
	hkRenderView.createHook(pRenderViewVFunc, &dRenderView);

	LPVOID calcViewModelViewAddr = (LPVOID)(m_Game->m_Offsets->CalcViewModelView.address);
	hkCalcViewModelView.createHook(calcViewModelViewAddr, &dCalcViewModelView);

	LPVOID serverFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ServerFireTerrorBullets.address);
	hkServerFireTerrorBullets.createHook(serverFireTerrorBulletsAddr, &dServerFireTerrorBullets);

	LPVOID clientFireTerrorBulletsAddr = (LPVOID)(m_Game->m_Offsets->ClientFireTerrorBullets.address);
	hkClientFireTerrorBullets.createHook(clientFireTerrorBulletsAddr, &dClientFireTerrorBullets);

	LPVOID ProcessUsercmdsAddr = (LPVOID)(m_Game->m_Offsets->ProcessUsercmds.address);
	hkProcessUsercmds.createHook(ProcessUsercmdsAddr, &dProcessUsercmds);

	LPVOID ReadUserCmdAddr = (LPVOID)(m_Game->m_Offsets->ReadUserCmd.address);
	hkReadUsercmd.createHook(ReadUserCmdAddr, &dReadUsercmd);

	LPVOID WriteUsercmdDeltaToBufferAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmdDeltaToBuffer.address);
	hkWriteUsercmdDeltaToBuffer.createHook(WriteUsercmdDeltaToBufferAddr, &dWriteUsercmdDeltaToBuffer);

	LPVOID WriteUsercmdAddr = (LPVOID)(m_Game->m_Offsets->WriteUsercmd.address);
	hkWriteUsercmd.createHook(WriteUsercmdAddr, &dWriteUsercmd);

	LPVOID AdjustEngineViewportAddr = (LPVOID)(m_Game->m_Offsets->AdjustEngineViewport.address);
	hkAdjustEngineViewport.createHook(AdjustEngineViewportAddr, &dAdjustEngineViewport);

	LPVOID ViewportAddr = (LPVOID)(m_Game->m_Offsets->Viewport.address);
	hkViewport.createHook(ViewportAddr, &dViewport);

	LPVOID GetViewportAddr = (LPVOID)(m_Game->m_Offsets->GetViewport.address);
	hkGetViewport.createHook(GetViewportAddr, &dGetViewport);

	LPVOID MeleeSwingClientAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingClient.address);
	hkTestMeleeSwingCollisionClient.createHook(MeleeSwingClientAddr, &dTestMeleeSwingCollisionClient);

	LPVOID MeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->TestMeleeSwingServer.address);
	hkTestMeleeSwingCollisionServer.createHook(MeleeSwingServerAddr, &dTestMeleeSwingCollisionServer);

	LPVOID DoMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->DoMeleeSwingServer.address);
	hkDoMeleeSwingServer.createHook(DoMeleeSwingServerAddr, &dDoMeleeSwingServer);

	LPVOID StartMeleeSwingServerAddr = (LPVOID)(m_Game->m_Offsets->StartMeleeSwingServer.address);
	hkStartMeleeSwingServer.createHook(StartMeleeSwingServerAddr, &dStartMeleeSwingServer);

	LPVOID PrimaryAttackServerAddr = (LPVOID)(m_Game->m_Offsets->PrimaryAttackServer.address);
	hkPrimaryAttackServer.createHook(PrimaryAttackServerAddr, &dPrimaryAttackServer);

	LPVOID ItemPostFrameServerAddr = (LPVOID)(m_Game->m_Offsets->ItemPostFrameServer.address);
	hkItemPostFrameServer.createHook(ItemPostFrameServerAddr, &dItemPostFrameServer);

	LPVOID GetPrimaryAttackActivityAddr = (LPVOID)(m_Game->m_Offsets->GetPrimaryAttackActivity.address);
	hkGetPrimaryAttackActivity.createHook(GetPrimaryAttackActivityAddr, &dGetPrimaryAttackActivity);

	LPVOID EyePositionAddr = (LPVOID)(m_Game->m_Offsets->EyePosition.address);
	hkEyePosition.createHook(EyePositionAddr, &dEyePosition);

	if (m_Game->m_Offsets->ServerPlayerEyePosition.valid &&
		m_Game->m_Offsets->ServerPlayerEyeAngles.valid &&
		(m_Game->m_Offsets->FindUseEntity.valid || m_Game->m_Offsets->PlayerUse.valid))
	{
		LPVOID serverPlayerEyePositionAddr = (LPVOID)(m_Game->m_Offsets->ServerPlayerEyePosition.address);
		LPVOID serverPlayerEyeAnglesAddr = (LPVOID)(m_Game->m_Offsets->ServerPlayerEyeAngles.address);
		hkServerPlayerEyePosition.createHook(serverPlayerEyePositionAddr, &dServerPlayerEyePosition);
		hkServerPlayerEyeAngles.createHook(serverPlayerEyeAnglesAddr, &dServerPlayerEyeAngles);
		if (m_Game->m_Offsets->FindUseEntity.valid)
		{
			LPVOID findUseEntityAddr = (LPVOID)(m_Game->m_Offsets->FindUseEntity.address);
			hkFindUseEntity.createHook(findUseEntityAddr, &dFindUseEntity);
		}
		if (m_Game->m_Offsets->PlayerUse.valid)
		{
			LPVOID playerUseAddr = (LPVOID)(m_Game->m_Offsets->PlayerUse.address);
			hkPlayerUse.createHook(playerUseAddr, &dPlayerUse);
		}
	}

	if (m_Game->m_Offsets->ClientFindUseEntity.valid &&
		m_Game->m_Offsets->ClientPlayerEyePosition.valid &&
		m_Game->m_Offsets->ClientPlayerEyeVectors.valid)
	{
		LPVOID clientFindUseEntityAddr = (LPVOID)(m_Game->m_Offsets->ClientFindUseEntity.address);
		LPVOID clientPlayerEyePositionAddr = (LPVOID)(m_Game->m_Offsets->ClientPlayerEyePosition.address);
		LPVOID clientPlayerEyeVectorsAddr = (LPVOID)(m_Game->m_Offsets->ClientPlayerEyeVectors.address);
		hkClientFindUseEntity.createHook(clientFindUseEntityAddr, &dClientFindUseEntity);
		hkClientPlayerEyePosition.createHook(clientPlayerEyePositionAddr, &dClientPlayerEyePosition);
		hkClientPlayerEyeVectors.createHook(clientPlayerEyeVectorsAddr, &dClientPlayerEyeVectors);
	}

	LPVOID DrawModelExecuteAddr = (LPVOID)(m_Game->m_Offsets->DrawModelExecute.address);
	hkDrawModelExecute.createHook(DrawModelExecuteAddr, &dDrawModelExecute);

	LPVOID PushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PushRenderTargetAndViewport.address);
	hkPushRenderTargetAndViewport.createHook(PushRenderTargetAddr, &dPushRenderTargetAndViewport);

	LPVOID PopRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PopRenderTargetAndViewport.address);
	hkPopRenderTargetAndViewport.createHook(PopRenderTargetAddr, &dPopRenderTargetAndViewport);

	LPVOID VGui_PaintAddr = (LPVOID)(m_Game->m_Offsets->VGui_Paint.address);
	hkVgui_Paint.createHook(VGui_PaintAddr, &dVGui_Paint);

	LPVOID IsSplitScreenAddr = (LPVOID)(m_Game->m_Offsets->IsSplitScreen.address);
	hkIsSplitScreen.createHook(IsSplitScreenAddr, &dIsSplitScreen);

	LPVOID PrePushRenderTargetAddr = (LPVOID)(m_Game->m_Offsets->PrePushRenderTarget.address);
	hkPrePushRenderTarget.createHook(PrePushRenderTargetAddr, &dPrePushRenderTarget);

	LPVOID SayTextAddr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientSayTextHandlerOffset);
	hkSayText.createHook(SayTextAddr, &dSayText);

	LPVOID SayText2Addr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientSayText2HandlerOffset);
	hkSayText2.createHook(SayText2Addr, &dSayText2);

	LPVOID TextMsgAddr = reinterpret_cast<LPVOID>(m_Game->m_BaseClient + kClientTextMsgHandlerOffset);
	hkTextMsg.createHook(TextMsgAddr, &dTextMsg);

	// alliedmodders/hl2sdk l4d2 public/engine/IEngineSound.h defines:
	//   #5 EmitSound(... float flAttenuation ...)
	//   #6 EmitSound(... soundlevel_t iSoundlevel ...)
	// No specialDSP stack argument exists in the L4D2 ABI. Hook both verified overloads
	// so MagazineInteraction can suppress native reload sounds while physical insertion is active.
	if (m_Game->m_EngineSound)
	{
		void** engineSoundVTable = nullptr;
#if defined(_MSC_VER)
		__try
		{
			engineSoundVTable = *reinterpret_cast<void***>(m_Game->m_EngineSound);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			engineSoundVTable = nullptr;
		}
#else
		engineSoundVTable = *reinterpret_cast<void***>(m_Game->m_EngineSound);
#endif

		constexpr size_t kEmitSoundAttenuationVFunc = 5;
		constexpr size_t kEmitSoundLevelVFunc = 6;
		if (engineSoundVTable &&
			engineSoundVTable[kEmitSoundAttenuationVFunc] &&
			engineSoundVTable[kEmitSoundLevelVFunc])
		{
			const int attenuationHookResult = hkEmitSoundAttenuation.createHook(
				engineSoundVTable[kEmitSoundAttenuationVFunc],
				&dEmitSoundAttenuation);
			const int levelHookResult = hkEmitSoundLevel.createHook(
				engineSoundVTable[kEmitSoundLevelVFunc],
				&dEmitSoundLevel);
			if (attenuationHookResult == 0 && levelHookResult == 0)
				Game::logMsg("[VR][MagazineInteraction][Audio] verified L4D2 IEngineSoundClient003 EmitSound hooks installed");
			else
				Game::logMsg("[VR][MagazineInteraction][Audio] warning: one or more verified EmitSound hooks failed to install");
		}
		else
		{
			Game::logMsg("[VR][MagazineInteraction][Audio] disabled: IEngineSoundClient003 vtable unavailable");
		}
	}
	else
	{
		Game::logMsg("[VR][MagazineInteraction][Audio] disabled: IEngineSoundClient003 interface unavailable");
	}

	if (m_Game->m_Offsets->UpdateLaserSight.valid)
	{
		LPVOID UpdateLaserSightAddr = (LPVOID)(m_Game->m_Offsets->UpdateLaserSight.address);
		hkUpdateLaserSight.createHook(UpdateLaserSightAddr, &dUpdateLaserSight);
	}

	if (m_Game->m_Offsets->UpdateFlashlight.valid)
	{
		LPVOID UpdateFlashlightAddr = (LPVOID)(m_Game->m_Offsets->UpdateFlashlight.address);
		hkUpdateFlashlight.createHook(UpdateFlashlightAddr, &dUpdateFlashlight);
	}

	if (m_Game->m_Offsets->UpdateFlashlightColor.valid)
	{
		LPVOID UpdateFlashlightColorAddr = (LPVOID)(m_Game->m_Offsets->UpdateFlashlightColor.address);
		hkUpdateFlashlightColor.createHook(UpdateFlashlightColorAddr, &dUpdateFlashlightColor);
	}

	const char* conVarSamples[] = { "name", "r_shadows", "cl_ragdoll_limit" };
	for (const char* sample : conVarSamples)
	{
		if (!hkConVarSetValueString.pTarget)
		{
			LPVOID target = m_Game->GetConVarStringSetValueTarget(sample);
			if (target)
				hkConVarSetValueString.createHook(target, &dConVarSetValueString);
		}

		if (!hkConVarSetValueFloat.pTarget)
		{
			LPVOID target = m_Game->GetConVarFloatSetValueTarget(sample);
			if (target)
				hkConVarSetValueFloat.createHook(target, &dConVarSetValueFloat);
		}

		if (!hkConVarSetValueInt.pTarget)
		{
			LPVOID target = m_Game->GetConVarIntSetValueTarget(sample);
			if (target)
				hkConVarSetValueInt.createHook(target, &dConVarSetValueInt);
		}

		if (hkConVarSetValueString.pTarget &&
			hkConVarSetValueFloat.pTarget &&
			hkConVarSetValueInt.pTarget)
			break;
	}

	uintptr_t clientModeAddress = m_Game->m_Offsets->g_pClientMode.address;
	if (!clientModeAddress)
	{
		Game::errorMsg("g_pClientMode address was null; aborting CreateMove hook installation");
		return 0;
	}

	void* clientMode = nullptr;
	constexpr int kMaxAttempts = 500;
	for (int attempt = 0; attempt < kMaxAttempts && !clientMode; ++attempt)
	{
		uintptr_t clientModePtr = *reinterpret_cast<uintptr_t*>(clientModeAddress);
		if (clientModePtr)
		{
			uintptr_t clientModeValue = *reinterpret_cast<uintptr_t*>(clientModePtr);
			if (clientModeValue)
			{
				clientMode = reinterpret_cast<void*>(clientModeValue);
				break;
			}
		}

		Sleep(10);
	}

	if (!clientMode)
	{
		Game::errorMsg("Timed out waiting for g_pClientMode; CreateMove hook not installed");
		return 0;
	}

	void*** clientModePtr = reinterpret_cast<void***>(clientMode);
	void** clientModeVTable = (clientModePtr != nullptr) ? *clientModePtr : nullptr;
	if (!clientModeVTable)
	{
		Game::errorMsg("Client mode vtable pointer was null; CreateMove hook not installed");
		return 0;
	}

	hkCreateMove.createHook(clientModeVTable[27], dCreateMove);
	return 1;
}


