#pragma once

#include "vr_hand_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace VrHandVmPose
{
    struct Snapshot
    {
        std::string modelName;
        std::vector<std::string> boneNames;
        std::vector<int> boneParents;
        VrHandMatrix4 modelWorldMatrix{};
        std::vector<VrHandMatrix4> boneWorldMatrices;
        std::uint32_t tick = 0;
        std::uint32_t sequence = 0;
        bool valid = false;
    };

    void Capture(
        const char* modelName,
        const std::vector<std::string>& boneNames,
        const std::vector<int>& boneParents,
        const VrHandMatrix4& modelWorldMatrix,
        const std::vector<VrHandMatrix4>& boneWorldMatrices);

    bool GetLatest(Snapshot& outSnapshot, std::uint32_t maxAgeMs);
}
