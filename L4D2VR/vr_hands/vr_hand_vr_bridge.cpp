#include "vr.h"

#include "game.h"
#include "sdk.h"
#include "sdk/ivdebugoverlay.h"
#include "vr_hand_system.h"
#include "vr_hand_math.h"

#include <d3d9.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

VR::VR() = default;

VR::~VR() = default;

namespace
{
    const std::string kNoManualReloadMagazineGlbPath;

    size_t ManualReloadNormalizeDelayedSoundsForReplay(
        std::vector<ManualReloadDelayedSound>& sounds,
        float* outRebaseSeconds = nullptr);
    std::string ManualReloadPrepareConsoleSoundSample(const std::string& rawSample);

    Vector MagazineInteractionNormalizeAxis(const Vector& axis, const Vector& fallback)
    {
        const Vector normalized = VrHandMath::Normalize(axis);
        return normalized.Length() > 0.0001f ? normalized : fallback;
    }

    int MagazineInteractionDominantAxis(const Vector& value)
    {
        const float ax = std::fabs(value.x);
        const float ay = std::fabs(value.y);
        const float az = std::fabs(value.z);
        if (ay >= ax && ay >= az)
            return 1;
        if (az >= ax && az >= ay)
            return 2;
        return 0;
    }

    float MagazineInteractionDistanceToBoxSourceUnits(
        const MagazineInteractionBoxSnapshot& box,
        const Vector& worldPoint)
    {
        const Vector delta = worldPoint - box.origin;
        const Vector local(
            VrHandMath::Dot(delta, box.axisX),
            VrHandMath::Dot(delta, box.axisY),
            VrHandMath::Dot(delta, box.axisZ));
        const Vector closest(
            std::clamp(local.x, box.mins.x, box.maxs.x),
            std::clamp(local.y, box.mins.y, box.maxs.y),
            std::clamp(local.z, box.mins.z, box.maxs.z));
        return (local - closest).Length();
    }

