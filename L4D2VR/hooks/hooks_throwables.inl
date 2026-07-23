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
		case C_WeaponCSBase::WeaponID::PISTOL: return "pistol";
		case C_WeaponCSBase::WeaponID::UZI: return "smg";
		case C_WeaponCSBase::WeaponID::PUMPSHOTGUN: return "pumpshotgun";
		case C_WeaponCSBase::WeaponID::AUTOSHOTGUN: return "autoshotgun";
		case C_WeaponCSBase::WeaponID::M16A1: return "rifle";
		case C_WeaponCSBase::WeaponID::HUNTING_RIFLE: return "hunting_rifle";
		case C_WeaponCSBase::WeaponID::MAC10: return "silenced_smg";
		case C_WeaponCSBase::WeaponID::SHOTGUN_CHROME: return "chrome_shotgun";
		case C_WeaponCSBase::WeaponID::SCAR: return "desert_rifle";
		case C_WeaponCSBase::WeaponID::SNIPER_MILITARY: return "military_sniper";
		case C_WeaponCSBase::WeaponID::SPAS: return "spas";
		case C_WeaponCSBase::WeaponID::FIRST_AID_KIT: return "first_aid_kit";
		case C_WeaponCSBase::WeaponID::PAIN_PILLS: return "pain_pills";
		case C_WeaponCSBase::WeaponID::GASCAN: return "gascan";
		case C_WeaponCSBase::WeaponID::PROPANE_TANK: return "propane_tank";
		case C_WeaponCSBase::WeaponID::OXYGEN_TANK: return "oxygen_tank";
		case C_WeaponCSBase::WeaponID::MELEE: return "melee";
		case C_WeaponCSBase::WeaponID::CHAINSAW: return "chainsaw";
		case C_WeaponCSBase::WeaponID::GRENADE_LAUNCHER: return "grenade_launcher";
		case C_WeaponCSBase::WeaponID::AMMO_PACK: return "ammo_pack";
		case C_WeaponCSBase::WeaponID::ADRENALINE: return "adrenaline";
		case C_WeaponCSBase::WeaponID::DEFIBRILLATOR: return "defibrillator";
		case C_WeaponCSBase::WeaponID::AK47: return "ak47";
		case C_WeaponCSBase::WeaponID::GNOME_CHOMPSKI: return "gnome";
		case C_WeaponCSBase::WeaponID::COLA_BOTTLES: return "cola_bottles";
		case C_WeaponCSBase::WeaponID::FIREWORKS_BOX: return "fireworks_box";
		case C_WeaponCSBase::WeaponID::INCENDIARY_AMMO: return "incendiary_ammo";
		case C_WeaponCSBase::WeaponID::FRAG_AMMO: return "explosive_ammo";
		case C_WeaponCSBase::WeaponID::MAGNUM: return "magnum";
		case C_WeaponCSBase::WeaponID::MP5: return "mp5";
		case C_WeaponCSBase::WeaponID::SG552: return "sg552";
		case C_WeaponCSBase::WeaponID::AWP: return "awp";
		case C_WeaponCSBase::WeaponID::SCOUT: return "scout";
		case C_WeaponCSBase::WeaponID::M60: return "m60";
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
		const bool inventoryDropThrow = ManualInventoryThrowWeaponIdRequiresCustomDrop(weaponId);
		if (!owner || !sourceWeapon || (!projectileThrow && !carryableThrow && !inventoryDropThrow))
			return false;
		if (projectileThrow && !Hooks::s_ManualThrowHooksReady)
			return false;
		if (carryableThrow && !ManualCarryThrowBackendIsReady(weaponId))
			return false;
		if (inventoryDropThrow && !ManualInventoryThrowBackendIsReady(weaponId))
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
		pending.inventoryDrop = inventoryDropThrow;
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

	static bool ManualCarryThrowTeleportDroppedEntity(
		void* entity,
		const ManualThrowPending& pending)
	{
		if (!entity)
			return false;

		constexpr size_t kTeleportVtableSlot = 118;
		void** vtable = nullptr;
#ifdef _MSC_VER
		__try
		{
			vtable = *reinterpret_cast<void***>(entity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
		}
#else
		vtable = *reinterpret_cast<void***>(entity);
#endif
		if (!vtable || !IsReadableMemoryRange(vtable + kTeleportVtableSlot, sizeof(void*)))
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] Teleport resolve failed entity=%p vtable=%p slot=%u",
				entity,
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
				"[VR][ManualCarryThrow] Teleport target invalid entity=%p slot=%u target=%p inServer=%d",
				entity,
				static_cast<unsigned int>(kTeleportVtableSlot),
				teleportTarget,
				targetInServer ? 1 : 0);
			return false;
		}
		auto teleport = reinterpret_cast<TeleportFn>(teleportTarget);

