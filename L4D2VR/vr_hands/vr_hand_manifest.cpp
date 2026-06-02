#include "vr_hand_manifest.h"

#include "game.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr const char* kLeftSkeletonAction = "/actions/main/in/SkeletonLeftHand";
    constexpr const char* kRightSkeletonAction = "/actions/main/in/SkeletonRightHand";

    bool ReadTextFile(const std::filesystem::path& path, std::string& out)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return false;
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        out = buffer.str();
        return true;
    }

    bool WriteTextFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        return stream.good();
    }

    size_t FindMatchingContainerEnd(const std::string& json, size_t start, char opening, char closing)
    {
        if (start == std::string::npos || start >= json.size() || json[start] != opening)
            return std::string::npos;

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (size_t i = start; i < json.size(); ++i)
        {
            const char c = json[i];
            if (inString)
            {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }

            if (c == '"')
            {
                inString = true;
                continue;
            }
            if (c == opening)
                ++depth;
            else if (c == closing)
            {
                --depth;
                if (depth == 0)
                    return i;
            }
        }
        return std::string::npos;
    }

    bool ContainerHasEntries(const std::string& json, size_t start, size_t end)
    {
        for (size_t i = start + 1; i < end; ++i)
        {
            if (!std::isspace(static_cast<unsigned char>(json[i])))
                return true;
        }
        return false;
    }

    bool AppendSkeletonActions(std::string& json)
    {
        const bool hasLeft = json.find(kLeftSkeletonAction) != std::string::npos;
        const bool hasRight = json.find(kRightSkeletonAction) != std::string::npos;
        if (hasLeft && hasRight)
            return true;

        const size_t actionsKey = json.find("\"actions\"");
        const size_t arrayStart = actionsKey == std::string::npos ? std::string::npos : json.find('[', actionsKey);
        const size_t arrayEnd = FindMatchingContainerEnd(json, arrayStart, '[', ']');
        if (arrayEnd == std::string::npos)
            return false;

        std::string actions;
        if (!hasLeft)
            actions += "    { \"name\": \"/actions/main/in/SkeletonLeftHand\", \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/left\" }";
        if (!hasRight)
        {
            if (!actions.empty())
                actions += ",\r\n";
            actions += "    { \"name\": \"/actions/main/in/SkeletonRightHand\", \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/right\" }";
        }

        const std::string insertion =
            std::string(ContainerHasEntries(json, arrayStart, arrayEnd) ? ",\r\n" : "\r\n") +
            actions + "\r\n";
        json.insert(arrayEnd, insertion);
        return true;
    }

    bool AppendSkeletonBindings(std::string& json)
    {
        const bool hasLeft = json.find("/actions/main/in/skeletonlefthand") != std::string::npos;
        const bool hasRight = json.find("/actions/main/in/skeletonrighthand") != std::string::npos;
        if (hasLeft && hasRight)
            return true;

        const size_t actionSetKey = json.find("\"/actions/main\"");
        const size_t objectStart = actionSetKey == std::string::npos ? std::string::npos : json.find('{', actionSetKey);
        const size_t objectEnd = FindMatchingContainerEnd(json, objectStart, '{', '}');
        if (objectEnd == std::string::npos)
            return false;

        std::string mappings;
        if (!hasLeft)
            mappings += "        { \"output\": \"/actions/main/in/skeletonlefthand\", \"path\": \"/user/hand/left/input/skeleton/left\" }";
        if (!hasRight)
        {
            if (!mappings.empty())
                mappings += ",\r\n";
            mappings += "        { \"output\": \"/actions/main/in/skeletonrighthand\", \"path\": \"/user/hand/right/input/skeleton/right\" }";
        }

        const size_t skeletonKey = json.find("\"skeleton\"", objectStart);
        if (skeletonKey != std::string::npos && skeletonKey < objectEnd)
        {
            const size_t arrayStart = json.find('[', skeletonKey);
            const size_t arrayEnd = FindMatchingContainerEnd(json, arrayStart, '[', ']');
            if (arrayEnd == std::string::npos || arrayEnd > objectEnd)
                return false;

            const std::string insertion =
                std::string(ContainerHasEntries(json, arrayStart, arrayEnd) ? ",\r\n" : "\r\n") +
                mappings + "\r\n      ";
            json.insert(arrayEnd, insertion);
            return true;
        }

        const std::string insertion =
            std::string(ContainerHasEntries(json, objectStart, objectEnd) ? ",\r\n" : "\r\n") +
            "      \"skeleton\": [\r\n" +
            mappings + "\r\n"
            "      ]\r\n";
        json.insert(objectEnd, insertion);
        return true;
    }

    std::filesystem::path MakeGeneratedBindingPath(const std::filesystem::path& originalPath)
    {
        const std::string generatedName = originalPath.stem().string() + ".l4d2vr_hands" + originalPath.extension().string();
        return originalPath.parent_path() / generatedName;
    }

    bool BuildPatchedBinding(const std::filesystem::path& originalPath, std::filesystem::path& outGeneratedPath)
    {
        std::string json;
        if (!ReadTextFile(originalPath, json))
            return false;
        if (!AppendSkeletonBindings(json))
            return false;

        outGeneratedPath = MakeGeneratedBindingPath(originalPath);
        return WriteTextFile(outGeneratedPath, json);
    }

    bool ExtractQuotedValue(const std::string& text, size_t keyPosition, size_t& outValueStart, size_t& outValueEnd)
    {
        const size_t colon = text.find(':', keyPosition);
        if (colon == std::string::npos)
            return false;
        const size_t firstQuote = text.find('"', colon + 1);
        if (firstQuote == std::string::npos)
            return false;

        bool escaped = false;
        for (size_t i = firstQuote + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '"')
            {
                outValueStart = firstQuote + 1;
                outValueEnd = i;
                return true;
            }
        }
        return false;
    }

    void PatchDefaultBindings(std::string& manifestJson, const std::filesystem::path& manifestDirectory)
    {
        constexpr const char* kBindingUrlKey = "\"binding_url\"";
        size_t searchPosition = 0;
        while (true)
        {
            const size_t keyPosition = manifestJson.find(kBindingUrlKey, searchPosition);
            if (keyPosition == std::string::npos)
                break;

            size_t valueStart = 0;
            size_t valueEnd = 0;
            if (!ExtractQuotedValue(manifestJson, keyPosition, valueStart, valueEnd))
                break;

            const std::string relativeUrl = manifestJson.substr(valueStart, valueEnd - valueStart);
            const std::filesystem::path originalBindingPath = manifestDirectory / std::filesystem::path(relativeUrl);
            std::filesystem::path generatedBindingPath;
            if (BuildPatchedBinding(originalBindingPath, generatedBindingPath))
            {
                const std::filesystem::path generatedRelativePath = generatedBindingPath.lexically_relative(manifestDirectory);
                const std::string generatedRelativeUrl = generatedRelativePath.generic_string();
                manifestJson.replace(valueStart, valueEnd - valueStart, generatedRelativeUrl);
                searchPosition = valueStart + generatedRelativeUrl.size();
            }
            else
            {
                Game::errorMsg(("[VR][Hands] Cannot patch SteamVR binding JSON: " + originalBindingPath.string()).c_str());
                searchPosition = valueEnd + 1;
            }
        }
    }
}

std::string BuildVrHandsActionManifestPath(const char* originalManifestPath)
{
    if (!originalManifestPath || !*originalManifestPath)
        return {};

    const std::filesystem::path originalPath(originalManifestPath);
    std::string json;
    if (!ReadTextFile(originalPath, json))
    {
        Game::errorMsg("[VR][Hands] Cannot read SteamVR action manifest");
        return originalPath.string();
    }

    if (!AppendSkeletonActions(json))
    {
        Game::errorMsg("[VR][Hands] SteamVR action manifest has no valid actions array");
        return originalPath.string();
    }

    PatchDefaultBindings(json, originalPath.parent_path());

    const std::filesystem::path generatedPath = originalPath.parent_path() / "action_manifest.l4d2vr_hands.json";
    if (!WriteTextFile(generatedPath, json))
    {
        Game::errorMsg("[VR][Hands] Cannot write patched SteamVR action manifest");
        return originalPath.string();
    }

    Game::logMsg("[VR][Hands] SteamVR skeletal actions enabled through %s", generatedPath.string().c_str());
    return generatedPath.string();
}