    float MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
        const MagazineInteractionBoxSnapshot& box,
        const Vector& controllerOrigin,
        const QAngle& controllerAngles,
        float sourceUnitsPerMeter)
    {
        Vector forward{};
        Vector right{};
        Vector up{};
        QAngle::AngleVectors(controllerAngles, &forward, &right, &up);
        forward = VrHandMath::Normalize(forward);
        right = VrHandMath::Normalize(right);
        up = VrHandMath::Normalize(up);

        const float nearOffset = 0.06f * sourceUnitsPerMeter;
        const float farOffset = 0.12f * sourceUnitsPerMeter;
        const Vector probes[] =
        {
            controllerOrigin,
            controllerOrigin + forward * nearOffset,
            controllerOrigin - forward * nearOffset,
            controllerOrigin + right * nearOffset,
            controllerOrigin - right * nearOffset,
            controllerOrigin + up * nearOffset,
            controllerOrigin - up * nearOffset,
            controllerOrigin + forward * farOffset,
            controllerOrigin - forward * farOffset,
            controllerOrigin + up * farOffset,
            controllerOrigin - up * farOffset
        };

        float nearest = FLT_MAX;
        for (const Vector& probe : probes)
            nearest = std::min(nearest, MagazineInteractionDistanceToBoxSourceUnits(box, probe));
        return nearest;
    }

    VrHandMatrix4 MagazineInteractionBuildBoxWorld(const MagazineInteractionBoxSnapshot& box)
    {
        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, box.axisX.x);
        VrHandMath::Set(out, 1, 0, box.axisX.y);
        VrHandMath::Set(out, 2, 0, box.axisX.z);
        VrHandMath::Set(out, 0, 1, box.axisY.x);
        VrHandMath::Set(out, 1, 1, box.axisY.y);
        VrHandMath::Set(out, 2, 1, box.axisY.z);
        VrHandMath::Set(out, 0, 2, box.axisZ.x);
        VrHandMath::Set(out, 1, 2, box.axisZ.y);
        VrHandMath::Set(out, 2, 2, box.axisZ.z);
        VrHandMath::Set(out, 0, 3, box.origin.x);
        VrHandMath::Set(out, 1, 3, box.origin.y);
        VrHandMath::Set(out, 2, 3, box.origin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildControllerWorldFromAxes(
        const Vector& origin,
        const Vector& forwardIn,
        const Vector& rightIn,
        const Vector& upIn)
    {
        Vector forward = VrHandMath::Normalize(forwardIn);
        Vector right = VrHandMath::Normalize(rightIn);
        Vector up = VrHandMath::Normalize(upIn);
        if (forward.Length() <= 0.0001f)
            forward = Vector(1.0f, 0.0f, 0.0f);
        if (right.Length() <= 0.0001f)
            right = Vector(0.0f, -1.0f, 0.0f);
        if (up.Length() <= 0.0001f)
            up = Vector(0.0f, 0.0f, 1.0f);

        VrHandMatrix4 out = VrHandMath::Identity();
        VrHandMath::Set(out, 0, 0, right.x);
        VrHandMath::Set(out, 1, 0, right.y);
        VrHandMath::Set(out, 2, 0, right.z);
        VrHandMath::Set(out, 0, 1, up.x);
        VrHandMath::Set(out, 1, 1, up.y);
        VrHandMath::Set(out, 2, 1, up.z);
        VrHandMath::Set(out, 0, 2, -forward.x);
        VrHandMath::Set(out, 1, 2, -forward.y);
        VrHandMath::Set(out, 2, 2, -forward.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    bool MagazineInteractionBuildFreshMagazinePickupBox(
        const VR* vr,
        MagazineInteractionBoxSnapshot& outBox,
        QAngle& outAngles)
    {
        if (!vr)
            return false;

        Vector bodyForward = vr->m_HmdForward;
        bodyForward.z = 0.0f;
        bodyForward = VrHandMath::Normalize(bodyForward);
        if (bodyForward.Length() <= 0.0001f)
            bodyForward = Vector(1.0f, 0.0f, 0.0f);

        const Vector worldUp(0.0f, 0.0f, 1.0f);
        Vector bodyRight(
            bodyForward.y * worldUp.z - bodyForward.z * worldUp.y,
            bodyForward.z * worldUp.x - bodyForward.x * worldUp.z,
            bodyForward.x * worldUp.y - bodyForward.y * worldUp.x);
        bodyRight = VrHandMath::Normalize(bodyRight);
        if (bodyRight.Length() <= 0.0001f)
            bodyRight = Vector(0.0f, -1.0f, 0.0f);

        const Vector bodyOrigin = vr->m_CameraAnchor
            + bodyForward * (vr->m_InventoryBodyOriginOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryBodyOriginOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryBodyOriginOffset.z * vr->m_VRScale);
        const Vector pickup = bodyOrigin
            + bodyForward * (vr->m_InventoryLeftWaistOffset.x * vr->m_VRScale)
            + bodyRight * (vr->m_InventoryLeftWaistOffset.y * vr->m_VRScale)
            + worldUp * (vr->m_InventoryLeftWaistOffset.z * vr->m_VRScale);

        const Vector half(
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.x) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.y) * vr->m_VRScale,
            std::max(0.005f, vr->m_MagazineInteractionFreshMagazineBoxHalfExtentsMeters.z) * vr->m_VRScale);

        outBox = {};
        outBox.origin = pickup;
        outBox.axisX = bodyForward;
        outBox.axisY = bodyRight;
        outBox.axisZ = worldUp;
        outBox.mins = Vector(-half.x, -half.y, -half.z);
        outBox.maxs = Vector(half.x, half.y, half.z);
        outBox.publishedAt = std::chrono::steady_clock::now();

        QAngle::VectorAngles(bodyForward, outAngles);
        outAngles.x = 0.0f;
        outAngles.z = 0.0f;
        return true;
    }

    void MagazineInteractionDrawFreshMagazinePickupBox(const VR* vr, const MagazineInteractionBoxSnapshot& box, const QAngle& angles)
    {
        if (!vr || !vr->m_MagazineBoxDebugEnabled || !vr->m_Game || !vr->m_Game->m_DebugOverlay)
            return;

        const float duration = std::max(0.02f, vr->m_LastFrameDuration * 2.0f);
        vr->m_Game->m_DebugOverlay->AddBoxOverlay(
            box.origin,
            box.mins,
            box.maxs,
            angles,
            40,
            210,
            255,
            170,
            duration);
    }

    VrHandMatrix4 MagazineInteractionBuildLocalTransform(
        float sourceUnitsPerMeter,
        const Vector& localPositionOffsetMeters,
        const Vector& localRotationOffsetDeg)
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
        return local;
    }

    VrHandMatrix4 MagazineInteractionBuildFreshHandMagazineWorld(const VR* vr)
    {
        if (!vr)
            return VrHandMath::Identity();

        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildControllerWorldFromAxes(
            vr->m_LeftControllerPosAbs,
            vr->m_LeftControllerForward,
            vr->m_LeftControllerRight,
            vr->m_LeftControllerUp);
        const VrHandMatrix4 local = MagazineInteractionBuildLocalTransform(
            vr->m_VRScale,
            vr->m_ManualReloadMagazineHandOffsetMeters,
            vr->m_ManualReloadMagazineHandRotationOffsetDeg);
        return VrHandMath::Multiply(controllerWorld, local);
    }

    bool MagazineInteractionMatrixBasisLooksValid(const VrHandMatrix4& matrix)
    {
        for (int column = 0; column < 3; ++column)
        {
            const Vector axis(
                VrHandMath::Get(matrix, 0, column),
                VrHandMath::Get(matrix, 1, column),
                VrHandMath::Get(matrix, 2, column));
            const float length = axis.Length();
            if (!std::isfinite(length) || length < 0.001f || length > 100.0f)
                return false;
        }
        return true;
    }

    bool MagazineInteractionMatrixLooksRenderable(const VrHandMatrix4& matrix)
    {
        for (float value : matrix.m)
        {
            if (!std::isfinite(value))
                return false;
        }

        const Vector origin(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
        if (std::fabs(origin.x) > 1000000.0f ||
            std::fabs(origin.y) > 1000000.0f ||
            std::fabs(origin.z) > 1000000.0f)
        {
            return false;
        }

        for (int column = 0; column < 3; ++column)
        {
            const Vector axis(
                VrHandMath::Get(matrix, 0, column),
                VrHandMath::Get(matrix, 1, column),
                VrHandMath::Get(matrix, 2, column));
            const float length = axis.Length();
            if (!std::isfinite(length) || length < 0.001f || length > 100.0f)
                return false;
        }
        return true;
    }

    VrHandMatrix4 MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(const VR* vr, const Vector& desiredCenter)
    {
        if (!vr)
            return VrHandMath::Identity();

        VrHandMatrix4 out = vr->m_MagazineInteractionSocketValid
            ? vr->m_MagazineInteractionSocketWorld
            : VrHandMath::Identity();
        const Vector centerLocal = (vr->m_MagazineInteractionSocketBox.mins +
            vr->m_MagazineInteractionSocketBox.maxs) * 0.5f;
        const Vector axisX = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 0), VrHandMath::Get(out, 1, 0), VrHandMath::Get(out, 2, 0)),
            Vector(1.0f, 0.0f, 0.0f));
        const Vector axisY = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 1), VrHandMath::Get(out, 1, 1), VrHandMath::Get(out, 2, 1)),
            Vector(0.0f, 1.0f, 0.0f));
        const Vector axisZ = MagazineInteractionNormalizeAxis(
            Vector(VrHandMath::Get(out, 0, 2), VrHandMath::Get(out, 1, 2), VrHandMath::Get(out, 2, 2)),
            Vector(0.0f, 0.0f, 1.0f));
        const Vector targetClipOrigin =
            desiredCenter -
            axisX * centerLocal.x -
            axisY * centerLocal.y -
            axisZ * centerLocal.z;
        VrHandMath::Set(out, 0, 3, targetClipOrigin.x);
        VrHandMath::Set(out, 1, 3, targetClipOrigin.y);
        VrHandMath::Set(out, 2, 3, targetClipOrigin.z);
        return out;
    }

    VrHandMatrix4 MagazineInteractionBuildFreshSocketOrientedMagazineWorld(const VR* vr)
    {
        if (!vr)
            return VrHandMath::Identity();

        const VrHandMatrix4 handPlaced = MagazineInteractionBuildFreshHandMagazineWorld(vr);
        return MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(
            vr,
            Vector(
                VrHandMath::Get(handPlaced, 0, 3),
                VrHandMath::Get(handPlaced, 1, 3),
                VrHandMath::Get(handPlaced, 2, 3)));
    }

    Vector MagazineInteractionMatrixOrigin(const VrHandMatrix4& matrix)
    {
        return Vector(
            VrHandMath::Get(matrix, 0, 3),
            VrHandMath::Get(matrix, 1, 3),
            VrHandMath::Get(matrix, 2, 3));
    }

    Vector MagazineInteractionMatrixAxis(const VrHandMatrix4& matrix, int column)
    {
        return VrHandMath::Normalize(Vector(
            VrHandMath::Get(matrix, 0, column),
            VrHandMath::Get(matrix, 1, column),
            VrHandMath::Get(matrix, 2, column)));
    }

    Vector MagazineInteractionWorldVectorToMatrixLocal(
        const VrHandMatrix4& matrix,
        const Vector& worldVector)
    {
        return Vector(
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 0)),
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 1)),
            VrHandMath::Dot(worldVector, MagazineInteractionMatrixAxis(matrix, 2)));
    }

    Vector MagazineInteractionMatrixLocalVectorToWorld(
        const VrHandMatrix4& matrix,
        const Vector& localVector)
    {
        return
            MagazineInteractionMatrixAxis(matrix, 0) * localVector.x +
            MagazineInteractionMatrixAxis(matrix, 1) * localVector.y +
            MagazineInteractionMatrixAxis(matrix, 2) * localVector.z;
    }

    VrHandMatrix4 MagazineInteractionMatrixWithOrigin(const VrHandMatrix4& matrix, const Vector& origin)
    {
        VrHandMatrix4 out = matrix;
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    Vector MagazineInteractionBuildBoltPullAxisWorld(
        const VR* vr,
        const MagazineInteractionBoxSnapshot& boltBox,
        const VrHandMatrix4& boltRestWorld)
    {
        if (!vr)
            return Vector(-1.0f, 0.0f, 0.0f);

        Vector publishedAxis = VrHandMath::Normalize(boltBox.pullAxisWorld);
        if (publishedAxis.Length() > 0.0001f)
            return publishedAxis;

        Vector localAxis = VrHandMath::Normalize(vr->m_MagazineInteractionBoltPullAxisLocal);
        if (localAxis.Length() > 0.0001f && MagazineInteractionMatrixBasisLooksValid(boltRestWorld))
        {
            Vector worldAxis = MagazineInteractionMatrixLocalVectorToWorld(boltRestWorld, localAxis);
            worldAxis = VrHandMath::Normalize(worldAxis);
            if (worldAxis.Length() > 0.0001f)
                return worldAxis;
        }

        return Vector(-1.0f, 0.0f, 0.0f);
    }

    VrHandMatrix4 MagazineInteractionBuildControllerRelation(
        const VrHandMatrix4& controllerWorld,
        const VrHandMatrix4& magazineWorld)
    {
        VrHandMatrix4 relation = VrHandMath::Identity();

        const Vector controllerOrigin = MagazineInteractionMatrixOrigin(controllerWorld);
        const Vector controllerAxes[3] =
        {
            MagazineInteractionMatrixAxis(controllerWorld, 0),
            MagazineInteractionMatrixAxis(controllerWorld, 1),
            MagazineInteractionMatrixAxis(controllerWorld, 2)
        };
        const Vector magazineOrigin = MagazineInteractionMatrixOrigin(magazineWorld);
        const Vector delta = magazineOrigin - controllerOrigin;
        const Vector localOrigin(
            VrHandMath::Dot(delta, controllerAxes[0]),
            VrHandMath::Dot(delta, controllerAxes[1]),
            VrHandMath::Dot(delta, controllerAxes[2]));

        for (int column = 0; column < 3; ++column)
        {
            const Vector magazineAxis = MagazineInteractionMatrixAxis(magazineWorld, column);
            VrHandMath::Set(relation, 0, column, VrHandMath::Dot(magazineAxis, controllerAxes[0]));
            VrHandMath::Set(relation, 1, column, VrHandMath::Dot(magazineAxis, controllerAxes[1]));
            VrHandMath::Set(relation, 2, column, VrHandMath::Dot(magazineAxis, controllerAxes[2]));
        }
        VrHandMath::Set(relation, 0, 3, localOrigin.x);
        VrHandMath::Set(relation, 1, 3, localOrigin.y);
        VrHandMath::Set(relation, 2, 3, localOrigin.z);
        return relation;
    }

    VrHandMatrix4 MagazineInteractionBuildWorldFromControllerRelation(
        const VrHandMatrix4& controllerWorld,
        const VrHandMatrix4& relation)
    {
        VrHandMatrix4 out = VrHandMath::Identity();

        const Vector controllerOrigin = MagazineInteractionMatrixOrigin(controllerWorld);
        const Vector controllerAxes[3] =
        {
            MagazineInteractionMatrixAxis(controllerWorld, 0),
            MagazineInteractionMatrixAxis(controllerWorld, 1),
            MagazineInteractionMatrixAxis(controllerWorld, 2)
        };

        for (int column = 0; column < 3; ++column)
        {
            const Vector localAxis(
                VrHandMath::Get(relation, 0, column),
                VrHandMath::Get(relation, 1, column),
                VrHandMath::Get(relation, 2, column));
            const Vector worldAxis =
                controllerAxes[0] * localAxis.x +
                controllerAxes[1] * localAxis.y +
                controllerAxes[2] * localAxis.z;
            VrHandMath::Set(out, 0, column, worldAxis.x);
            VrHandMath::Set(out, 1, column, worldAxis.y);
            VrHandMath::Set(out, 2, column, worldAxis.z);
        }

        const Vector localOrigin(
            VrHandMath::Get(relation, 0, 3),
            VrHandMath::Get(relation, 1, 3),
            VrHandMath::Get(relation, 2, 3));
        const Vector worldOrigin =
            controllerOrigin +
            controllerAxes[0] * localOrigin.x +
            controllerAxes[1] * localOrigin.y +
            controllerAxes[2] * localOrigin.z;
        VrHandMath::Set(out, 0, 3, worldOrigin.x);
        VrHandMath::Set(out, 1, 3, worldOrigin.y);
        VrHandMath::Set(out, 2, 3, worldOrigin.z);
        return out;
    }

    Vector MagazineInteractionBoxCenterLocal(const MagazineInteractionBoxSnapshot& box)
    {
        return (box.mins + box.maxs) * 0.5f;
    }

    Vector MagazineInteractionMatrixPointWorld(const VrHandMatrix4& matrix, const Vector& localPoint)
    {
        return MagazineInteractionMatrixOrigin(matrix) +
            MagazineInteractionMatrixAxis(matrix, 0) * localPoint.x +
            MagazineInteractionMatrixAxis(matrix, 1) * localPoint.y +
            MagazineInteractionMatrixAxis(matrix, 2) * localPoint.z;
    }

    MagazineInteractionBoxSnapshot MagazineInteractionBuildWorldBoxSnapshot(
        const MagazineInteractionBoxSnapshot& localBox,
        const VrHandMatrix4& world)
    {
        MagazineInteractionBoxSnapshot out = localBox;
        out.origin = MagazineInteractionMatrixOrigin(world);
        out.axisX = MagazineInteractionMatrixAxis(world, 0);
        out.axisY = MagazineInteractionMatrixAxis(world, 1);
        out.axisZ = MagazineInteractionMatrixAxis(world, 2);
        out.publishedAt = std::chrono::steady_clock::now();
        return out;
    }

    void MagazineInteractionProjectBoxOntoAxis(
        const VrHandMatrix4& world,
        const MagazineInteractionBoxSnapshot& box,
        const Vector& axis,
        float& outMin,
        float& outMax)
    {
        outMin = FLT_MAX;
        outMax = -FLT_MAX;
        for (int z = 0; z <= 1; ++z)
        {
            for (int y = 0; y <= 1; ++y)
            {
                for (int x = 0; x <= 1; ++x)
                {
                    const Vector local(
                        x ? box.maxs.x : box.mins.x,
                        y ? box.maxs.y : box.mins.y,
                        z ? box.maxs.z : box.mins.z);
                    const float projected = VrHandMath::Dot(
                        MagazineInteractionMatrixPointWorld(world, local),
                        axis);
                    outMin = std::min(outMin, projected);
                    outMax = std::max(outMax, projected);
                }
            }
        }
    }

    float MagazineInteractionIntervalOverlap(float aMin, float aMax, float bMin, float bMax)
    {
        return std::max(0.0f, std::min(aMax, bMax) - std::max(aMin, bMin));
    }

    VrHandMatrix4 MagazineInteractionBuildWorldAtBoxCenter(
        const VrHandMatrix4& orientationWorld,
        const MagazineInteractionBoxSnapshot& box,
        const Vector& desiredCenter)
    {
        VrHandMatrix4 out = orientationWorld;
        const Vector axisX = MagazineInteractionMatrixAxis(orientationWorld, 0);
        const Vector axisY = MagazineInteractionMatrixAxis(orientationWorld, 1);
        const Vector axisZ = MagazineInteractionMatrixAxis(orientationWorld, 2);
        const Vector centerLocal = MagazineInteractionBoxCenterLocal(box);
        const Vector origin =
            desiredCenter -
            axisX * centerLocal.x -
            axisY * centerLocal.y -
            axisZ * centerLocal.z;

        VrHandMath::Set(out, 0, 0, axisX.x);
        VrHandMath::Set(out, 1, 0, axisX.y);
        VrHandMath::Set(out, 2, 0, axisX.z);
        VrHandMath::Set(out, 0, 1, axisY.x);
        VrHandMath::Set(out, 1, 1, axisY.y);
        VrHandMath::Set(out, 2, 1, axisY.z);
        VrHandMath::Set(out, 0, 2, axisZ.x);
        VrHandMath::Set(out, 1, 2, axisZ.y);
        VrHandMath::Set(out, 2, 2, axisZ.z);
        VrHandMath::Set(out, 0, 3, origin.x);
        VrHandMath::Set(out, 1, 3, origin.y);
        VrHandMath::Set(out, 2, 3, origin.z);
        return out;
    }

    bool MagazineInteractionTryReadInt(const void* entity, int offset, int& out)
    {
        if (!entity || offset < 0)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            out = *reinterpret_cast<const int*>(reinterpret_cast<const unsigned char*>(entity) + offset);
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out = 0;
            return false;
        }
#endif
    }

    bool MagazineInteractionWeaponUsesDetachableMagazine(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::MP5:
        case C_WeaponCSBase::WeaponID::SG552:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:
        case C_WeaponCSBase::WeaponID::M60:
            return true;
        default:
            return false;
        }
    }

    bool MagazineInteractionWeaponRequiresManualBolt(C_WeaponCSBase::WeaponID weaponId)
    {
        return MagazineInteractionWeaponUsesDetachableMagazine(weaponId);
    }

    int MagazineInteractionDefaultMaxClip(C_WeaponCSBase::WeaponID weaponId, int currentClip)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL: return currentClip > 15 ? 30 : 15;
        case C_WeaponCSBase::WeaponID::MAGNUM: return 8;
        case C_WeaponCSBase::WeaponID::UZI: return 50;
        case C_WeaponCSBase::WeaponID::MAC10: return 50;
        case C_WeaponCSBase::WeaponID::MP5: return 50;
        case C_WeaponCSBase::WeaponID::M16A1: return 50;
        case C_WeaponCSBase::WeaponID::AK47: return 40;
        case C_WeaponCSBase::WeaponID::SCAR: return 60;
        case C_WeaponCSBase::WeaponID::SG552: return 50;
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE: return 15;
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY: return 30;
        case C_WeaponCSBase::WeaponID::AWP: return 20;
        case C_WeaponCSBase::WeaponID::SCOUT: return 15;
        case C_WeaponCSBase::WeaponID::M60: return 150;
        default: return 0;
        }
    }

    std::string MagazineInteractionClipOutSoundSample(const VR* vr)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return "weapons/dual_pistol/gunother/dualpistol_clip_out_1.wav";
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return "weapons/magnum/gunother/pistol_clip_out_1.wav";
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return "weapons/pistol/gunother/pistol_clip_out_1.wav";
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return "weapons/rifle_ak47/gunother/rifle_clip_out_1.wav";
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return "weapons/rifle_desert/gunother/rifle_clip_out_1.wav";
        if (lowerModel.find("sg552") != std::string::npos)
            return "weapons/sg552/gunother/sg552_clipout.wav";
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return "weapons/smg_silenced/gunother/smg_clip_out_1.wav";
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return "weapons/mp5navy/gunother/mp5_clipout.wav";
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return "weapons/smg/gunother/smg_clip_out_1.wav";
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return "weapons/hunting_rifle/gunother/hunting_rifle_clipout.wav";
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return "weapons/sniper_military/gunother/sniper_military_clip_out_1.wav";
        if (lowerModel.find("awp") != std::string::npos)
            return "weapons/awp/gunother/awp_clipout.wav";
        if (lowerModel.find("scout") != std::string::npos)
            return "weapons/scout/gunother/scout_clipout.wav";
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return "weapons/machinegun_m60/gunother/rifle_clip_out_1.wav";
        }
        if (lowerModel.find("rifle") != std::string::npos ||
            lowerModel.find("sg552") != std::string::npos)
        {
            return "weapons/rifle/gunother/rifle_clip_out_1.wav";
        }
        return std::string();
    }

    std::string MagazineInteractionClipInSoundSample(const VR* vr)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return "weapons/dual_pistol/gunother/dualpistol_clip_in_1.wav";
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return "weapons/magnum/gunother/pistol_clip_in_1.wav";
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return "weapons/pistol/gunother/pistol_clip_in_1.wav";
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return "weapons/rifle_ak47/gunother/rifle_clip_in_1.wav";
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return "weapons/rifle_desert/gunother/rifle_clip_in_1.wav";
        if (lowerModel.find("sg552") != std::string::npos)
            return "weapons/sg552/gunother/sg552_clipin.wav";
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return "weapons/smg_silenced/gunother/smg_clip_in_1.wav";
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return "weapons/mp5navy/gunother/mp5_clipin.wav";
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return "weapons/smg/gunother/smg_clip_in_1.wav";
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return "weapons/hunting_rifle/gunother/hunting_rifle_clipin.wav";
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return "weapons/sniper_military/gunother/sniper_military_clip_in_1.wav";
        if (lowerModel.find("awp") != std::string::npos)
            return "weapons/awp/gunother/awp_clipin.wav";
        if (lowerModel.find("scout") != std::string::npos)
            return "weapons/scout/gunother/scout_clipin.wav";
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return "weapons/machinegun_m60/gunother/rifle_clip_in_1.wav";
        }
        if (lowerModel.find("rifle") != std::string::npos)
            return "weapons/rifle/gunother/rifle_clip_in_1.wav";
        return std::string();
    }

    std::string MagazineInteractionBoltFallbackSoundSample(const VR* vr, bool forward)
    {
        if (!vr)
            return std::string();

        std::string lowerModel = vr->m_MagazineInteractionMagazineModelName;
        std::transform(lowerModel.begin(), lowerModel.end(), lowerModel.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const char* suffix = forward ? "forward" : "back";
        auto slide = [&](const char* directory, const char* stem, bool numbered = true) -> std::string
        {
            std::string sample = std::string("weapons/") + directory + "/gunother/" + stem + "_slide" + suffix;
            if (numbered)
                sample += "_1";
            sample += ".wav";
            return sample;
        };
        auto bolt = [&](const char* directory, const char* stem) -> std::string
        {
            return std::string("weapons/") + directory + "/gunother/" + stem + "_bolt" + suffix + ".wav";
        };

        if (lowerModel.find("dual_pistol") != std::string::npos ||
            lowerModel.find("dual_pistola") != std::string::npos)
        {
            return slide("dual_pistol", "dualpistol");
        }
        if (lowerModel.find("desert_eagle") != std::string::npos ||
            lowerModel.find("magnum") != std::string::npos)
        {
            return slide("magnum", "pistol");
        }
        if (lowerModel.find("pistol") != std::string::npos)
            return slide("pistol", "pistol");
        if (lowerModel.find("rifle_ak47") != std::string::npos ||
            lowerModel.find("ak47") != std::string::npos)
        {
            return slide("rifle_ak47", "rifle");
        }
        if (lowerModel.find("desert_rifle") != std::string::npos)
            return slide("rifle_desert", "rifle");
        if (lowerModel.find("sg552") != std::string::npos)
            return slide("sg552", "sg552", false);
        if (lowerModel.find("silenced_smg") != std::string::npos ||
            lowerModel.find("smg_silenced") != std::string::npos)
        {
            return slide("smg_silenced", "smg");
        }
        if (lowerModel.find("mp5") != std::string::npos)
            return slide("mp5navy", "mp5", false);
        if (lowerModel.find("smg") != std::string::npos ||
            lowerModel.find("uzi") != std::string::npos)
        {
            return slide("smg", "smg");
        }
        if (lowerModel.find("huntingrifle") != std::string::npos ||
            lowerModel.find("hunting_rifle") != std::string::npos)
        {
            return bolt("hunting_rifle", "hunting_rifle");
        }
        if (lowerModel.find("sniper_military") != std::string::npos)
            return slide("sniper_military", "sniper_military");
        if (lowerModel.find("awp") != std::string::npos)
            return bolt("awp", "awp");
        if (lowerModel.find("scout") != std::string::npos)
            return bolt("scout", "scout");
        if (lowerModel.find("m60") != std::string::npos ||
            lowerModel.find("machinegun_m60") != std::string::npos)
        {
            return slide("machinegun_m60", "rifle");
        }
        if (lowerModel.find("rifle") != std::string::npos ||
            lowerModel.find("sg552") != std::string::npos)
        {
            return slide("rifle", "rifle");
        }
        return std::string();
    }

    std::string MagazineInteractionLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool MagazineInteractionSoundMatchesCurrentWeapon(const VR* vr, const char* sample)
    {
        if (!vr || !sample || !*sample)
            return false;

        const std::string lower = MagazineInteractionLowerAscii(sample);
        const std::string model = MagazineInteractionLowerAscii(vr->m_MagazineInteractionMagazineModelName);
        auto has = [&](const char* token) -> bool
            {
                return token && *token && lower.find(token) != std::string::npos;
            };
        auto modelHas = [&](const char* token) -> bool
            {
                return token && *token && model.find(token) != std::string::npos;
            };

        const bool knownSpecificWeapon =
            has("hunting_rifle") ||
            has("sniper_military") ||
            has("rifle_ak47") ||
            has("ak47") ||
            has("rifle_desert") ||
            has("desert_rifle") ||
            has("smg_silenced") ||
            has("silenced_smg") ||
            has("smg_mp5") ||
            has("mp5") ||
            has("desert_eagle") ||
            has("magnum") ||
            has("dual_pistol") ||
            has("dualpistol") ||
            has("snip_awp") ||
            has("awp") ||
            has("snip_scout") ||
            has("scout") ||
            has("machinegun_m60") ||
            has("m60");

        switch (static_cast<C_WeaponCSBase::WeaponID>(vr->m_MagazineInteractionWeaponId))
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
            return has("pistol") && !has("magnum") && !has("desert_eagle");
        case C_WeaponCSBase::WeaponID::MAGNUM:
            return has("magnum") || has("desert_eagle") ||
                (has("pistol") && modelHas("desert_eagle"));
        case C_WeaponCSBase::WeaponID::UZI:
            return has("weapons/smg/gunother/") || has("weapons\\smg\\gunother\\") ||
                (has("smg") && !has("silenced") && !has("mp5"));
        case C_WeaponCSBase::WeaponID::MAC10:
            return has("smg_silenced") || has("silenced_smg") ||
                (has("smg") && modelHas("silenced_smg"));
        case C_WeaponCSBase::WeaponID::MP5:
            return has("mp5") || has("smg_mp5");
        case C_WeaponCSBase::WeaponID::M16A1:
            return has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\") ||
                (has("rifle") && modelHas("v_rifle.mdl"));
        case C_WeaponCSBase::WeaponID::AK47:
            return has("rifle_ak47") || has("ak47") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("ak47"));
        case C_WeaponCSBase::WeaponID::SCAR:
            return has("rifle_desert") || has("desert_rifle") || has("scar");
        case C_WeaponCSBase::WeaponID::SG552:
            return has("sg552") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("sg552"));
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
            return has("hunting_rifle") || has("huntingrifle");
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
            if (has("hunting_rifle") || has("huntingrifle"))
                return false;
            return has("sniper_military") || has("military_sniper");
        case C_WeaponCSBase::WeaponID::AWP:
            return has("awp") || has("snip_awp");
        case C_WeaponCSBase::WeaponID::SCOUT:
            return has("scout") || has("snip_scout");
        case C_WeaponCSBase::WeaponID::M60:
            return has("m60") || has("machinegun_m60") ||
                ((has("weapons/rifle/gunother/") || has("weapons\\rifle\\gunother\\")) && modelHas("m60"));
        default:
            break;
        }

        return !knownSpecificWeapon;
    }

    void MagazineInteractionPlayClipOutSound(VR* vr)
    {
        if (!vr || !vr->m_Game)
            return;

        const std::string sample = MagazineInteractionClipOutSoundSample(vr);
        if (sample.empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no synthetic clip-out sample for model=%s weaponId=%d",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        const std::string command = "playvol \"" + sample + "\" 1.000";
        vr->m_MagazineInteractionSyntheticClipOutSample = sample;
        vr->m_MagazineInteractionSyntheticClipOutStarted = std::chrono::steady_clock::now();
        vr->m_Game->ClientCmd_Unrestricted(command.c_str());
        vr->m_MagazineInteractionSyntheticClipOutSample.clear();
        vr->m_MagazineInteractionSyntheticClipOutStarted = {};
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] played synthetic clip-out sample=%s",
            sample.c_str());
    }

    void MagazineInteractionPlayClipInSound(VR* vr)
    {
        if (!vr || !vr->m_Game)
            return;

        const std::string sample = MagazineInteractionClipInSoundSample(vr);
        if (sample.empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no synthetic clip-in sample for model=%s weaponId=%d",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        const std::string command = "playvol \"" + sample + "\" 1.000";
        vr->m_MagazineInteractionSyntheticClipInSample = sample;
        vr->m_MagazineInteractionSyntheticClipInStarted = std::chrono::steady_clock::now();
        vr->m_Game->ClientCmd_Unrestricted(command.c_str());
        vr->m_MagazineInteractionSyntheticClipInSample.clear();
        vr->m_MagazineInteractionSyntheticClipInStarted = {};
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] played synthetic clip-in sample=%s",
            sample.c_str());
    }

    void MagazineInteractionPlayBoltSound(VR* vr, bool forward)
    {
        if (!vr || !vr->m_Game)
            return;

        const std::string sample = forward
            ? (!vr->m_MagazineInteractionCapturedBoltForwardSample.empty()
                ? vr->m_MagazineInteractionCapturedBoltForwardSample
                : MagazineInteractionBoltFallbackSoundSample(vr, true))
            : (!vr->m_MagazineInteractionCapturedBoltBackSample.empty()
                ? vr->m_MagazineInteractionCapturedBoltBackSample
                : MagazineInteractionBoltFallbackSoundSample(vr, false));
        const std::string prepared = ManualReloadPrepareConsoleSoundSample(sample);
        if (prepared.empty())
        {
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] no synthetic bolt-%s sample for model=%s weaponId=%d",
                forward ? "forward" : "back",
                vr->m_MagazineInteractionMagazineModelName.c_str(),
                vr->m_MagazineInteractionWeaponId);
            return;
        }

        const std::string command = "playvol \"" + prepared + "\" 1.000";
        if (forward)
        {
            vr->m_MagazineInteractionSyntheticBoltForwardSample = prepared;
            vr->m_MagazineInteractionSyntheticBoltForwardStarted = std::chrono::steady_clock::now();
        }
        else
        {
            vr->m_MagazineInteractionSyntheticBoltBackSample = prepared;
            vr->m_MagazineInteractionSyntheticBoltBackStarted = std::chrono::steady_clock::now();
        }
        vr->m_Game->ClientCmd_Unrestricted(command.c_str());
        if (forward)
        {
            vr->m_MagazineInteractionSyntheticBoltForwardSample.clear();
            vr->m_MagazineInteractionSyntheticBoltForwardStarted = {};
        }
        else
        {
            vr->m_MagazineInteractionSyntheticBoltBackSample.clear();
            vr->m_MagazineInteractionSyntheticBoltBackStarted = {};
        }
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] played synthetic bolt-%s sample=%s source=%s",
            forward ? "forward" : "back",
            prepared.c_str(),
            ((forward && !vr->m_MagazineInteractionCapturedBoltForwardSample.empty()) ||
                (!forward && !vr->m_MagazineInteractionCapturedBoltBackSample.empty()))
            ? "captured"
            : "fallback");
    }

    const char* MagazineInteractionBlockedFireEmptySound(int weaponId)
    {
        const auto id = static_cast<C_WeaponCSBase::WeaponID>(weaponId);
        return (id == C_WeaponCSBase::WeaponID::PISTOL ||
            id == C_WeaponCSBase::WeaponID::MAGNUM)
            ? "ClipEmpty_Pistol"
            : "ClipEmpty_Rifle";
    }

    bool MagazineInteractionSoundLooksClipEmpty(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clipempty") != std::string::npos ||
            lower.find("clip_empty") != std::string::npos ||
            lower.find("clip.empty") != std::string::npos ||
            lower.find("clip-empty") != std::string::npos;
    }

    bool MagazineInteractionReadActiveWeapon(
        C_BasePlayer* localPlayer,
        C_WeaponCSBase*& outWeapon,
        C_WeaponCSBase::WeaponID& outWeaponId,
        int& outClip)
    {
        outWeapon = nullptr;
        outWeaponId = C_WeaponCSBase::WeaponID::NONE;
        outClip = -1;
        if (!localPlayer)
            return false;
#if defined(_MSC_VER)
        __try
        {
#endif
            outWeapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
            if (!outWeapon)
                return false;
            outWeaponId = outWeapon->GetWeaponID();
            if (!MagazineInteractionTryReadInt(outWeapon, VR::kClip1Offset, outClip))
                return false;
            return true;
#if defined(_MSC_VER)
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            outWeapon = nullptr;
            outWeaponId = C_WeaponCSBase::WeaponID::NONE;
            outClip = -1;
            return false;
        }
#endif
    }
}