#ifdef _MSC_VER
		__try
		{
			teleport(entity, &pending.origin, nullptr, &pending.velocity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Game::logMsg(
				"[VR][ManualCarryThrow] Teleport call exception entity=%p target=%p",
				entity,
				teleportTarget);
			return false;
		}
#else
		teleport(entity, &pending.origin, nullptr, &pending.velocity);
#endif
		return true;
	}

	enum class ManualCarryImpactTargetKind
	{
		None,
		CommonInfected,
		SpecialInfected,
		Witch,
		InfectedFallback,
	};

	struct ManualCarryImpactTrack
	{
		bool active = false;
		void* entity = nullptr;
		void* entityVtable = nullptr;
		void* owner = nullptr;
		int weaponId = 0;
		Vector previousOrigin{};
		Vector launchDirection{};
		float launchSpeed = 0.0f;
		float knockbackFraction = 0.25f;
		bool meleeDamageImpact = false;
		int maxPenetrations = 1;
		int penetrationCount = 0;
		std::array<void*, 6> hitTargets{};
		bool collisionPending = false;
		void* collisionTarget = nullptr;
		void* collisionTargetVtable = nullptr;
		Vector collisionVelocity{};
		float collisionSpeed = 0.0f;
		std::chrono::steady_clock::time_point armedAt{};
	};

	static std::array<ManualCarryImpactTrack, 16> s_ManualCarryImpactTracks{};

	// x86 gamevcollisionevent_t layout from server physics.h. The base
	// vcollisionevent_t is 0x20 bytes; L4D2 then appends three pairs of vectors
	// and finally the two CBaseEntity pointers at offset 0x68.
	struct ManualCarryGameVCollisionEvent
	{
		void* physicsObjects[2];
		int surfaceProps[2];
		bool isCollision;
		bool isShadowCollision;
		unsigned char alignmentPadding[2];
		float deltaCollisionTime;
		float collisionSpeed;
		void* internalData;
		Vector preVelocity[2];
		Vector postVelocity[2];
		Vector preAngularVelocity[2];
		void* entities[2];
	};
	static_assert(offsetof(ManualCarryGameVCollisionEvent, entities) == 0x68,
		"Unexpected gamevcollisionevent_t layout");

	static const char* ManualCarryImpactTargetKindName(ManualCarryImpactTargetKind kind)
	{
		switch (kind)
		{
		case ManualCarryImpactTargetKind::CommonInfected: return "common";
		case ManualCarryImpactTargetKind::SpecialInfected: return "special";
		case ManualCarryImpactTargetKind::Witch: return "witch";
		case ManualCarryImpactTargetKind::InfectedFallback: return "infected";
		default: return "none";
		}
	}

	struct ManualCarrySpawnedPhysicsProp
	{
		bool valid = false;
		void* sourceWeapon = nullptr;
		void* sourceWeaponVtable = nullptr;
		void* spawnedEntity = nullptr;
		void* spawnedEntityVtable = nullptr;
		int weaponId = static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		std::chrono::steady_clock::time_point createdAt{};
	};

	static std::array<ManualCarrySpawnedPhysicsProp, 16> s_ManualCarrySpawnedPhysicsProps{};

	static int ManualCarryThrowReadServerWeaponId(void* sourceWeapon)
	{
		if (!sourceWeapon)
			return static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
#ifdef _MSC_VER
		__try
		{
#endif
			return reinterpret_cast<Server_WeaponCSBase*>(sourceWeapon)->GetWeaponID();
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return static_cast<int>(C_WeaponCSBase::WeaponID::NONE);
		}
#endif
	}

	static void ManualCarryThrowCacheSpawnedPhysicsProp(
		void* sourceWeapon,
		void* spawnedEntity,
		int weaponId,
		const char* capturePath)
	{
		if (!sourceWeapon || !spawnedEntity ||
			!ManualCarryThrowWeaponIdUsesSpawnedPhysicsProp(weaponId))
		{
			return;
		}

		ManualCarrySpawnedPhysicsProp* selected = nullptr;
		for (ManualCarrySpawnedPhysicsProp& candidate : s_ManualCarrySpawnedPhysicsProps)
		{
			if (candidate.valid && candidate.sourceWeapon == sourceWeapon)
			{
				selected = &candidate;
				break;
			}
			if (!candidate.valid && !selected)
				selected = &candidate;
		}
		if (!selected)
		{
			selected = &*std::min_element(
				s_ManualCarrySpawnedPhysicsProps.begin(),
				s_ManualCarrySpawnedPhysicsProps.end(),
				[](const ManualCarrySpawnedPhysicsProp& lhs, const ManualCarrySpawnedPhysicsProp& rhs)
				{
					return lhs.createdAt < rhs.createdAt;
				});
		}

		*selected = {};
		selected->valid = true;
		selected->sourceWeapon = sourceWeapon;
		selected->sourceWeaponVtable = ManualThrowReadEntityVtable(sourceWeapon);
		selected->spawnedEntity = spawnedEntity;
		selected->spawnedEntityVtable = ManualThrowReadEntityVtable(spawnedEntity);
		selected->weaponId = weaponId;
		selected->createdAt = std::chrono::steady_clock::now();
		Game::logMsg(
			"[VR][ManualCarryThrow] cached pre-release physics_prop path=%s type=%s source=%p dropped=%p",
			capturePath ? capturePath : "unknown",
			ManualCarryThrowWeaponName(weaponId),
			sourceWeapon,
			spawnedEntity);
	}

	static void* ManualCarryThrowClaimSpawnedPhysicsProp(
		void* sourceWeapon,
		int weaponId)
	{
		if (!sourceWeapon || !ManualCarryThrowWeaponIdUsesSpawnedPhysicsProp(weaponId))
			return nullptr;

		const auto now = std::chrono::steady_clock::now();
		constexpr float kMaximumCandidateAgeSeconds = 20.0f;
		ManualCarrySpawnedPhysicsProp* selected = nullptr;
		for (ManualCarrySpawnedPhysicsProp& candidate : s_ManualCarrySpawnedPhysicsProps)
		{
			if (!candidate.valid)
				continue;
			const float age = std::chrono::duration<float>(now - candidate.createdAt).count();
			if (age < 0.0f || age > kMaximumCandidateAgeSeconds ||
				ManualThrowReadEntityVtable(candidate.spawnedEntity) != candidate.spawnedEntityVtable)
			{
				candidate = {};
				continue;
			}
			if (candidate.sourceWeapon == sourceWeapon && candidate.weaponId == weaponId &&
				(!selected || candidate.createdAt > selected->createdAt))
			{
				selected = &candidate;
			}
		}
		if (!selected)
			return nullptr;

		void* spawnedEntity = selected->spawnedEntity;
		const float age = std::chrono::duration<float>(now - selected->createdAt).count();
		Game::logMsg(
			"[VR][ManualCarryThrow] claimed cached physics_prop type=%s source=%p dropped=%p age=%.3f",
			ManualCarryThrowWeaponName(weaponId),
			sourceWeapon,
			spawnedEntity,
			age);
		*selected = {};
		return spawnedEntity;
	}

	static bool ManualCarryImpactReadServerClassName(
		void* entity,
		char* output,
		size_t outputCapacity)
	{
		if (!output || outputCapacity == 0)
			return false;
		output[0] = '\0';
		if (!entity)
			return false;

#ifdef _MSC_VER
		__try
		{
#endif
			IServerUnknown* unknown = reinterpret_cast<IServerUnknown*>(entity);
			void* networkable = unknown->GetNetworkable();
			if (!networkable)
				return false;

			void** vtable = *reinterpret_cast<void***>(networkable);
			constexpr size_t kGetClassNameVtableSlot = 3;
			if (!vtable || !IsReadableMemoryRange(vtable + kGetClassNameVtableSlot, sizeof(void*)))
				return false;

			using GetClassNameFn = const char* (__thiscall*)(void*);
			auto getClassName = reinterpret_cast<GetClassNameFn>(vtable[kGetClassNameVtableSlot]);
			if (!getClassName)
				return false;

			const char* source = getClassName(networkable);
			if (!source)
				return false;

			size_t length = 0;
			for (; length + 1 < outputCapacity; ++length)
			{
				if (!IsReadableMemoryRange(source + length, sizeof(char)))
					break;
				const char value = source[length];
				output[length] = value;
				if (value == '\0')
					return length > 0;
			}
			output[length] = '\0';
			return length > 0;
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			output[0] = '\0';
			return false;
		}
#endif
	}

	static bool ManualCarryImpactReadTargetState(void* entity, int& team, unsigned char& lifeState)
	{
		// These are server.dll CBaseEntity offsets, not the similarly named client
		// recv-table offsets in VR.  The current server's DT_BaseEntity send table
		// registers m_iTeamNum at 0x238, while the CBaseEntity data map registers
		// m_lifeState at 0xF0.  Reading the client offsets here caused every real
		// infected collision to be rejected before it could be queued.
		constexpr size_t kServerTeamNumOffset = 0x238;
		constexpr size_t kServerLifeStateOffset = 0xF0;

		team = 0;
		lifeState = 1;
		if (!entity)
			return false;

		const unsigned char* base = reinterpret_cast<const unsigned char*>(entity);
		if (!IsReadableMemoryRange(base + kServerTeamNumOffset, sizeof(team)) ||
			!IsReadableMemoryRange(base + kServerLifeStateOffset, sizeof(lifeState)))
		{
			return false;
		}

#ifdef _MSC_VER
		__try
		{
#endif
			team = *reinterpret_cast<const int*>(base + kServerTeamNumOffset);
			lifeState = *reinterpret_cast<const unsigned char*>(base + kServerLifeStateOffset);
			return true;
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			team = 0;
			lifeState = 1;
			return false;
		}
#endif
	}

	class ManualCarryImpactInfectedTraceFilter final : public CTraceFilter
	{
	public:
		ManualCarryImpactInfectedTraceFilter(
			IHandleEntity* thrownEntity,
			IHandleEntity* owner,
			const ManualCarryImpactTrack* track)
			: CTraceFilter(thrownEntity, 0)
			, m_Owner(owner)
			, m_Track(track)
		{
		}

		bool ShouldHitEntity(IHandleEntity* candidate, int contentsMask) override
		{
			(void)contentsMask;
			if (!candidate || candidate == m_pPassEnt || candidate == m_Owner)
				return false;
			if (m_Track)
			{
				for (void* hitTarget : m_Track->hitTargets)
				{
					if (hitTarget == candidate)
						return false;
				}
			}

			int team = 0;
			unsigned char lifeState = 1;
			return ManualCarryImpactReadTargetState(candidate, team, lifeState) &&
				team == 3 && lifeState == 0;
		}

		TraceType GetTraceType() const override
		{
			// World geometry and unrelated props must not hide an infected that the
			// carried prop physically overlaps during this movement segment.
			return TraceType::TRACE_ENTITIES_ONLY;
		}

	private:
		IHandleEntity* m_Owner = nullptr;
		const ManualCarryImpactTrack* m_Track = nullptr;
	};

	class ManualCarryImpactWorldOnlyTraceFilter final : public CTraceFilter
	{
	public:
		ManualCarryImpactWorldOnlyTraceFilter()
			: CTraceFilter(nullptr, 0)
		{
		}

		bool ShouldHitEntity(IHandleEntity*, int) override
		{
			return false;
		}

		TraceType GetTraceType() const override
		{
			return TraceType::TRACE_WORLD_ONLY;
		}
	};

	// IEngineTrace::EnumerateEntities uses this single-method callback ABI.
	// Keep it local so the existing compact trace SDK declaration does not need
	// to grow solely for manual carry impact handling.
	class ManualCarryImpactEntityCollector
	{
	public:
		virtual bool EnumEntity(IHandleEntity* candidate)
		{
			if (!candidate)
				return true;
			void* entity = candidate;
			for (size_t i = 0; i < count; ++i)
			{
				if (entities[i] == entity)
					return true;
			}
			if (count < entities.size())
				entities[count++] = entity;
			return true;
		}

		std::array<void*, 128> entities{};
		size_t count = 0;
	};

	static ManualCarryImpactTargetKind ManualCarryImpactClassifyTarget(
		void* entity,
		char* className,
		size_t classNameCapacity)
	{
		int team = 0;
		unsigned char lifeState = 1;
		if (!ManualCarryImpactReadTargetState(entity, team, lifeState) || team != 3 || lifeState != 0)
			return ManualCarryImpactTargetKind::None;

		const bool hasClassName = ManualCarryImpactReadServerClassName(
			entity,
			className,
			classNameCapacity);
		if (!hasClassName)
			return ManualCarryImpactTargetKind::InfectedFallback;

		std::string lowered(className);
		std::transform(
			lowered.begin(),
			lowered.end(),
			lowered.begin(),
			[](unsigned char value) { return static_cast<char>(std::tolower(value)); });

		if (lowered.find("witch") != std::string::npos)
			return ManualCarryImpactTargetKind::Witch;
		if (lowered.find("infected") != std::string::npos)
			return ManualCarryImpactTargetKind::CommonInfected;
		if (lowered.find("player") != std::string::npos ||
			lowered.find("terror") != std::string::npos)
		{
			return ManualCarryImpactTargetKind::SpecialInfected;
		}

		// A readable, known non-infected classname is a stronger signal than the
		// team netvar alone and prevents accidental impulses on unusual team-3 entities.
		return ManualCarryImpactTargetKind::None;
	}

	static bool ManualCarryImpactReadOrigin(void* entity, Vector& origin)
	{
		origin = {};
		if (!entity || !Hooks::m_Game || !Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.valid)
		{
			return false;
		}

		using GetAbsOriginFn = Vector* (__thiscall*)(void*);
		auto getAbsOrigin = reinterpret_cast<GetAbsOriginFn>(
			Hooks::m_Game->m_Offsets->CBaseEntity_GetAbsOrigin_Server.address);
#ifdef _MSC_VER
		__try
		{
#endif
			Vector* source = getAbsOrigin(entity);
			if (!source || !IsReadableMemoryRange(source, sizeof(Vector)))
				return false;
			origin = *source;
			return ManualThrowIsFiniteVector(origin);
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			origin = {};
			return false;
		}
#endif
	}

	static bool ManualCarryImpactEnumerateArea(
		const Vector& minimums,
		const Vector& maximums,
		ManualCarryImpactEntityCollector& collector)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_EngineTraceServer)
			return false;

		constexpr size_t kEnumerateEntitiesBoxVtableSlot = 11;
		void** vtable = nullptr;
