namespace
{
	enum class ManualThrowableKind : int
	{
		Molotov,
		PipeBomb,
		VomitJar,
	};

	static bool ManualThrowIsFiniteVector(const Vector& value)
	{
		return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
	}

	static void* ManualThrowReadEntityVtable(void* entity)
	{
		if (!entity)
			return nullptr;
#ifdef _MSC_VER
		__try
		{
			return *reinterpret_cast<void**>(entity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
#else
		return *reinterpret_cast<void**>(entity);
#endif
	}

	static bool ManualThrowWeaponIdIsThrowable(int weaponId)
	{
		return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::MOLOTOV) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PIPE_BOMB) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::VOMITJAR);
	}

	static bool ManualThrowWeaponIdIsCarryable(int weaponId)
	{
		return weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::GASCAN) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::PROPANE_TANK) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::OXYGEN_TANK) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::GNOME_CHOMPSKI) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::COLA_BOTTLES) ||
			weaponId == static_cast<int>(C_WeaponCSBase::WeaponID::FIREWORKS_BOX);
	}

	static const char* ManualCarryThrowWeaponName(int weaponId)
	{
		switch (weaponId)
		{
		case C_WeaponCSBase::WeaponID::GASCAN: return "gascan";
		case C_WeaponCSBase::WeaponID::PROPANE_TANK: return "propane_tank";
		case C_WeaponCSBase::WeaponID::OXYGEN_TANK: return "oxygen_tank";
		case C_WeaponCSBase::WeaponID::GNOME_CHOMPSKI: return "gnome";
		case C_WeaponCSBase::WeaponID::COLA_BOTTLES: return "cola_bottles";
		case C_WeaponCSBase::WeaponID::FIREWORKS_BOX: return "fireworks_box";
		default: return "unknown";
		}
	}

	static int ManualThrowExpectedWeaponId(ManualThrowableKind kind)
	{
		switch (kind)
		{
		case ManualThrowableKind::Molotov:
			return static_cast<int>(C_WeaponCSBase::WeaponID::MOLOTOV);
		case ManualThrowableKind::PipeBomb:
			return static_cast<int>(C_WeaponCSBase::WeaponID::PIPE_BOMB);
		case ManualThrowableKind::VomitJar:
			return static_cast<int>(C_WeaponCSBase::WeaponID::VOMITJAR);
		default:
			return static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		}
	}

	static const char* ManualThrowKindName(ManualThrowableKind kind)
	{
		switch (kind)
		{
		case ManualThrowableKind::Molotov:
			return "molotov";
		case ManualThrowableKind::PipeBomb:
			return "pipe_bomb";
		case ManualThrowableKind::VomitJar:
			return "vomitjar";
		default:
			return "unknown";
		}
	}

	static void ManualThrowClearPoseHistory(Player& player)
	{
		for (ManualThrowPoseSample& sample : player.manualThrowPoseSamples)
			sample = {};
		player.manualThrowPoseCount = 0;
		player.manualThrowLastTick = 0;
	}

	static void ManualThrowResetPlayerState(Player& player)
	{
		ManualThrowClearPoseHistory(player);
		player.throwableAimWeaponTag = 0;
		player.throwableAimWeaponId = static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		player.throwableAimTicks = 0;
		player.throwableAimPrevAttackDown = false;
		player.throwableAimPrevWeaponThrowable = false;
		player.manualCarryThrowLastDecodedReleaseTick = 0;
		player.manualThrowPending = {};
	}

	static void ManualThrowRecordPoseSample(Player& player, int tick, const Vector& position, const QAngle& angles)
	{
		if (tick <= 0 || !ManualThrowIsFiniteVector(position) || !IsFiniteViewAngle(angles))
			return;

		if (player.manualThrowLastTick > 0 &&
			(tick > player.manualThrowLastTick + 16 || tick < player.manualThrowLastTick - 16))
		{
			ManualThrowClearPoseHistory(player);
		}

		for (int i = 0; i < player.manualThrowPoseCount; ++i)
		{
			ManualThrowPoseSample& sample = player.manualThrowPoseSamples[static_cast<size_t>(i)];
			if (sample.valid && sample.tick == tick)
			{
				sample.position = position;
				sample.angles = angles;
				player.manualThrowLastTick = (std::max)(player.manualThrowLastTick, tick);
				return;
			}
		}

		ManualThrowPoseSample sample{};
		sample.valid = true;
		sample.tick = tick;
		sample.position = position;
		sample.angles = angles;

		if (player.manualThrowPoseCount < static_cast<int>(Player::kManualThrowPoseSampleCount))
		{
			player.manualThrowPoseSamples[static_cast<size_t>(player.manualThrowPoseCount)] = sample;
			++player.manualThrowPoseCount;
		}
		else
		{
			player.manualThrowPoseSamples[0] = sample;
		}

		std::sort(
			player.manualThrowPoseSamples.begin(),
			player.manualThrowPoseSamples.begin() + player.manualThrowPoseCount,
			[](const ManualThrowPoseSample& lhs, const ManualThrowPoseSample& rhs)
			{
				return lhs.tick < rhs.tick;
			});

		player.manualThrowLastTick = (std::max)(player.manualThrowLastTick, tick);
	}

	static bool ManualThrowEstimateReleaseVelocity(
		const Player& player,
		int releaseTick,
		int velocityWindowTicks,
		float peakVelocityBlend,
		Vector& linearVelocity,
		Vector& angularVelocity)
	{
		linearVelocity = {};
		angularVelocity = {};

		if (player.manualThrowPoseCount < 2 || releaseTick <= 0)
			return false;

		velocityWindowTicks = std::clamp(velocityWindowTicks, 1, static_cast<int>(Player::kManualThrowPoseSampleCount) - 1);
		peakVelocityBlend = std::clamp(peakVelocityBlend, 0.0f, 1.0f);
		constexpr float kServerTickSeconds = 1.0f / 30.0f;
		float totalWeight = 0.0f;
		Vector peakLinearVelocity{};
		float peakLinearSpeedSqr = -1.0f;

		for (int i = 1; i < player.manualThrowPoseCount; ++i)
		{
			const ManualThrowPoseSample& previous = player.manualThrowPoseSamples[static_cast<size_t>(i - 1)];
			const ManualThrowPoseSample& current = player.manualThrowPoseSamples[static_cast<size_t>(i)];
			if (!previous.valid || !current.valid || current.tick > releaseTick)
				continue;
			if (releaseTick - previous.tick > velocityWindowTicks)
				continue;

			const int tickDelta = current.tick - previous.tick;
			if (tickDelta <= 0 || tickDelta > velocityWindowTicks)
				continue;

			const float deltaSeconds = static_cast<float>(tickDelta) * kServerTickSeconds;
			if (deltaSeconds <= 0.0f)
				continue;

			const Vector segmentVelocity = (current.position - previous.position) / deltaSeconds;
			Vector segmentAngularVelocity(
				AngleDeltaDeg(current.angles.x, previous.angles.x) / deltaSeconds,
				AngleDeltaDeg(current.angles.y, previous.angles.y) / deltaSeconds,
				AngleDeltaDeg(current.angles.z, previous.angles.z) / deltaSeconds);
			if (!ManualThrowIsFiniteVector(segmentVelocity) || !ManualThrowIsFiniteVector(segmentAngularVelocity))
				continue;

			const int ageTicks = (std::max)(0, releaseTick - current.tick);
			const float recencyWeight = 1.0f + static_cast<float>((std::max)(0, velocityWindowTicks - ageTicks));
			linearVelocity += segmentVelocity * recencyWeight;
			angularVelocity += segmentAngularVelocity * recencyWeight;
			totalWeight += recencyWeight;

			const float speedSqr = segmentVelocity.LengthSqr();
			if (std::isfinite(speedSqr) && speedSqr > peakLinearSpeedSqr)
			{
				peakLinearSpeedSqr = speedSqr;
				peakLinearVelocity = segmentVelocity;
			}
		}

		if (totalWeight <= 0.0f)
			return false;

		linearVelocity /= totalWeight;
		angularVelocity /= totalWeight;
		if (peakLinearSpeedSqr >= 0.0f && peakVelocityBlend > 0.0f)
		{
			linearVelocity = linearVelocity * (1.0f - peakVelocityBlend) +
				peakLinearVelocity * peakVelocityBlend;
		}

		return ManualThrowIsFiniteVector(linearVelocity) && ManualThrowIsFiniteVector(angularVelocity);
	}

	static void ManualThrowClampVectorMagnitude(Vector& value, float maximum)
	{
		if (!ManualThrowIsFiniteVector(value))
		{
			value = {};
			return;
		}

		if (maximum <= 0.0f)
		{
			value = {};
			return;
		}

		const float length = value.Length();
		if (std::isfinite(length) && length > maximum && length > 0.0001f)
			value *= maximum / length;
	}

	static bool ManualThrowPreparePending(
		Player& player,
		void* owner,
		void* sourceWeapon,
		int weaponId,
		int releaseTick)
	{
		player.manualThrowPending = {};

		if (!Hooks::m_VR || !Hooks::m_VR->m_ManualThrowEnabled)
			return false;
		const bool projectileThrow = ManualThrowWeaponIdIsThrowable(weaponId);
		const bool carryableThrow = ManualThrowWeaponIdIsCarryable(weaponId);
		if (!owner || !sourceWeapon || (!projectileThrow && !carryableThrow))
			return false;
		if (projectileThrow && !Hooks::s_ManualThrowHooksReady)
			return false;
		if (carryableThrow && !Hooks::s_ManualCarryThrowHookReady)
			return false;

		Vector measuredVelocity{};
		Vector measuredAngularVelocity{};
		ManualThrowEstimateReleaseVelocity(
			player,
			releaseTick,
			Hooks::m_VR->m_ManualThrowVelocityWindowTicks,
			Hooks::m_VR->m_ManualThrowPeakVelocityBlend,
			measuredVelocity,
			measuredAngularVelocity);

		const float overallScale = Hooks::m_VR->m_ManualThrowVelocityScale;
		const float horizontalScale = overallScale * Hooks::m_VR->m_ManualThrowHorizontalVelocityScale;
		const float verticalScale = overallScale * Hooks::m_VR->m_ManualThrowVerticalVelocityScale;
		measuredVelocity.x *= horizontalScale;
		measuredVelocity.y *= horizontalScale;
		measuredVelocity.z *= verticalScale;

		const float horizontalSpeed = std::sqrt(
			measuredVelocity.x * measuredVelocity.x +
			measuredVelocity.y * measuredVelocity.y);
		if (std::isfinite(horizontalSpeed))
			measuredVelocity.z += horizontalSpeed * Hooks::m_VR->m_ManualThrowArcLiftRatio;

		ManualThrowClampVectorMagnitude(measuredVelocity, Hooks::m_VR->m_ManualThrowMaxVelocity);
		ManualThrowClampVectorMagnitude(measuredAngularVelocity, 1440.0f);

		Vector spawnDirection = measuredVelocity;
		if (VectorNormalize(spawnDirection) <= 0.0001f)
		{
			QAngle::AngleVectors(player.controllerAngle, &spawnDirection, nullptr, nullptr);
			if (VectorNormalize(spawnDirection) <= 0.0001f)
				spawnDirection = Vector(1.0f, 0.0f, 0.0f);
		}

		ManualThrowPending pending{};
		pending.valid = true;
		pending.weaponId = weaponId;
		pending.releaseTick = releaseTick;
		pending.owner = owner;
		pending.sourceWeapon = sourceWeapon;
		pending.sourceWeaponVtable = ManualThrowReadEntityVtable(sourceWeapon);
		pending.origin = player.controllerPos + spawnDirection * 5.0f;
		pending.angles = player.controllerAngle;
		pending.velocity = measuredVelocity;
		pending.angularVelocity = measuredAngularVelocity;
		player.manualThrowPending = pending;
		return true;
	}

	static int ManualThrowResolveOwnerPlayerIndex(void* owner)
	{
		if (!owner || !Hooks::m_Game)
			return -1;

		if (Hooks::m_Game->m_CurrentUsercmdPlayer == owner &&
			Hooks::m_Game->IsValidPlayerIndex(Hooks::m_Game->m_CurrentUsercmdID))
		{
			return Hooks::m_Game->m_CurrentUsercmdID;
		}

		if (!Hooks::m_Game->m_Offsets || !Hooks::m_Game->m_Offsets->CBaseEntity_entindex.valid)
			return -1;

		using EntindexFn = int(__thiscall*)(void*);
		auto entindex = reinterpret_cast<EntindexFn>(Hooks::m_Game->m_Offsets->CBaseEntity_entindex.address);
		if (!entindex)
			return -1;

#ifdef _MSC_VER
		__try
		{
			const int playerIndex = entindex(owner);
			return Hooks::m_Game->IsValidPlayerIndex(playerIndex) ? playerIndex : -1;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -1;
		}
#else
		const int playerIndex = entindex(owner);
		return Hooks::m_Game->IsValidPlayerIndex(playerIndex) ? playerIndex : -1;
#endif
	}

	static void* ManualThrowProjectileCreate(
		ManualThrowableKind kind,
		tThrowableProjectileCreate original,
		const Vector& position,
		const QAngle& angles,
		const Vector& velocity,
		const Vector& angularVelocity,
		void* owner)
	{
		if (!original)
			return nullptr;
		if (!Hooks::s_ManualThrowHooksReady || !Hooks::m_VR || !Hooks::m_VR->m_ManualThrowEnabled)
			return original(position, angles, velocity, angularVelocity, owner);

		const int playerIndex = ManualThrowResolveOwnerPlayerIndex(owner);
		if (!Hooks::m_Game || !Hooks::m_Game->IsValidPlayerIndex(playerIndex))
			return original(position, angles, velocity, angularVelocity, owner);

		Player& player = Hooks::m_Game->m_PlayersVRInfo[static_cast<size_t>(playerIndex)];
		ManualThrowPending& pending = player.manualThrowPending;
		const int expectedWeaponId = ManualThrowExpectedWeaponId(kind);
		const int pendingAgeTicks = player.manualThrowLastTick - pending.releaseTick;
		if (!player.isUsingVR || !pending.valid || pending.owner != owner ||
			pending.weaponId != expectedWeaponId || pendingAgeTicks < 0 || pendingAgeTicks > 48)
		{
			if (pending.valid && (pendingAgeTicks < 0 || pendingAgeTicks > 48))
				pending = {};
			return original(position, angles, velocity, angularVelocity, owner);
		}

		const Vector finalPosition = pending.origin;
		const QAngle finalAngles = pending.angles;
		const Vector finalVelocity = pending.velocity;
		const Vector finalAngularVelocity = pending.angularVelocity;
		pending = {};

		Game::logMsg(
			"[VR][ManualThrow] apply type=%s player=%d tick=%d origin=(%.1f %.1f %.1f) originalVel=(%.1f %.1f %.1f) finalVel=(%.1f %.1f %.1f) angular=(%.1f %.1f %.1f)",
			ManualThrowKindName(kind),
			playerIndex,
			player.manualThrowLastTick,
			finalPosition.x, finalPosition.y, finalPosition.z,
			velocity.x, velocity.y, velocity.z,
			finalVelocity.x, finalVelocity.y, finalVelocity.z,
			finalAngularVelocity.x, finalAngularVelocity.y, finalAngularVelocity.z);

		return original(finalPosition, finalAngles, finalVelocity, finalAngularVelocity, owner);
	}

	static bool ManualCarryThrowTeleportDroppedWeapon(
		void* weapon,
		const ManualThrowPending& pending)
	{
		if (!weapon)
			return false;

		constexpr size_t kTeleportVtableSlot = 118;
		void** vtable = nullptr;
#ifdef _MSC_VER
		__try
		{
			vtable = *reinterpret_cast<void***>(weapon);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
		}
#else
		vtable = *reinterpret_cast<void***>(weapon);
#endif
		if (!vtable || !IsReadableMemoryRange(vtable + kTeleportVtableSlot, sizeof(void*)))
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] Teleport resolve failed weapon=%p vtable=%p slot=%u",
				weapon,
				vtable,
				static_cast<unsigned int>(kTeleportVtableSlot));
			return false;
		}

		using TeleportFn = void(__thiscall*)(
			void*,
			const Vector*,
			const QAngle*,
			const Vector*);
		void* teleportTarget = vtable[kTeleportVtableSlot];
		HMODULE serverModule = GetModuleHandleA("server.dll");
		MODULEINFO moduleInfo{};
		const bool targetInServer = serverModule &&
			GetModuleInformation(GetCurrentProcess(), serverModule, &moduleInfo, sizeof(moduleInfo)) &&
			reinterpret_cast<uintptr_t>(teleportTarget) >= reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll) &&
			reinterpret_cast<uintptr_t>(teleportTarget) <
				reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll) + static_cast<uintptr_t>(moduleInfo.SizeOfImage);
		if (!teleportTarget || !targetInServer)
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] Teleport target invalid weapon=%p slot=%u target=%p inServer=%d",
				weapon,
				static_cast<unsigned int>(kTeleportVtableSlot),
				teleportTarget,
				targetInServer ? 1 : 0);
			return false;
		}
		auto teleport = reinterpret_cast<TeleportFn>(teleportTarget);