void VR::PublishMagazineInteractionBox(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ,
    const Vector& mins,
    const Vector& maxs,
    uint32_t frameSeq,
    int entityIndex,
    int boneIndex,
    const char* modelName)
{
    if (!m_MagazineInteractionEnabled && !m_MagazineBoxDebugEnabled)
        return;

    MagazineInteractionBoxSnapshot snapshot{};
    snapshot.origin = origin;
    snapshot.axisX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    snapshot.axisY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    snapshot.axisZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));
    snapshot.mins = mins;
    snapshot.maxs = maxs;
    snapshot.frameSeq = frameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.boneIndex = boneIndex;
    snapshot.modelName = modelName ? modelName : "";
    snapshot.publishedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
    snapshot.publishSeq = ++m_MagazineInteractionPublishSeq;
    m_MagazineInteractionBox = snapshot;
    m_MagazineInteractionBoxValid = true;
}

void VR::PublishMagazineInteractionBoltBox(
    const Vector& origin,
    const Vector& axisX,
    const Vector& axisY,
    const Vector& axisZ,
    const Vector& mins,
    const Vector& maxs,
    const Vector& pullAxisWorld,
    uint32_t frameSeq,
    int entityIndex,
    int boneIndex,
    const char* modelName)
{
    if (!m_MagazineInteractionEnabled && !m_MagazineBoxDebugEnabled)
        return;

    MagazineInteractionBoxSnapshot snapshot{};
    snapshot.origin = origin;
    snapshot.axisX = MagazineInteractionNormalizeAxis(axisX, Vector(1.0f, 0.0f, 0.0f));
    snapshot.axisY = MagazineInteractionNormalizeAxis(axisY, Vector(0.0f, 1.0f, 0.0f));
    snapshot.axisZ = MagazineInteractionNormalizeAxis(axisZ, Vector(0.0f, 0.0f, 1.0f));
    snapshot.pullAxisWorld = MagazineInteractionNormalizeAxis(pullAxisWorld, Vector(0.0f, 0.0f, 0.0f));
    snapshot.mins = mins;
    snapshot.maxs = maxs;
    snapshot.frameSeq = frameSeq;
    snapshot.entityIndex = entityIndex;
    snapshot.boneIndex = boneIndex;
    snapshot.modelName = modelName ? modelName : "";
    snapshot.publishedAt = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
    snapshot.publishSeq = ++m_MagazineInteractionBoltPublishSeq;
    m_MagazineInteractionBoltBox = snapshot;
    m_MagazineInteractionBoltBoxValid = true;
}

bool VR::GetMagazineInteractionBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoxMutex);
    if (!m_MagazineInteractionBoxValid)
        return false;
    outSnapshot = m_MagazineInteractionBox;
    return true;
}

bool VR::GetMagazineInteractionBoltBox(MagazineInteractionBoxSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(m_MagazineInteractionBoltBoxMutex);
    if (!m_MagazineInteractionBoltBoxValid)
        return false;
    outSnapshot = m_MagazineInteractionBoltBox;
    return true;
}

bool VR::IsMagazineInteractionLeftHandActive() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt ||
        m_MagazineInteractionSuppressLeftInputUntilRelease ||
        m_MagazineInteractionLeftHandHolding;
}

bool VR::IsMagazineInteractionManualActive() const
{
    return m_MagazineInteractionState != MagazineInteractionManualState::Idle;
}

void VR::MarkMagazineInteractionReloadCommandIssued()
{
    if (m_MagazineInteractionReloadCommandPending && !m_MagazineInteractionReloadCommandIssued)
    {
        Game::logMsg(
            "[VR][MagazineInteraction] backend reload command issued after physical old magazine pull");
    }
    m_MagazineInteractionReloadCommandPending = false;
    m_MagazineInteractionReloadCommandIssued = true;
}

bool VR::IsMagazineInteractionReloadCommandActive() const
{
    return m_MagazineInteractionReloadCommandPending ||
        (m_MagazineInteractionReloadTriggered && m_MagazineInteractionReloadCommandIssued &&
            m_MagazineInteractionReloadCommandHoldUntil.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() < m_MagazineInteractionReloadCommandHoldUntil);
}

bool VR::ShouldSuppressMagazineInteractionEmptyClipAutoReload(C_BasePlayer* localPlayer) const
{
    if (!m_MagazineInteractionEnabled ||
        !m_MagazineInteractionSuppressEmptyClipAutoReload ||
        !m_IsVREnabled ||
        !m_VrHandsEnabled)
    {
        return false;
    }
    if (IsMagazineInteractionReloadCommandActive())
        return false;

    C_BasePlayer* player = localPlayer;
    if (!player && m_Game && m_Game->m_EngineClient)
    {
        const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
        if (localPlayerIndex > 0)
            player = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
    }

    C_WeaponCSBase* activeWeapon = nullptr;
    C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
    int activeClip = -1;
    if (!MagazineInteractionReadActiveWeapon(player, activeWeapon, activeWeaponId, activeClip) ||
        !activeWeapon ||
        !MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId) ||
        activeClip != 0)
    {
        return false;
    }

    static std::chrono::steady_clock::time_point s_lastLog{};
    const auto now = std::chrono::steady_clock::now();
    if (s_lastLog.time_since_epoch().count() == 0 ||
        std::chrono::duration<float>(now - s_lastLog).count() >= 1.0f)
    {
        s_lastLog = now;
        Game::logMsg(
            "[VR][MagazineInteraction] suppressing empty-clip automatic reload weaponId=%d clip=%d; physical magazine reload required",
            static_cast<int>(activeWeaponId),
            activeClip);
    }
    return true;
}

bool VR::IsMagazineInteractionBlockingFire() const
{
    return IsMagazineInteractionManualActive();
}

void VR::PlayMagazineInteractionBlockedFireEmptySound()
{
    if (!m_Game)
        return;

    const bool magazineInteractionBlocksFire = IsMagazineInteractionBlockingFire();
    int weaponId = magazineInteractionBlocksFire ? m_MagazineInteractionWeaponId : 0;

    if (weaponId == 0 || !magazineInteractionBlocksFire)
    {
        C_BasePlayer* player = nullptr;
        if (m_Game->m_EngineClient)
        {
            const int localPlayerIndex = m_Game->m_EngineClient->GetLocalPlayer();
            if (localPlayerIndex > 0)
                player = reinterpret_cast<C_BasePlayer*>(m_Game->GetClientEntity(localPlayerIndex));
        }

        C_WeaponCSBase* activeWeapon = nullptr;
        C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
        int activeClip = -1;
        if (!MagazineInteractionReadActiveWeapon(player, activeWeapon, activeWeaponId, activeClip) ||
            activeClip != 0)
        {
            if (!magazineInteractionBlocksFire)
                return;
        }
        else
        {
            weaponId = static_cast<int>(activeWeaponId);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_MagazineInteractionEmptyFireSoundLastPlayed.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(now - m_MagazineInteractionEmptyFireSoundLastPlayed).count() < 0.12f)
    {
        return;
    }

    const char* soundName = MagazineInteractionBlockedFireEmptySound(weaponId);
    const char* soundPath = (std::strcmp(soundName, "ClipEmpty_Pistol") == 0)
        ? "weapons/clipempty_pistol.wav"
        : "weapons/clipempty_rifle.wav";
    const std::string soundSpec = std::string("game:") + soundPath;
    const bool feedbackPlayed = TryPlayKillSoundSpec(soundSpec, 1.0f, nullptr, false);
    if (!feedbackPlayed)
    {
        const std::string command = std::string("play ") + soundPath;
        m_Game->ClientCmd_Unrestricted(command.c_str());
    }
    m_MagazineInteractionEmptyFireSoundLastPlayed = now;
    Game::logMsg(
        "[VR][MagazineInteraction][Audio] played blocked-fire empty sound name=%s spec=%s feedback=%d weaponId=%d state=%d blocking=%d",
        soundName,
        soundSpec.c_str(),
        feedbackPlayed ? 1 : 0,
        weaponId,
        static_cast<int>(m_MagazineInteractionState),
        magazineInteractionBlocksFire ? 1 : 0);
}

bool VR::ShouldFreezeMagazineInteractionViewmodel() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt;
}

bool VR::ShouldHideMagazineInteractionNativeClip() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
}

bool VR::ShouldDrawMagazineInteractionDetachedMagazine() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
}

bool VR::GetMagazineInteractionDetachedMagazineWorld(VrHandMatrix4& outWorld) const
{
    if (!ShouldDrawMagazineInteractionDetachedMagazine())
        return false;
    std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
    outWorld = m_MagazineInteractionDetachedMagazineWorld;
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

bool VR::ShouldMoveMagazineInteractionBolt() const
{
    return m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt &&
        m_MagazineInteractionBoltRestValid &&
        MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltWorld);
}

bool VR::GetMagazineInteractionBoltWorld(VrHandMatrix4& outWorld) const
{
    if (!ShouldMoveMagazineInteractionBolt())
        return false;
    outWorld = m_MagazineInteractionBoltWorld;
    return MagazineInteractionMatrixLooksRenderable(outWorld);
}

void VR::CancelMagazineInteractionManual()
{
    const bool wasActive = IsMagazineInteractionManualActive() || m_MagazineInteractionLeftHandHolding;
    m_MagazineInteractionState = MagazineInteractionManualState::Idle;
    m_MagazineInteractionLeftHandHolding = false;
    m_MagazineInteractionReloadTriggered = false;
    m_MagazineInteractionReloadCommandPending = false;
    m_MagazineInteractionReloadCommandIssued = false;
    m_MagazineInteractionReloadCommandHoldUntil = {};
    m_MagazineInteractionOldMagazinePulled = false;
    m_MagazineInteractionWeapon = nullptr;
    m_MagazineInteractionWeaponId = 0;
    m_MagazineInteractionStartClip = -1;
    m_MagazineInteractionMagazineBoneIndex = -1;
    m_MagazineInteractionViewmodelEntityIndex = -1;
    m_MagazineInteractionMagazineModelName.clear();
    m_MagazineInteractionSocketValid = false;
    m_MagazineInteractionBoltRestValid = false;
    m_MagazineInteractionSocketWorld = {};
    m_MagazineInteractionBoltRestWorld = {};
    m_MagazineInteractionBoltWorld = {};
    m_MagazineInteractionControllerToMagazine = {};
    {
        std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
        m_MagazineInteractionDetachedMagazineWorld = {};
    }
    m_MagazineInteractionBoltRestBox = {};
    m_MagazineInteractionBoltPullAxisWorld = {};
    m_MagazineInteractionGrabStartLeftControllerPosAbs = {};
    m_MagazineInteractionHeldMagazineCenterOffsetLocal = {};
    m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
    m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
    m_MagazineInteractionBoltPullDistance = 0.0f;
    m_MagazineInteractionBoltMaxPullDistance = 0.0f;
    m_MagazineInteractionBoltReachedRear = false;
    m_MagazineInteractionBoltPullAxisSignLocked = false;
    m_MagazineInteractionStarted = {};
    m_MagazineInteractionFreshGrabbedAt = {};
    m_MagazineInteractionPostInsertStarted = {};
    m_MagazineInteractionBoltStageStarted = {};
    m_MagazineInteractionBoltGrabbedAt = {};
    m_MagazineInteractionSyntheticClipOutSample.clear();
    m_MagazineInteractionSyntheticClipOutStarted = {};
    m_MagazineInteractionSyntheticClipInSample.clear();
    m_MagazineInteractionSyntheticClipInStarted = {};
    m_MagazineInteractionSyntheticBoltBackSample.clear();
    m_MagazineInteractionSyntheticBoltBackStarted = {};
    m_MagazineInteractionSyntheticBoltForwardSample.clear();
    m_MagazineInteractionSyntheticBoltForwardStarted = {};
    m_MagazineInteractionCapturedBoltBackSample.clear();
    m_MagazineInteractionCapturedBoltForwardSample.clear();
    m_MagazineInteractionCapturedBoltBackSoundScore = -1;
    m_MagazineInteractionCapturedBoltForwardSoundScore = -1;
    m_MagazineInteractionEmptyFireSoundLastPlayed = {};
    m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
    if (wasActive)
        Game::logMsg("[VR][MagazineInteraction] reset manual magazine interaction state");
}