#ifdef _MSC_VER
		__try
		{
			vtable = *reinterpret_cast<void***>(Hooks::m_Game->m_EngineTraceServer);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			vtable = nullptr;
		}
#else
		vtable = *reinterpret_cast<void***>(Hooks::m_Game->m_EngineTraceServer);
#endif
		if (!vtable || !IsReadableMemoryRange(
			vtable + kEnumerateEntitiesBoxVtableSlot,
			sizeof(void*)))
		{
			return false;
		}

		void* target = vtable[kEnumerateEntitiesBoxVtableSlot];
		HMODULE engineModule = GetModuleHandleA("engine.dll");
		MODULEINFO moduleInfo{};
		const bool targetInEngine = engineModule && target &&
			GetModuleInformation(GetCurrentProcess(), engineModule, &moduleInfo, sizeof(moduleInfo)) &&
			reinterpret_cast<uintptr_t>(target) >= reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll) &&
			reinterpret_cast<uintptr_t>(target) <
				reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll) +
				static_cast<uintptr_t>(moduleInfo.SizeOfImage);
		if (!targetInEngine)
			return false;

		using EnumerateEntitiesBoxFn = void(__thiscall*)(
			void*,
			const Vector&,
			const Vector&,
			ManualCarryImpactEntityCollector*);
		auto enumerate = reinterpret_cast<EnumerateEntitiesBoxFn>(target);
