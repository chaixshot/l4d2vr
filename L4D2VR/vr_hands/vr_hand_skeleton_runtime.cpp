#include "vr_hand_skeleton_runtime.h"

#include "vr_hand_math.h"

#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/base/span.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    ozz::math::Transform ToOzzTransform(const vr::VRBoneTransform_t& transform, bool forceIdentity)
    {
        if (forceIdentity)
            return ozz::math::Transform::identity();

        ozz::math::Transform out = ozz::math::Transform::identity();
        out.translation = { transform.position.v[0], transform.position.v[1], transform.position.v[2] };
        out.rotation = { transform.orientation.x, transform.orientation.y, transform.orientation.z, transform.orientation.w };
        return out;
    }

    std::string ToLowerAscii(std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    VrHandMatrix4 ToVrHandMatrix(const ozz::math::Float4x4& matrix)
    {
        VrHandMatrix4 out{};
        for (int column = 0; column < 4; ++column)
            ozz::math::StorePtrU(matrix.cols[column], &out.m[static_cast<size_t>(column) * 4u]);
        return out;
    }
}

struct VrHandSkeletonRuntime::Impl
{
    vr::VRActionHandle_t action = vr::k_ulInvalidActionHandle;
    std::vector<vr::BoneIndex_t> parents;
    std::vector<std::string> openVrJointNames;
    std::vector<vr::VRBoneTransform_t> transforms;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
    ozz::vector<ozz::math::SoaTransform> locals;
    ozz::vector<ozz::math::Float4x4> models;
    std::unordered_map<std::string, int> jointIndexByName;
    std::vector<int> ozzJointToOpenVrBone;
    bool hasPose = false;
};

VrHandSkeletonRuntime::VrHandSkeletonRuntime()
    : m_Impl(std::make_unique<Impl>())
{
}

VrHandSkeletonRuntime::~VrHandSkeletonRuntime() = default;