bool VR::UpdateMagazineInteraction(C_BasePlayer* localPlayer, bool leftGripDown, bool leftGripJustPressed)
{
    const auto now = std::chrono::steady_clock::now();
    if (m_MagazineInteractionSuppressLeftInputUntilRelease && !leftGripDown)
    {
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        Game::logMsg("[VR][MagazineInteraction] left grip released after physical reload; normal left-hand input restored");
    }
    if (m_MagazineInteractionSuppressLeftInputUntilRelease)
        return false;
    constexpr float kMagazineInteractionReloadCommandHoldSeconds = 0.35f;
    auto reloadCommandPending = [&]() -> bool
    {
        if (m_MagazineInteractionReloadCommandPending && !m_MagazineInteractionReloadCommandIssued)
            return true;
        return m_MagazineInteractionReloadTriggered &&
            m_MagazineInteractionReloadCommandIssued &&
            m_MagazineInteractionReloadCommandHoldUntil.time_since_epoch().count() != 0 &&
            now < m_MagazineInteractionReloadCommandHoldUntil;
    };

    auto startImmediateReloadCommand = [&](const char* reason)
    {
        m_MagazineInteractionReloadTriggered = true;
        m_MagazineInteractionReloadCommandPending = true;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<float>(kMagazineInteractionReloadCommandHoldSeconds));
        Game::logMsg(
            "[VR][MagazineInteraction] clip-out accepted; immediate backend reload command queued reason=%s hold=%.2fs",
            reason ? reason : "unknown",
            kMagazineInteractionReloadCommandHoldSeconds);
    };

    auto setDetachedMagazineWorld = [&](const VrHandMatrix4& world)
    {
        if (!MagazineInteractionMatrixLooksRenderable(world))
        {
            static std::chrono::steady_clock::time_point s_lastBadWorldLog{};
            if (s_lastBadWorldLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - s_lastBadWorldLog).count() >= 0.50f)
            {
                s_lastBadWorldLog = now;
                Game::logMsg(
                    "[VR][MagazineInteraction] ignored invalid detached magazine world origin=(%.2f %.2f %.2f)",
                    VrHandMath::Get(world, 0, 3),
                    VrHandMath::Get(world, 1, 3),
                    VrHandMath::Get(world, 2, 3));
            }
            return;
        }
        std::lock_guard<std::mutex> lock(m_MagazineInteractionPoseMutex);
        m_MagazineInteractionDetachedMagazineWorld = world;
    };

    auto buildHeldMagazineWorldFromLeftHand = [&]() -> VrHandMatrix4
    {
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildControllerWorldFromAxes(
            m_LeftControllerPosAbs,
            m_LeftControllerForward,
            m_LeftControllerRight,
            m_LeftControllerUp);
        VrHandMatrix4 orientationWorld = MagazineInteractionBuildWorldFromControllerRelation(
            controllerWorld,
            m_MagazineInteractionControllerToMagazine);
        if (!MagazineInteractionMatrixBasisLooksValid(orientationWorld))
            orientationWorld = MagazineInteractionBuildFreshHandMagazineWorld(this);

        const Vector desiredCenter =
            MagazineInteractionMatrixOrigin(controllerWorld) +
            MagazineInteractionMatrixLocalVectorToWorld(
                controllerWorld,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal);
        return MagazineInteractionBuildWorldAtBoxCenter(
            orientationWorld,
            m_MagazineInteractionSocketBox,
            desiredCenter);
    };

    auto updateDetachedMagazineFromLeftHand = [&]()
    {
        setDetachedMagazineWorld(buildHeldMagazineWorldFromLeftHand());
    };

    auto updateFreshDetachedMagazineFromLeftHand = [&]()
    {
        setDetachedMagazineWorld(buildHeldMagazineWorldFromLeftHand());
    };

    auto refreshSocketFromPublishedViewmodelBox = [&]()
    {
        if (!m_MagazineInteractionSocketValid)
            return;

        MagazineInteractionBoxSnapshot box{};
        if (!GetMagazineInteractionBox(box))
            return;

        const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
            return;
        if (m_MagazineInteractionViewmodelEntityIndex >= 0 &&
            box.entityIndex != m_MagazineInteractionViewmodelEntityIndex)
        {
            return;
        }
        if (m_MagazineInteractionMagazineBoneIndex >= 0 &&
            box.boneIndex != m_MagazineInteractionMagazineBoneIndex)
        {
            return;
        }
        if (!m_MagazineInteractionMagazineModelName.empty() &&
            !box.modelName.empty() &&
            box.modelName != m_MagazineInteractionMagazineModelName)
        {
            return;
        }

        const VrHandMatrix4 boxWorld = MagazineInteractionBuildBoxWorld(box);
        if (!MagazineInteractionMatrixLooksRenderable(boxWorld))
            return;

        m_MagazineInteractionSocketBox = box;
        m_MagazineInteractionSocketWorld = boxWorld;
    };

    auto setBoltPullDistance = [&](float pullDistance)
    {
        const float maxPull = std::max(
            0.0f,
            std::max(m_MagazineInteractionBoltMaxPullDistance,
                m_MagazineInteractionBoltPullDistanceMeters * m_VRScale));
        m_MagazineInteractionBoltPullDistance = std::clamp(pullDistance, 0.0f, maxPull);
        if (!m_MagazineInteractionBoltRestValid ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltRestWorld))
        {
            m_MagazineInteractionBoltWorld = {};
            return;
        }

        Vector axis = VrHandMath::Normalize(m_MagazineInteractionBoltPullAxisWorld);
        if (axis.Length() <= 0.0001f)
            axis = MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltPullAxisWorld = axis;

        const Vector restOrigin = MagazineInteractionMatrixOrigin(m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltWorld = MagazineInteractionMatrixWithOrigin(
            m_MagazineInteractionBoltRestWorld,
            restOrigin + axis * m_MagazineInteractionBoltPullDistance);
    };

    auto getFreshBoltBoxForActiveViewmodel = [&](MagazineInteractionBoxSnapshot& box) -> bool
    {
        if (!GetMagazineInteractionBoltBox(box))
            return false;

        const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
            return false;
        if (m_MagazineInteractionViewmodelEntityIndex >= 0 &&
            box.entityIndex != m_MagazineInteractionViewmodelEntityIndex)
        {
            return false;
        }
        if (!m_MagazineInteractionMagazineModelName.empty() &&
            !box.modelName.empty() &&
            box.modelName != m_MagazineInteractionMagazineModelName)
        {
            return false;
        }
        return MagazineInteractionMatrixLooksRenderable(MagazineInteractionBuildBoxWorld(box));
    };

    auto refreshBoltFromPublishedViewmodelBox = [&]()
    {
        if (!m_MagazineInteractionBoltRestValid)
            return;

        MagazineInteractionBoxSnapshot box{};
        if (!getFreshBoltBoxForActiveViewmodel(box))
            return;

        const VrHandMatrix4 boxWorld = MagazineInteractionBuildBoxWorld(box);
        if (!MagazineInteractionMatrixLooksRenderable(boxWorld))
            return;

        m_MagazineInteractionBoltRestBox = box;
        m_MagazineInteractionBoltRestWorld = boxWorld;
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        setBoltPullDistance(m_MagazineInteractionBoltPullDistance);
    };

    auto beginBoltStage = [&](C_WeaponCSBase::WeaponID weaponId, const char* reason) -> bool
    {
        if (!MagazineInteractionWeaponRequiresManualBolt(weaponId))
            return false;

        MagazineInteractionBoxSnapshot boltBox{};
        if (!getFreshBoltBoxForActiveViewmodel(boltBox))
        {
            Game::logMsg(
                "[VR][MagazineInteraction] cannot start bolt stage: no fresh bolt/slide box weaponId=%d model=%s reason=%s",
                static_cast<int>(weaponId),
                m_MagazineInteractionMagazineModelName.c_str(),
                reason ? reason : "unknown");
            return false;
        }

        const float requiredPull = std::max(0.0f, m_MagazineInteractionBoltPullDistanceMeters) * m_VRScale;
        if (requiredPull <= 0.001f)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] skipped bolt stage because pull distance is disabled weaponId=%d reason=%s",
                static_cast<int>(weaponId),
                reason ? reason : "unknown");
            return false;
        }

        m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionBoltRestBox = boltBox;
        m_MagazineInteractionBoltRestValid = true;
        m_MagazineInteractionBoltRestWorld = MagazineInteractionBuildBoxWorld(boltBox);
        m_MagazineInteractionBoltPullAxisWorld =
            MagazineInteractionBuildBoltPullAxisWorld(this, m_MagazineInteractionBoltRestBox, m_MagazineInteractionBoltRestWorld);
        m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = {};
        m_MagazineInteractionBoltGrabStartPullDistance = 0.0f;
        m_MagazineInteractionBoltPullDistance = 0.0f;
        m_MagazineInteractionBoltMaxPullDistance = requiredPull;
        m_MagazineInteractionBoltReachedRear = false;
        m_MagazineInteractionBoltPullAxisSignLocked = false;
        m_MagazineInteractionBoltStageStarted = now;
        m_MagazineInteractionBoltGrabbedAt = {};
        setBoltPullDistance(0.0f);
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        const Vector boltBoxOffsetMeters =
            (m_VRScale > 0.001f)
            ? ((boltBox.mins + boltBox.maxs) * (0.5f / m_VRScale))
            : Vector(0.0f, 0.0f, 0.0f);
        Game::logMsg(
            "[VR][MagazineInteraction] backend reload complete; bolt stage armed weaponId=%d ent=%d bone=%d pull=%.2f return=%.2f axis=(%.2f %.2f %.2f) boxOffsetMeters=(%.3f %.3f %.3f) model=%s reason=%s",
            static_cast<int>(weaponId),
            boltBox.entityIndex,
            boltBox.boneIndex,
            requiredPull,
            std::max(0.0f, m_MagazineInteractionBoltReturnDistanceMeters) * m_VRScale,
            m_MagazineInteractionBoltPullAxisWorld.x,
            m_MagazineInteractionBoltPullAxisWorld.y,
            m_MagazineInteractionBoltPullAxisWorld.z,
            boltBoxOffsetMeters.x,
            boltBoxOffsetMeters.y,
            boltBoxOffsetMeters.z,
            boltBox.modelName.c_str(),
            reason ? reason : "unknown");
        return true;
    };

    auto completeBoltStage = [&](const char* reason, bool suppressLeftInputUntilRelease)
    {
        setBoltPullDistance(0.0f);
        MagazineInteractionPlayBoltSound(this, true);
        TriggerPhysicalHandHapticPulse(true, 0.030f, 95.0f, 0.45f, 2);
        Game::logMsg(
            "[VR][MagazineInteraction] bolt returned to battery; physical reload complete reason=%s",
            reason ? reason : "unknown");
        CancelMagazineInteractionManual();
        if (suppressLeftInputUntilRelease)
        {
            m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            Game::logMsg("[VR][MagazineInteraction] bolt completed while left grip is still held; suppressing normal reload until release");
        }
    };

    auto detachedMagazineFitsSocket = [&]() -> bool
    {
        if (!m_MagazineInteractionSocketValid)
            return false;

        const VrHandMatrix4 socketWorld = m_MagazineInteractionSocketWorld;
        const VrHandMatrix4 detachedWorld = m_MagazineInteractionDetachedMagazineWorld;
        if (!MagazineInteractionMatrixLooksRenderable(socketWorld) ||
            !MagazineInteractionMatrixLooksRenderable(detachedWorld))
        {
            return false;
        }

        const float minDot = std::cos(std::clamp(m_MagazineInteractionSocketCaptureAngleDeg, 0.0f, 89.0f) *
            3.14159265358979323846f / 180.0f);
        const Vector detachedAxes[3] =
        {
            MagazineInteractionMatrixAxis(detachedWorld, 0),
            MagazineInteractionMatrixAxis(detachedWorld, 1),
            MagazineInteractionMatrixAxis(detachedWorld, 2)
        };
        const Vector socketAxes[3] =
        {
            MagazineInteractionMatrixAxis(socketWorld, 0),
            MagazineInteractionMatrixAxis(socketWorld, 1),
            MagazineInteractionMatrixAxis(socketWorld, 2)
        };
        for (int axis = 0; axis < 3; ++axis)
        {
            if (std::fabs(VrHandMath::Dot(detachedAxes[axis], socketAxes[axis])) < minDot)
                return false;
        }

        const int insertionAxis = std::clamp(
            MagazineInteractionDominantAxis(m_ManualReloadMagazineInsertionAxisLocal),
            0,
            2);
        const float overlapFraction = std::clamp(
            m_MagazineInteractionSocketRequiredOverlapFraction,
            0.0f,
            1.0f);
        const float requiredDepth = std::max(
            0.0f,
            m_MagazineInteractionSocketRequiredDepthMeters) * m_VRScale;
        for (int axis = 0; axis < 3; ++axis)
        {
            float socketMin = 0.0f;
            float socketMax = 0.0f;
            float detachedMin = 0.0f;
            float detachedMax = 0.0f;
            MagazineInteractionProjectBoxOntoAxis(
                socketWorld,
                m_MagazineInteractionSocketBox,
                socketAxes[axis],
                socketMin,
                socketMax);
            MagazineInteractionProjectBoxOntoAxis(
                detachedWorld,
                m_MagazineInteractionSocketBox,
                socketAxes[axis],
                detachedMin,
                detachedMax);

            const float socketSpan = std::max(0.001f, socketMax - socketMin);
            const float detachedSpan = std::max(0.001f, detachedMax - detachedMin);
            const float overlap = MagazineInteractionIntervalOverlap(
                socketMin,
                socketMax,
                detachedMin,
                detachedMax);
            const float requiredOverlap = (axis == insertionAxis)
                ? std::max(requiredDepth, std::min(socketSpan, detachedSpan) * overlapFraction)
                : std::min(socketSpan, detachedSpan) * overlapFraction;
            if (overlap < requiredOverlap)
                return false;
        }
        return true;
    };

    C_WeaponCSBase* activeWeapon = nullptr;
    C_WeaponCSBase::WeaponID activeWeaponId = C_WeaponCSBase::WeaponID::NONE;
    int activeClip = -1;
    const bool hasActiveWeapon = MagazineInteractionReadActiveWeapon(
        localPlayer,
        activeWeapon,
        activeWeaponId,
        activeClip);

    if (!m_MagazineInteractionEnabled || !m_IsVREnabled || !m_VrHandsEnabled || !hasActiveWeapon)
    {
        CancelMagazineInteractionManual();
        return false;
    }

    auto beginMagazineInteractionSession = [&](const MagazineInteractionBoxSnapshot& box)
    {
        m_MagazineInteractionReloadTriggered = false;
        m_MagazineInteractionReloadCommandPending = false;
        m_MagazineInteractionReloadCommandIssued = false;
        m_MagazineInteractionReloadCommandHoldUntil = {};
        m_MagazineInteractionSuppressLeftInputUntilRelease = false;
        m_MagazineInteractionOldMagazinePulled = false;
        m_MagazineInteractionWeapon = activeWeapon;
        m_MagazineInteractionWeaponId = static_cast<int>(activeWeaponId);
        m_MagazineInteractionStartClip = activeClip;
        m_MagazineInteractionMagazineBoneIndex = box.boneIndex;
        m_MagazineInteractionViewmodelEntityIndex = box.entityIndex;
        m_MagazineInteractionMagazineModelName = box.modelName;
        m_MagazineInteractionSocketBox = box;
        m_MagazineInteractionSocketValid = true;
        m_MagazineInteractionSocketWorld = MagazineInteractionBuildBoxWorld(box);
        setDetachedMagazineWorld(m_MagazineInteractionSocketWorld);
        m_MagazineInteractionGrabStartLeftControllerPosAbs = m_LeftControllerPosAbs;
        m_MagazineInteractionStarted = now;
        m_MagazineInteractionFreshGrabbedAt = {};
        m_MagazineInteractionPostInsertStarted = {};
        m_MagazineInteractionSyntheticClipOutSample.clear();
        m_MagazineInteractionSyntheticClipOutStarted = {};
        m_MagazineInteractionSyntheticClipInSample.clear();
        m_MagazineInteractionSyntheticClipInStarted = {};
        m_MagazineInteractionEmptyFireSoundLastPlayed = {};
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
    };

    auto getMagazineBoxForAutoEject = [&](MagazineInteractionBoxSnapshot& box, float& ageSeconds, bool& usedCachedBox) -> bool
    {
        ageSeconds = -1.0f;
        usedCachedBox = false;
        const float freshMaxAgeSeconds = std::max(0.02f, m_MagazineInteractionStaleSeconds);
        static std::chrono::steady_clock::time_point s_lastAutoEjectBoxLog{};
        auto logLimited = [&](const char* reason, int entityIndex, int boneIndex)
        {
            if (s_lastAutoEjectBoxLog.time_since_epoch().count() != 0 &&
                std::chrono::duration<float>(now - s_lastAutoEjectBoxLog).count() < 1.0f)
            {
                return;
            }
            s_lastAutoEjectBoxLog = now;
            Game::logMsg(
                "[VR][MagazineInteraction] empty clip auto-eject cannot start: %s age=%.3fs freshMax=%.3fs ent=%d bone=%d",
                reason ? reason : "unknown",
                ageSeconds,
                freshMaxAgeSeconds,
                entityIndex,
                boneIndex);
        };

        if (!GetMagazineInteractionBox(box))
        {
            logLimited("no published magazine box", -1, -1);
            return false;
        }

        ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
        if (ageSeconds <= freshMaxAgeSeconds)
            return true;

        if (MagazineInteractionMatrixLooksRenderable(MagazineInteractionBuildBoxWorld(box)))
        {
            usedCachedBox = true;
            if (s_lastAutoEjectBoxLog.time_since_epoch().count() == 0 ||
                std::chrono::duration<float>(now - s_lastAutoEjectBoxLog).count() >= 1.0f)
            {
                s_lastAutoEjectBoxLog = now;
                Game::logMsg(
                    "[VR][MagazineInteraction] empty clip auto-eject using last known magazine box age=%.3fs freshMax=%.3fs ent=%d bone=%d model=%s",
                    ageSeconds,
                    freshMaxAgeSeconds,
                    box.entityIndex,
                    box.boneIndex,
                    box.modelName.c_str());
            }
            return true;
        }

        logLimited("last known magazine box invalid", box.entityIndex, box.boneIndex);
        return false;
    };

    if (IsMagazineInteractionManualActive() && activeWeapon != m_MagazineInteractionWeapon)
    {
        Game::logMsg("[VR][MagazineInteraction] active weapon changed; canceling manual magazine interaction");
        CancelMagazineInteractionManual();
        return false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        refreshSocketFromPublishedViewmodelBox();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload)
    {
        const float elapsed = std::chrono::duration<float>(now - m_MagazineInteractionPostInsertStarted).count();
        const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, activeClip);
        const bool clipUpdated =
            activeClip > m_MagazineInteractionStartClip ||
            (maxClip > 0 && activeClip >= maxClip && activeClip != m_MagazineInteractionStartClip);
        if ((elapsed >= 0.20f && clipUpdated) || elapsed >= 3.0f)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] native reload animation skipped; backend reload wait complete elapsed=%.3fs clip=%d startClip=%d max=%d updated=%d; entering bolt stage",
                elapsed,
                activeClip,
                m_MagazineInteractionStartClip,
                maxClip,
                clipUpdated ? 1 : 0);

            if (!beginBoltStage(activeWeaponId, clipUpdated ? "clip-updated" : "backend-timeout"))
            {
                const bool suppressLeftInputUntilRelease = leftGripDown;
                CancelMagazineInteractionManual();
                if (suppressLeftInputUntilRelease)
                {
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
                    Game::logMsg("[VR][MagazineInteraction] backend reload complete while left grip is still held; suppressing normal reload until release");
                }
            }
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab)
    {
        refreshBoltFromPublishedViewmodelBox();
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        setBoltPullDistance(0.0f);

        if (!m_MagazineInteractionBoltRestValid)
        {
            const float elapsed = std::chrono::duration<float>(now - m_MagazineInteractionBoltStageStarted).count();
            if (elapsed >= 2.0f)
            {
                Game::logMsg("[VR][MagazineInteraction] bolt stage lost rest pose; completing reload without physical bolt after %.3fs", elapsed);
                const bool suppressLeftInputUntilRelease = leftGripDown;
                CancelMagazineInteractionManual();
                if (suppressLeftInputUntilRelease)
                    m_MagazineInteractionSuppressLeftInputUntilRelease = true;
            }
            return false;
        }

        if (leftGripDown)
        {
            const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
                m_MagazineInteractionBoltRestBox,
                m_LeftControllerPosAbs,
                m_LeftControllerAngAbs,
                m_VRScale);
            const float grabRange = std::max(0.0f, m_MagazineInteractionBoltGrabPaddingMeters) * m_VRScale;
            if (grabDistance <= grabRange)
            {
                m_MagazineInteractionState = MagazineInteractionManualState::HoldingBolt;
                m_MagazineInteractionLeftHandHolding = true;
                m_MagazineInteractionBoltGrabStartLeftControllerPosAbs = m_LeftControllerPosAbs;
                m_MagazineInteractionBoltGrabStartPullDistance = m_MagazineInteractionBoltPullDistance;
                m_MagazineInteractionBoltGrabbedAt = now;
                m_MagazineInteractionBoltPullAxisSignLocked = false;
                m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
                Game::logMsg(
                    "[VR][MagazineInteraction] bolt grabbed distance=%.2f range=%.2f pull=%.2f axis=(%.2f %.2f %.2f) model=%s",
                    grabDistance,
                    grabRange,
                    m_MagazineInteractionBoltMaxPullDistance,
                    m_MagazineInteractionBoltPullAxisWorld.x,
                    m_MagazineInteractionBoltPullAxisWorld.y,
                    m_MagazineInteractionBoltPullAxisWorld.z,
                    m_MagazineInteractionBoltRestBox.modelName.c_str());
            }
            else if (leftGripJustPressed)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] bolt grab ignored; left hand outside bolt box distance=%.2f range=%.2f ent=%d bone=%d model=%s",
                    grabDistance,
                    grabRange,
                    m_MagazineInteractionBoltRestBox.entityIndex,
                    m_MagazineInteractionBoltRestBox.boneIndex,
                    m_MagazineInteractionBoltRestBox.modelName.c_str());
            }
        }
        return false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt)
    {
        if (!m_MagazineInteractionBoltRestValid ||
            !MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionBoltRestWorld))
        {
            Game::logMsg("[VR][MagazineInteraction] bolt hold lost rest pose; canceling bolt hold");
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            return false;
        }

        const float requiredPull = std::max(0.001f, m_MagazineInteractionBoltMaxPullDistance);
        const float returnDistance = std::clamp(
            m_MagazineInteractionBoltReturnDistanceMeters * m_VRScale,
            0.0f,
            requiredPull);

        if (!leftGripDown)
        {
            if (m_MagazineInteractionBoltReachedRear)
            {
                completeBoltStage("released-after-rear", false);
            }
            else
            {
                setBoltPullDistance(0.0f);
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBoltGrab;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionBoltGrabbedAt = {};
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                Game::logMsg("[VR][MagazineInteraction] bolt released before rear threshold; bolt returned to rest");
            }
            return false;
        }

        Vector axis = VrHandMath::Normalize(m_MagazineInteractionBoltPullAxisWorld);
        const Vector handDelta = m_LeftControllerPosAbs - m_MagazineInteractionBoltGrabStartLeftControllerPosAbs;
        float handPullDistance = VrHandMath::Dot(handDelta, axis);
        if (!m_MagazineInteractionBoltPullAxisSignLocked)
        {
            const float signLockDistance = std::max(0.10f, 0.006f * m_VRScale);
            if (std::fabs(handPullDistance) >= signLockDistance)
            {
                if (handPullDistance < 0.0f)
                {
                    axis = axis * -1.0f;
                    m_MagazineInteractionBoltPullAxisWorld = axis;
                    handPullDistance = -handPullDistance;
                }
                m_MagazineInteractionBoltPullAxisSignLocked = true;
                Game::logMsg(
                    "[VR][MagazineInteraction] bolt pull axis sign locked axis=(%.2f %.2f %.2f) firstProjected=%.2f",
                    axis.x,
                    axis.y,
                    axis.z,
                    handPullDistance);
            }
        }
        const float desiredPull = m_MagazineInteractionBoltGrabStartPullDistance + handPullDistance;
        setBoltPullDistance(desiredPull);
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);

        if (!m_MagazineInteractionBoltReachedRear &&
            m_MagazineInteractionBoltPullDistance >= requiredPull)
        {
            m_MagazineInteractionBoltReachedRear = true;
            MagazineInteractionPlayBoltSound(this, false);
            TriggerPhysicalHandHapticPulse(true, 0.024f, 110.0f, 0.38f, 2);
            Game::logMsg(
                "[VR][MagazineInteraction] bolt rear threshold reached pull=%.2f required=%.2f handProjected=%.2f",
                m_MagazineInteractionBoltPullDistance,
                requiredPull,
                handPullDistance);
        }

        if (m_MagazineInteractionBoltReachedRear &&
            m_MagazineInteractionBoltPullDistance <= returnDistance)
        {
            completeBoltStage("returned-to-battery", leftGripDown);
        }
        return false;
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine)
    {
        m_MagazineInteractionLeftHandHolding = false;
        m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
        MagazineInteractionBoxSnapshot pickupBox{};
        QAngle pickupAngles{};
        const bool hasPickupBox = MagazineInteractionBuildFreshMagazinePickupBox(this, pickupBox, pickupAngles);
        VrHandMatrix4 pickupMagazineWorld{};
        MagazineInteractionBoxSnapshot freshGrabBox{};
        if (hasPickupBox)
        {
            pickupMagazineWorld = MagazineInteractionBuildSocketOrientedMagazineWorldAtCenter(this, pickupBox.origin);
            freshGrabBox = MagazineInteractionBuildWorldBoxSnapshot(m_MagazineInteractionSocketBox, pickupMagazineWorld);
            if (m_MagazineBoxDebugEnabled)
            {
                QAngle freshGrabAngles = pickupAngles;
                QAngle::VectorAngles(
                    MagazineInteractionMatrixAxis(pickupMagazineWorld, 0),
                    MagazineInteractionMatrixAxis(pickupMagazineWorld, 2),
                    freshGrabAngles);
                MagazineInteractionDrawFreshMagazinePickupBox(this, freshGrabBox, freshGrabAngles);
            }
            setDetachedMagazineWorld(pickupMagazineWorld);
        }

        if (leftGripDown && leftGripJustPressed)
        {
            if (!hasPickupBox)
                return reloadCommandPending();

            const float grabDistance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
                freshGrabBox,
                m_LeftControllerPosAbs,
                m_LeftControllerAngAbs,
                m_VRScale);
            const float grabRange = std::max(0.0f, m_MagazineInteractionFreshMagazineGrabRangeMeters) * m_VRScale;
            if (grabDistance > grabRange)
            {
                Game::logMsg(
                    "[VR][MagazineInteraction] fresh magazine grab ignored; left hand outside fresh magazine box distance=%.2f range=%.2f",
                    grabDistance,
                    grabRange);
                return reloadCommandPending();
            }

            m_MagazineInteractionState = MagazineInteractionManualState::HoldingFreshMagazine;
            m_MagazineInteractionLeftHandHolding = true;
            const VrHandMatrix4 controllerWorld = MagazineInteractionBuildControllerWorldFromAxes(
                m_LeftControllerPosAbs,
                m_LeftControllerForward,
                m_LeftControllerRight,
                m_LeftControllerUp);
            const VrHandMatrix4 freshMagazineWorld = pickupMagazineWorld;
            m_MagazineInteractionControllerToMagazine =
                MagazineInteractionBuildControllerRelation(controllerWorld, freshMagazineWorld);
            const bool relationCaptured =
                MagazineInteractionMatrixBasisLooksValid(m_MagazineInteractionControllerToMagazine);
            setDetachedMagazineWorld(freshMagazineWorld);
            m_MagazineInteractionFreshGrabbedAt = now;
            m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
            const Vector freshClipOrigin = MagazineInteractionMatrixOrigin(freshMagazineWorld);
            const Vector freshCenterLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
            const Vector freshCenterWorld = MagazineInteractionMatrixPointWorld(freshMagazineWorld, freshCenterLocal);
            m_MagazineInteractionHeldMagazineCenterOffsetLocal =
                MagazineInteractionWorldVectorToMatrixLocal(
                    controllerWorld,
                    freshCenterWorld - MagazineInteractionMatrixOrigin(controllerWorld));
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine grabbed from fresh magazine box distance=%.2f range=%.2f relationCaptured=%d clipOrigin=(%.2f %.2f %.2f) visibleCenter=(%.2f %.2f %.2f) centerLocalOffset=(%.2f %.2f %.2f) centerLocal=(%.2f %.2f %.2f) model=%s; move it into MagazineSocket",
                grabDistance,
                grabRange,
                relationCaptured ? 1 : 0,
                freshClipOrigin.x,
                freshClipOrigin.y,
                freshClipOrigin.z,
                freshCenterWorld.x,
                freshCenterWorld.y,
                freshCenterWorld.z,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.x,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.y,
                m_MagazineInteractionHeldMagazineCenterOffsetLocal.z,
                freshCenterLocal.x,
                freshCenterLocal.y,
                freshCenterLocal.z,
                m_MagazineInteractionMagazineModelName.c_str());
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine)
    {
        const float freshGrabAgeSeconds =
            (m_MagazineInteractionFreshGrabbedAt.time_since_epoch().count() != 0)
            ? std::chrono::duration<float>(now - m_MagazineInteractionFreshGrabbedAt).count()
            : 999.0f;
        if (!leftGripDown && freshGrabAgeSeconds >= 0.18f)
        {
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine dropped before socket insertion age=%.3fs",
                freshGrabAgeSeconds);
            return reloadCommandPending();
        }

        updateFreshDetachedMagazineFromLeftHand();
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        if (detachedMagazineFitsSocket())
        {
            MagazineInteractionPlayClipInSound(this);
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForBackendReload;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionPostInsertStarted = now;
            m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
            Game::logMsg(
                "[VR][MagazineInteraction] fresh magazine inserted into MagazineSocket; waiting for backend reload clip=%d startClip=%d",
                activeClip,
                m_MagazineInteractionStartClip);
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine)
    {
        if (!leftGripDown)
        {
            if (m_MagazineInteractionReloadTriggered)
            {
                m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
                m_MagazineInteractionLeftHandHolding = false;
                m_MagazineInteractionLeftHandPoseActive.store(0, std::memory_order_relaxed);
                Game::logMsg("[VR][MagazineInteraction] pulled old magazine released; old magazine stays hidden, waiting for fresh magazine");
                return reloadCommandPending();
            }
            else
            {
                Game::logMsg("[VR][MagazineInteraction] old magazine released before pull threshold; restoring native magazine");
                CancelMagazineInteractionManual();
            }
            return false;
        }

        const VrHandMatrix4 heldMagazineWorld = buildHeldMagazineWorldFromLeftHand();
        setDetachedMagazineWorld(heldMagazineWorld);
        m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
        const float handPullDistance = (m_LeftControllerPosAbs - m_MagazineInteractionGrabStartLeftControllerPosAbs).Length();
        const float handTriggerDistance = std::max(0.0f, m_MagazineInteractionPullTriggerMeters) * m_VRScale;
        float magazinePullDistance = 0.0f;
        if (m_MagazineInteractionSocketValid &&
            MagazineInteractionMatrixLooksRenderable(m_MagazineInteractionSocketWorld) &&
            MagazineInteractionMatrixLooksRenderable(heldMagazineWorld))
        {
            const Vector centerLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
            const Vector socketCenter = MagazineInteractionMatrixPointWorld(
                m_MagazineInteractionSocketWorld,
                centerLocal);
            const Vector heldCenter = MagazineInteractionMatrixPointWorld(
                heldMagazineWorld,
                centerLocal);
            magazinePullDistance = (heldCenter - socketCenter).Length();
        }
        const float magazineTriggerDistance =
            std::max(0.0f, m_MagazineInteractionPullTriggerByMagazineMeters) * m_VRScale;
        const bool handPulled = handTriggerDistance <= 0.0f || handPullDistance >= handTriggerDistance;
        const bool magazinePulled = magazineTriggerDistance <= 0.0f || magazinePullDistance >= magazineTriggerDistance;
        if (!m_MagazineInteractionReloadTriggered && (handPulled || magazinePulled))
        {
            m_MagazineInteractionOldMagazinePulled = true;
            MagazineInteractionPlayClipOutSound(this);
            startImmediateReloadCommand("clip-out");
            Game::logMsg(
                "[VR][MagazineInteraction] old magazine pull threshold reached after clip-out handDistance=%.2f handThreshold=%.2f magazineDistance=%.2f magazineThreshold=%.2f",
                handPullDistance,
                handTriggerDistance,
                magazinePullDistance,
                magazineTriggerDistance);
            return reloadCommandPending();
        }
        return reloadCommandPending();
    }

    if (m_MagazineInteractionSuppressEmptyClipAutoReload &&
        activeClip == 0 &&
        MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
    {
        MagazineInteractionBoxSnapshot box{};
        float boxAgeSeconds = -1.0f;
        bool usedCachedBox = false;
        if (getMagazineBoxForAutoEject(box, boxAgeSeconds, usedCachedBox))
        {
            beginMagazineInteractionSession(box);
            m_MagazineInteractionState = MagazineInteractionManualState::WaitingForFreshMagazine;
            m_MagazineInteractionLeftHandHolding = false;
            m_MagazineInteractionOldMagazinePulled = true;
            MagazineInteractionPlayClipOutSound(this);
            startImmediateReloadCommand("empty-clip-auto-eject");
            Game::logMsg(
                "[VR][MagazineInteraction] empty clip auto-ejected magazine; waiting for fresh magazine weaponId=%d clip=%d ent=%d bone=%d age=%.3fs cached=%d model=%s",
                static_cast<int>(activeWeaponId),
                activeClip,
                box.entityIndex,
                box.boneIndex,
                boxAgeSeconds,
                usedCachedBox ? 1 : 0,
                box.modelName.c_str());
            return reloadCommandPending();
        }
    }

    if (!leftGripDown)
        return false;

    if (!MagazineInteractionWeaponUsesDetachableMagazine(activeWeaponId))
        return false;

    const int maxClip = MagazineInteractionDefaultMaxClip(activeWeaponId, activeClip);
    if (maxClip > 0 && activeClip >= maxClip)
    {
        if (leftGripJustPressed)
        {
            Game::logMsg(
                "[VR][MagazineInteraction] ignored full magazine weaponId=%d clip=%d max=%d",
                static_cast<int>(activeWeaponId),
                activeClip,
                maxClip);
        }
        return false;
    }

    MagazineInteractionBoxSnapshot box{};
    if (!GetMagazineInteractionBox(box))
        return false;

    const float ageSeconds = std::chrono::duration<float>(now - box.publishedAt).count();
    if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
        return false;

    const float grabPadding = std::max(0.0f, m_MagazineInteractionGrabPaddingMeters) * m_VRScale;
    const float distance = MagazineInteractionNearestLeftHandProbeDistanceSourceUnits(
        box,
        m_LeftControllerPosAbs,
        m_LeftControllerAngAbs,
        m_VRScale);
    if (distance > grabPadding)
    {
        static std::chrono::steady_clock::time_point s_lastMissLog{};
        if (s_lastMissLog.time_since_epoch().count() == 0 ||
            std::chrono::duration<float>(now - s_lastMissLog).count() >= 0.50f)
        {
            s_lastMissLog = now;
            Game::logMsg(
                "[VR][MagazineInteraction] left grip held but magazine box is out of reach nearest=%.2f padding=%.2f ent=%d bone=%d age=%.3fs",
                distance,
                grabPadding,
                box.entityIndex,
                box.boneIndex,
                ageSeconds);
        }
        return false;
    }

    beginMagazineInteractionSession(box);
    m_MagazineInteractionState = MagazineInteractionManualState::HoldingOldMagazine;
    m_MagazineInteractionLeftHandHolding = true;
    {
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildControllerWorldFromAxes(
            m_LeftControllerPosAbs,
            m_LeftControllerForward,
            m_LeftControllerRight,
            m_LeftControllerUp);
        m_MagazineInteractionControllerToMagazine =
            MagazineInteractionBuildControllerRelation(controllerWorld, m_MagazineInteractionSocketWorld);
    }
    const Vector socketCenterLocal = MagazineInteractionBoxCenterLocal(m_MagazineInteractionSocketBox);
    const Vector socketCenterWorld = MagazineInteractionMatrixPointWorld(
        m_MagazineInteractionSocketWorld,
        socketCenterLocal);
    {
        const VrHandMatrix4 controllerWorld = MagazineInteractionBuildControllerWorldFromAxes(
            m_LeftControllerPosAbs,
            m_LeftControllerForward,
            m_LeftControllerRight,
            m_LeftControllerUp);
        m_MagazineInteractionHeldMagazineCenterOffsetLocal =
            MagazineInteractionWorldVectorToMatrixLocal(
                controllerWorld,
                socketCenterWorld - MagazineInteractionMatrixOrigin(controllerWorld));
    }
    m_MagazineInteractionLeftHandPoseActive.store(1, std::memory_order_relaxed);
    Game::logMsg(
        "[VR][MagazineInteraction] old magazine grabbed; froze viewmodel and hid native clip weaponId=%d clip=%d ent=%d bone=%d distance=%.2f padding=%.2f centerLocalOffset=(%.2f %.2f %.2f) model=%s",
        static_cast<int>(activeWeaponId),
        activeClip,
        box.entityIndex,
        box.boneIndex,
        distance,
        grabPadding,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.x,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.y,
        m_MagazineInteractionHeldMagazineCenterOffsetLocal.z,
        box.modelName.c_str());
    return false;
}

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

    // Detached magazines are primarily rendered by repeating the native Source
    // viewmodel draw with every non-clip bone moved out of view. While a fresh
    // magazine is held, keep a lightweight standalone wire box visible as a
    // fallback because the matching weapon viewmodel draw is not guaranteed to
    // occur every frame.
    const VrHandMatrix4* manualReloadMagazineWorldPtr = nullptr;
    const bool manualReloadMagazineUseViewmodelLayer = false;
    VrHandMatrix4 standaloneMagazineBoxWorld{};
    const VrHandMatrix4* standaloneMagazineBoxWorldPtr = nullptr;
    Vector standaloneMagazineBoxMins(0.0f, 0.0f, 0.0f);
    Vector standaloneMagazineBoxMaxs(0.0f, 0.0f, 0.0f);
    const bool standaloneMagazineBoxUseViewmodelLayer = true;
    VrHandMatrix4 magazineSocketCaptureBoxWorld{};
    const VrHandMatrix4* magazineSocketCaptureBoxWorldPtr = nullptr;
    Vector magazineSocketCaptureBoxMins(0.0f, 0.0f, 0.0f);
    Vector magazineSocketCaptureBoxMaxs(0.0f, 0.0f, 0.0f);
    const bool magazineSocketCaptureBoxUseViewmodelLayer = true;
    VrHandMatrix4 currentMagazineBoxWorld{};
    const VrHandMatrix4* currentMagazineBoxWorldPtr = nullptr;
    Vector currentMagazineBoxMins(0.0f, 0.0f, 0.0f);
    Vector currentMagazineBoxMaxs(0.0f, 0.0f, 0.0f);
    const bool currentMagazineBoxUseViewmodelLayer = true;
    if (m_MagazineBoxDebugEnabled &&
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine &&
        m_MagazineInteractionSocketValid &&
        GetMagazineInteractionDetachedMagazineWorld(standaloneMagazineBoxWorld))
    {
        standaloneMagazineBoxWorldPtr = &standaloneMagazineBoxWorld;
        standaloneMagazineBoxMins = m_MagazineInteractionSocketBox.mins;
        standaloneMagazineBoxMaxs = m_MagazineInteractionSocketBox.maxs;
    }
    if (m_MagazineBoxDebugEnabled &&
        m_MagazineInteractionSocketValid &&
        (m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine))
    {
        magazineSocketCaptureBoxWorld = m_MagazineInteractionSocketWorld;
        magazineSocketCaptureBoxWorldPtr = &magazineSocketCaptureBoxWorld;
        magazineSocketCaptureBoxMins = m_MagazineInteractionSocketBox.mins;
        magazineSocketCaptureBoxMaxs = m_MagazineInteractionSocketBox.maxs;
    }
    if (m_MagazineBoxDebugEnabled)
    {
        auto tryUseCurrentDebugBox = [&](const MagazineInteractionBoxSnapshot& debugBox)
            {
                const float ageSeconds = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - debugBox.publishedAt).count();
                if (ageSeconds > std::max(0.02f, m_MagazineInteractionStaleSeconds))
                    return false;

                currentMagazineBoxWorld = MagazineInteractionBuildBoxWorld(debugBox);
                if (!MagazineInteractionMatrixLooksRenderable(currentMagazineBoxWorld))
                    return false;

                currentMagazineBoxWorldPtr = &currentMagazineBoxWorld;
                currentMagazineBoxMins = debugBox.mins;
                currentMagazineBoxMaxs = debugBox.maxs;
                return true;
            };

        const bool preferBoltBox =
            m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
            m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt;

        bool usedCurrentDebugBox = false;
        if (preferBoltBox)
        {
            MagazineInteractionBoxSnapshot boltDebugBox{};
            usedCurrentDebugBox = GetMagazineInteractionBoltBox(boltDebugBox) &&
                tryUseCurrentDebugBox(boltDebugBox);
        }
        if (!usedCurrentDebugBox)
        {
            MagazineInteractionBoxSnapshot debugBox{};
            if (GetMagazineInteractionBox(debugBox))
                tryUseCurrentDebugBox(debugBox);
        }
    }
    const Vector currentViewmodelPosition = GetRecommendedViewmodelAbsPos();
    const QAngle currentViewmodelAngles = GetRecommendedViewmodelAbsAngle();

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
        currentViewmodelPosition,
        currentViewmodelAngles,
        m_VrHandsLeftPoseOffsetMeters,
        m_VrHandsLeftPoseRotationOffsetDeg,
        m_VrHandsRightPoseOffsetMeters,
        m_VrHandsRightPoseRotationOffsetDeg,
        kNoManualReloadMagazineGlbPath,
        manualReloadMagazineWorldPtr,
        manualReloadMagazineUseViewmodelLayer,
        standaloneMagazineBoxWorldPtr,
        standaloneMagazineBoxMins,
        standaloneMagazineBoxMaxs,
        standaloneMagazineBoxUseViewmodelLayer,
        magazineSocketCaptureBoxWorldPtr,
        magazineSocketCaptureBoxMins,
        magazineSocketCaptureBoxMaxs,
        magazineSocketCaptureBoxUseViewmodelLayer,
        currentMagazineBoxWorldPtr,
        currentMagazineBoxMins,
        currentMagazineBoxMaxs,
        currentMagazineBoxUseViewmodelLayer,
        m_MagazineInteractionLeftHandPoseActive.load(std::memory_order_relaxed) != 0,
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
    constexpr float kManualReloadMinimumPostInsertTailSeconds = 0.75f;
    constexpr float kManualReloadPostInsertBoundaryWaitTimeoutSeconds = 8.25f;

    bool ManualReloadWeaponUsesDetachableMagazine(C_WeaponCSBase::WeaponID weaponId)
    {
        switch (weaponId)
        {
        case C_WeaponCSBase::WeaponID::PISTOL:
        case C_WeaponCSBase::WeaponID::UZI:
        case C_WeaponCSBase::WeaponID::M16A1:
        case C_WeaponCSBase::WeaponID::HUNTING_RIFLE:
        case C_WeaponCSBase::WeaponID::MAC10:
        case C_WeaponCSBase::WeaponID::SCAR:
        case C_WeaponCSBase::WeaponID::SNIPER_MILITARY:
        case C_WeaponCSBase::WeaponID::AK47:
        case C_WeaponCSBase::WeaponID::MAGNUM:
        case C_WeaponCSBase::WeaponID::MP5:
        case C_WeaponCSBase::WeaponID::SG552:
        case C_WeaponCSBase::WeaponID::AWP:
        case C_WeaponCSBase::WeaponID::SCOUT:
            return true;
        default:
            return false;
        }
    }

    Vector ManualReloadGetInsertionAxisLocal(const VR* vr)
    {
        if (!vr)
            return Vector(0.0f, -1.0f, 0.0f);
        return vr->m_ManualReloadResolvedInsertionAxisValid
            ? vr->m_ManualReloadResolvedInsertionAxisLocal
            : vr->m_ManualReloadMagazineInsertionAxisLocal;
    }

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

    bool ManualReloadSoundLooksWeaponRelated(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("weapon") != std::string::npos ||
            lower.find("reload") != std::string::npos ||
            lower.find("clip") != std::string::npos ||
            lower.find("mag") != std::string::npos ||
            lower.find("bolt") != std::string::npos ||
            lower.find("slide") != std::string::npos ||
            lower.find("rifle") != std::string::npos ||
            lower.find("smg") != std::string::npos ||
            lower.find("pistol") != std::string::npos ||
            lower.find("sniper") != std::string::npos;
    }

    bool ManualReloadSoundStartsInsertTail(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos ||
            lower.find("clip_locked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos;
    }

    bool MagazineInteractionSoundLooksClipOut(const char* sample)
    {
        if (!sample || !*sample)
            return false;

        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("clip_out") != std::string::npos ||
            lower.find("clip-out") != std::string::npos ||
            lower.find("clipout") != std::string::npos ||
            lower.find("mag_out") != std::string::npos ||
            lower.find("mag-out") != std::string::npos ||
            lower.find("magout") != std::string::npos ||
            lower.find("magazine_out") != std::string::npos ||
            lower.find("magazine-out") != std::string::npos ||
            lower.find("magazineout") != std::string::npos ||
            lower.find("clip.remove") != std::string::npos ||
            lower.find("mag.remove") != std::string::npos ||
            lower.find("magazine.remove") != std::string::npos;
    }

    enum class ManualReloadDelayedSoundStage
    {
        Other,
        Insert,
        Lock,
        Ready,
        SlideBack,
        SlideForward
    };

    std::string ManualReloadLowerSoundSample(const std::string& sample)
    {
        std::string lower(sample);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower;
    }

    ManualReloadDelayedSoundStage ManualReloadClassifyDelayedSound(const std::string& sample)
    {
        const std::string lower = ManualReloadLowerSoundSample(sample);
        if (lower.find("clip_in") != std::string::npos ||
            lower.find("clip-in") != std::string::npos ||
            lower.find("clipin") != std::string::npos ||
            lower.find("clip.insert") != std::string::npos ||
            lower.find("mag_in") != std::string::npos ||
            lower.find("mag-in") != std::string::npos ||
            lower.find("magin") != std::string::npos ||
            lower.find("magazine_in") != std::string::npos ||
            lower.find("magazine-in") != std::string::npos ||
            lower.find("magazinein") != std::string::npos ||
            lower.find("mag.insert") != std::string::npos ||
            lower.find("insert") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::Insert;
        }
        if (lower.find("clip_locked") != std::string::npos ||
            lower.find("clip-locked") != std::string::npos ||
            lower.find("cliplocked") != std::string::npos ||
            lower.find("mag_locked") != std::string::npos ||
            lower.find("mag-locked") != std::string::npos ||
            lower.find("maglocked") != std::string::npos ||
            lower.find("magazine_locked") != std::string::npos ||
            lower.find("magazine-locked") != std::string::npos ||
            lower.find("magazinelocked") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::Lock;
        }
        if (lower.find("slideback") != std::string::npos ||
            lower.find("slide_back") != std::string::npos ||
            lower.find("slide-back") != std::string::npos ||
            lower.find("boltback") != std::string::npos ||
            lower.find("bolt_back") != std::string::npos ||
            lower.find("bolt-back") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::SlideBack;
        }
        if (lower.find("slideforward") != std::string::npos ||
            lower.find("slide_forward") != std::string::npos ||
            lower.find("slide-forward") != std::string::npos ||
            lower.find("boltforward") != std::string::npos ||
            lower.find("bolt_forward") != std::string::npos ||
            lower.find("bolt-forward") != std::string::npos)
        {
            return ManualReloadDelayedSoundStage::SlideForward;
        }
        if (lower.find("ready") != std::string::npos)
            return ManualReloadDelayedSoundStage::Ready;
        return ManualReloadDelayedSoundStage::Other;
    }

    int ManualReloadSoundSpecificityScore(const std::string& sample)
    {
        const std::string lower = ManualReloadLowerSoundSample(sample);
        if (lower.find("weapons/rifle/gunother/") != std::string::npos ||
            lower.find("weapons\\rifle\\gunother\\") != std::string::npos ||
            lower.find("weapons/smg/gunother/") != std::string::npos ||
            lower.find("weapons\\smg\\gunother\\") != std::string::npos ||
            lower.find("weapons/pistol/gunother/") != std::string::npos ||
            lower.find("weapons\\pistol\\gunother\\") != std::string::npos ||
            lower.find("weapons/sniper/gunother/") != std::string::npos ||
            lower.find("weapons\\sniper\\gunother\\") != std::string::npos)
        {
            return 0;
        }
        return lower.find("weapon") != std::string::npos ? 1 : 0;
    }

    size_t ManualReloadNormalizeDelayedSoundsForReplay(
        std::vector<ManualReloadDelayedSound>& sounds,
        float* outRebaseSeconds)
    {
        if (outRebaseSeconds)
            *outRebaseSeconds = 0.0f;
        if (sounds.empty())
            return 0;

        size_t selectedInsertIndex = sounds.size();
        int selectedInsertScore = -1;
        for (size_t i = 0; i < sounds.size(); ++i)
        {
            if (ManualReloadClassifyDelayedSound(sounds[i].sample) != ManualReloadDelayedSoundStage::Insert)
                continue;

            const int score = ManualReloadSoundSpecificityScore(sounds[i].sample);
            if (selectedInsertIndex == sounds.size() || score > selectedInsertScore ||
                (score == selectedInsertScore && i > selectedInsertIndex))
            {
                selectedInsertIndex = i;
                selectedInsertScore = score;
            }
        }

        if (selectedInsertIndex == sounds.size())
            return 0;

        struct StageChoice
        {
            ManualReloadDelayedSoundStage stage = ManualReloadDelayedSoundStage::Other;
            size_t sourceIndex = 0;
            int specificity = -1;
        };

        std::vector<StageChoice> classifiedChoices;
        std::vector<size_t> otherChoices;
        for (size_t i = selectedInsertIndex; i < sounds.size(); ++i)
        {
            const ManualReloadDelayedSoundStage stage = ManualReloadClassifyDelayedSound(sounds[i].sample);
            if (stage == ManualReloadDelayedSoundStage::Other)
            {
                bool duplicate = false;
                for (size_t selected : otherChoices)
                {
                    if (sounds[selected].sample == sounds[i].sample)
                    {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate)
                    otherChoices.push_back(i);
                continue;
            }

            const int specificity = ManualReloadSoundSpecificityScore(sounds[i].sample);
            StageChoice* choice = nullptr;
            for (StageChoice& existing : classifiedChoices)
            {
                if (existing.stage == stage)
                {
                    choice = &existing;
                    break;
                }
            }
            if (!choice)
            {
                classifiedChoices.push_back({ stage, i, specificity });
                continue;
            }
            if (specificity > choice->specificity)
            {
                choice->sourceIndex = i;
                choice->specificity = specificity;
            }
        }

        std::vector<size_t> selectedIndices;
        selectedIndices.reserve(classifiedChoices.size() + otherChoices.size());
        for (const StageChoice& choice : classifiedChoices)
            selectedIndices.push_back(choice.sourceIndex);
        selectedIndices.insert(selectedIndices.end(), otherChoices.begin(), otherChoices.end());
        std::sort(selectedIndices.begin(), selectedIndices.end());

        const float rebaseSeconds = sounds[selectedInsertIndex].offsetSeconds;
        if (outRebaseSeconds)
            *outRebaseSeconds = rebaseSeconds;
        std::vector<ManualReloadDelayedSound> normalized;
        normalized.reserve(selectedIndices.size());
        for (size_t selected : selectedIndices)
        {
            ManualReloadDelayedSound event = sounds[selected];
            event.offsetSeconds = std::max(0.0f, event.offsetSeconds - rebaseSeconds);
            normalized.push_back(std::move(event));
        }

        const size_t removedCount = sounds.size() - normalized.size();
        sounds.swap(normalized);
        return removedCount;
    }

    std::string ManualReloadPrepareConsoleSoundSample(const std::string& rawSample)
    {
        size_t start = 0;
        while (start < rawSample.size())
        {
            const char c = rawSample[start];
            if (c == '*' || c == '#' || c == '@' || c == '>' || c == '<' ||
                c == '^' || c == ')' || c == '}' || c == '$' || c == '?' || c == '!')
            {
                ++start;
                continue;
            }
            break;
        }

        std::string sample = rawSample.substr(start);
        if (sample.rfind("sound/", 0) == 0 || sample.rfind("sound\\", 0) == 0)
            sample.erase(0, 6);

        std::string escaped;
        escaped.reserve(sample.size());
        for (char c : sample)
        {
            if (c == '"' || c == '\\')
                escaped.push_back('\\');
            if (c == '\r' || c == '\n' || c == ';')
                continue;
            escaped.push_back(c);
        }
        return escaped;
    }

    std::string MagazineInteractionNormalizeSoundForCompare(const std::string& rawSample)
    {
        return MagazineInteractionLowerAscii(ManualReloadPrepareConsoleSoundSample(rawSample));
    }

    bool MagazineInteractionShouldLetSyntheticSoundPlay(
        std::string& pendingSample,
        std::chrono::steady_clock::time_point& pendingStarted,
        const char* sample,
        std::chrono::steady_clock::time_point now)
    {
        if (!sample || !*sample || pendingSample.empty() ||
            pendingStarted.time_since_epoch().count() == 0)
        {
            return false;
        }

        const float ageSeconds = std::chrono::duration<float>(
            now - pendingStarted).count();
        if (ageSeconds < 0.0f || ageSeconds > 0.50f)
        {
            pendingSample.clear();
            pendingStarted = {};
            return false;
        }

        const std::string pending = MagazineInteractionNormalizeSoundForCompare(
            pendingSample);
        const std::string current = MagazineInteractionNormalizeSoundForCompare(sample);
        if (pending.empty() || current.empty() || pending != current)
            return false;

        pendingSample.clear();
        pendingStarted = {};
        return true;
    }

}

bool VR::CaptureMagazineInteractionSound(int entityIndex, const char* sample, float volume, int flags, int pitch)
{
    constexpr int kSoundChangeVolume = (1 << 0);
    constexpr int kSoundChangePitch = (1 << 1);
    constexpr int kSoundStop = (1 << 2);
    constexpr int kSoundStopLooping = (1 << 5);
    constexpr int kNonStartFlags = kSoundChangeVolume | kSoundChangePitch | kSoundStop | kSoundStopLooping;
    if ((flags & kNonStartFlags) != 0)
        return false;
    if (MagazineInteractionSoundLooksClipEmpty(sample))
        return false;

    const bool waitingForInsertTail =
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForFreshMagazine ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingFreshMagazine;
    const bool removingOldMagazine =
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingOldMagazine;
    const bool waitingForBoltAction =
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBoltGrab ||
        m_MagazineInteractionState == MagazineInteractionManualState::HoldingBolt;
    const bool suppressingNativeReload =
        removingOldMagazine ||
        waitingForInsertTail ||
        m_MagazineInteractionState == MagazineInteractionManualState::WaitingForBackendReload ||
        waitingForBoltAction;
    if (!sample || !*sample || !m_Game || !m_MagazineInteractionReloadTriggered ||
        !suppressingNativeReload)
    {
        return false;
    }

    const int localPlayerIndex = (m_Game->m_EngineClient != nullptr)
        ? m_Game->m_EngineClient->GetLocalPlayer()
        : -1;
    const bool fromViewmodel = m_MagazineInteractionViewmodelEntityIndex > 0 &&
        entityIndex == m_MagazineInteractionViewmodelEntityIndex;
    const bool fromLocalWeaponPath = (entityIndex == -1 || entityIndex == localPlayerIndex) &&
        ManualReloadSoundLooksWeaponRelated(sample);
    if (!fromViewmodel && !fromLocalWeaponPath)
        return false;

    const auto now = std::chrono::steady_clock::now();
    if (MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticClipOutSample,
        m_MagazineInteractionSyntheticClipOutStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic clip-out play sample=%s",
            sample);
        return false;
    }
    if (MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticClipInSample,
        m_MagazineInteractionSyntheticClipInStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic clip-in play sample=%s",
            sample);
        return false;
    }
    if (MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticBoltBackSample,
        m_MagazineInteractionSyntheticBoltBackStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic bolt-back play sample=%s",
            sample);
        return false;
    }
    if (MagazineInteractionShouldLetSyntheticSoundPlay(
        m_MagazineInteractionSyntheticBoltForwardSample,
        m_MagazineInteractionSyntheticBoltForwardStarted,
        sample,
        now))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] let synthetic bolt-forward play sample=%s",
            sample);
        return false;
    }

    if (!ManualReloadSoundLooksWeaponRelated(sample) &&
        !ManualReloadSoundStartsInsertTail(sample) &&
        !MagazineInteractionSoundLooksClipOut(sample))
    {
        return false;
    }

    if (!MagazineInteractionSoundMatchesCurrentWeapon(this, sample))
    {
        Game::logMsg(
            "[VR][MagazineInteraction][Audio] swallowed mismatched native reload sound weaponId=%d model=%s sample=%s state=%d",
            m_MagazineInteractionWeaponId,
            m_MagazineInteractionMagazineModelName.c_str(),
            sample,
            static_cast<int>(m_MagazineInteractionState));
        return true;
    }

    const ManualReloadDelayedSoundStage delayedStage = ManualReloadClassifyDelayedSound(sample);
    if (delayedStage == ManualReloadDelayedSoundStage::SlideBack)
    {
        const int score = ManualReloadSoundSpecificityScore(sample);
        if (score >= m_MagazineInteractionCapturedBoltBackSoundScore)
        {
            m_MagazineInteractionCapturedBoltBackSample = sample;
            m_MagazineInteractionCapturedBoltBackSoundScore = score;
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] captured native bolt-back sample score=%d sample=%s",
                score,
                sample);
        }
    }
    else if (delayedStage == ManualReloadDelayedSoundStage::SlideForward)
    {
        const int score = ManualReloadSoundSpecificityScore(sample);
        if (score >= m_MagazineInteractionCapturedBoltForwardSoundScore)
        {
            m_MagazineInteractionCapturedBoltForwardSample = sample;
            m_MagazineInteractionCapturedBoltForwardSoundScore = score;
            Game::logMsg(
                "[VR][MagazineInteraction][Audio] captured native bolt-forward sample score=%d sample=%s",
                score,
                sample);
        }
    }

    Game::logMsg(
        "[VR][MagazineInteraction][Audio] swallowed native reload sound sample=%s state=%d",
        sample,
        static_cast<int>(m_MagazineInteractionState));
    return true;
}