#ifdef _MSC_VER
		__try
		{
			enumerate(Hooks::m_Game->m_EngineTraceServer, minimums, maximums, &collector);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		enumerate(Hooks::m_Game->m_EngineTraceServer, minimums, maximums, &collector);
		return true;
#endif
	}

	static bool ManualCarryImpactHasWorldLineOfSight(
		const Vector& impactOrigin,
		const Vector& targetOrigin)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_EngineTraceServer)
			return false;

		Ray_t ray;
		ray.Init(impactOrigin, targetOrigin + Vector(0.0f, 0.0f, 32.0f));
		ManualCarryImpactWorldOnlyTraceFilter worldFilter;
		CGameTrace trace{};
		TraceRoomscaleServerRay(
			Hooks::m_Game->m_EngineTraceServer,
			ray,
			MASK_NPCWORLDSTATIC | CONTENTS_PLAYERCLIP,
			&worldFilter,
			trace);
		return !trace.startsolid && !trace.allsolid && trace.fraction >= 0.999f;
	}

	static bool ManualCarryImpactApplyNativeShove(
		void* target,
		ManualCarryImpactTargetKind targetKind,
		void* pusher,
		const Vector& shoveDirection)
	{
		if (!target || !pusher || !Hooks::m_Game || !Hooks::m_Game->m_Offsets)
			return false;

#ifdef _MSC_VER
		__try
		{
#endif
			if (targetKind == ManualCarryImpactTargetKind::SpecialInfected)
			{
				using OnShovedBySurvivorFn = void(__thiscall*)(
					void*,
					void*,
					const Vector&);
				auto onShoved = reinterpret_cast<OnShovedBySurvivorFn>(
					Hooks::m_Game->m_Offsets->CTerrorPlayer_OnShovedBySurvivor_Server.address);
				if (!onShoved)
					return false;
				Vector nativeDirection = shoveDirection * 100.0f;
				onShoved(target, pusher, nativeDirection);
				return true;
			}

			using RTDynamicCastFn = void* (__cdecl*)(void*, int, void*, void*, int);
			auto runtimeCast = reinterpret_cast<RTDynamicCastFn>(
				Hooks::m_Game->m_Offsets->Server_RTDynamicCast.address);
			if (!runtimeCast)
				return false;

			void* nextBot = runtimeCast(
				target,
				0,
				reinterpret_cast<void*>(Hooks::m_Game->m_Offsets->Server_RTTI_CBaseEntity.address),
				reinterpret_cast<void*>(Hooks::m_Game->m_Offsets->Server_RTTI_INextBot.address),
				0);
			if (!nextBot)
				return false;

			// Call the RTTI-verified L4D2 propagation routine directly. This is the
			// one-argument slot-28 event; the public Source SDK's slot-35 layout does
			// not match L4D2 and calling that slot corrupts the x86 caller stack.
			using NextBotOnShovedFn = void(__thiscall*)(void*, void*);
			auto onShoved = reinterpret_cast<NextBotOnShovedFn>(
				Hooks::m_Game->m_Offsets->INextBotEventResponder_OnShoved_Server.address);
			if (!onShoved)
				return false;
			onShoved(nextBot, pusher);
			return true;
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#endif
	}

	static int ManualCarryImpactApplyNativeAreaShove(
		ManualCarryImpactTrack& track,
		void* primaryTarget,
		const Vector& impactOrigin,
		const char* detectionPath,
		float& effectRadius,
		int& commonCount,
		int& specialCount,
		int& witchCount)
	{
		effectRadius = 48.0f + 80.0f * std::clamp(track.knockbackFraction, 0.25f, 1.0f);
		commonCount = 0;
		specialCount = 0;
		witchCount = 0;

		ManualCarryImpactEntityCollector collector;
		collector.EnumEntity(reinterpret_cast<IHandleEntity*>(primaryTarget));
		const Vector extent(effectRadius, effectRadius, effectRadius);
		const bool enumerated = ManualCarryImpactEnumerateArea(
			impactOrigin - extent,
			impactOrigin + extent,
			collector);

		int appliedCount = 0;
		for (size_t i = 0; i < collector.count; ++i)
		{
			void* candidate = collector.entities[i];
			if (!candidate || candidate == track.entity || candidate == track.owner)
				continue;

			char className[64]{};
			const ManualCarryImpactTargetKind kind = ManualCarryImpactClassifyTarget(
				candidate,
				className,
				sizeof(className));
			if (kind == ManualCarryImpactTargetKind::None)
				continue;

			Vector targetOrigin{};
			if (!ManualCarryImpactReadOrigin(candidate, targetOrigin))
				continue;
			const float distance = (targetOrigin - impactOrigin).Length();
			if (!std::isfinite(distance) || distance > effectRadius)
				continue;
			if (candidate != primaryTarget &&
				!ManualCarryImpactHasWorldLineOfSight(impactOrigin, targetOrigin))
			{
				continue;
			}

			Vector shoveDirection = targetOrigin - impactOrigin;
			shoveDirection.z = 0.0f;
			if (VectorNormalize(shoveDirection) <= 0.0001f)
			{
				shoveDirection = track.launchDirection;
				shoveDirection.z = 0.0f;
				if (VectorNormalize(shoveDirection) <= 0.0001f)
					shoveDirection = Vector(1.0f, 0.0f, 0.0f);
			}

			if (!ManualCarryImpactApplyNativeShove(
				candidate,
				kind,
				track.owner,
				shoveDirection))
			{
				continue;
			}

			++appliedCount;
			switch (kind)
			{
			case ManualCarryImpactTargetKind::CommonInfected:
			case ManualCarryImpactTargetKind::InfectedFallback:
				++commonCount;
				break;
			case ManualCarryImpactTargetKind::SpecialInfected:
				++specialCount;
				break;
			case ManualCarryImpactTargetKind::Witch:
				++witchCount;
				break;
			default:
				break;
			}
		}

		Game::logMsg(
			"[VR][ManualCarryImpact] native shove area path=%s type=%s origin=(%.1f %.1f %.1f) strength=%.0f%% radius=%.1f enumerated=%d candidates=%u applied=%d common=%d special=%d witch=%d",
			detectionPath ? detectionPath : "unknown",
			ManualCarryThrowWeaponName(track.weaponId),
			impactOrigin.x,
			impactOrigin.y,
			impactOrigin.z,
			track.knockbackFraction * 100.0f,
			effectRadius,
			enumerated ? 1 : 0,
			static_cast<unsigned int>(collector.count),
			appliedCount,
			commonCount,
			specialCount,
			witchCount);
		return appliedCount;
	}

	static bool ManualCarryImpactResolveSweptPropContact(
		const ManualCarryImpactTrack& track,
		const Vector& contactOrigin)
	{
		if (!track.entity || !ManualThrowIsFiniteVector(contactOrigin))
			return false;

		Vector impactDirection = track.launchDirection;
		if (VectorNormalize(impactDirection) <= 0.0001f)
			impactDirection = Vector(1.0f, 0.0f, 0.0f);

		// These carry props do not normally collide with infected.  Place the prop
		// at the swept-hull contact point and give it a small rebound so the visual
		// result matches the synthetic hit instead of letting it pass through.
		ManualThrowPending contactMove{};
		contactMove.origin = contactOrigin - impactDirection * 2.0f;
		const float reboundSpeed = std::clamp(track.launchSpeed * 0.18f, 30.0f, 180.0f);
		contactMove.velocity = impactDirection * -reboundSpeed;
		contactMove.velocity.z = (std::max)(contactMove.velocity.z, 30.0f);
		return ManualCarryThrowTeleportDroppedEntity(track.entity, contactMove);
	}

	static bool ManualInventoryImpactContinueThrough(
		ManualCarryImpactTrack& track,
		const Vector& contactOrigin)
	{
		Vector direction = track.launchDirection;
		if (VectorNormalize(direction) <= 0.0001f)
			direction = Vector(1.0f, 0.0f, 0.0f);

		ManualThrowPending continuation{};
		continuation.origin = contactOrigin + direction * 28.0f;
		continuation.velocity = direction * (std::max)(track.launchSpeed, 120.0f);
		const bool moved = ManualCarryThrowTeleportDroppedEntity(track.entity, continuation);
		if (moved)
			track.previousOrigin = continuation.origin;
		return moved;
	}

	static bool ManualInventoryImpactHasHitTarget(
		const ManualCarryImpactTrack& track,
		void* target)
	{
		return std::find(track.hitTargets.begin(), track.hitTargets.end(), target) !=
			track.hitTargets.end();
	}

	static bool ManualInventoryImpactApplyMeleeDamage(
		ManualCarryImpactTrack& track,
		void* target,
		const Vector& impactOrigin,
		const char* detectionPath)
	{
		if (!target || !track.owner || ManualInventoryImpactHasHitTarget(track, target) ||
			!Hooks::m_Game || !Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->CTakeDamageInfoCtor_Server.valid ||
			!Hooks::m_Game->m_Offsets->CBaseEntity_TakeDamage_Server.valid)
		{
			return false;
		}

		// L4D2's CTakeDamageInfo is 0x60 bytes. Construct it with the game's own
		// constructor so EHANDLE fields and private defaults stay ABI-correct, then
		// call the non-virtual CBaseEntity::TakeDamage wrapper. A full melee hit is
		// 250 base damage; throw strength controls penetration count, not damage.
		alignas(16) std::array<unsigned char, 0x60> damageInfo{};
		using DamageInfoCtorFn = void(__thiscall*)(void*, void*, void*, float, int, int);
		using TakeDamageFn = int(__thiscall*)(void*, const void*);
		auto constructDamageInfo = reinterpret_cast<DamageInfoCtorFn>(
			Hooks::m_Game->m_Offsets->CTakeDamageInfoCtor_Server.address);
		auto takeDamage = reinterpret_cast<TakeDamageFn>(
			Hooks::m_Game->m_Offsets->CBaseEntity_TakeDamage_Server.address);
		if (!constructDamageInfo || !takeDamage)
			return false;

		constexpr float kFullMeleeDamage = 250.0f;
		constexpr int kDamageClub = (1 << 7);
		int result = 0;
#ifdef _MSC_VER
		__try
		{
#endif
			constructDamageInfo(
				damageInfo.data(),
				track.owner,
				track.owner,
				kFullMeleeDamage,
				kDamageClub,
				0);
			Vector damageForce = track.launchDirection * (kFullMeleeDamage * 8.0f);
			*reinterpret_cast<Vector*>(damageInfo.data() + 0x00) = damageForce;
			*reinterpret_cast<Vector*>(damageInfo.data() + 0x0C) = impactOrigin;
			*reinterpret_cast<Vector*>(damageInfo.data() + 0x18) = impactOrigin;
			result = takeDamage(target, damageInfo.data());
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Game::logMsg(
				"[VR][ManualInventoryImpact] damage exception path=%s type=%s target=%p",
				detectionPath ? detectionPath : "unknown",
				ManualCarryThrowWeaponName(track.weaponId),
				target);
			return false;
		}
#endif

		if (track.penetrationCount < static_cast<int>(track.hitTargets.size()))
			track.hitTargets[static_cast<size_t>(track.penetrationCount)] = target;
		++track.penetrationCount;
		Game::logMsg(
			"[VR][ManualInventoryImpact] melee-damage path=%s type=%s target=%p damage=%.0f result=%d penetration=%d/%d",
			detectionPath ? detectionPath : "unknown",
			ManualCarryThrowWeaponName(track.weaponId),
			target,
			kFullMeleeDamage,
			result,
			track.penetrationCount,
			track.maxPenetrations);
		return true;
	}

	static bool ManualCarryImpactApplyTrackedTarget(
		ManualCarryImpactTrack& track,
		void* target,
		const Vector* contactVelocity,
		const Vector* contactOrigin,
		const char* detectionPath)
	{
		(void)contactVelocity;
		if (!target || target == track.entity || target == track.owner)
			return false;

		char className[64]{};
		const ManualCarryImpactTargetKind targetKind = ManualCarryImpactClassifyTarget(
			target,
			className,
			sizeof(className));
		if (targetKind == ManualCarryImpactTargetKind::None)
			return false;

		Vector areaOrigin{};
		if (contactOrigin && ManualThrowIsFiniteVector(*contactOrigin))
			areaOrigin = *contactOrigin;
		else if (!ManualCarryImpactReadOrigin(target, areaOrigin))
			areaOrigin = track.previousOrigin;

		if (track.meleeDamageImpact)
		{
			return ManualInventoryImpactApplyMeleeDamage(
				track,
				target,
				areaOrigin,
				detectionPath);
		}

		float effectRadius = 0.0f;
		int commonCount = 0;
		int specialCount = 0;
		int witchCount = 0;
		const int appliedCount = ManualCarryImpactApplyNativeAreaShove(
			track,
			target,
			areaOrigin,
			detectionPath,
			effectRadius,
			commonCount,
			specialCount,
			witchCount);

		Game::logMsg(
			"[VR][ManualCarryImpact] %s path=%s type=%s directTarget=%s class=%s entity=%p targetEntity=%p speed=%.1f collisionSpeed=%.1f strength=%.0f%% radius=%.1f affected=%d",
			appliedCount > 0 ? "native-shove" : "native-shove-failed",
			detectionPath ? detectionPath : "unknown",
			ManualCarryThrowWeaponName(track.weaponId),
			ManualCarryImpactTargetKindName(targetKind),
			className[0] ? className : "<unknown>",
			track.entity,
			target,
			track.launchSpeed,
			track.collisionSpeed,
			track.knockbackFraction * 100.0f,
			effectRadius,
			appliedCount);
		return appliedCount > 0;
	}

	static void ManualCarryImpactQueuePhysicsCollision(
		void* entity,
		int index,
		void* collisionEvent)
	{
		if (!Hooks::s_ManualCarryImpactKnockbackReady || !entity ||
			(index != 0 && index != 1) || !collisionEvent ||
			!IsReadableMemoryRange(collisionEvent, sizeof(ManualCarryGameVCollisionEvent)))
		{
			return;
		}

		ManualCarryImpactTrack* matchingTrack = nullptr;
		for (ManualCarryImpactTrack& track : s_ManualCarryImpactTracks)
		{
			if (track.active && track.entity == entity)
			{
				matchingTrack = &track;
				break;
			}
		}
		if (!matchingTrack || matchingTrack->collisionPending)
			return;

		const ManualCarryGameVCollisionEvent* event =
			reinterpret_cast<const ManualCarryGameVCollisionEvent*>(collisionEvent);
		void* target = event->entities[index == 0 ? 1 : 0];
		if (!target || target == matchingTrack->entity || target == matchingTrack->owner)
			return;
		if (matchingTrack->meleeDamageImpact &&
			ManualInventoryImpactHasHitTarget(*matchingTrack, target))
		{
			return;
		}

		int team = 0;
		unsigned char lifeState = 1;
		if (!ManualCarryImpactReadTargetState(target, team, lifeState) ||
			team != 3 || lifeState != 0)
		{
			return;
		}

		matchingTrack->collisionPending = true;
		matchingTrack->collisionTarget = target;
		matchingTrack->collisionTargetVtable = ManualThrowReadEntityVtable(target);
		matchingTrack->collisionVelocity = event->preVelocity[index];
		matchingTrack->collisionSpeed = std::isfinite(event->collisionSpeed)
			? event->collisionSpeed
			: matchingTrack->collisionVelocity.Length();
		Game::logMsg(
			"[VR][ManualCarryImpact] collision queued type=%s entity=%p targetEntity=%p index=%d collisionSpeed=%.1f preVel=(%.1f %.1f %.1f)",
			ManualCarryThrowWeaponName(matchingTrack->weaponId),
			entity,
			target,
			index,
			matchingTrack->collisionSpeed,
			matchingTrack->collisionVelocity.x,
			matchingTrack->collisionVelocity.y,
			matchingTrack->collisionVelocity.z);
	}

	static void ManualCarryImpactArm(void* entity, const ManualThrowPending& pending)
	{
		if (!Hooks::s_ManualCarryImpactKnockbackReady || !entity || !Hooks::m_VR)
			return;

		const float launchSpeed = pending.velocity.Length();
		if (!std::isfinite(launchSpeed))
			return;

		const float maximumThrowSpeed = (std::max)(1.0f, Hooks::m_VR->m_ManualThrowMaxVelocity);
		const float normalizedStrength = std::clamp(launchSpeed / maximumThrowSpeed, 0.0f, 1.0f);
		const float knockbackFraction = 0.25f + normalizedStrength * 0.75f;

		Vector launchDirection = pending.velocity;
		if (VectorNormalize(launchDirection) <= 0.0001f)
		{
			QAngle::AngleVectors(pending.angles, &launchDirection, nullptr, nullptr);
			if (VectorNormalize(launchDirection) <= 0.0001f)
				launchDirection = Vector(1.0f, 0.0f, 0.0f);
		}

		ManualCarryImpactTrack* selected = nullptr;
		for (ManualCarryImpactTrack& track : s_ManualCarryImpactTracks)
		{
			if (track.active && track.entity == entity)
			{
				selected = &track;
				break;
			}
			if (!track.active && !selected)
				selected = &track;
		}
		if (!selected)
		{
			selected = &*std::min_element(
				s_ManualCarryImpactTracks.begin(),
				s_ManualCarryImpactTracks.end(),
				[](const ManualCarryImpactTrack& lhs, const ManualCarryImpactTrack& rhs)
				{
					return lhs.armedAt < rhs.armedAt;
				});
		}

		*selected = {};
		selected->active = true;
		selected->entity = entity;
		selected->entityVtable = ManualThrowReadEntityVtable(entity);
		selected->owner = pending.owner;
		selected->weaponId = pending.weaponId;
		selected->previousOrigin = pending.origin;
		selected->launchDirection = launchDirection;
		selected->launchSpeed = launchSpeed;
		selected->knockbackFraction = knockbackFraction;
		selected->meleeDamageImpact = ManualInventoryThrowWeaponIdDoesMeleeDamage(pending.weaponId);
		selected->maxPenetrations = selected->meleeDamageImpact
			? std::clamp(1 + static_cast<int>(std::floor(normalizedStrength * 5.0f + 0.0001f)), 1, 6)
			: 1;
		selected->armedAt = std::chrono::steady_clock::now();

		Game::logMsg(
			"[VR][ManualCarryImpact] armed type=%s entity=%p speed=%.1f strength=%.0f%% damage=%d penetrations=%d",
			ManualCarryThrowWeaponName(pending.weaponId),
			entity,
			launchSpeed,
			knockbackFraction * 100.0f,
			selected->meleeDamageImpact ? 1 : 0,
			selected->maxPenetrations);
	}

	static void ManualCarryImpactUpdate()
	{
		if (!Hooks::s_ManualCarryImpactKnockbackReady || !Hooks::m_Game ||
			!Hooks::m_Game->m_EngineTraceServer)
		{
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		constexpr float kTrackLifetimeSeconds = 4.0f;
		constexpr float kMinimumTraceDistance = 0.25f;
		constexpr float kMaximumTraceDistance = 512.0f;
		// Approximate the carried objects' collision volume rather than tracing only
		// their center. This is deliberately a little generous for the fireworks box.
		const Vector traceMins(-18.0f, -18.0f, -18.0f);
		const Vector traceMaxs(18.0f, 18.0f, 18.0f);

		for (ManualCarryImpactTrack& track : s_ManualCarryImpactTracks)
		{
			if (!track.active)
				continue;
			const float trackAge = std::chrono::duration<float>(now - track.armedAt).count();
			if (trackAge > kTrackLifetimeSeconds)
			{
				Game::logMsg(
					"[VR][ManualCarryImpact] expired type=%s entity=%p speed=%.1f without infected hit",
					ManualCarryThrowWeaponName(track.weaponId),
					track.entity,
					track.launchSpeed);
				track = {};
				continue;
			}
			if (ManualThrowReadEntityVtable(track.entity) != track.entityVtable)
			{
				Game::logMsg(
					"[VR][ManualCarryImpact] cancelled type=%s entity=%p reason=vtable-changed",
					ManualCarryThrowWeaponName(track.weaponId),
					track.entity);
				track = {};
				continue;
			}

			if (track.collisionPending)
			{
				void* collisionTarget = track.collisionTarget;
				const Vector collisionVelocity = track.collisionVelocity;
				const bool targetStillValid =
					ManualThrowReadEntityVtable(collisionTarget) == track.collisionTargetVtable;
				if (targetStillValid && ManualCarryImpactApplyTrackedTarget(
					track,
					collisionTarget,
					&collisionVelocity,
					nullptr,
					"physics-collision"))
				{
					if (track.meleeDamageImpact &&
						track.penetrationCount < track.maxPenetrations)
					{
						Vector contactOrigin = track.previousOrigin;
						ManualCarryImpactReadOrigin(track.entity, contactOrigin);
						ManualInventoryImpactContinueThrough(track, contactOrigin);
					}
					else
					{
						Vector contactOrigin = track.previousOrigin;
						ManualCarryImpactReadOrigin(track.entity, contactOrigin);
						ManualCarryImpactResolveSweptPropContact(track, contactOrigin);
						track = {};
						continue;
					}
				}
				track.collisionPending = false;
				track.collisionTarget = nullptr;
				track.collisionTargetVtable = nullptr;
				track.collisionVelocity = {};
				track.collisionSpeed = 0.0f;
			}

			Vector currentOrigin{};
			if (!ManualCarryImpactReadOrigin(track.entity, currentOrigin))
			{
				Game::logMsg(
					"[VR][ManualCarryImpact] cancelled type=%s entity=%p reason=origin-read-failed",
					ManualCarryThrowWeaponName(track.weaponId),
					track.entity);
				track = {};
				continue;
			}

			const float movementDistance = (currentOrigin - track.previousOrigin).Length();
			if (!std::isfinite(movementDistance) || movementDistance > kMaximumTraceDistance)
			{
				Game::logMsg(
					"[VR][ManualCarryImpact] cancelled type=%s entity=%p reason=invalid-move distance=%.1f",
					ManualCarryThrowWeaponName(track.weaponId),
					track.entity,
					movementDistance);
				track = {};
				continue;
			}
			if (movementDistance < kMinimumTraceDistance)
				continue;

			Ray_t ray;
			ray.Init(track.previousOrigin, currentOrigin, traceMins, traceMaxs);
			ManualCarryImpactInfectedTraceFilter filter(
				reinterpret_cast<IHandleEntity*>(track.entity),
				reinterpret_cast<IHandleEntity*>(track.owner),
				&track);
			CGameTrace trace{};
			TraceRoomscaleServerRay(
				Hooks::m_Game->m_EngineTraceServer,
				ray,
				MASK_SHOT,
				&filter,
				trace);
			track.previousOrigin = currentOrigin;

			void* target = trace.m_pEnt;
			if (!target || target == track.entity || target == track.owner)
				continue;

			if (ManualCarryImpactApplyTrackedTarget(
				track,
				target,
				nullptr,
				&trace.endpos,
				"trace-fallback"))
			{
				const bool shouldPenetrate = track.meleeDamageImpact &&
					track.penetrationCount < track.maxPenetrations;
				const bool propContactResolved = shouldPenetrate
					? ManualInventoryImpactContinueThrough(track, trace.endpos)
					: ManualCarryImpactResolveSweptPropContact(track, trace.endpos);
				Game::logMsg(
					"[VR][ManualCarryImpact] swept contact propResponse=%s type=%s entity=%p contact=(%.1f %.1f %.1f) penetration=%d/%d",
					propContactResolved ? (shouldPenetrate ? "penetrate" : "rebound") : "failed",
					ManualCarryThrowWeaponName(track.weaponId),
					track.entity,
					trace.endpos.x,
					trace.endpos.y,
					trace.endpos.z,
					track.penetrationCount,
					track.maxPenetrations);
				// Each carried prop creates one native shove pulse. This prevents a
				// resting or bouncing prop from repeatedly staggering the same group.
				if (!shouldPenetrate)
					track = {};
			}
		}
	}

	struct ManualEmptyHandsInventoryState
	{
		bool readable = false;
		bool hasDummy = false;
		bool hasActualItem = false;
	};

	static bool ManualEmptyHandsPlaceholderIsTrackedDummy(
		const Player& player,
		void* weapon)
	{
		return weapon &&
			weapon == player.manualEmptyHandsDummyPistol &&
			ManualThrowReadEntityVtable(weapon) == player.manualEmptyHandsDummyPistolVtable;
	}

	static void ManualEmptyHandsPlaceholderPublish(int playerIndex, bool active)
	{
		if (!Hooks::m_VR || !Hooks::m_Game || !Hooks::m_Game->m_EngineClient ||
			Hooks::m_Game->m_EngineClient->GetLocalPlayer() != playerIndex)
		{
			return;
		}

		const bool previous = Hooks::m_VR->m_ManualInventoryEmptyHandsActive.exchange(
			active,
			std::memory_order_acq_rel);
		if (previous != active)
		{
			Game::logMsg(
				"[VR][ManualEmptyHands] placeholder render state active=%d",
				active ? 1 : 0);
		}
	}

	static bool ManualEmptyHandsPlaceholderOwnerIsLiveSurvivor(void* ownerPlayer)
	{
		if (!ownerPlayer)
			return false;

		const unsigned char* base = reinterpret_cast<const unsigned char*>(ownerPlayer);
		constexpr size_t kServerLifeStateOffset = 0xF0;
		constexpr size_t kServerTeamNumOffset = 0x238;
		if (!IsReadableMemoryRange(base + kServerLifeStateOffset, sizeof(unsigned char)) ||
			!IsReadableMemoryRange(base + kServerTeamNumOffset, sizeof(int)))
		{
			return false;
		}

		unsigned char lifeState = 1;
		int teamNum = 0;
#ifdef _MSC_VER
		__try
		{
#endif
			lifeState = *reinterpret_cast<const unsigned char*>(
				base + kServerLifeStateOffset);
			teamNum = *reinterpret_cast<const int*>(base + kServerTeamNumOffset);
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#endif
		return lifeState == 0 && teamNum == 2;
	}

	static ManualEmptyHandsInventoryState ManualEmptyHandsPlaceholderScanInventory(
		void* ownerPlayer,
		const Player& player)
	{
		ManualEmptyHandsInventoryState result{};
		if (!ownerPlayer)
			return result;

#ifdef _MSC_VER
		__try
		{
#endif
			void** ownerVtable = *reinterpret_cast<void***>(ownerPlayer);
			if (!ownerVtable)
				return result;

			using WeaponGetSlotFn = void* (__thiscall*)(void*, int);
			auto weaponGetSlot = *reinterpret_cast<WeaponGetSlotFn*>(
				reinterpret_cast<unsigned char*>(ownerVtable) + 0x480);
			if (!weaponGetSlot)
				return result;

			result.readable = true;
			for (int slot = 0; slot < 6; ++slot)
			{
				void* weapon = weaponGetSlot(ownerPlayer, slot);
				if (!weapon)
					continue;
				if (ManualEmptyHandsPlaceholderIsTrackedDummy(player, weapon))
					result.hasDummy = true;
				else
					result.hasActualItem = true;
			}
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			result = {};
		}
#endif
		return result;
	}

	static void ManualEmptyHandsPlaceholderForgetDummy(Player& player)
	{
		player.manualEmptyHandsDummyPistol = nullptr;
		player.manualEmptyHandsDummyPistolVtable = nullptr;
	}

	static bool ManualEmptyHandsPlaceholderRemoveDummy(
		int playerIndex,
		void* ownerPlayer,
		Player& player,
		const char* reason)
	{
		void* dummy = player.manualEmptyHandsDummyPistol;
		const bool dummyStillValid = ManualEmptyHandsPlaceholderIsTrackedDummy(player, dummy);
		ManualEmptyHandsPlaceholderForgetDummy(player);
		if (!dummyStillValid || !ownerPlayer || !Hooks::m_Game ||
			!Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->ManualEmptyHandsRemovePlayerItem.valid ||
			!Hooks::m_Game->m_Offsets->ManualEmptyHandsUtilRemove.valid)
		{
			return false;
		}

		using RemovePlayerItemFn = bool(__thiscall*)(void*, void*);
		using UtilRemoveFn = void(__cdecl*)(void*);
		auto removePlayerItem = reinterpret_cast<RemovePlayerItemFn>(
			Hooks::m_Game->m_Offsets->ManualEmptyHandsRemovePlayerItem.address);
		auto utilRemove = reinterpret_cast<UtilRemoveFn>(
			Hooks::m_Game->m_Offsets->ManualEmptyHandsUtilRemove.address);
		bool detached = false;
#ifdef _MSC_VER
		__try
		{
#endif
			detached = removePlayerItem(ownerPlayer, dummy);
			utilRemove(dummy);
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Game::logMsg(
				"[VR][ManualEmptyHands] placeholder remove exception player=%d dummy=%p reason=%s",
				playerIndex,
				dummy,
				reason ? reason : "unknown");
			return false;
		}
#endif
		Game::logMsg(
			"[VR][ManualEmptyHands] placeholder removed player=%d dummy=%p detached=%d reason=%s",
			playerIndex,
			dummy,
			detached ? 1 : 0,
			reason ? reason : "unknown");
		return true;
	}

	static bool ManualEmptyHandsPlaceholderGiveDummy(
		int playerIndex,
		void* ownerPlayer,
		Player& player)
	{
		if (!ownerPlayer || player.manualEmptyHandsDummyPistol ||
			!Hooks::m_Game || !Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->ManualEmptyHandsRemovePlayerItem.valid ||
			!Hooks::m_Game->m_Offsets->ManualEmptyHandsUtilRemove.valid)
		{
			return false;
		}

		void* dummy = nullptr;
#ifdef _MSC_VER
		__try
		{
#endif
			void** ownerVtable = *reinterpret_cast<void***>(ownerPlayer);
			if (!ownerVtable)
				return false;

			// CTerrorPlayer's GiveNamedItem virtual is slot 0x6BC in the current
			// server build. This is the same native path used by survivor spawn.
			using GiveNamedItemFn = void* (__thiscall*)(
				void*,
				const char*,
				int,
				bool,
				const Vector*);
			auto giveNamedItem = *reinterpret_cast<GiveNamedItemFn*>(
				reinterpret_cast<unsigned char*>(ownerVtable) + 0x6BC);
			if (!giveNamedItem)
				return false;
			dummy = giveNamedItem(ownerPlayer, "weapon_pistol", 0, true, nullptr);
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			dummy = nullptr;
		}
#endif
		void* dummyVtable = ManualThrowReadEntityVtable(dummy);
		if (!dummy || !dummyVtable)
		{
			Game::logMsg(
				"[VR][ManualEmptyHands] placeholder give failed player=%d",
				playerIndex);
			return false;
		}

		player.manualEmptyHandsDummyPistol = dummy;
		player.manualEmptyHandsDummyPistolVtable = dummyVtable;
		Game::logMsg(
			"[VR][ManualEmptyHands] placeholder pistol given player=%d dummy=%p",
			playerIndex,
			dummy);
		return true;
	}

	static void ManualEmptyHandsPlaceholderPrepareForUse(
		int playerIndex,
		void* ownerPlayer,
		Player& player,
		bool useDown)
	{
		player.manualEmptyHandsPlaceholderUseDown = useDown;
		if (player.manualEmptyHandsPlaceholderArmed && useDown &&
			player.manualEmptyHandsDummyPistol)
		{
			// Remove the dummy before Source handles IN_USE. In particular, a real
			// pistol must see an empty secondary slot instead of upgrading the dummy
			// to a dual-pistol entity that cannot be distinguished after the pickup.
			ManualEmptyHandsPlaceholderRemoveDummy(
				playerIndex,
				ownerPlayer,
				player,
				"use/pickup");
		}
	}

	static void ManualEmptyHandsPlaceholderUpdate(
		int playerIndex,
		void* ownerPlayer,
		bool completedManualThrow)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->IsValidPlayerIndex(playerIndex) ||
			!ownerPlayer)
		{
			ManualEmptyHandsPlaceholderPublish(playerIndex, false);
			return;
		}

		Player& player = Hooks::m_Game->m_PlayersVRInfo[static_cast<size_t>(playerIndex)];
		if (!player.isUsingVR || !ManualEmptyHandsPlaceholderOwnerIsLiveSurvivor(ownerPlayer))
		{
			ManualEmptyHandsPlaceholderRemoveDummy(
				playerIndex,
				ownerPlayer,
				player,
				"inactive-player");
			player.manualEmptyHandsPlaceholderArmed = false;
			player.manualEmptyHandsPlaceholderUseDown = false;
			ManualEmptyHandsPlaceholderPublish(playerIndex, false);
			return;
		}

		if (player.manualEmptyHandsDummyPistol &&
			!ManualEmptyHandsPlaceholderIsTrackedDummy(
				player,
				player.manualEmptyHandsDummyPistol))
		{
			Game::logMsg(
				"[VR][ManualEmptyHands] placeholder entity expired player=%d dummy=%p",
				playerIndex,
				player.manualEmptyHandsDummyPistol);
			ManualEmptyHandsPlaceholderForgetDummy(player);
		}

		ManualEmptyHandsInventoryState inventory =
			ManualEmptyHandsPlaceholderScanInventory(ownerPlayer, player);
		if (!inventory.readable)
		{
			ManualEmptyHandsPlaceholderPublish(
				playerIndex,
				player.manualEmptyHandsPlaceholderArmed);
			return;
		}

		if (inventory.hasActualItem)
		{
			ManualEmptyHandsPlaceholderRemoveDummy(
				playerIndex,
				ownerPlayer,
				player,
				"actual-item-acquired");
			player.manualEmptyHandsPlaceholderArmed = false;
			player.manualEmptyHandsPlaceholderUseDown = false;
			ManualEmptyHandsPlaceholderPublish(playerIndex, false);
			return;
		}

		if (player.manualEmptyHandsDummyPistol && !inventory.hasDummy)
		{
			ManualEmptyHandsPlaceholderRemoveDummy(
				playerIndex,
				ownerPlayer,
				player,
				"dummy-left-inventory");
		}

		if (completedManualThrow)
			player.manualEmptyHandsPlaceholderArmed = true;

		if (player.manualEmptyHandsPlaceholderArmed &&
			!player.manualEmptyHandsPlaceholderUseDown &&
			!player.manualEmptyHandsDummyPistol)
		{
			ManualEmptyHandsPlaceholderGiveDummy(playerIndex, ownerPlayer, player);
		}

		ManualEmptyHandsPlaceholderPublish(
			playerIndex,
			player.manualEmptyHandsPlaceholderArmed);
	}

	static bool ManualInventoryThrowExecutePendingDrop(
		int playerIndex,
		void* ownerPlayer)
	{
		if (!Hooks::m_Game || !Hooks::m_Game->m_Offsets ||
			!Hooks::m_Game->m_Offsets->ManualInventoryWeaponDrop.valid ||
			!Hooks::m_Game->IsValidPlayerIndex(playerIndex) || !ownerPlayer)
		{
			return false;
		}

		Player& player = Hooks::m_Game->m_PlayersVRInfo[static_cast<size_t>(playerIndex)];
		ManualThrowPending& pending = player.manualThrowPending;
		if (!player.isUsingVR || !pending.valid || !pending.inventoryDrop ||
			pending.inventoryDropExecuted || pending.owner != ownerPlayer ||
			!pending.sourceWeapon ||
			ManualThrowReadEntityVtable(pending.sourceWeapon) != pending.sourceWeaponVtable)
		{
			return false;
		}

		// The similarly named CWeaponCarry::DropToPhysicsProp routine is only valid
		// for carry-weapon subclasses: it calls carry-only vtable entries and faults
		// when handed a gun, melee weapon, pack, or medicine. Weapon_Drop is the
		// common CBaseCombatCharacter path used by arbitrary inventory weapons.
		using WeaponDropFn = void(__thiscall*)(
			void*,
			void*,
			const Vector*,
			const Vector*);
		auto weaponDrop = reinterpret_cast<WeaponDropFn>(
			Hooks::m_Game->m_Offsets->ManualInventoryWeaponDrop.address);
#ifdef _MSC_VER
		__try
		{
#endif
			weaponDrop(ownerPlayer, pending.sourceWeapon, nullptr, &pending.velocity);
#ifdef _MSC_VER
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Game::logMsg(
				"[VR][ManualInventoryThrow] native drop exception player=%d type=%s source=%p",
				playerIndex,
				ManualCarryThrowWeaponName(pending.weaponId),
				pending.sourceWeapon);
			pending = {};
			return false;
		}
#endif
		pending.inventoryDropExecuted = true;
		Game::logMsg(
			"[VR][ManualInventoryThrow] native weapon drop player=%d type=%s source=%p velocity=(%.1f %.1f %.1f)",
			playerIndex,
			ManualCarryThrowWeaponName(pending.weaponId),
			pending.sourceWeapon,
			pending.velocity.x,
			pending.velocity.y,
			pending.velocity.z);
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
			if (pending.valid && ManualInventoryThrowWeaponIdIsSupported(pending.weaponId) &&
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
			!ManualInventoryThrowWeaponIdIsSupported(pending.weaponId) ||
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

		ManualThrowPending consumed = pending;
		pending = {};
		const bool usesSpawnedPhysicsProp =
			ManualCarryThrowWeaponIdUsesSpawnedPhysicsProp(consumed.weaponId);
		if (usesSpawnedPhysicsProp && !consumed.spawnedPhysicsProp)
		{
			consumed.spawnedPhysicsProp = ManualCarryThrowClaimSpawnedPhysicsProp(
				consumed.sourceWeapon,
				consumed.weaponId);
		}
		void* droppedEntity = usesSpawnedPhysicsProp
			? consumed.spawnedPhysicsProp
			: consumed.sourceWeapon;
		const bool applied = ManualCarryThrowTeleportDroppedEntity(
			droppedEntity,
			consumed);
		if (applied)
			ManualCarryImpactArm(droppedEntity, consumed);
		Game::logMsg(
			"[VR][ManualCarryThrow] %s detached type=%s player=%d tick=%d source=%p dropped=%p spawned=%d active=%p finalVel=(%.1f %.1f %.1f) origin=(%.1f %.1f %.1f)",
			applied ? "apply" : "failed to apply",
			ManualCarryThrowWeaponName(consumed.weaponId),
			playerIndex,
			consumed.releaseTick,
			consumed.sourceWeapon,
			droppedEntity,
			usesSpawnedPhysicsProp ? 1 : 0,
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

void* __fastcall Hooks::dManualCarryCreatePhysicsProp(void* ecx, void* edx)
{
	(void)edx;
	void* droppedEntity = hkManualCarryCreatePhysicsProp.fOriginal
		? hkManualCarryCreatePhysicsProp.fOriginal(ecx)
		: nullptr;
	if (droppedEntity && ecx)
	{
		const int weaponId = ManualCarryThrowReadServerWeaponId(ecx);
		ManualCarryThrowCacheSpawnedPhysicsProp(
			ecx,
			droppedEntity,
			weaponId,
			"CWeaponCarry::CreatePhysicsProp");
	}
	return droppedEntity;
}

void* __cdecl Hooks::dManualCarryCreateEntityByName(
	const char* className,
	int forcedEdictIndex,
	bool runScriptHook)
{
	void* droppedEntity = hkManualCarryCreateEntityByName.fOriginal
		? hkManualCarryCreateEntityByName.fOriginal(className, forcedEdictIndex, runScriptHook)
		: nullptr;
	if (!droppedEntity || !s_ManualCarryThrowPropSpawnHookReady ||
		!m_ServerProcessingUsercmd || !m_Game || !m_VR ||
		!m_VR->m_ManualThrowEnabled ||
		!className || _stricmp(className, "physics_prop") != 0 ||
		!m_Game->IsValidPlayerIndex(m_ServerProcessingUsercmdPlayerIndex))
	{
		return droppedEntity;
	}

	Player& player = m_Game->m_PlayersVRInfo[
		static_cast<size_t>(m_ServerProcessingUsercmdPlayerIndex)];
	ManualThrowPending& pending = player.manualThrowPending;
	if (player.isUsingVR && pending.valid &&
		pending.owner == m_ServerProcessingUsercmdPlayer &&
		!pending.spawnedPhysicsProp &&
		ManualCarryThrowWeaponIdUsesSpawnedPhysicsProp(pending.weaponId))
	{
		pending.spawnedPhysicsProp = droppedEntity;
		Game::logMsg(
			"[VR][ManualCarryThrow] captured spawned physics_prop type=%s player=%d tick=%d source=%p dropped=%p",
			ManualCarryThrowWeaponName(pending.weaponId),
			m_ServerProcessingUsercmdPlayerIndex,
			pending.releaseTick,
			pending.sourceWeapon,
			droppedEntity);
	}

	return droppedEntity;
}

void __fastcall Hooks::dCBaseEntityVPhysicsCollision(
	void* ecx,
	void* edx,
	int index,
	void* collisionEvent)
{
	(void)edx;
	ManualCarryImpactQueuePhysicsCollision(ecx, index, collisionEvent);
	if (hkCBaseEntityVPhysicsCollision.fOriginal)
		hkCBaseEntityVPhysicsCollision.fOriginal(ecx, index, collisionEvent);
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