bool VrHandSkeletonRuntime::Initialize(vr::IVRInput* input, vr::VRActionHandle_t action, std::string& outError)
{
    outError.clear();
    if (!input || action == vr::k_ulInvalidActionHandle)
    {
        outError = "invalid OpenVR skeletal action handle";
        return false;
    }

    uint32_t boneCount = 0;
    if (input->GetBoneCount(action, &boneCount) != vr::VRInputError_None || boneCount == 0 || boneCount > 64)
    {
        outError = "OpenVR returned an invalid hand bone count";
        return false;
    }

    m_Impl->action = action;
    m_Impl->parents.assign(boneCount, vr::k_unInvalidBoneIndex);
    m_Impl->openVrJointNames.assign(boneCount, std::string{});
    m_Impl->transforms.assign(boneCount, vr::VRBoneTransform_t{});

    if (input->GetBoneHierarchy(action, m_Impl->parents.data(), boneCount) != vr::VRInputError_None)
    {
        outError = "OpenVR GetBoneHierarchy failed";
        return false;
    }

    for (uint32_t i = 0; i < boneCount; ++i)
    {
        char name[vr::k_unMaxBoneNameLength]{};
        if (input->GetBoneName(action, static_cast<vr::BoneIndex_t>(i), name, static_cast<uint32_t>(sizeof(name))) != vr::VRInputError_None)
        {
            outError = "OpenVR GetBoneName failed";
            return false;
        }
        m_Impl->openVrJointNames[i] = name;
    }

    if (input->GetSkeletalReferenceTransforms(
        action,
        vr::VRSkeletalTransformSpace_Parent,
        vr::VRSkeletalReferencePose_BindPose,
        m_Impl->transforms.data(),
        boneCount) != vr::VRInputError_None)
    {
        outError = "OpenVR GetSkeletalReferenceTransforms failed";
        return false;
    }

    std::vector<std::vector<uint32_t>> children(boneCount);
    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const vr::BoneIndex_t parent = m_Impl->parents[i];
        if (parent == vr::k_unInvalidBoneIndex)
        {
            roots.push_back(i);
            continue;
        }
        if (parent < 0 || static_cast<uint32_t>(parent) >= boneCount)
        {
            outError = "OpenVR returned an invalid parent bone index";
            return false;
        }
        children[static_cast<size_t>(parent)].push_back(i);
    }
    if (roots.empty())
    {
        outError = "OpenVR hand skeleton has no root bone";
        return false;
    }

    ozz::animation::offline::RawSkeleton rawSkeleton;
    std::function<void(uint32_t, ozz::animation::offline::RawSkeleton::Joint::Children&)> appendJoint;
    appendJoint = [&](uint32_t index, ozz::animation::offline::RawSkeleton::Joint::Children& destination)
        {
            destination.resize(destination.size() + 1u);
            auto& joint = destination.back();
            joint.name = m_Impl->openVrJointNames[index].c_str();
            joint.transform = ToOzzTransform(m_Impl->transforms[index], m_Impl->parents[index] == vr::k_unInvalidBoneIndex);
            for (uint32_t child : children[index])
                appendJoint(child, joint.children);
        };
    for (uint32_t root : roots)
        appendJoint(root, rawSkeleton.roots);

    ozz::animation::offline::SkeletonBuilder builder;
    m_Impl->skeleton = builder(rawSkeleton);
    if (!m_Impl->skeleton || m_Impl->skeleton->num_joints() != static_cast<int>(boneCount))
    {
        outError = "ozz SkeletonBuilder failed for the OpenVR hand hierarchy";
        return false;
    }

    m_Impl->locals.resize(static_cast<size_t>(m_Impl->skeleton->num_soa_joints()));
    m_Impl->models.resize(static_cast<size_t>(m_Impl->skeleton->num_joints()));
    std::unordered_map<std::string, int> openVrBoneIndexByName;
    for (uint32_t i = 0; i < boneCount; ++i)
    {
        const std::string& name = m_Impl->openVrJointNames[i];
        openVrBoneIndexByName.emplace(name, static_cast<int>(i));
        openVrBoneIndexByName.emplace(ToLowerAscii(name), static_cast<int>(i));
    }

    m_Impl->jointIndexByName.clear();
    m_Impl->ozzJointToOpenVrBone.assign(static_cast<size_t>(m_Impl->skeleton->num_joints()), -1);
    const auto jointNames = m_Impl->skeleton->joint_names();
    for (int i = 0; i < m_Impl->skeleton->num_joints(); ++i)
    {
        const std::string name = jointNames[static_cast<size_t>(i)] ? jointNames[static_cast<size_t>(i)] : "";
        m_Impl->jointIndexByName.emplace(name, i);
        m_Impl->jointIndexByName.emplace(ToLowerAscii(name), i);

        auto openVrBone = openVrBoneIndexByName.find(name);
        if (openVrBone == openVrBoneIndexByName.end())
            openVrBone = openVrBoneIndexByName.find(ToLowerAscii(name));
        if (openVrBone == openVrBoneIndexByName.end())
        {
            outError = "ozz skeleton joint does not map back to an OpenVR bone: " + name;
            return false;
        }
        m_Impl->ozzJointToOpenVrBone[static_cast<size_t>(i)] = openVrBone->second;
    }

    m_Impl->hasPose = false;
    return true;
}