bool VR::IsManualReloadActive() const
{
    return m_ManualReloadState != ManualReloadState::Idle;
}

bool VR::IsManualReloadBlockingFire() const
{
    // The gameplay reload command is intentionally decoupled from the physical insertion.
    // Block fire from the first reload frame until the delayed visual/audio tail has finished.
    return IsManualReloadActive();
}

bool VR::ShouldHideManualReloadNativeClip() const
{
    return m_ManualReloadHideNativeClip;
}

bool VR::CaptureManualReloadSound(int entityIndex, const char* sample, float volume, int flags, int pitch)
{
    // Do not swallow updates or stops for sounds that may already be playing. Clip-out audio belongs
    // to the visible removal phase and stays live. Delay only the hidden post-insert tail, starting
    // from the first actual magazine insertion sound.
    constexpr int kSoundChangeVolume = (1 << 0);
    constexpr int kSoundChangePitch = (1 << 1);
    constexpr int kSoundStop = (1 << 2);
    constexpr int kSoundStopLooping = (1 << 5);
    constexpr int kNonStartFlags = kSoundChangeVolume | kSoundChangePitch | kSoundStop | kSoundStopLooping;
    if ((flags & kNonStartFlags) != 0)
        return false;

    if (!sample || !*sample || !m_Game ||
        (m_ManualReloadState != ManualReloadState::WaitingForFreshMagazineGrab &&
            m_ManualReloadState != ManualReloadState::HoldingFreshMagazine &&
            m_ManualReloadState != ManualReloadState::AwaitingNativePostInsertBoundary))
    {
        return false;
    }

    const int localPlayerIndex = (m_Game->m_EngineClient != nullptr)
        ? m_Game->m_EngineClient->GetLocalPlayer()
        : -1;
    const bool fromViewmodel = m_ManualReloadViewmodelEntityIndex > 0 &&
        entityIndex == m_ManualReloadViewmodelEntityIndex;
    const bool fromLocalWeaponPath = (entityIndex == -1 || entityIndex == localPlayerIndex) &&
        ManualReloadSoundLooksWeaponRelated(sample);
    if (!fromViewmodel && !fromLocalWeaponPath)
        return false;

    const auto now = std::chrono::steady_clock::now();
    ManualReloadDelayedSound event;
    bool queued = false;
    bool insertTailStartedNow = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        if (!m_ManualReloadSoundInsertTailStarted)
        {
            if (!ManualReloadSoundStartsInsertTail(sample))
                return false;

            m_ManualReloadSoundInsertTailStarted = true;
            m_ManualReloadSoundCaptureStarted = now;
            m_ManualReloadAudioInsertVisualOffsetSeconds = std::max(
                0.0f,
                m_ManualReloadVisualResumeDurationSeconds);
            m_ManualReloadAudioInsertVisualOffsetValid = true;
            insertTailStartedNow = true;
        }

        event.sample = sample;
        event.offsetSeconds = std::clamp(
            std::chrono::duration<float>(now - m_ManualReloadSoundCaptureStarted).count(),
            0.0f,
            3.0f);
        event.volume = std::clamp(volume, 0.0f, 1.0f);
        event.pitch = std::clamp(pitch, 1, 255);

        if (m_ManualReloadDelayedSounds.size() < 64)
        {
            const bool duplicate = !m_ManualReloadDelayedSounds.empty() &&
                m_ManualReloadDelayedSounds.back().sample == event.sample &&
                std::fabs(m_ManualReloadDelayedSounds.back().offsetSeconds - event.offsetSeconds) < 0.015f;
            if (!duplicate)
            {
                m_ManualReloadDelayedSounds.push_back(event);
                queued = true;
            }
        }
    }

    if (insertTailStartedNow)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] insert-tail capture started ent=%d visualOffset=%.3fs sample=%s",
            entityIndex,
            m_ManualReloadAudioInsertVisualOffsetSeconds,
            sample);
    }
    if (queued)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] delayed hidden-tail sound ent=%d offset=%.3fs sample=%s",
            entityIndex,
            event.offsetSeconds,
            sample);
    }
    return true;
}

