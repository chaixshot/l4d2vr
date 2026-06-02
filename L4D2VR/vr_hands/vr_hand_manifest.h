#pragma once

#include <string>

// Returns either the original manifest path or a generated copy containing
// the two OpenVR skeletal input actions required by the independent hand renderer.
std::string BuildVrHandsActionManifestPath(const char* originalManifestPath);