#ifdef _MSC_VER
		__try
		{
			teleport(weapon, &pending.origin, nullptr, &pending.velocity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] Teleport call exception weapon=%p target=%p",
				weapon,
				teleportTarget);
			return false;
		}
#else
		teleport(weapon, &pending.origin, nullptr, &pending.velocity);
#endif
		return true;
	}

	static bool ManualCarryThrowApplyPendingAfterWeaponDetached(
		int playerIndex,
		void* ownerPlayer,
		void* activeWeaponAfterUsercmd,
		bool activeWeaponReadSucceeded)
	{
		if (!Hooks::m_Game || !Hooks::m_VR || !Hooks::m_VR->m_ManualThrowEnabled ||
			!Hooks::m_Game->IsValidPlayerIndex(playerIndex) || !ownerPlayer)
		{
			return false;
		}

		Player& player = Hooks::m_Game->m_PlayersVRInfo[static_cast<size_t>(playerIndex)];
		ManualThrowPending& pending = player.manualThrowPending;
		if (!activeWeaponReadSucceeded)
		{
			if (pending.valid && ManualThrowWeaponIdIsCarryable(pending.weaponId) &&
				pending.owner == ownerPlayer && !pending.velocityMismatchLogged)
			{
				pending.velocityMismatchLogged = true;
				Game::logMsg(
					"[VR][ManualCarryThrow] active weapon read failed player=%d tick=%d source=%p",
					playerIndex,
					pending.releaseTick,
					pending.sourceWeapon);
			}
			return false;
		}

		if (!player.isUsingVR || !pending.valid ||
			!ManualThrowWeaponIdIsCarryable(pending.weaponId) ||
			pending.owner != ownerPlayer || !pending.sourceWeapon)
		{
			return false;
		}

		const int pendingAgeTicks = player.manualThrowLastTick - pending.releaseTick;
		if (pendingAgeTicks < 0 || pendingAgeTicks > 48)
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] detached pending expired type=%s player=%d releaseTick=%d lastTick=%d age=%d source=%p active=%p",
				ManualCarryThrowWeaponName(pending.weaponId),
				playerIndex,
				pending.releaseTick,
				player.manualThrowLastTick,
				pendingAgeTicks,
				pending.sourceWeapon,
				activeWeaponAfterUsercmd);
			pending = {};
			return false;
		}

		if (activeWeaponAfterUsercmd == pending.sourceWeapon)
		{
			if (!pending.velocityMismatchLogged)
			{
				pending.velocityMismatchLogged = true;
				Game::logMsg(
					"[VR][ManualCarryThrow] waiting for detach type=%s player=%d tick=%d source=%p active=%p",
					ManualCarryThrowWeaponName(pending.weaponId),
					playerIndex,
					pending.releaseTick,
					pending.sourceWeapon,
					activeWeaponAfterUsercmd);
			}
			return false;
		}

		const ManualThrowPending consumed = pending;
		pending = {};
		const bool applied = ManualCarryThrowTeleportDroppedWeapon(
			consumed.sourceWeapon,
			consumed);
		Game::logMsg(
			"[VR][ManualCarryThrow] %s detached type=%s player=%d tick=%d source=%p active=%p finalVel=(%.1f %.1f %.1f) origin=(%.1f %.1f %.1f)",
			applied ? "apply" : "failed to apply",
			ManualCarryThrowWeaponName(consumed.weaponId),
			playerIndex,
			consumed.releaseTick,
			consumed.sourceWeapon,
			activeWeaponAfterUsercmd,
			consumed.velocity.x,
			consumed.velocity.y,
			consumed.velocity.z,
			consumed.origin.x,
			consumed.origin.y,
			consumed.origin.z);
		return applied;
	}

}

void* __cdecl Hooks::dMolotovProjectileCreate(
	const Vector& position,
	const QAngle& angles,
	const Vector& velocity,
	const Vector& angularVelocity,
	void* owner)
{
	return ManualThrowProjectileCreate(
		ManualThrowableKind::Molotov,
		hkMolotovProjectileCreate.fOriginal,
		position,
		angles,
		velocity,
		angularVelocity,
		owner);
}

void* __cdecl Hooks::dPipeBombProjectileCreate(
	const Vector& position,
	const QAngle& angles,
	const Vector& velocity,
	const Vector& angularVelocity,
	void* owner)
{
	return ManualThrowProjectileCreate(
		ManualThrowableKind::PipeBomb,
		hkPipeBombProjectileCreate.fOriginal,
		position,
		angles,
		velocity,
		angularVelocity,
		owner);
}

void* __cdecl Hooks::dVomitJarProjectileCreate(
	const Vector& position,
	const QAngle& angles,
	const Vector& velocity,
	const Vector& angularVelocity,
	void* owner)
{
	return ManualThrowProjectileCreate(
		ManualThrowableKind::VomitJar,
		hkVomitJarProjectileCreate.fOriginal,
		position,
		angles,
		velocity,
		angularVelocity,
		owner);
}