void VR::ReplayManualReloadDelayedSounds()
{
    if (m_ManualReloadState != ManualReloadState::ResumingNativeReloadWithMagazine ||
        !m_Game ||
        m_ManualReloadSoundReplayStarted.time_since_epoch().count() == 0)
    {
        return;
    }

    const float elapsedSeconds = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - m_ManualReloadSoundReplayStarted).count();
    std::vector<ManualReloadDelayedSound> due;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        while (m_ManualReloadDelayedSoundReplayIndex < m_ManualReloadDelayedSounds.size() &&
            m_ManualReloadDelayedSounds[m_ManualReloadDelayedSoundReplayIndex].offsetSeconds <= elapsedSeconds)
        {
            due.push_back(m_ManualReloadDelayedSounds[m_ManualReloadDelayedSoundReplayIndex]);
            ++m_ManualReloadDelayedSoundReplayIndex;
        }
    }

    for (const ManualReloadDelayedSound& event : due)
    {
        const std::string sample = ManualReloadPrepareConsoleSoundSample(event.sample);
        if (sample.empty())
            continue;

        char volume[32] = {};
        std::snprintf(volume, sizeof(volume), "%.3f", static_cast<double>(event.volume));
        const std::string command = "playvol \"" + sample + "\" " + volume;
        m_Game->ClientCmd_Unrestricted(command.c_str());
        Game::logMsg(
            "[VR][ManualReload][Audio] replayed delayed sound offset=%.3fs sample=%s",
            event.offsetSeconds,
            event.sample.c_str());
    }
}