bool VrHandSkeletonRuntime::Update(vr::IVRInput* input, vr::EVRSkeletalMotionRange motionRange, std::string& outError)
{
    outError.clear();
    if (!input || !m_Impl->skeleton || m_Impl->action == vr::k_ulInvalidActionHandle)
    {
        outError = "hand skeleton runtime is not initialized";
        return false;
    }

    vr::InputSkeletalActionData_t actionData{};
    if (input->GetSkeletalActionData(m_Impl->action, &actionData, sizeof(actionData)) != vr::VRInputError_None || !actionData.bActive)
    {
        m_Impl->hasPose = false;
        return false;
    }

    if (input->GetSkeletalBoneData(
        m_Impl->action,
        vr::VRSkeletalTransformSpace_Parent,
        motionRange,
        m_Impl->transforms.data(),
        static_cast<uint32_t>(m_Impl->transforms.size())) != vr::VRInputError_None)
    {
        outError = "OpenVR GetSkeletalBoneData failed";
        m_Impl->hasPose = false;
        return false;
    }

    for (size_t soa = 0; soa < m_Impl->locals.size(); ++soa)
    {
        float tx[4]{}, ty[4]{}, tz[4]{};
        float qx[4]{}, qy[4]{}, qz[4]{}, qw[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        float sx[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        float sy[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        float sz[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        for (size_t lane = 0; lane < 4; ++lane)
        {
            const size_t ozzJointIndex = soa * 4u + lane;
            if (ozzJointIndex >= m_Impl->ozzJointToOpenVrBone.size())
                continue;
            const int mappedBone = m_Impl->ozzJointToOpenVrBone[ozzJointIndex];
            if (mappedBone < 0 || static_cast<size_t>(mappedBone) >= m_Impl->transforms.size())
            {
                outError = "ozz joint does not map to a valid OpenVR bone";
                m_Impl->hasPose = false;
                return false;
            }
            const size_t openVrBoneIndex = static_cast<size_t>(mappedBone);
            const bool isRoot = m_Impl->parents[openVrBoneIndex] == vr::k_unInvalidBoneIndex;
            const ozz::math::Transform transform = ToOzzTransform(m_Impl->transforms[openVrBoneIndex], isRoot);
            tx[lane] = transform.translation.x;
            ty[lane] = transform.translation.y;
            tz[lane] = transform.translation.z;
            qx[lane] = transform.rotation.x;
            qy[lane] = transform.rotation.y;
            qz[lane] = transform.rotation.z;
            qw[lane] = transform.rotation.w;
            sx[lane] = transform.scale.x;
            sy[lane] = transform.scale.y;
            sz[lane] = transform.scale.z;
        }

        ozz::math::SoaTransform& local = m_Impl->locals[soa];
        local.translation = ozz::math::SoaFloat3::Load(
            ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]),
            ozz::math::simd_float4::Load(ty[0], ty[1], ty[2], ty[3]),
            ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]));
        local.rotation = ozz::math::SoaQuaternion::Load(
            ozz::math::simd_float4::Load(qx[0], qx[1], qx[2], qx[3]),
            ozz::math::simd_float4::Load(qy[0], qy[1], qy[2], qy[3]),
            ozz::math::simd_float4::Load(qz[0], qz[1], qz[2], qz[3]),
            ozz::math::simd_float4::Load(qw[0], qw[1], qw[2], qw[3]));
        local.scale = ozz::math::SoaFloat3::Load(
            ozz::math::simd_float4::Load(sx[0], sx[1], sx[2], sx[3]),
            ozz::math::simd_float4::Load(sy[0], sy[1], sy[2], sy[3]),
            ozz::math::simd_float4::Load(sz[0], sz[1], sz[2], sz[3]));
    }

    ozz::animation::LocalToModelJob job;
    job.skeleton = m_Impl->skeleton.get();
    job.input = ozz::make_span(m_Impl->locals);
    job.output = ozz::make_span(m_Impl->models);
    if (!job.Run())
    {
        outError = "ozz LocalToModelJob failed";
        m_Impl->hasPose = false;
        return false;
    }

    m_Impl->hasPose = true;
    return true;
}

bool VrHandSkeletonRuntime::BuildSkinningPalette(const VrHandMeshAsset& asset, std::vector<VrHandMatrixRows3x4>& outPalette, std::string& outError) const
{
    outError.clear();
    outPalette.clear();
    if (!m_Impl->skeleton || !m_Impl->hasPose)
        return false;
    if (!asset.IsValid())
    {
        outError = "hand mesh asset is invalid";
        return false;
    }
    if (asset.jointNames.size() > 64)
    {
        outError = "hand mesh uses more than 64 joints";
        return false;
    }

    outPalette.resize(asset.jointNames.size());
    for (size_t meshJoint = 0; meshJoint < asset.jointNames.size(); ++meshJoint)
    {
        const std::string& name = asset.jointNames[meshJoint];
        auto found = m_Impl->jointIndexByName.find(name);
        if (found == m_Impl->jointIndexByName.end())
            found = m_Impl->jointIndexByName.find(ToLowerAscii(name));
        if (found == m_Impl->jointIndexByName.end() && name.empty() && meshJoint < m_Impl->models.size())
        {
            found = m_Impl->jointIndexByName.end();
            const VrHandMatrix4 model = ToVrHandMatrix(m_Impl->models[meshJoint]);
            outPalette[meshJoint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(model, asset.inverseBindMatrices[meshJoint]));
            continue;
        }
        if (found == m_Impl->jointIndexByName.end())
        {
            outError = "hand GLB joint name does not exist in the OpenVR skeleton: " + name;
            outPalette.clear();
            return false;
        }

        const VrHandMatrix4 model = ToVrHandMatrix(m_Impl->models[static_cast<size_t>(found->second)]);
        outPalette[meshJoint] = VrHandMath::ToRows3x4(VrHandMath::Multiply(model, asset.inverseBindMatrices[meshJoint]));
    }
    return true;
}

bool VrHandSkeletonRuntime::IsInitialized() const
{
    return m_Impl && m_Impl->skeleton != nullptr;
}

bool VrHandSkeletonRuntime::HasPose() const
{
    return m_Impl && m_Impl->hasPose;
}

int VrHandSkeletonRuntime::JointCount() const
{
    return (m_Impl && m_Impl->skeleton) ? m_Impl->skeleton->num_joints() : 0;
}