float VR::GetManualReloadReplayDurationSeconds() const
{
    const float replayStartOffsetSeconds = m_ManualReloadVisualReplayStartOffsetValid
        ? std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds)
        : 0.0f;
    const float remainingVisualSeconds = std::max(
        0.0f,
        m_ManualReloadVisualResumeDurationSeconds - replayStartOffsetSeconds);
    float durationSeconds = std::clamp(
        std::max(0.10f, remainingVisualSeconds + 0.05f),
        0.10f,
        3.0f);
    std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
    if (!m_ManualReloadDelayedSounds.empty())
        durationSeconds = std::max(durationSeconds, m_ManualReloadDelayedSounds.back().offsetSeconds + 0.05f);
    return std::clamp(durationSeconds, 0.10f, 3.0f);
}

void VR::StartManualReloadPostInsertReplay(const char* reason)
{
    if (!m_ManualReloadVisualReplayStartOffsetValid)
        return;

    m_ManualReloadState = ManualReloadState::ResumingNativeReloadWithMagazine;
    m_ManualReloadResumeStarted = std::chrono::steady_clock::now();
    m_ManualReloadSoundReplayStarted = m_ManualReloadResumeStarted;
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    size_t prunedSoundCount = 0;
    size_t replaySoundCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        prunedSoundCount = ManualReloadNormalizeDelayedSoundsForReplay(m_ManualReloadDelayedSounds);
        replaySoundCount = m_ManualReloadDelayedSounds.size();
        m_ManualReloadDelayedSoundReplayIndex = 0;
    }
    if (prunedSoundCount > 0)
    {
        Game::logMsg(
            "[VR][ManualReload][Audio] normalized insert-tail replay kept=%u pruned=%u",
            static_cast<unsigned int>(replaySoundCount),
            static_cast<unsigned int>(prunedSoundCount));
    }
    m_ManualReloadViewmodelFrozen = false;
    Game::logMsg(
        "[VR][ManualReload] native magazine restored; replaying post-insert tail reason=%s offset=%.3fs duration=%.3fs",
        reason ? reason : "unknown",
        m_ManualReloadVisualReplayStartOffsetSeconds,
        GetManualReloadReplayDurationSeconds());
}

bool VR::TryStartManualReloadPostInsertReplay(const char* reason)
{
    float audioInsertVisualOffsetSeconds = 0.0f;
    bool audioInsertVisualOffsetValid = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        audioInsertVisualOffsetSeconds = m_ManualReloadAudioInsertVisualOffsetSeconds;
        audioInsertVisualOffsetValid = m_ManualReloadAudioInsertVisualOffsetValid;
    }

    // Workshop replacement models often return their visible magazine bone to the socket only
    // after the useful hand/bolt tail has already elapsed. The first clip-in sound is emitted at
    // the real insertion boundary, so prefer it whenever it moves the visual replay point earlier.
    // SnapManualReloadNativeMagazineToSocket keeps the magazine subtree fixed at the socket while
    // these earlier poses replay, preventing a second visible insertion.
    if (audioInsertVisualOffsetValid &&
        (!m_ManualReloadVisualReplayStartOffsetValid ||
            audioInsertVisualOffsetSeconds < m_ManualReloadVisualReplayStartOffsetSeconds))
    {
        const bool hadVisualBoundary = m_ManualReloadVisualReplayStartOffsetValid;
        const float previousOffsetSeconds = m_ManualReloadVisualReplayStartOffsetSeconds;
        m_ManualReloadVisualReplayStartOffsetSeconds = std::max(0.0f, audioInsertVisualOffsetSeconds);
        m_ManualReloadVisualReplayStartOffsetValid = true;
        Game::logMsg(
            "[VR][ManualReload] post-insert visual replay aligned to audio insert boundary offset=%.3fs previousVisualBoundary=%s%.3fs",
            m_ManualReloadVisualReplayStartOffsetSeconds,
            hadVisualBoundary ? "" : "unavailable/",
            previousOffsetSeconds);
    }

    if (!m_ManualReloadVisualReplayStartOffsetValid)
        return false;

    const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
    float remainingSeconds = std::max(
        0.0f,
        capturedSeconds - std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds));

    // Do not start playback as soon as the native magazine first reaches the socket.
    // At that instant the cache usually contains no post-insert frames yet. Wait until
    // a visible tail has actually been sampled, or until the hidden native sequence ends.
    if (!m_ManualReloadTailCaptureComplete &&
        remainingSeconds < kManualReloadMinimumPostInsertTailSeconds)
    {
        return false;
    }

    // Some replacement models report the visual magazine boundary only at the very end
    // of their native sequence. Preserve a useful final window instead of jumping to idle.
    // The native magazine subtree is pinned to the socket while this window replays, so
    // shifting the replay point slightly earlier cannot redraw a second magazine insertion.
    if (remainingSeconds < kManualReloadMinimumPostInsertTailSeconds && capturedSeconds > 0.0f)
    {
        const float previousOffset = m_ManualReloadVisualReplayStartOffsetSeconds;
        m_ManualReloadVisualReplayStartOffsetSeconds = std::max(
            0.0f,
            capturedSeconds - kManualReloadMinimumPostInsertTailSeconds);
        remainingSeconds = std::max(
            0.0f,
            capturedSeconds - m_ManualReloadVisualReplayStartOffsetSeconds);
        Game::logMsg(
            "[VR][ManualReload] post-insert replay boundary widened offset=%.3fs->%.3fs captured=%.3fs preservedTail=%.3fs",
            previousOffset,
            m_ManualReloadVisualReplayStartOffsetSeconds,
            capturedSeconds,
            remainingSeconds);
    }

    StartManualReloadPostInsertReplay(reason);
    return true;
}

void VR::UseManualReloadPostInsertFallbackBoundary(const char* reason)
{
    if (m_ManualReloadVisualReplayStartOffsetValid)
        return;

    const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
    // Preserve a real tail instead of jumping straight to idle. This fallback is used only when
    // the replacement model never exposes a reliable visual reinsertion boundary.
    const float preservedTailSeconds = std::clamp(capturedSeconds * 0.25f, 0.35f, 0.75f);
    m_ManualReloadVisualReplayStartOffsetSeconds = std::max(0.0f, capturedSeconds - preservedTailSeconds);
    m_ManualReloadVisualReplayStartOffsetValid = true;
    Game::logMsg(
        "[VR][ManualReload] post-insert boundary fallback reason=%s captured=%.3fs offset=%.3fs preservedTail=%.3fs",
        reason ? reason : "unknown",
        capturedSeconds,
        m_ManualReloadVisualReplayStartOffsetSeconds,
        std::max(0.0f, capturedSeconds - m_ManualReloadVisualReplayStartOffsetSeconds));
}

void VR::BeginManualReload(C_BasePlayer* localPlayer)
{
    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
        return;

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || !ManualReloadWeaponUsesDetachableMagazine(weapon->GetWeaponID()))
        return;

    CancelManualReload();
    m_ManualReloadWeapon = weapon;
    m_ManualReloadWeaponId = static_cast<int>(weapon->GetWeaponID());
    m_ManualReloadMagazineModelName.clear();
    m_ManualReloadMagazineBoneIndex = -1;
    m_ManualReloadMagazineMotionBoneIndex = -1;
    m_ManualReloadViewmodelEntityIndex = -1;
    m_ManualReloadMotionProbeValid = false;
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        m_ManualReloadDelayedSounds.clear();
        m_ManualReloadDelayedSoundReplayIndex = 0;
        m_ManualReloadSoundInsertTailStarted = false;
        m_ManualReloadSoundCaptureStarted = {};
        m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
        m_ManualReloadAudioInsertVisualOffsetValid = false;
    }
    m_ManualReloadSoundReplayStarted = {};
    m_ManualReloadState = ManualReloadState::WatchingNativeClipRemoval;
    m_ManualReloadStarted = std::chrono::steady_clock::now();
    m_ManualReloadResolvedInsertionAxisValid = false;
    m_ManualReloadVisualResumeDurationSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetValid = false;
    m_ManualReloadNativeVisualClipWasAway = false;
    m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
    m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    Game::logMsg(
        "[VR][ManualReload] begin weaponId=%d; scanning current viewmodel for detachable magazine bone",
        static_cast<int>(weapon->GetWeaponID()));
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
    m_ManualReloadWeaponId = 0;
    m_ManualReloadViewmodelEntity = nullptr;
    m_ManualReloadViewmodelEntityIndex = -1;
    m_ManualReloadMagazineModelName.clear();
    m_ManualReloadMagazineBoneIndex = -1;
    m_ManualReloadMagazineMotionBoneIndex = -1;
    m_ManualReloadSocketValid = false;
    m_ManualReloadMotionProbeValid = false;
    m_ManualReloadHideNativeClip = false;
    m_ManualReloadMagazineInsertionArmed = false;
    m_ManualReloadFrozenSequence = -1;
    m_ManualReloadFrozenCycle = 0.0f;
    m_ManualReloadOriginalPlaybackRate = 1.0f;
    m_ManualReloadViewmodelFrozen = false;
    m_ManualReloadResolvedInsertionAxisLocal = { 0.0f, -1.0f, 0.0f };
    m_ManualReloadResolvedInsertionAxisValid = false;
    m_ManualReloadVisualResumeDurationSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
    m_ManualReloadVisualReplayStartOffsetValid = false;
    m_ManualReloadTailCaptureComplete = false;
    m_ManualReloadNativeVisualClipWasAway = false;
    m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
    m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
    m_ManualReloadStarted = {};
    m_ManualReloadResumeStarted = {};
    m_ManualReloadPostInsertBoundaryWaitStarted = {};
    {
        std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
        m_ManualReloadDelayedSounds.clear();
        m_ManualReloadDelayedSoundReplayIndex = 0;
        m_ManualReloadSoundInsertTailStarted = false;
        m_ManualReloadSoundCaptureStarted = {};
        m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
        m_ManualReloadAudioInsertVisualOffsetValid = false;
    }
    m_ManualReloadSoundReplayStarted = {};
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
                1.0f,
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
            1.0f,
            m_ManualReloadMagazineHandOffsetMeters,
            m_ManualReloadMagazineHandRotationOffsetDeg);
        return true;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithMagazine)
    {
        const VrHandMatrix4 local = ManualReloadBuildLocalTransform(
            m_VRScale,
            1.0f,
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
            Game::logMsg("[VR][ManualReload][MouseTest] F6 ignored: equip a detachable-magazine firearm and keep single-threaded rendering enabled");
            return;
        }

        m_Game->ClientCmd_Unrestricted("+reload");
        m_ManualReloadMouseTestReloadPulseOwned = true;
        Game::logMsg("[VR][ManualReload][MouseTest] F6 started native reload; after pause press F7, then hold PageDown to push the native Source magazine into the socket");
        Game::logMsg("[VR][ManualReload][MouseTest] controls: F7=grab  Home=reset alignment  arrows=lateral move  PageUp/PageDown=pull/push  numpad 8/2 4/6 7/9=rotate  Delete=drop  F9=cancel");
        return;
    }

    const Vector insertionAxis = ManualReloadNormalize(ManualReloadGetInsertionAxisLocal(this));
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
                    "[VR][ManualReload][MouseTest] F7 spawned aligned native magazine socket=(%.2f %.2f %.2f) guide=(%.2f %.2f %.2f); hold PageDown to insert",
                    socketWorld.x, socketWorld.y, socketWorld.z,
                    guideWorld.x, guideWorld.y, guideWorld.z);
            }
            else
            {
                Game::logMsg("[VR][ManualReload][MouseTest] F7 spawned aligned native magazine at guide start; hold PageDown to insert");
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
        Game::logMsg("[VR][ManualReload][MouseTest] native magazine dropped; press F7 to spawn another one");
        return;
    }

    if (resetJustPressed)
    {
        resetMagazineToGuideStart();
        Game::logMsg("[VR][ManualReload][MouseTest] native magazine reset to aligned guide start");
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

    ReplayManualReloadDelayedSounds();

    if (!m_ManualReloadEnabled || !m_VrHandsEnabled || !m_Game || m_Game->GetMatQueueMode() != 0 || !localPlayer)
    {
        CancelManualReload();
        return;
    }

    C_WeaponCSBase* weapon = reinterpret_cast<C_WeaponCSBase*>(localPlayer->GetActiveWeapon());
    if (!weapon || weapon != m_ManualReloadWeapon || !ManualReloadWeaponUsesDetachableMagazine(weapon->GetWeaponID()))
    {
        CancelManualReload();
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval &&
        m_ManualReloadStarted.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_ManualReloadStarted).count() >= 5.0f)
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
            Game::logMsg("[VR][ManualReload] fresh native magazine grabbed from left waist");
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
    const Vector insertionAxis = ManualReloadNormalize(ManualReloadGetInsertionAxisLocal(this));
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
        m_ManualReloadMagazineInsertionArmed = false;
        // The detached Source-material copy disappears immediately. Restore the native magazine
        // at the socket now, but do not jump to idle when the hidden animation has not reached
        // its post-insert boundary yet. In that case the visible gun remains frozen briefly while
        // DrawModelExecute keeps sampling the hidden Source tail.
        m_ManualReloadHideNativeClip = false;
        m_ManualReloadViewmodelFrozen = false;

        m_ManualReloadState = ManualReloadState::AwaitingNativePostInsertBoundary;
        m_ManualReloadPostInsertBoundaryWaitStarted = std::chrono::steady_clock::now();
        if (!TryStartManualReloadPostInsertReplay("captured-boundary-ready-at-player-insert"))
        {
            const float capturedSeconds = std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds);
            const float replayOffsetSeconds = m_ManualReloadVisualReplayStartOffsetValid
                ? std::max(0.0f, m_ManualReloadVisualReplayStartOffsetSeconds)
                : 0.0f;
            Game::logMsg(
                "[VR][ManualReload] player inserted magazine; detached copy removed and native magazine restored at socket; waiting for hidden post-insert tail samples captured=%.3fs offset=%.3fs remaining=%.3fs",
                capturedSeconds,
                replayOffsetSeconds,
                std::max(0.0f, capturedSeconds - replayOffsetSeconds));
        }
    }
}

void VR::OnManualReloadViewmodelPose(
    const char* modelName,
    void* viewmodelEntity,
    int viewmodelEntityIndex,
    const VrHandMatrix4& modelAnchorWorld,
    const VrHandMatrix4& nativeClipWorld,
    const VrHandMatrix4& nativeMotionProbeWorld)
{
    (void)modelName;
    if (!IsManualReloadActive() || !viewmodelEntity)
        return;

    m_ManualReloadViewmodelEntity = viewmodelEntity;
    if (viewmodelEntityIndex > 0)
        m_ManualReloadViewmodelEntityIndex = viewmodelEntityIndex;
    VrHandMatrix4 inverseModelAnchor{};
    if (!VrHandMath::Invert4x4(modelAnchorWorld, inverseModelAnchor))
        return;
    const VrHandMatrix4 currentClipLocal = VrHandMath::Multiply(inverseModelAnchor, nativeClipWorld);
    const VrHandMatrix4 currentMotionProbeLocal = VrHandMath::Multiply(inverseModelAnchor, nativeMotionProbeWorld);

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

    if (!m_ManualReloadMotionProbeValid)
    {
        m_ManualReloadMotionProbeLocal = currentMotionProbeLocal;
        m_ManualReloadMotionProbeValid = true;
    }

    int sequence = -1;
    float cycle = 0.0f;
    float playbackRate = 1.0f;
    if (!ManualReloadReadViewmodelAnimation(viewmodelEntity, sequence, cycle, playbackRate))
        return;

    if (m_ManualReloadState == ManualReloadState::WatchingNativeClipRemoval)
    {
        const Vector currentProbe = ManualReloadMatrixTranslation(currentMotionProbeLocal);
        const Vector initialProbe = ManualReloadMatrixTranslation(m_ManualReloadMotionProbeLocal);
        const float movedMeters = ManualReloadVectorLength(currentProbe - initialProbe) / std::max(0.001f, m_VRScale);
        if (movedMeters >= m_ManualReloadNativeClipLeaveDistanceMeters)
        {
            const Vector outwardModelLocal = currentProbe - initialProbe;
            if (ManualReloadVectorLength(outwardModelLocal) > 0.0001f)
            {
                VrHandMatrix4 inverseSocketLocal{};
                if (VrHandMath::Invert4x4(ManualReloadStripScale(m_ManualReloadSocketLocal), inverseSocketLocal))
                {
                    m_ManualReloadResolvedInsertionAxisLocal = ManualReloadNormalize(
                        ManualReloadTransformDirection(inverseSocketLocal, outwardModelLocal));
                    m_ManualReloadResolvedInsertionAxisValid = true;
                }
            }
            m_ManualReloadVisualResumeDurationSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetValid = false;
            m_ManualReloadTailCaptureComplete = false;
            const Vector currentVisualClip = ManualReloadMatrixTranslation(currentClipLocal);
            const Vector initialVisualClip = ManualReloadMatrixTranslation(m_ManualReloadSocketLocal);
            const float visualClipMovedMeters = ManualReloadVectorLength(currentVisualClip - initialVisualClip) /
                std::max(0.001f, m_VRScale);
            const float visualAwayThresholdMeters = std::clamp(
                m_ManualReloadNativeClipLeaveDistanceMeters * 0.50f,
                0.012f,
                0.040f);
            m_ManualReloadNativeVisualClipMaxDistanceMeters = visualClipMovedMeters;
            m_ManualReloadNativeVisualClipWasAway = visualClipMovedMeters >= visualAwayThresholdMeters;
            m_ManualReloadNativeMotionProbeMaxDistanceMeters = movedMeters;
            m_ManualReloadPostInsertBoundaryWaitStarted = {};
            {
                std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
                m_ManualReloadDelayedSounds.clear();
                m_ManualReloadDelayedSoundReplayIndex = 0;
                m_ManualReloadSoundInsertTailStarted = false;
                m_ManualReloadSoundCaptureStarted = {};
                m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
                m_ManualReloadAudioInsertVisualOffsetValid = false;
            }
            m_ManualReloadSoundReplayStarted = {};
            m_ManualReloadFrozenSequence = sequence;
            m_ManualReloadFrozenCycle = cycle;
            m_ManualReloadOriginalPlaybackRate = playbackRate;
            // Do not freeze Source's internal animation state. DrawModelExecute freezes only the visible
            // submitted bones while the hidden native sequence keeps progressing so its tail can be sampled.
            m_ManualReloadViewmodelFrozen = false;
            m_ManualReloadHideNativeClip = true;
            m_ManualReloadState = ManualReloadState::WaitingForFreshMagazineGrab;
            const Vector resolvedAxis = ManualReloadGetInsertionAxisLocal(this);
            Game::logMsg(
                "[VR][ManualReload] native clip left weapon %.3fm; animation paused; inferred insertion axis=(%.3f %.3f %.3f)",
                movedMeters,
                resolvedAxis.x,
                resolvedAxis.y,
                resolvedAxis.z);
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::WaitingForFreshMagazineGrab ||
        m_ManualReloadState == ManualReloadState::HoldingFreshMagazine ||
        m_ManualReloadState == ManualReloadState::AwaitingNativePostInsertBoundary)
    {
        const Vector currentProbe = ManualReloadMatrixTranslation(currentMotionProbeLocal);
        const Vector initialProbe = ManualReloadMatrixTranslation(m_ManualReloadMotionProbeLocal);
        const float motionMovedMeters = ManualReloadVectorLength(currentProbe - initialProbe) /
            std::max(0.001f, m_VRScale);
        m_ManualReloadNativeMotionProbeMaxDistanceMeters = std::max(
            m_ManualReloadNativeMotionProbeMaxDistanceMeters,
            motionMovedMeters);

        const Vector currentVisualClip = ManualReloadMatrixTranslation(currentClipLocal);
        const Vector initialVisualClip = ManualReloadMatrixTranslation(m_ManualReloadSocketLocal);
        const float visualClipMovedMeters = ManualReloadVectorLength(currentVisualClip - initialVisualClip) /
            std::max(0.001f, m_VRScale);
        m_ManualReloadNativeVisualClipMaxDistanceMeters = std::max(
            m_ManualReloadNativeVisualClipMaxDistanceMeters,
            visualClipMovedMeters);

        const float visualAwayThresholdMeters = std::clamp(
            m_ManualReloadNativeClipLeaveDistanceMeters * 0.50f,
            0.012f,
            0.040f);
        const float visualReturnedThresholdMeters = std::clamp(
            m_ManualReloadNativeClipLeaveDistanceMeters * 0.35f,
            0.006f,
            0.018f);
        if (visualClipMovedMeters >= visualAwayThresholdMeters)
            m_ManualReloadNativeVisualClipWasAway = true;

        if (!m_ManualReloadVisualReplayStartOffsetValid &&
            m_ManualReloadFrozenSequence >= 0 &&
            m_ManualReloadNativeVisualClipWasAway &&
            visualClipMovedMeters <= visualReturnedThresholdMeters)
        {
            m_ManualReloadVisualReplayStartOffsetSeconds =
                std::max(0.0f, m_ManualReloadVisualResumeDurationSeconds + (1.0f / 90.0f));
            m_ManualReloadVisualReplayStartOffsetValid = true;
            Game::logMsg(
                "[VR][ManualReload] hidden native visual magazine reinsertion boundary captured offset=%.3fs visualDistance=%.3fm motionDistance=%.3fm",
                m_ManualReloadVisualReplayStartOffsetSeconds,
                visualClipMovedMeters,
                motionMovedMeters);
        }

        if (!m_ManualReloadTailCaptureComplete &&
            m_ManualReloadFrozenSequence >= 0 &&
            sequence != m_ManualReloadFrozenSequence)
        {
            m_ManualReloadTailCaptureComplete = true;
            Game::logMsg(
                "[VR][ManualReload] hidden native reload tail capture finished duration=%.3fs sequence=%d->%d",
                m_ManualReloadVisualResumeDurationSeconds,
                m_ManualReloadFrozenSequence,
                sequence);
        }

        if (m_ManualReloadState == ManualReloadState::AwaitingNativePostInsertBoundary)
        {
            if (!TryStartManualReloadPostInsertReplay("visual-native-reinsertion-boundary"))
            {
                const float waitingSeconds =
                    (m_ManualReloadPostInsertBoundaryWaitStarted.time_since_epoch().count() != 0)
                    ? std::chrono::duration<float>(std::chrono::steady_clock::now() - m_ManualReloadPostInsertBoundaryWaitStarted).count()
                    : 0.0f;
                if (!m_ManualReloadVisualReplayStartOffsetValid &&
                    (m_ManualReloadTailCaptureComplete ||
                        waitingSeconds >= kManualReloadPostInsertBoundaryWaitTimeoutSeconds))
                {
                    UseManualReloadPostInsertFallbackBoundary(
                        m_ManualReloadTailCaptureComplete ? "tail-finished-without-visual-boundary" : "boundary-wait-timeout");
                    TryStartManualReloadPostInsertReplay("fallback-preserved-tail");
                }
            }
        }
        return;
    }

    if (m_ManualReloadState == ManualReloadState::ResumingNativeReloadWithMagazine)
    {
        const auto now = std::chrono::steady_clock::now();
        const float resumedSeconds = std::chrono::duration<float>(now - m_ManualReloadResumeStarted).count();
        ReplayManualReloadDelayedSounds();
        const float replayDurationSeconds = GetManualReloadReplayDurationSeconds();
        if (resumedSeconds >= replayDurationSeconds)
        {
            m_ManualReloadHideNativeClip = false;
            m_ManualReloadState = ManualReloadState::Idle;
            m_ManualReloadWeapon = nullptr;
            m_ManualReloadWeaponId = 0;
            m_ManualReloadViewmodelEntity = nullptr;
            m_ManualReloadViewmodelEntityIndex = -1;
            m_ManualReloadMagazineModelName.clear();
            m_ManualReloadMagazineBoneIndex = -1;
            m_ManualReloadMagazineMotionBoneIndex = -1;
            m_ManualReloadSocketValid = false;
            m_ManualReloadMotionProbeValid = false;
            m_ManualReloadMagazineInsertionArmed = false;
            m_ManualReloadFrozenSequence = -1;
            m_ManualReloadFrozenCycle = 0.0f;
            m_ManualReloadOriginalPlaybackRate = 1.0f;
            m_ManualReloadViewmodelFrozen = false;
            m_ManualReloadResolvedInsertionAxisValid = false;
            m_ManualReloadVisualResumeDurationSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetSeconds = 0.0f;
            m_ManualReloadVisualReplayStartOffsetValid = false;
            m_ManualReloadTailCaptureComplete = false;
            m_ManualReloadNativeVisualClipWasAway = false;
            m_ManualReloadNativeVisualClipMaxDistanceMeters = 0.0f;
            m_ManualReloadNativeMotionProbeMaxDistanceMeters = 0.0f;
            m_ManualReloadStarted = {};
            m_ManualReloadResumeStarted = {};
            m_ManualReloadPostInsertBoundaryWaitStarted = {};
            {
                std::lock_guard<std::mutex> lock(m_ManualReloadSoundMutex);
                m_ManualReloadDelayedSounds.clear();
                m_ManualReloadDelayedSoundReplayIndex = 0;
                m_ManualReloadSoundInsertTailStarted = false;
                m_ManualReloadSoundCaptureStarted = {};
                m_ManualReloadAudioInsertVisualOffsetSeconds = 0.0f;
                m_ManualReloadAudioInsertVisualOffsetValid = false;
            }
            m_ManualReloadSoundReplayStarted = {};
            Game::logMsg("[VR][ManualReload] captured reload tail finished; native viewmodel returned to live idle pose");
        }
    }
}
