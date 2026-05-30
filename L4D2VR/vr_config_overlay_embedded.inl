// Embedded VR config editor for L4D2VR.
// Included by game.cpp so the existing vcxproj does not need a new ClCompile item.
// Open/close with F8. This panel only exposes options that exist in L4D2VRConfigTool.

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif


#include <atomic>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cctype>

namespace
{
    constexpr int kCfgOverlayW = 1280;
    constexpr int kCfgOverlayH = 900;
    constexpr int kCfgOverlayRowH = 46;
    constexpr int kCfgOverlayGroupH = 32;
    constexpr int kCfgOverlayRowsVisible = 11;

    constexpr int kCfgLangX = 914;
    constexpr int kCfgReloadX = 1008;
    constexpr int kCfgSaveX = 1100;
    constexpr int kCfgPagePrevX = 610;
    constexpr int kCfgPageNextX = 720;
    constexpr int kCfgPageButtonW = 96;
    constexpr int kCfgPageButtonH = 42;
    constexpr int kCfgCloseX = 1180;
    constexpr int kCfgTopButtonY = 24;
    constexpr int kCfgTopButtonH = 42;
    constexpr int kCfgLangW = 76;
    constexpr int kCfgReloadW = 82;
    constexpr int kCfgSaveW = 70;
    constexpr int kCfgCloseW = 76;

    constexpr int kCfgRowsX = 24;
    constexpr int kCfgRowsY = 156;
    constexpr int kCfgRowsBottom = kCfgOverlayH - 156;
    constexpr int kCfgRowsW = 1232;
    constexpr int kCfgSliderX = 500;
    constexpr int kCfgSliderW = 560;
    constexpr int kCfgSliderH = 26;
    constexpr int kCfgValueX = 1082;
    constexpr int kCfgMinusX = 1118;
    constexpr int kCfgPlusX = 1184;
    constexpr int kCfgAdjustButtonW = 50;
    constexpr int kCfgComponentX = 500;
    constexpr int kCfgComponentButtonW = 62;
    constexpr int kCfgComponentButtonH = 30;
    constexpr int kCfgComponentGap = 8;
    constexpr int kCfgComponentValueX = 792;
    constexpr int kCfgStringEditX = 1100;
    constexpr int kCfgStringEditW = 120;

    constexpr int kCfgMenuButtonW = 260;
    constexpr int kCfgMenuButtonH = 90;

    constexpr uint32_t kCfgOverlaySortOrderBase = 0x7FFFFF00u;
    constexpr uint32_t kCfgOverlaySortOrderActive = 0x7FFFFF10u;
    constexpr uint32_t kCfgMenuButtonSortOrder = 0x7FFFFF20u;
    // After joystick/trackpad scrolling, temporarily ignore hover-to-select mouse move
    // events from the VR laser pointer. Without this guard, a stationary pointer over a
    // row can immediately steal selection back from the scroll target.
    constexpr uint32_t kCfgHoverSelectSuppressAfterScrollMs = 260u;

    enum class CfgOptionType
    {
        Bool,
        Float,
        Int,
        Color,
        String,
        Vec3
    };

    struct CfgOptionSpec
    {
        const char* key;
        CfgOptionType type;
        const char* groupZhUtf8;
        const char* titleZhUtf8;
        float minValue;
        float maxValue;
        const char* defaultValue;
    };

    static const CfgOptionSpec kCfgOptionSpecs[] =
    {
        { "ConfigOverlayDistanceMeters", CfgOptionType::Float, "\351\205\215\347\275\256\351\235\242\346\235\277", "\351\205\215\347\275\256\351\235\242\346\235\277\350\267\235\347\246\273", 0.6f, 3.0f, "1.35" },
        { "ConfigOverlaySizeMeters", CfgOptionType::Float, "\351\205\215\347\275\256\351\235\242\346\235\277", "\351\205\215\347\275\256\351\235\242\346\235\277\345\244\247\345\260\217", 0.8f, 4.0f, "2.05" },
        { "VRScale", CfgOptionType::Float, "\xE8\xA7\x86\xE8\xA7\x92 / \xE5\xB0\xBA\xE5\xBA\xA6", "\xE4\xB8\x96\xE7\x95\x8C\xE7\xBC\xA9\xE6\x94\xBE", 30.0f, 55.0f, "43.2" },
        { "Roomscale1To1Movement", CfgOptionType::Bool, "\346\210\277\351\227\264\347\247\273\345\212\250", "1:1\347\247\273\345\212\250", 0.0f, 0.0f, "false" },
        { "Roomscale1To1ServerMove", CfgOptionType::Bool, "\346\210\277\351\227\264\347\247\273\345\212\250", "VR Server Move", 0.0f, 0.0f, "true" },
        { "Roomscale1To1MovementScale", CfgOptionType::Float, "\346\210\277\351\227\264\347\247\273\345\212\250", "1:1\347\247\273\345\212\250\350\267\235\347\246\273\345\200\215\347\216\207", 0.0f, 4.0f, "1.0" },
        { "Roomscale1To1MinApplyMeters", CfgOptionType::Float, "\346\210\277\351\227\264\347\247\273\345\212\250", "\346\234\200\345\260\217\345\272\224\347\224\250\350\267\235\347\246\273", 0.0f, 0.3f, "0.1" },
        { "Roomscale1To1PhysicalCrouch", CfgOptionType::Bool, "\346\210\277\351\227\264\347\247\273\345\212\250", "\347\216\260\345\256\236\350\271\262\344\274\217", 0.0f, 0.0f, "true" },
        { "Roomscale1To1CrouchEnterMeters", CfgOptionType::Float, "\346\210\277\351\227\264\347\247\273\345\212\250", "\350\271\262\344\274\217\350\247\246\345\217\221\351\253\230\345\272\246\345\267\256", 0.05f, 1.0f, "0.25" },
        { "Roomscale1To1CrouchExitMeters", CfgOptionType::Float, "\346\210\277\351\227\264\347\247\273\345\212\250", "\350\271\262\344\274\217\351\207\212\346\224\276\351\253\230\345\272\246\345\267\256", 0.0f, 1.0f, "0.18" },
        { "IPDScale", CfgOptionType::Float, "\xE8\xA7\x86\xE8\xA7\x92 / \xE5\xB0\xBA\xE5\xBA\xA6", "\xE7\x9E\xB3\xE8\xB7\x9D\xE7\xBC\xA9\xE6\x94\xBE", 0.8f, 1.2f, "1.0" },
        { "LeftHanded", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE5\xB7\xA6\xE6\x89\x8B\xE6\x8C\x81\xE6\xAD\xA6", 0.0f, 0.0f, "false" },
        { "MoveDirectionFromController", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE6\x89\x8B\xE6\x9F\x84\xE5\x86\xB3\xE5\xAE\x9A\xE5\x89\x8D\xE8\xBF\x9B\xE6\x96\xB9\xE5\x90\x91", 0.0f, 0.0f, "false" },
        { "TurnSpeed", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE5\xB9\xB3\xE6\xBB\x91\xE8\xBD\xAC\xE5\x90\x91\xE9\x80\x9F\xE5\xBA\xA6", 0.1f, 1.0f, "0.3" },
        { "SnapTurning", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE5\x90\xAF\xE7\x94\xA8\xE5\x88\x86\xE6\xAE\xB5\xE8\xBD\xAC\xE5\x90\x91", 0.0f, 0.0f, "false" },
        { "SnapTurnAngle", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE5\x88\x86\xE6\xAE\xB5\xE8\xBD\xAC\xE5\x90\x91\xE8\xA7\x92\xE5\xBA\xA6", 15.0f, 120.0f, "45.0" },
        { "ControllerSmoothing", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE8\xBD\xAC\xE5\x90\x91", "\xE6\x89\x8B\xE6\x9F\x84\xE8\xBE\x93\xE5\x85\xA5\xE5\xB9\xB3\xE6\xBB\x91", 0.0f, 0.8f, "0.0" },
        { "MouseModeEnabled", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE5\x90\xAF\xE7\x94\xA8\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", 0.0f, 0.0f, "false" },
        { "MouseModeAimFromHmd", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE4\xBB\x8E\xE5\xA4\xB4\xE6\x98\xBE\xE7\x9E\x84\xE5\x87\x86", 0.0f, 0.0f, "true" },
        { "MouseModeYawSensitivity", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\xBC\xA0\xE6\xA0\x87\xE6\xB0\xB4\xE5\xB9\xB3\xE7\x81\xB5\xE6\x95\x8F\xE5\xBA\xA6", -0.2f, 0.2f, "0.01" },
        { "MouseModePitchSensitivity", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\xBC\xA0\xE6\xA0\x87\xE5\x9E\x82\xE7\x9B\xB4\xE7\x81\xB5\xE6\x95\x8F\xE5\xBA\xA6", -0.2f, 0.2f, "0.01" },
        { "MouseModePitchAffectsView", CfgOptionType::Bool, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\xBC\xA0\xE6\xA0\x87\xE4\xBF\xAF\xE4\xBB\xB0\xE5\xB8\xA6\xE5\x8A\xA8\xE8\xA7\x86\xE8\xA7\x92", 0.0f, 0.0f, "true" },
        { "MouseModeTurnSmoothing", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE6\xB0\xB4\xE5\xB9\xB3\xE5\xB9\xB3\xE6\xBB\x91 (\xE7\xA7\x92)", 0.0f, 0.25f, "0.03" },
        { "MouseModePitchSmoothing", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE5\x9E\x82\xE7\x9B\xB4\xE5\xB9\xB3\xE6\xBB\x91 (\xE7\xA7\x92)", 0.0f, 0.25f, "0.03" },
        { "MouseModeViewmodelAnchorOffset", CfgOptionType::Vec3, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\xBC\xA0\xE6\xA0\x87\xE6\xA8\xA1\xE5\xBC\x8F Viewmodel \xE9\x94\x9A\xE7\x82\xB9\xE5\x81\x8F\xE7\xA7\xBB (\xE7\xB1\xB3)", -1.0f, 1.0f, "0.42,0.0,-0.28" },
        { "MouseModeAimConvergeDistance", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\xBC\xA0\xE6\xA0\x87\xE6\xA8\xA1\xE5\xBC\x8F\xE6\xB1\x87\xE8\x81\x9A\xE8\xB7\x9D\xE7\xA6\xBB (\xE5\x8D\x95\xE4\xBD\x8D)", 0.0f, 8192.0f, "2048" },
        { "MouseModeScopeToggleKey", CfgOptionType::String, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F\xE5\xBC\x80\xE9\x95\x9C\xE5\x88\x87\xE6\x8D\xA2\xE6\x8C\x89\xE9\x94\xAE", 0.0f, 0.0f, "key:q" },
        { "ForceNonVRServerMovement", CfgOptionType::Bool, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "\xE9\x9D\x9EVR\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\x85\xBC\xE5\xAE\xB9\xE6\xA8\xA1\xE5\xBC\x8F", 0.0f, 0.0f, "false" },
        { "AfkFallGuardMinDownSpeed", CfgOptionType::Float, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "AFK \xE9\x98\xB2\xE5\x9D\xA0\xE8\x90\xBD\xE6\x9C\x80\xE5\xB0\x8F\xE4\xB8\x8B\xE8\x90\xBD\xE9\x80\x9F\xE5\xBA\xA6", 0.0f, 2000.0f, "560" },
        { "AfkFallGuardTraceDistance", CfgOptionType::Float, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "AFK \xE9\x98\xB2\xE5\x9D\xA0\xE8\x90\xBD\xE6\x8E\xA2\xE6\xB5\x8B\xE8\xB7\x9D\xE7\xA6\xBB", 1.0f, 1024.0f, "256" },
        { "AfkFallGuardLeadSeconds", CfgOptionType::Float, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "AFK \xE9\x98\xB2\xE5\x9D\xA0\xE8\x90\xBD\xE6\x8F\x90\xE5\x89\x8D\xE9\x87\x8F (\xE7\xA7\x92)", 0.01f, 1.0f, "0.25" },
        { "AfkFallGuardCooldownSeconds", CfgOptionType::Float, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "AFK \xE9\x98\xB2\xE5\x9D\xA0\xE8\x90\xBD\xE5\x86\xB7\xE5\x8D\xB4 (\xE7\xA7\x92)", 0.0f, 10.0f, "1.0" },
        { "AfkFallGuardDebugLog", CfgOptionType::Bool, "\xE5\xA4\x9A\xE4\xBA\xBA / \xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8", "AFK \xE9\x98\xB2\xE5\x9D\xA0\xE8\x90\xBD\xE6\x97\xA5\xE5\xBF\x97", 0.0f, 0.0f, "false" },
        { "AutoMatQueueMode", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "\xE5\xA4\x9A\xE6\xA0\xB8\xE6\xB8\xB2\xE6\x9F\x93", 0.0f, 0.0f, "false" },
        { "QueuedSubmitUseRenderPoseToken", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "\xE5\xA4\x9A\xE6\xA0\xB8\xE5\xAE\x8C\xE5\x85\xA8\xE5\xB8\xA7\xE5\x90\x8C\xE6\xAD\xA5", 0.0f, 0.0f, "true" },
        { "ReShadeVRCompat", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "ReShade VR\xE5\x85\xBC\xE5\xAE\xB9\xE6\xA8\xA1\xE5\xBC\x8F", 0.0f, 0.0f, "false" },
        { "DesktopMirrorEnabled", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "\xE6\xA1\x8C\xE9\x9D\xA2\xE9\x95\x9C\xE5\x83\x8F", 0.0f, 0.0f, "true" },
        { "QueuedRenderPoseRelaxPercent", CfgOptionType::Int, "Performance", "Queued Pose Relax (%)", 0.0f, 100.0f, "0" },
        { "LocalVScriptConvarsEnabled", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "\xE5\x90\xAF\xE7\x94\xA8LOD\xE4\xBC\x98\xE5\x8C\x96", 0.0f, 0.0f, "false" },
        { "ShadowTweaksEnabled", CfgOptionType::Bool, "\xE6\x80\xA7\xE8\x83\xBD", "\xE9\x98\xB4\xE5\xBD\xB1\xE4\xBC\x98\xE5\x8C\x96", 0.0f, 0.0f, "false" },
        { "HudDistance", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE4\xB8\xBB\xE7\x95\x8C\xE9\x9D\xA2\xEF\xBC\x89", "HUD \xE8\xB7\x9D\xE7\xA6\xBB", 0.5f, 3.0f, "1.3" },
        { "HudSize", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE4\xB8\xBB\xE7\x95\x8C\xE9\x9D\xA2\xEF\xBC\x89", "HUD \xE5\xB0\xBA\xE5\xAF\xB8", 0.8f, 3.0f, "1.3" },
        { "FixedHudXOffset", CfgOptionType::Float, "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD \345\267\246\345\217\263\345\201\217\347\247\273", -1.0f, 1.0f, "0.0" },
        { "FixedHudYOffset", CfgOptionType::Float, "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD \344\270\212\344\270\213\345\201\217\347\247\273", -1.0f, 1.0f, "0.25" },
        { "TopHudCurvature", CfgOptionType::Float, "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD \346\233\262\347\216\207", 0.0f, 1.0f, "0.2" },
        { "HudFollowHmdMovement", CfgOptionType::Bool, "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD \350\267\237\351\232\217HMD\344\270\212\344\270\213", 0.0f, 0.0f, "false" },
        { "HudAlwaysVisible", CfgOptionType::Bool, "HUD\xEF\xBC\x88\xE4\xB8\xBB\xE7\x95\x8C\xE9\x9D\xA2\xEF\xBC\x89", "HUD \xE6\x80\xBB\xE6\x98\xAF\xE5\x8F\xAF\xE8\xA7\x81", 0.0f, 0.0f, "false" },
        { "LeftWristHudEnabled", CfgOptionType::Bool, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\x90\xAF\xE7\x94\xA8\xE7\x8A\xB6\xE6\x80\x81HUD\xEF\xBC\x88\xE5\x89\xAF\xE6\x89\x8B\xEF\xBC\x89", 0.0f, 0.0f, "false" },
        { "LeftWristHudWidthMeters", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE7\x8A\xB6\xE6\x80\x81HUD\xE5\xAE\xBD\xE5\xBA\xA6\xEF\xBC\x88\xE7\xB1\xB3\xEF\xBC\x89", 0.01f, 0.4f, "0.1" },
        { "LeftWristHudXOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE7\x8A\xB6\xE6\x80\x81HUD X\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "0.01" },
        { "LeftWristHudYOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE7\x8A\xB6\xE6\x80\x81HUD Y\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "0.01" },
        { "LeftWristHudZOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE7\x8A\xB6\xE6\x80\x81HUD Z\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "-0.0" },
        { "LeftWristHudAngleOffset", CfgOptionType::Vec3, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE7\x8A\xB6\xE6\x80\x81HUD\xE8\xA7\x92\xE5\xBA\xA6\xE5\x81\x8F\xE7\xA7\xBB (\xE4\xBF\xAF\xE4\xBB\xB0,\xE5\x81\x8F\xE8\x88\xAA,\xE7\xBF\xBB\xE6\xBB\x9A)", -180.0f, 180.0f, "-75,0,0" },
        { "RightAmmoHudEnabled", CfgOptionType::Bool, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\x90\xAF\xE7\x94\xA8\xE5\xBC\xB9\xE8\x8D\xAFHUD\xEF\xBC\x88\xE6\x8C\x81\xE6\x9E\xAA\xE6\x89\x8B\xEF\xBC\x89", 0.0f, 0.0f, "false" },
        { "RightAmmoHudWidthMeters", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\xBC\xB9\xE8\x8D\xAFHUD\xE5\xAE\xBD\xE5\xBA\xA6\xEF\xBC\x88\xE7\xB1\xB3\xEF\xBC\x89", 0.01f, 0.5f, "0.8" },
        { "RightAmmoHudXOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\xBC\xB9\xE8\x8D\xAFHUD X\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "-0.07" },
        { "RightAmmoHudYOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\xBC\xB9\xE8\x8D\xAFHUD Y\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "0.03" },
        { "RightAmmoHudZOffset", CfgOptionType::Float, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\xBC\xB9\xE8\x8D\xAFHUD Z\xE5\x81\x8F\xE7\xA7\xBB", -0.25f, 0.25f, "-0.09" },
        { "RightAmmoHudAngleOffset", CfgOptionType::Vec3, "HUD\xEF\xBC\x88\xE6\x89\x8B\xE6\x9F\x84\xEF\xBC\x89", "\xE5\xBC\xB9\xE8\x8D\xAFHUD\xE8\xA7\x92\xE5\xBA\xA6\xE5\x81\x8F\xE7\xA7\xBB (\xE4\xBF\xAF\xE4\xBB\xB0,\xE5\x81\x8F\xE8\x88\xAA,\xE7\xBF\xBB\xE6\xBB\x9A)", -180.0f, 180.0f, "-75,0,0" },
        { "HideArms", CfgOptionType::Bool, "\xE6\x89\x8B\xE9\x83\xA8 / \xE8\xB0\x83\xE8\xAF\x95", "\xE9\x9A\x90\xE8\x97\x8F\xE6\x89\x8B\xE8\x87\x82", 0.0f, 0.0f, "false" },
        { "RequireSecondaryAttackForItemSwitch", CfgOptionType::Bool, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE5\x88\x87\xE6\x8D\xA2\xE7\x89\xA9\xE5\x93\x81\xE9\x9C\x80\xE5\x89\xAF\xE6\x94\xBB\xE9\x94\xAE", 0.0f, 0.0f, "false" },
        { "VoiceRecordCombo", CfgOptionType::String, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE8\xAF\xAD\xE9\x9F\xB3\xE8\x81\x8A\xE5\xA4\xA9\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", 0.0f, 0.0f, "Crouch+Reload" },
        { "QuickTurnCombo", CfgOptionType::String, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE5\xBF\xAB\xE9\x80\x9F\xE8\xBD\xAC\xE8\xBA\xAB\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", 0.0f, 0.0f, "SecondaryAttack+Crouch" },
        { "ViewmodelAdjustEnabled", CfgOptionType::Bool, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE5\x90\xAF\xE7\x94\xA8\xE6\xAD\xA6\xE5\x99\xA8\xE4\xBD\x8D\xE7\xBD\xAE\xE8\xB0\x83\xE6\x95\xB4", 0.0f, 0.0f, "false" },
        { "ViewmodelAdjustCombo", CfgOptionType::String, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE6\xAD\xA6\xE5\x99\xA8\xE4\xBD\x8D\xE7\xBD\xAE\xE8\xB0\x83\xE6\x95\xB4\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", 0.0f, 0.0f, "Reload+SecondaryAttack" },
        { "ViewmodelAdjustMoveSpeed", CfgOptionType::Float, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE8\xA7\x86\xE6\xA8\xA1\xE8\xB0\x83\xE6\x95\xB4\xE7\xA7\xBB\xE5\x8A\xA8\xE9\x80\x9F\xE5\xBA\xA6", 0.1f, 5.0f, "1.0" },
        { "ViewmodelAdjustRotateSpeed", CfgOptionType::Float, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE8\xA7\x86\xE6\xA8\xA1\xE8\xB0\x83\xE6\x95\xB4\xE6\x97\x8B\xE8\xBD\xAC\xE9\x80\x9F\xE5\xBA\xA6", 0.1f, 5.0f, "1.0" },
        { "ViewmodelDisableMoveBob", CfgOptionType::Bool, "\xE4\xBA\xA4\xE4\xBA\x92 / \xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE", "\xE7\xA6\x81\xE7\x94\xA8\xE6\xAD\xA6\xE5\x99\xA8\xE7\xA7\xBB\xE5\x8A\xA8\xE6\x99\x83\xE5\x8A\xA8", 0.0f, 0.0f, "false" },
        { "AutoRepeatSemiAutoFire", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x8D\x95\xE5\x8F\x91\xE6\x9E\xAA\xE9\x95\xBF\xE6\x8C\x89\xE8\xBF\x9E\xE5\x8F\x91", 0.0f, 0.0f, "false" },
        { "AutoRepeatSemiAutoFireHz", CfgOptionType::Float, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE8\xBF\x9E\xE7\x82\xB9\xE9\xA2\x91\xE7\x8E\x87 (Hz)", 1.0f, 12.0f, "20.0" },
        { "AutoRepeatSprayPushEnabled", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE8\x87\xAA\xE5\x8A\xA8 Spray-Push", 0.0f, 0.0f, "false" },
        { "HitSoundEnabled", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x91\xBD\xE4\xB8\xAD\xE9\x9F\xB3\xE6\x95\x88\xE5\x8F\x8D\xE9\xA6\x88", 0.0f, 0.0f, "false" },
        { "HitSoundSpec", CfgOptionType::String, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x91\xBD\xE4\xB8\xAD\xE9\x9F\xB3\xE6\x95\x88", 0.0f, 0.0f, "game:vrmod/hit.mp3" },
        { "HitSoundVolume", CfgOptionType::Float, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x8F\x8D\xE9\xA6\x88\xE9\x9F\xB3\xE9\x87\x8F", 0.5f, 2.0f, "1.2" },
        { "KillSoundEnabled", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x87\xBB\xE6\x9D\x80\xE9\x9F\xB3\xE6\x95\x88\xE5\x8F\x8D\xE9\xA6\x88", 0.0f, 0.0f, "false" },
        { "KillSoundNormalSpec", CfgOptionType::String, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE6\x99\xAE\xE9\x80\x9A\xE5\x87\xBB\xE6\x9D\x80\xE9\x9F\xB3\xE6\x95\x88", 0.0f, 0.0f, "game:vrmod/kill.mp3" },
        { "KillSoundHeadshotSpec", CfgOptionType::String, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE7\x88\x86\xE5\xA4\xB4\xE5\x87\xBB\xE6\x9D\x80\xE9\x9F\xB3\xE6\x95\x88", 0.0f, 0.0f, "game:vrmod/headshot.mp3" },
        { "KillSoundVolume", CfgOptionType::Float, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x87\xBB\xE6\x9D\x80\xE9\x9F\xB3\xE9\x87\x8F", 0.0f, 2.0f, "1.8" },
        { "HeadshotSoundVolume", CfgOptionType::Float, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE7\x88\x86\xE5\xA4\xB4\xE9\x9F\xB3\xE9\x87\x8F", 0.0f, 2.0f, "1.3" },
        { "KillIndicatorEnabled", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x91\xBD\xE4\xB8\xAD / \xE5\x87\xBB\xE6\x9D\x80\xE5\x9B\xBE\xE6\xA0\x87", 0.0f, 0.0f, "false" },
        { "KillIndicatorMaterialBaseSpec", CfgOptionType::String, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x87\xBB\xE6\x9D\x80\xE5\x9B\xBE\xE6\xA0\x87\xE6\x9D\x90\xE8\xB4\xA8\xE7\x9B\xAE\xE5\xBD\x95", 0.0f, 0.0f, "overlays/2965700751" },
        { "BlockFireOnFriendlyAimEnabled", CfgOptionType::Bool, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "\xE7\xA6\x81\xE6\xAD\xA2\xE5\x90\x91\xE9\x98\x9F\xE5\x8F\x8B\xE5\xBC\x80\xE7\x81\xAB", 0.0f, 0.0f, "false" },
        { "MotionGestureSwingThreshold", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE6\x8C\xA5\xE5\x8A\xA8\xE5\x88\xA4\xE5\xAE\x9A\xE9\x98\x88\xE5\x80\xBC", 0.5f, 50.0f, "2" },
        { "MotionGestureDownSwingThreshold", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE4\xB8\x8B\xE5\x8A\x88\xE5\x88\xA4\xE5\xAE\x9A\xE9\x98\x88\xE5\x80\xBC", 0.5f, 4.0f, "2.0" },
        { "MotionGestureJumpThreshold", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE8\xB7\xB3\xE8\xB7\x83\xE6\x89\x8B\xE5\x8A\xBF\xE9\x98\x88\xE5\x80\xBC", 0.5f, 4.0f, "2.0" },
        { "MotionGestureCooldown", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE6\x89\x8B\xE5\x8A\xBF\xE5\x86\xB7\xE5\x8D\xB4", 0.0f, 2.0f, "0.8" },
        { "MotionGestureHoldDuration", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE6\x89\x8B\xE5\x8A\xBF\xE6\x8C\x89\xE4\xBD\x8F\xE6\x97\xB6\xE9\x95\xBF", 0.0f, 1.0f, "0.2" },
        { "InventoryGestureRange", CfgOptionType::Float, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE9\x81\x93\xE5\x85\xB7\xE9\x94\x9A\xE7\x82\xB9\xE6\x8A\x93\xE5\x8F\x96\xE7\x9A\x84\xE6\x9C\x89\xE6\x95\x88\xE8\x8C\x83\xE5\x9B\xB4", 0.1f, 0.5f, "0.16" },
        { "ShowInventoryAnchors", CfgOptionType::Bool, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE6\x98\xBE\xE7\xA4\xBA\xE9\x81\x93\xE5\x85\xB7\xE6\xA0\x8F\xE6\x8A\x93\xE5\x8F\x96\xE5\x8C\xBA\xE5\x9F\x9F\xEF\xBC\x88\xE4\xB8\x8D\xE7\x86\x9F\xE6\x82\x89\xE4\xBD\x8D\xE7\xBD\xAE\xE6\x97\xB6\xE5\xBB\xBA\xE8\xAE\xAE\xE5\xBC\x80\xE5\x90\xAF\xEF\xBC\x89", 0.0f, 0.0f, "true" },
        { "ScopeEnabled", CfgOptionType::Bool, "\xE5\x85\x89\xE5\xAD\xA6", "\xE5\x90\xAF\xE7\x94\xA8\xE6\x9E\xAA\xE6\xA2\xB0\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C", 0.0f, 0.0f, "false" },
        { "ScopeRTTSize", CfgOptionType::Int, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE6\xB8\xB2\xE6\x9F\x93\xE5\x88\x86\xE8\xBE\xA8\xE7\x8E\x87", 128.0f, 1024.0f, "512" },
        { "ScopeReticleAlpha", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x87\x86\xE6\x98\x9F\xE9\x80\x8F\xE6\x98\x8E\xE5\xBA\xA6", 0.0f, 1.0f, "1.0" },
        { "ScopeDefaultFov", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4" "FOV", 1.0f, 45.0f, "20" },
        { "ScopeMagnificationFovRange", CfgOptionType::String, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x80\x8D\xE7\x8E\x87" "FOV\xE8\x8C\x83\xE5\x9B\xB4", 0.0f, 0.0f, "3,20" },
        { "ScopeMagnificationAdjustSpeed", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x80\x8D\xE7\x8E\x87\xE8\xB0\x83\xE8\x8A\x82\xE9\x80\x9F\xE5\xBA\xA6", 0.0f, 180.0f, "18" },
        { "ScopeSizeRange", CfgOptionType::String, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE8\x8C\x83\xE5\x9B\xB4", 0.0f, 0.0f, "0.03,0.30" },
        { "ScopeSizeAdjustSpeed", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE8\xB0\x83\xE8\x8A\x82\xE9\x80\x9F\xE5\xBA\xA6", 0.0f, 5.0f, "0.12" },
        { "ScopeOffsetAdjustMoveSpeed", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE4\xBD\x8D\xE7\xBD\xAE\xE8\xB0\x83\xE6\x95\xB4\xE9\x80\x9F\xE5\xBA\xA6", 0.1f, 5.0f, "1.0" },
        { "ScopeDefaultOverlayWidthMeters", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4\xE5\xA4\xA7\xE5\xB0\x8F", 0.01f, 0.5f, "0.3" },
        { "ScopeDefaultOverlayOffset", CfgOptionType::Vec3, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x81\x8F\xE7\xA7\xBB", -2.0f, 2.0f, "0.02,0.04,-0.06" },
        { "ScopeAimSensitivityFovReductionRate", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE5\xAE\x9E\xE6\x97\xB6 FOV \xE7\x81\xB5\xE6\x95\x8F\xE5\xBA\xA6\xE9\x99\x8D\xE4\xBD\x8E\xE9\x80\x9F\xE7\x8E\x87", 0.0f, 4.0f, "0.75" },
        { "ScopeZNear", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xBF\x91\xE8\xA3\x81\xE5\x89\xAA\xE9\x9D\xA2", 0.1f, 6.0f, "2" },
        { "ScopeOverlayAngleOffset", CfgOptionType::Vec3, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xA6\x86\xE7\x9B\x96\xE5\xB1\x82\xE8\xA7\x92\xE5\xBA\xA6\xE5\x81\x8F\xE7\xA7\xBB (\xE4\xBF\xAF\xE4\xBB\xB0,\xE5\x81\x8F\xE8\x88\xAA,\xE7\xBF\xBB\xE6\xBB\x9A)", -180.0f, 180.0f, "-45,-5,-5" },
        { "ScopeRequireLookThrough", CfgOptionType::Bool, "\xE5\x85\x89\xE5\xAD\xA6", "\xE9\x9C\x80\xE8\xA6\x81\xE8\xB4\xB4\xE8\xBF\x91\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE6\x89\x8D\xE6\x98\xBE\xE7\xA4\xBA", 0.0f, 0.0f, "true" },
        { "ScopeLookThroughDistanceMeters", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE8\xB4\xB4\xE8\xBF\x91\xE8\xB7\x9D\xE7\xA6\xBB", 0.01f, 2.0f, "0.5" },
        { "ScopeLookThroughAngleDeg", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE8\xB4\xB4\xE8\xBF\x91\xE8\xA7\x92\xE5\xBA\xA6", 1.0f, 89.0f, "60" },
        { "ScopeOverlayAlwaysVisible", CfgOptionType::Bool, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xA6\x86\xE7\x9B\x96\xE5\xB1\x82\xE5\xA7\x8B\xE7\xBB\x88\xE5\x8F\xAF\xE8\xA7\x81", 0.0f, 0.0f, "false" },
        { "ScopeOverlayIdleAlpha", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\x97\xB2\xE7\xBD\xAE\xE9\x80\x8F\xE6\x98\x8E\xE5\xBA\xA6", 0.0f, 1.0f, "0.5" },
        { "CustomAction1Command", CfgOptionType::String, "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C", "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C" "1\xE6\x8C\x87\xE4\xBB\xA4", 0.0f, 0.0f, "thirdpersonshoulder" },
        { "CustomAction2Command", CfgOptionType::String, "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C", "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C" "2\xE6\x8C\x87\xE4\xBB\xA4", 0.0f, 0.0f, "" },
        { "CustomAction3Command", CfgOptionType::String, "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C", "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C" "3\xE6\x8C\x87\xE4\xBB\xA4", 0.0f, 0.0f, "" },
        { "CustomAction4Command", CfgOptionType::String, "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C", "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C" "4\xE6\x8C\x87\xE4\xBB\xA4", 0.0f, 0.0f, "" },
        { "CustomAction5Command", CfgOptionType::String, "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C", "\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE5\x8A\xA8\xE4\xBD\x9C" "5\xE6\x8C\x87\xE4\xBB\xA4", 0.0f, 0.0f, "vr_afk_fall_guard_hold" },
        { "AntiAliasing", CfgOptionType::Int, "\xE6\x80\xA7\xE8\x83\xBD", "\xE6\x8A\x97\xE9\x94\xAF\xE9\xBD\xBF\xE7\xBA\xA7\xE5\x88\xAB", 0.0f, 8.0f, "0" },
        { "ThirdPersonVRCameraOffset", CfgOptionType::Float, "\xE7\x9B\xB8\xE6\x9C\xBA / \xE7\xAC\xAC\xE4\xB8\x89\xE4\xBA\xBA\xE7\xA7\xB0", "\xE7\xAC\xAC\xE4\xB8\x89\xE4\xBA\xBA\xE7\xA7\xB0\xE7\x9B\xB8\xE6\x9C\xBA\xE5\x81\x8F\xE7\xA7\xBB", -200.0f, 200.0f, "38" },
        { "D3DAimLineOverlayEnabled", CfgOptionType::Bool, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "\xE5\x90\xAF\xE7\x94\xA8\xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF", 0.0f, 0.0f, "false" },
        { "AimLineOnlyWhenLaserSight", CfgOptionType::Bool, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "\xE4\xBB\x85\xE5\x9C\xA8\xE6\xBF\x80\xE5\x85\x89\xE7\x9E\x84\xE5\x87\x86\xE5\xBC\x80\xE5\x90\xAF\xE6\x97\xB6\xE6\x98\xBE\xE7\xA4\xBA\xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF", 0.0f, 0.0f, "false" },
        { "D3DAimLineOverlayWidthPixels", CfgOptionType::Float, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "D3D \xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF\xE5\xAE\xBD\xE5\xBA\xA6", 0.0f, 20.0f, "2.0" },
        { "D3DAimLineOverlayOutlinePixels", CfgOptionType::Float, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "D3D \xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF\xE6\x8F\x8F\xE8\xBE\xB9\xE5\xAE\xBD\xE5\xBA\xA6", 0.0f, 20.0f, "1.0" },
        { "D3DAimLineOverlayEndpointPixels", CfgOptionType::Float, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "D3D \xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF\xE7\xAB\xAF\xE7\x82\xB9\xE5\xA4\xA7\xE5\xB0\x8F", 0.0f, 20.0f, "1.5" },
        { "D3DAimLineOverlayColor", CfgOptionType::Color, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "D3D \xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF\xE9\xA2\x9C\xE8\x89\xB2", 0.0f, 0.0f, "255,0,0,100" },
        { "D3DAimLineOverlayOutlineColor", CfgOptionType::Color, "\xE8\xBE\x85\xE5\x8A\xA9\xE7\x9E\x84\xE5\x87\x86", "D3D \xE7\x9E\x84\xE5\x87\x86\xE7\xBA\xBF\xE6\x8F\x8F\xE8\xBE\xB9\xE9\xA2\x9C\xE8\x89\xB2", 0.0f, 0.0f, "255,0,0,1" },
        { "MotionGesturePushThreshold", CfgOptionType::Float, "\xE5\x8A\xA8\xE4\xBD\x9C\xE6\x89\x8B\xE5\x8A\xBF", "\xE6\x8E\xA8\xE6\x89\x8B\xE5\x8A\xBF\xE9\x98\x88\xE5\x80\xBC", 0.0f, 10.0f, "1.5" },
        { "InventoryBodyOriginOffset", CfgOptionType::Vec3, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE8\xBA\xAB\xE4\xBD\x93\xE5\x8E\x9F\xE7\x82\xB9\xE5\x81\x8F\xE7\xA7\xBB (x,y,z)", -2.0f, 2.0f, "-0.1,0.0,-0.28" },
        { "InventoryAnchorColor", CfgOptionType::Color, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F\xE9\x94\x9A\xE7\x82\xB9\xE9\xA2\x9C\xE8\x89\xB2", 0.0f, 0.0f, "0,255,255,255" },
        { "InventoryHudMarkerDistance", CfgOptionType::Float, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F HUD \xE6\xA0\x87\xE8\xAE\xB0\xE8\xB7\x9D\xE7\xA6\xBB", -2.0f, 2.0f, "0.45" },
        { "InventoryHudMarkerUpOffset", CfgOptionType::Float, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F HUD \xE6\xA0\x87\xE8\xAE\xB0\xE4\xB8\x8A\xE7\xA7\xBB\xE5\x81\x8F\xE7\xA7\xBB", -2.0f, 2.0f, "-0.10" },
        { "InventoryHudMarkerSeparation", CfgOptionType::Float, "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F / \xE9\x94\x9A\xE7\x82\xB9", "\xE7\x89\xA9\xE5\x93\x81\xE6\xA0\x8F HUD \xE6\xA0\x87\xE8\xAE\xB0\xE9\x97\xB4\xE8\xB7\x9D", 0.0f, 2.0f, "0.14" },
        { "ScopeCameraOffset", CfgOptionType::Vec3, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\x9B\xB8\xE6\x9C\xBA\xE5\x81\x8F\xE7\xA7\xBB (x,y,z)", -180.0f, 180.0f, "12,0,3" },
        { "ScopeCameraAngleOffset", CfgOptionType::Vec3, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\x9B\xB8\xE6\x9C\xBA\xE8\xA7\x92\xE5\xBA\xA6\xE5\x81\x8F\xE7\xA7\xBB (\xE4\xBF\xAF\xE4\xBB\xB0,\xE5\x81\x8F\xE8\x88\xAA,\xE7\xBF\xBB\xE6\xBB\x9A)", -180.0f, 180.0f, "0,0,0" },
        { "ScopeStabilizationEnabled", CfgOptionType::Bool, "\xE5\x85\x89\xE5\xAD\xA6", "\xE5\x90\xAF\xE7\x94\xA8\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\xA8\xB3\xE5\xAE\x9A", 0.0f, 0.0f, "true" },
        { "ScopeStabilizationMinCutoff", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\xA8\xB3\xE5\xAE\x9A\xE6\x9C\x80\xE5\xB0\x8F\xE6\x88\xAA\xE6\xAD\xA2\xE9\xA2\x91\xE7\x8E\x87", 0.0f, 10.0f, "0.5" },
        { "ScopeStabilizationBeta", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\xA8\xB3\xE5\xAE\x9A Beta", 0.0f, 10.0f, "0.5" },
        { "ScopeStabilizationDCutoff", CfgOptionType::Float, "\xE5\x85\x89\xE5\xAD\xA6", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE7\xA8\xB3\xE5\xAE\x9A\xE5\xAF\xBC\xE6\x95\xB0\xE6\x88\xAA\xE6\xAD\xA2\xE9\xA2\x91\xE7\x8E\x87", 0.0f, 10.0f, "1.0" },
        { "MouseModeScopedViewmodelAnchorOffset", CfgOptionType::Vec3, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F\xE5\xBC\x80\xE9\x95\x9C\xE9\x94\x9A\xE7\x82\xB9\xE5\x81\x8F\xE7\xA7\xBB (x,y,z)", -2.0f, 2.0f, "0.35,0.0,-0.13" },
        { "MouseModeHmdAimSensitivity", CfgOptionType::Float, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F\xE5\xA4\xB4\xE6\x98\xBE\xE7\x9E\x84\xE5\x87\x86\xE7\x81\xB5\xE6\x95\x8F\xE5\xBA\xA6", 0.0f, 10.0f, "1" },
        { "MouseModeScopeOverlayOffset", CfgOptionType::Vec3, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xA6\x86\xE7\x9B\x96\xE5\xB1\x82\xE5\x81\x8F\xE7\xA7\xBB (x,y,z)", -2.0f, 2.0f, "0,-0.02,-0.3" },
        { "MouseModeScopeOverlayAngleOffset", CfgOptionType::Vec3, "\xE8\xBE\x93\xE5\x85\xA5 / \xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F", "\xE9\x94\xAE\xE9\xBC\xA0\xE6\xA8\xA1\xE5\xBC\x8F\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xA6\x86\xE7\x9B\x96\xE5\xB1\x82\xE8\xA7\x92\xE5\xBA\xA6\xE5\x81\x8F\xE7\xA7\xBB", -180.0f, 180.0f, "0,0,0" },
        { "AutoRepeatSprayPushDelayTicks", CfgOptionType::Int, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "Spray-Push \xE5\xBB\xB6\xE8\xBF\x9F Tick", 0.0f, 120.0f, "0" },
        { "AutoRepeatSprayPushHoldTicks", CfgOptionType::Int, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "Spray-Push \xE4\xBF\x9D\xE6\x8C\x81 Tick", 0.0f, 120.0f, "1" },
        { "HitIndicatorEnabled", CfgOptionType::Bool, "\xE6\xAD\xA6\xE5\x99\xA8 / \xE5\xBC\x80\xE7\x81\xAB", "\xE5\x91\xBD\xE4\xB8\xAD\xE6\x8C\x87\xE7\xA4\xBA\xE5\x99\xA8", 0.0f, 0.0f, "false" },
        { "FlashlightEnhancementEnabled", CfgOptionType::Bool, "\xE9\x80\x9A\xE7\x94\xA8", "\xE6\x89\x8B\xE7\x94\xB5\xE5\xA2\x9E\xE5\xBC\xBA", 0.0f, 0.0f, "false" },
        { "AutoFlashlightEnabled", CfgOptionType::Bool, "\xE9\x80\x9A\xE7\x94\xA8", "\xE8\x87\xAA\xE5\x8A\xA8\xE6\x89\x8B\xE7\x94\xB5\xE7\xAD\x92", 0.0f, 0.0f, "true" },
        { "AutoFlashlightDarkThreshold", CfgOptionType::Float, "\xE9\x80\x9A\xE7\x94\xA8", "Auto Flashlight Dark Threshold", 0.0f, 255.0f, "45" },
        { "AutoFlashlightBrightThreshold", CfgOptionType::Float, "\xE9\x80\x9A\xE7\x94\xA8", "Auto Flashlight Bright Threshold", 0.0f, 255.0f, "80" },
    };

    constexpr int kCfgOptionSpecCount = (int)(sizeof(kCfgOptionSpecs) / sizeof(kCfgOptionSpecs[0]));
    struct CfgOptionTextSpec
    {
        const char* key;
        const char* groupEnUtf8;
        const char* groupZhUtf8;
        const char* titleEnUtf8;
        const char* titleZhUtf8;
        const char* descEnUtf8;
        const char* descZhUtf8;
        const char* tipEnUtf8;
        const char* tipZhUtf8;
    };

    static const CfgOptionTextSpec kCfgOptionTextSpecs[] =
    {
        { "ConfigOverlayDistanceMeters", "Config Overlay", "\351\205\215\347\275\256\351\235\242\346\235\277", "Panel Distance", "\351\205\215\347\275\256\351\235\242\346\235\277\350\267\235\347\246\273", "Distance from the HMD to the built-in config panel (meters).", "\351\205\215\347\275\256\351\235\242\346\235\277\347\233\270\345\257\271\345\244\264\346\230\276\347\232\204\345\211\215\346\226\271\350\267\235\347\246\273\357\274\210\347\261\263\357\274\211\343\200\202", "Increase it if the panel is too close; decrease it if the panel is too far.", "\351\235\242\346\235\277\345\244\252\350\264\264\350\204\270\345\260\261\350\260\203\345\244\247\357\274\214\345\244\252\350\277\234\345\260\261\350\260\203\345\260\217\343\200\202" },
        { "ConfigOverlaySizeMeters", "Config Overlay", "\351\205\215\347\275\256\351\235\242\346\235\277", "Panel Size", "\351\205\215\347\275\256\351\235\242\346\235\277\345\244\247\345\260\217", "Physical width of the built-in config panel (meters). Height follows the panel aspect ratio.", "\351\205\215\347\275\256\351\235\242\346\235\277\347\232\204\347\211\251\347\220\206\345\256\275\345\272\246\357\274\210\347\261\263\357\274\211\357\274\214\351\253\230\345\272\246\344\274\232\346\214\211\351\235\242\346\235\277\346\257\224\344\276\213\350\207\252\345\212\250\350\256\241\347\256\227\343\200\202", "Increase it if the text is too small; decrease it if the panel blocks too much view.", "\346\226\207\345\255\227\345\244\252\345\260\217\345\260\261\350\260\203\345\244\247\357\274\214\351\201\256\346\214\241\345\244\252\345\244\232\345\260\261\350\260\203\345\260\217\343\200\202" },
        { "VRScale", "View / Scale", "\350\247\206\350\247\222 / \345\260\272\345\272\246", "World Scale", "\344\270\226\347\225\214\347\274\251\346\224\276", "Adjusts overall world scale (distance and size perception).", "\350\260\203\346\225\264\346\225\264\344\275\223\344\270\226\347\225\214\345\260\272\345\272\246\357\274\210\350\267\235\347\246\273\344\270\216\345\244\247\345\260\217\346\204\237\347\237\245\357\274\211\343\200\202", "Keep close to real-world meter scale. 43.2 covers most play spaces.", "\345\260\275\351\207\217\344\277\235\346\214\201\344\270\216\347\234\237\345\256\236\344\270\226\347\225\214\346\216\245\350\277\221\343\200\20243.2\344\270\200\350\210\254\346\234\200\345\220\210\351\200\202\343\200\202" },
        { "Roomscale1To1Movement", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "Enable 1:1 Roomscale Movement", "\345\220\257\347\224\2501:1\346\210\277\351\227\264\347\247\273\345\212\250", "Converts physical HMD movement into normal in-game movement commands.", "\346\212\212\345\244\264\346\230\276\347\216\260\345\256\236\347\247\273\345\212\250\350\275\254\346\215\242\346\210\220\346\270\270\346\210\217\345\206\205\346\240\207\345\207\206\347\247\273\345\212\250\346\214\207\344\273\244\343\200\202", "Works on normal servers because it sends standard movement input.", "\345\217\221\351\200\201\347\232\204\346\230\257\346\240\207\345\207\206\347\247\273\345\212\250\350\276\223\345\205\245\357\274\214\346\211\200\344\273\245\345\217\257\344\273\245\347\224\250\345\234\250\346\231\256\351\200\232\346\234\215\345\212\241\345\231\250\343\200\202" },
        { "Roomscale1To1ServerMove", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "VR Server Move", "VR Server Move", "When local server.dll is VR-aware, applies room movement to the server player origin after a hull collision sweep.", "When local server.dll is VR-aware, applies room movement to the server player origin after a hull collision sweep.", "If unavailable, the normal CUserCmd compatibility path is used.", "If unavailable, the normal CUserCmd compatibility path is used." },
        { "Roomscale1To1MovementScale", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "1:1 Movement Scale", "1:1\347\247\273\345\212\250\350\267\235\347\246\273\345\200\215\347\216\207", "Multiplies HMD physical movement before it becomes in-game movement.", "\346\212\212\345\244\264\346\230\276\347\216\260\345\256\236\344\275\215\347\247\273\350\275\254\346\215\242\346\210\220\346\270\270\346\210\217\345\206\205\347\247\273\345\212\250\345\211\215\345\206\215\344\271\230\344\273\245\350\277\231\344\270\252\345\200\215\347\216\207\343\200\202", "1.0 means 10cm real movement targets 10cm in game; increase it if in-game movement is too short.", "1.0\350\241\250\347\244\272\347\216\260\345\256\236\347\247\273\345\212\25010cm\347\233\256\346\240\207\344\271\237\346\230\257\346\270\270\346\210\217\345\206\20510cm\357\274\233\346\270\270\346\210\217\351\207\214\350\265\260\345\276\227\345\244\252\347\237\255\345\260\261\350\260\203\345\244\247\343\200\202" },
        { "Roomscale1To1MinApplyMeters", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "Minimum Apply Distance", "\346\234\200\345\260\217\345\272\224\347\224\250\350\267\235\347\246\273", "Small HMD movement below this distance is treated as tracking noise.", "\344\275\216\344\272\216\350\277\231\344\270\252\350\267\235\347\246\273\347\232\204\345\244\264\346\230\276\347\247\273\345\212\250\344\274\232\350\242\253\345\275\223\344\275\234\350\267\237\350\270\252\345\231\252\345\243\260\343\200\202", "Use 0.005 for responsive walking; lower only if tracking is stable.", "\345\273\272\350\256\2560.005\357\274\214\350\267\237\350\270\252\345\276\210\347\250\263\345\206\215\350\260\203\344\275\216\343\200\202" },
        { "Roomscale1To1PhysicalCrouch", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "Physical Crouch", "\347\216\260\345\256\236\350\271\262\344\274\217", "Makes real HMD height drops hold the in-game crouch button.", "\345\244\264\346\230\276\347\216\260\345\256\236\351\253\230\345\272\246\344\270\213\351\231\215\346\227\266\346\214\201\346\234\211\346\270\270\346\210\217\345\206\205\350\271\262\344\274\217\346\214\211\351\224\256\343\200\202", "ResetPosition recalibrates the standing height reference.", "ResetPosition\344\274\232\351\207\215\346\226\260\346\240\207\345\256\232\347\253\231\347\253\213\351\253\230\345\272\246\343\200\202" },
        { "Roomscale1To1CrouchEnterMeters", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "Crouch Enter Drop", "\350\271\262\344\274\217\350\247\246\345\217\221\351\253\230\345\272\246\345\267\256", "HMD drop from standing height required to start crouching.", "\344\273\216\347\253\231\347\253\213\351\253\230\345\272\246\344\270\213\351\231\215\345\244\232\345\260\221\347\261\263\345\220\216\350\247\246\345\217\221\350\271\262\344\274\217\343\200\202", "Raise this if crouch triggers too easily.", "\350\277\207\344\272\216\345\256\271\346\230\223\350\247\246\345\217\221\345\260\261\350\260\203\345\244\247\343\200\202" },
        { "Roomscale1To1CrouchExitMeters", "Roomscale Movement", "\346\210\277\351\227\264\347\247\273\345\212\250", "Crouch Exit Drop", "\350\271\262\344\274\217\351\207\212\346\224\276\351\253\230\345\272\246\345\267\256", "Crouch releases when the HMD drop rises back above this smaller threshold.", "\345\244\264\346\230\276\345\233\236\345\215\207\345\210\260\350\277\231\344\270\252\350\276\203\345\260\217\351\230\210\345\200\274\344\271\213\344\270\212\345\220\216\351\207\212\346\224\276\350\271\262\344\274\217\343\200\202", "Keep it lower than enter drop to avoid flicker.", "\350\256\276\345\276\227\346\257\224\350\247\246\345\217\221\351\230\210\345\200\274\344\275\216\357\274\214\351\201\277\345\205\215\350\276\271\347\225\214\346\212\226\345\212\250\343\200\202" },
        { "IPDScale", "View / Scale", "\350\247\206\350\247\222 / \345\260\272\345\272\246", "IPD Scale", "\347\236\263\350\267\235\347\274\251\346\224\276", "Multiplies headset IPD to fine-tune stereo separation.", "\346\214\211\346\257\224\344\276\213\350\260\203\346\225\264\345\244\264\346\230\276\347\236\263\350\267\235\344\273\245\345\276\256\350\260\203\347\253\213\344\275\223\345\210\206\347\246\273\345\272\246\343\200\202", "Use for small comfort tweaks only.", "\344\273\205\347\224\250\344\272\216\345\260\217\345\271\205\350\210\222\351\200\202\345\272\246\345\276\256\350\260\203\343\200\202" },
        { "LeftHanded", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Left-Handed Mode", "\345\267\246\346\211\213\346\214\201\346\255\246", "Swaps dominant hand interactions for left-handed players.", "\344\270\272\345\267\246\346\211\213\347\216\251\345\256\266\345\210\207\346\215\242\344\270\273\350\246\201\344\272\244\344\272\222\346\211\213\343\200\202", "Toggle if you primarily aim with the left controller.", "\345\246\202\346\236\234\344\270\273\350\246\201\347\224\250\345\267\246\346\211\213\347\236\204\345\207\206\357\274\214\350\257\267\345\274\200\345\220\257\343\200\202" },
        { "MoveDirectionFromController", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Controller determines the direction of movement", "\346\211\213\346\237\204\345\206\263\345\256\232\345\211\215\350\277\233\346\226\271\345\220\221", "", "", "", "" },
        { "TurnSpeed", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Smooth Turn Speed", "\345\271\263\346\273\221\350\275\254\345\220\221\351\200\237\345\272\246", "Controls smooth turning speed.", "\346\216\247\345\210\266\345\271\263\346\273\221\350\275\254\345\220\221\347\232\204\350\275\254\351\200\237\343\200\202", "0.2~0.6 is comfortable for most people.", "\345\244\232\346\225\260\344\272\272\351\200\202\345\220\210 0.2~0.6\343\200\202" },
        { "SnapTurning", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Use Snap Turning", "\345\220\257\347\224\250\345\210\206\346\256\265\350\275\254\345\220\221", "Turns in fixed increments instead of smooth rotation.", "\344\275\277\347\224\250\345\233\272\345\256\232\350\247\222\345\272\246\345\210\206\346\256\265\350\275\254\345\220\221\357\274\214\346\233\277\344\273\243\350\277\236\347\273\255\346\227\213\350\275\254\343\200\202", "Preferable for motion-sickness-prone players.", "\345\256\271\346\230\223\346\231\225\345\212\250\347\227\207\347\232\204\347\216\251\345\256\266\345\273\272\350\256\256\345\274\200\345\220\257\343\200\202" },
        { "SnapTurnAngle", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Snap Turn Angle", "\345\210\206\346\256\265\350\275\254\345\220\221\350\247\222\345\272\246", "Degrees turned per snap when snap turning is enabled.", "\345\210\206\346\256\265\350\275\254\345\220\221\346\227\266\346\257\217\346\254\241\346\227\213\350\275\254\347\232\204\350\247\222\345\272\246\343\200\202", "30\302\260~60\302\260 is common. Higher = fewer snaps, lower = finer control.", "30\302\260~60\302\260 \346\257\224\350\276\203\345\270\270\350\247\201\343\200\202\350\247\222\345\272\246\350\266\212\345\244\247\357\274\214\346\254\241\346\225\260\350\266\212\345\260\221\357\274\233\350\266\212\345\260\217\350\266\212\347\262\276\347\273\206\343\200\202" },
        { "ControllerSmoothing", "Input / Turning", "\350\276\223\345\205\245 / \350\275\254\345\220\221", "Controller Input Smoothing", "\346\211\213\346\237\204\350\276\223\345\205\245\345\271\263\346\273\221", "Smooths controller input to reduce jitter.", "\345\271\263\346\273\221\345\244\204\347\220\206\346\211\213\346\237\204\350\276\223\345\205\245\344\273\245\345\207\217\345\260\221\346\212\226\345\212\250\343\200\202", "Keep under 0.5 to avoid noticeable latency.", "\345\273\272\350\256\256\344\275\216\344\272\216 0.5 \344\273\245\351\201\277\345\205\215\346\230\216\346\230\276\345\273\266\350\277\237\343\200\202" },
        { "MouseModeEnabled", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Enable Mouse Mode", "\345\220\257\347\224\250\351\224\256\351\274\240\346\250\241\345\274\217", "Enables desktop-style mouse aiming while staying in VR rendering.", "\345\220\257\347\224\250\346\241\214\351\235\242\345\274\217\351\224\256\351\274\240\347\236\204\345\207\206\357\274\214\344\275\206\344\273\215\344\277\235\346\214\201VR\346\270\262\346\237\223\343\200\202", "Mouse X turns your body; mouse Y aims (and optionally tilts view).", "\351\274\240\346\240\207X\346\216\247\345\210\266\350\275\254\350\272\253\357\274\233\351\274\240\346\240\207Y\346\216\247\345\210\266\344\277\257\344\273\260\357\274\210\345\217\257\351\200\211\345\220\214\346\227\266\345\270\246\345\212\250\350\247\206\350\247\222\357\274\211\343\200\202" },
        { "MouseModeAimFromHmd", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Aim From HMD", "\344\273\216\345\244\264\346\230\276\347\236\204\345\207\206", "When enabled, mouse-mode aiming is driven by the HMD center ray instead of the fixed viewmodel anchor.", "\345\274\200\345\220\257\345\220\216\357\274\214\351\224\256\351\274\240\346\250\241\345\274\217\347\232\204\347\236\204\345\207\206\345\260\206\346\224\271\344\270\272\344\275\277\347\224\250\345\244\264\346\230\276\344\270\255\345\277\203\345\260\204\347\272\277\357\274\214\350\200\214\344\270\215\346\230\257\345\233\272\345\256\232\347\232\204 viewmodel \351\224\232\347\202\271\343\200\202", "Recommended if you want to keep holding the gun/stocks naturally and use the headset to fine-aim.", "\351\200\202\345\220\210\345\270\214\346\234\233\344\277\235\346\214\201\346\217\241\346\236\252\345\247\277\345\212\277\344\270\215\345\217\230\343\200\201\347\224\250\345\244\264\346\230\276\345\201\232\345\276\256\350\260\203\347\236\204\345\207\206\347\232\204\345\234\272\346\231\257\343\200\202" },
        { "MouseModeYawSensitivity", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Yaw Sensitivity", "\351\274\240\346\240\207\346\260\264\345\271\263\347\201\265\346\225\217\345\272\246", "How much mouse X rotates yaw per count.", "\351\274\240\346\240\207\346\260\264\345\271\263\346\257\217\344\270\252\350\256\241\346\225\260\345\257\274\350\207\264\347\232\204\350\275\254\345\220\221\345\271\205\345\272\246\343\200\202", "Negative values invert direction.", "\350\264\237\345\200\274\345\217\257\345\217\215\345\220\221\343\200\202" },
        { "MouseModePitchSensitivity", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Pitch Sensitivity", "\351\274\240\346\240\207\345\236\202\347\233\264\347\201\265\346\225\217\345\272\246", "How much mouse Y changes pitch per count.", "\351\274\240\346\240\207\345\236\202\347\233\264\346\257\217\344\270\252\350\256\241\346\225\260\345\257\274\350\207\264\347\232\204\344\277\257\344\273\260\345\217\230\345\214\226\343\200\202", "Negative values invert direction.", "\350\264\237\345\200\274\345\217\257\345\217\215\345\220\221\343\200\202" },
        { "MouseModePitchAffectsView", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Pitch Tilts View", "\351\274\240\346\240\207\344\277\257\344\273\260\345\270\246\345\212\250\350\247\206\350\247\222", "When enabled, mouse Y also tilts the rendered view up/down (adds a pitch offset on top of HMD tracking).", "\345\274\200\345\220\257\345\220\216\357\274\214\351\274\240\346\240\207Y\344\271\237\344\274\232\345\270\246\345\212\250\346\270\262\346\237\223\350\247\206\350\247\222\344\270\212\344\270\213\344\277\257\344\273\260\357\274\210\345\234\250\345\244\264\346\230\276\350\277\275\350\270\252\345\237\272\347\241\200\344\270\212\345\217\240\345\212\240\344\277\257\344\273\260\345\201\217\347\247\273\357\274\211\343\200\202", "Useful if aiming at high/low targets is difficult without moving your head. May increase motion sickness.", "\351\200\202\345\220\210\344\270\215\346\203\263\346\212\254\345\244\264/\344\275\216\345\244\264\344\271\237\350\203\275\345\205\250\346\226\271\345\220\221\347\236\204\345\207\206\347\232\204\345\234\272\346\231\257\357\274\214\344\275\206\345\217\257\350\203\275\346\233\264\345\256\271\346\230\223\346\231\225\343\200\202" },
        { "MouseModeTurnSmoothing", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Yaw Smoothing (seconds)", "\346\260\264\345\271\263\345\271\263\346\273\221 (\347\247\222)", "Smooths mouse-driven yaw (turning) to avoid 'stepping' when CreateMove rate < VR refresh.", "\345\257\271\351\274\240\346\240\207\351\251\261\345\212\250\347\232\204\346\260\264\345\271\263\350\275\254\345\220\221\345\201\232\345\271\263\346\273\221\357\274\214\351\201\277\345\205\215 CreateMove \351\242\221\347\216\207\344\275\216\344\272\216 VR \345\210\267\346\226\260\347\216\207\346\227\266\345\207\272\347\216\260\345\217\260\351\230\266\346\204\237\343\200\202", "0 disables smoothing. Typical: 0.03~0.08.", "0 \345\205\263\351\227\255\345\271\263\346\273\221\343\200\202\345\270\270\347\224\250\357\274\2320.03~0.08\343\200\202" },
        { "MouseModePitchSmoothing", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Pitch Smoothing (seconds)", "\345\236\202\347\233\264\345\271\263\346\273\221 (\347\247\222)", "Smooths mouse-driven pitch (aim up/down) to avoid stutter when CreateMove rate < VR refresh.", "\345\257\271\351\274\240\346\240\207\351\251\261\345\212\250\347\232\204\345\236\202\347\233\264\347\236\204\345\207\206\345\201\232\345\271\263\346\273\221\357\274\214\351\201\277\345\205\215 CreateMove \351\242\221\347\216\207\344\275\216\344\272\216 VR \345\210\267\346\226\260\347\216\207\346\227\266\345\207\272\347\216\260\345\215\241\351\241\277/\345\217\260\351\230\266\343\200\202", "0 disables smoothing. Typical: 0.03~0.08.", "0 \345\205\263\351\227\255\345\271\263\346\273\221\343\200\202\345\270\270\347\224\250\357\274\2320.03~0.08\343\200\202" },
        { "MouseModeViewmodelAnchorOffset", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Mode Viewmodel Anchor Offset (m)", "\351\274\240\346\240\207\346\250\241\345\274\217 Viewmodel \351\224\232\347\202\271\345\201\217\347\247\273 (\347\261\263)", "HMD-local offset for the fixed viewmodel/aim origin (meters).", "\345\237\272\344\272\216 HMD \347\232\204\345\233\272\345\256\232 viewmodel/\347\236\204\345\207\206\350\265\267\347\202\271\345\201\217\347\247\273\357\274\210\347\261\263\345\210\266\357\274\211\343\200\202", "X=forward, Y=right, Z=up. Tune so the gun sits where you want.", "X=\345\211\215\346\226\271\357\274\214Y=\345\217\263\346\226\271\357\274\214Z=\344\270\212\346\226\271\343\200\202\350\260\203\345\210\260\346\236\252\345\234\250\344\275\240\346\203\263\350\246\201\347\232\204\344\275\215\347\275\256\343\200\202" },
        { "MouseModeAimConvergeDistance", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Mode Convergence Distance (units)", "\351\274\240\346\240\207\346\250\241\345\274\217\346\261\207\350\201\232\350\267\235\347\246\273 (\345\215\225\344\275\215)", "Scheme B: viewmodel ray is steered to intersect the HMD-center ray at this distance (Source units).", "\346\226\271\346\241\210B\357\274\232\350\256\251 viewmodel \345\217\221\345\207\272\347\232\204\347\236\204\345\207\206\345\260\204\347\272\277\345\234\250\350\257\245\350\267\235\347\246\273\344\270\216\350\247\206\347\272\277\344\270\255\345\277\203\345\260\204\347\272\277\347\233\270\344\272\244\357\274\210Source \345\215\225\344\275\215\357\274\211\343\200\202", "Helps keep the aim line near screen center even if you move the anchor. 2048~4096 is common.", "\345\215\263\344\275\277\350\260\203\346\225\264\351\224\232\347\202\271\357\274\214\344\271\237\350\203\275\350\256\251\347\236\204\345\207\206\347\272\277\350\277\234\345\244\204\345\233\236\345\210\260\350\247\206\351\207\216\344\270\255\345\277\203\343\200\202\345\270\270\347\224\250 2048~4096\343\200\202" },
        { "MouseModeScopeToggleKey", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse Mode Scope Toggle Key", "\351\224\256\351\274\240\346\250\241\345\274\217\345\274\200\351\225\234\345\210\207\346\215\242\346\214\211\351\224\256", "Keyboard key used to toggle the mouse-mode scope overlay on/off.", "\347\224\250\344\272\216\345\274\200/\345\205\263\351\224\256\351\274\240\346\250\241\345\274\217\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\347\232\204\351\224\256\347\233\230\346\214\211\351\224\256\343\200\202", "Format: key:<name> (e.g., key:6, key:f9). Leave empty to disable.", "\346\240\274\345\274\217\357\274\232key:<\346\214\211\351\224\256\345\220\215>\357\274\210\345\246\202 key:6\343\200\201key:f9\357\274\211\343\200\202\347\225\231\347\251\272\350\241\250\347\244\272\347\246\201\347\224\250\343\200\202" },
        { "ForceNonVRServerMovement", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "Non-VR Server Compatibility Mode", "\351\235\236VR\346\234\215\345\212\241\345\231\250\345\205\274\345\256\271\346\250\241\345\274\217", "Converts VR movement/interaction to be more acceptable to standard servers.", "\345\260\206VR\347\247\273\345\212\250\344\270\216\344\272\244\344\272\222\350\275\254\346\215\242\344\270\272\346\233\264\347\254\246\345\220\210\344\274\240\347\273\237\346\234\215\345\212\241\345\231\250\347\232\204\345\275\242\345\274\217\343\200\202", "Recommended for public multiplayer servers.", "\351\235\236\350\207\252\345\267\261\345\273\272\346\210\277\345\244\232\345\277\205\351\241\273\345\274\200\345\220\257\343\200\202" },
        { "AfkFallGuardMinDownSpeed", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "AFK Fall Guard Minimum Down Speed", "AFK \351\230\262\345\235\240\350\220\275\346\234\200\345\260\217\344\270\213\350\220\275\351\200\237\345\272\246", "Minimum downward velocity required before the held Custom Action 5 guard may issue go_away_from_keyboard.", "\346\214\211\344\275\217 SteamVR \350\207\252\345\256\232\344\271\211\345\212\250\344\275\234 5 \346\227\266\357\274\214\345\217\252\346\234\211\344\270\213\350\220\275\351\200\237\345\272\246\350\276\276\345\210\260\346\255\244\351\230\210\345\200\274\346\211\215\344\274\232\345\260\235\350\257\225\350\247\246\345\217\221\351\227\262\347\275\256\343\200\202", "Raise it if the guard arms during harmless drops.", "\345\260\217\345\236\213\344\270\213\345\217\260\351\230\266\344\271\237\344\274\232\350\247\246\345\217\221\346\227\266\345\260\206\345\205\266\350\260\203\351\253\230\343\200\202" },
        { "AfkFallGuardTraceDistance", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "AFK Fall Guard Trace Distance", "AFK \351\230\262\345\235\240\350\220\275\346\216\242\346\265\213\350\267\235\347\246\273", "Maximum downward hull-sweep distance used to locate an approaching landing surface.", "\345\220\221\344\270\213\346\211\253\346\217\217\345\217\257\350\203\275\345\215\263\345\260\206\346\216\245\350\247\246\347\232\204\350\220\275\347\202\271\357\274\214\345\215\225\344\275\215\344\270\272 Source units\343\200\202", "Increase it only if very fast falls are detected too late.", "\345\217\252\346\234\211\351\235\236\345\270\270\345\277\253\347\232\204\344\270\213\350\220\275\346\243\200\346\265\213\345\244\252\346\231\232\346\227\266\346\211\215\350\260\203\345\244\247\343\200\202" },
        { "AfkFallGuardLeadSeconds", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "AFK Fall Guard Lead Time", "AFK \351\230\262\345\235\240\350\220\275\346\217\220\345\211\215\351\207\217", "Issues the idle command when predicted landing is within this many seconds.", "\351\242\204\346\265\213\347\246\273\350\220\275\345\234\260\345\211\251\344\275\231\346\227\266\351\227\264\344\275\216\344\272\216\346\255\244\345\200\274\346\227\266\350\247\246\345\217\221\357\274\214\345\215\225\344\275\215\344\270\272\347\247\222\343\200\202", "Higher latency servers may need a larger value.", "\346\234\215\345\212\241\345\231\250\345\273\266\350\277\237\350\276\203\351\253\230\346\227\266\345\217\257\344\273\245\350\260\203\345\244\247\343\200\202" },
        { "AfkFallGuardCooldownSeconds", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "AFK Fall Guard Cooldown", "AFK \351\230\262\345\235\240\350\220\275\345\206\267\345\215\264", "Minimum interval between idle-command attempts after releasing and pressing the hold action again.", "\346\235\276\345\274\200\345\271\266\351\207\215\346\226\260\346\214\211\344\270\213\350\247\246\345\217\221\351\224\256\345\220\216\357\274\214\344\270\244\346\254\241\351\227\262\347\275\256\345\260\235\350\257\225\344\271\213\351\227\264\347\232\204\346\234\200\345\260\217\351\227\264\351\232\224\343\200\202", "Keep it above zero to avoid accidental command spam.", "\345\273\272\350\256\256\344\277\235\346\214\201\345\244\247\344\272\216 0\357\274\214\351\201\277\345\205\215\350\257\257\350\247\246\345\217\221\345\257\274\350\207\264\346\214\207\344\273\244\350\277\236\345\217\221\343\200\202" },
        { "AfkFallGuardDebugLog", "Multiplayer / Server", "\345\244\232\344\272\272 / \346\234\215\345\212\241\345\231\250", "AFK Fall Guard Debug Log", "AFK \351\230\262\345\235\240\350\220\275\346\227\245\345\277\227", "Logs velocity, predicted landing distance and lead time when an idle attempt is issued.", "\350\247\246\345\217\221\351\227\262\347\275\256\345\260\235\350\257\225\346\227\266\350\256\260\345\275\225\344\270\213\350\220\275\351\200\237\345\272\246\343\200\201\350\220\275\347\202\271\350\267\235\347\246\273\344\270\216\351\242\204\346\265\213\346\227\266\351\227\264\343\200\202", "Enable temporarily while tuning remote-server latency.", "\345\217\252\345\273\272\350\256\256\345\234\250\350\260\203\350\257\225\350\277\234\347\250\213\346\234\215\345\212\241\345\231\250\345\273\266\350\277\237\346\227\266\346\232\202\346\227\266\345\274\200\345\220\257\343\200\202" },
        { "AutoMatQueueMode", "Performance", "\346\200\247\350\203\275", "Multi-core rendering", "\345\244\232\346\240\270\346\270\262\346\237\223", "Turns on multi-core rendering for the mod. Do not enable the in-game multi-core rendering option.", "\347\224\250\344\272\216\345\274\200\345\220\257\345\267\245\345\205\267\345\206\205\347\232\204\345\244\232\346\240\270\346\270\262\346\237\223\345\212\237\350\203\275\357\274\214\344\270\215\350\246\201\345\216\273\345\274\200\345\220\257\346\270\270\346\210\217\351\207\214\347\232\204\345\244\232\346\240\270\346\270\262\346\237\223\351\200\211\351\241\271\343\200\202", "May cause ghosting. Not recommended for standing play.", "\345\274\200\345\220\257\345\220\216\345\217\257\350\203\275\345\207\272\347\216\260\351\207\215\345\275\261\357\274\214\344\270\215\345\273\272\350\256\256\347\253\231\345\247\277\346\270\270\347\216\251\346\227\266\344\275\277\347\224\250\343\200\202" },
        { "QueuedSubmitUseRenderPoseToken", "Performance", "\xE6\x80\xA7\xE8\x83\xBD", "Multi-core Full Frame Sync", "\xE5\xA4\x9A\xE6\xA0\xB8\xE5\xAE\x8C\xE5\x85\xA8\xE5\xB8\xA7\xE5\x90\x8C\xE6\xAD\xA5", "When enabled, multi-core rendering sacrifices some FPS to keep the image completely free of ghosting.", "\xE5\xBC\x80\xE5\x90\xAF\xE5\x90\x8E\xE5\xA4\x9A\xE6\xA0\xB8\xE6\xB8\xB2\xE6\x9F\x93\xE5\xB0\x86\xE7\x89\xBA\xE7\x89\xB2\xE4\xB8\x80\xE5\xAE\x9A\xE5\xB8\xA7\xE6\x95\xB0\xE4\xBF\x9D\xE6\x8C\x81\xE7\x94\xBB\xE9\x9D\xA2\xE5\xAE\x8C\xE5\x85\xA8\xE4\xB8\x8D\xE9\x87\x8D\xE5\xBD\xB1\xEF\xBC\x8C\xE4\xBD\x86\xE5\xA6\x82\xE6\x9E\x9C\xE6\x98\xAF\xE5\x9D\x90\xE7\x9D\x80\xE7\x8E\xA9\xE5\x8F\xAF\xE4\xBB\xA5\xE5\xB0\x9D\xE8\xAF\x95\xE5\x85\xB3\xE9\x97\xAD\xE5\xAE\x83\xE8\x8E\xB7\xE5\xBE\x97\xE6\x9C\x80\xE5\xA4\xA7\xE5\xB8\xA7\xE6\x95\xB0\xE6\x8F\x90\xE5\x8D\x87\xEF\xBC\x8C\xE5\x9B\xA0\xE4\xB8\xBA\xE5\x9D\x90\xE7\x9D\x80\xE4\xB8\x8D\xE9\x9C\x80\xE8\xA6\x81\xE9\xA2\x91\xE7\xB9\x81\xE8\xBD\xAC\xE5\x8A\xA8\xE5\xA4\xB4\xE9\x83\xA8\xE3\x80\x82", "If you play seated, try disabling it for maximum FPS; fast head turns may show ghosting.", "\xE5\x85\xB3\xE9\x97\xAD\xE5\x90\x8E\xE5\xB8\xA7\xE6\x95\xB0\xE5\x8F\xAF\xE8\x83\xBD\xE6\x9B\xB4\xE9\xAB\x98\xEF\xBC\x8C\xE4\xBD\x86\xE5\xBF\xAB\xE9\x80\x9F\xE8\xBD\xAC\xE5\xA4\xB4\xE6\x97\xB6\xE5\x8F\xAF\xE8\x83\xBD\xE5\x87\xBA\xE7\x8E\xB0\xE9\x87\x8D\xE5\xBD\xB1\xE3\x80\x82" },
        { "ReShadeVRCompat", "Performance", "\xE6\x80\xA7\xE8\x83\xBD", "ReShade VR Compatibility", "ReShade VR\xE5\x85\xBC\xE5\xAE\xB9\xE6\xA8\xA1\xE5\xBC\x8F", "Uses a conservative per-eye VR render path for ReShade compatibility.", "\xE5\x90\xAF\xE7\x94\xA8\xE9\x9D\xA2\xE5\x90\x91\x20\x52\x65\x53\x68\x61\x64\x65\x20\xE7\x9A\x84\xE4\xBF\x9D\xE5\xAE\x88\xE9\x80\x90\xE7\x9C\xBC\x20\x56\x52\x20\xE6\xB8\xB2\xE6\x9F\x93\xE8\xB7\xAF\xE5\xBE\x84\xE3\x80\x82", "Enable only when ReShade is actually installed and loaded. It may reduce performance or affect stability.", "\xE5\x8F\xAA\xE5\x9C\xA8\xE5\xAE\x9E\xE9\x99\x85\xE5\xAE\x89\xE8\xA3\x85\xE5\xB9\xB6\xE5\x8A\xA0\xE8\xBD\xBD\x20\x52\x65\x53\x68\x61\x64\x65\x20\xE6\x97\xB6\xE5\xBC\x80\xE5\x90\xAF\xEF\xBC\x8C\xE5\x8F\xAF\xE8\x83\xBD\xE9\x99\x8D\xE4\xBD\x8E\xE6\x80\xA7\xE8\x83\xBD\xE6\x88\x96\xE5\xBD\xB1\xE5\x93\x8D\xE7\xA8\xB3\xE5\xAE\x9A\xE6\x80\xA7\xE3\x80\x82" },
        { "DesktopMirrorEnabled", "Performance", "\xE6\x80\xA7\xE8\x83\xBD", "Desktop mirror", "\xE6\xA1\x8C\xE9\x9D\xA2\xE9\x95\x9C\xE5\x83\x8F", "Copies the selected VR eye to the desktop game window.", "\xE5\xB0\x86\xE9\x80\x89\xE5\xAE\x9A\xE7\x9A\x84VR\xE5\x8D\x95\xE7\x9C\xBC\xE7\x94\xBB\xE9\x9D\xA2\xE5\xA4\x8D\xE5\x88\xB6\xE5\x88\xB0\xE6\xA1\x8C\xE9\x9D\xA2\xE6\xB8\xB8\xE6\x88\x8F\xE7\xAA\x97\xE5\x8F\xA3\xE3\x80\x82", "Disable if you only use the headset view or want less desktop output work.", "\xE5\x8F\xAA\xE4\xBD\xBF\xE7\x94\xA8\xE5\xA4\xB4\xE6\x98\xBE\xE8\xA7\x86\xE5\x9B\xBE\xEF\xBC\x8C\xE6\x88\x96\xE6\x83\xB3\xE5\x87\x8F\xE5\xB0\x91\xE6\xA1\x8C\xE9\x9D\xA2\xE8\xBE\x93\xE5\x87\xBA\xE5\xBC\x80\xE9\x94\x80\xE6\x97\xB6\xE5\x8F\xAF\xE5\x85\xB3\xE9\x97\xAD\xE3\x80\x82" },
        { "QueuedRenderPoseRelaxPercent", "Performance", "Performance", "Queued Pose Relax (%)", "Queued Pose Relax (%)", "Periodically bypasses the strict fresh-pose wait and allows repeated-pose frames to submit for more perceived FPS.", "Periodically bypasses the strict fresh-pose wait and allows repeated-pose frames to submit for more perceived FPS.", "0 is strict. Try 5 or 10 first; higher values trade more ghosting for cadence.", "0 is strict. Try 5 or 10 first; higher values trade more ghosting for cadence." },
        { "LocalVScriptConvarsEnabled", "Performance", "\346\200\247\350\203\275", "Enable LOD optimization", "\345\220\257\347\224\250LOD\344\274\230\345\214\226", "Use a more aggressive optimization scheme that slightly reduces visual quality while also improving performance.", "\344\275\277\347\224\250\346\233\264\346\277\200\350\277\233\347\232\204\344\274\230\345\214\226\346\226\271\346\241\210\357\274\214\351\231\215\344\275\216\346\233\264\345\244\232\350\247\206\350\247\211\350\241\250\347\216\260\357\274\214\345\244\247\345\271\205\346\217\220\351\253\230\346\200\247\350\203\275\343\200\202", "", "" },
        { "ShadowTweaksEnabled", "Performance", "\346\200\247\350\203\275", "Shadow Tweaks", "\351\230\264\345\275\261\344\274\230\345\214\226", "Applies the configured shadow cvar profile while playing.", "\345\234\250\346\270\270\346\210\217\344\270\255\345\272\224\347\224\250\351\205\215\347\275\256\347\232\204\351\230\264\345\275\261 cvar \346\226\271\346\241\210\343\200\202", "Turn this on only when you want the custom shadow profile to override the game's defaults.", "\345\217\252\345\234\250\351\234\200\350\246\201\350\207\252\345\256\232\344\271\211\351\230\264\345\275\261\346\226\271\346\241\210\350\246\206\347\233\226\346\270\270\346\210\217\351\273\230\350\256\244\345\200\274\346\227\266\345\274\200\345\220\257\343\200\202" },
        { "HudDistance", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Distance", "HUD \350\267\235\347\246\273", "Distance from the head to the main HUD plane.", "\345\244\264\351\203\250\345\210\260\344\270\273HUD\345\271\263\351\235\242\347\232\204\350\267\235\347\246\273\343\200\202", "Closer feels larger; farther reduces eye strain.", "\350\266\212\350\277\221\350\266\212\345\244\247\357\274\214\350\266\212\350\277\234\350\266\212\346\212\244\347\234\274\343\200\202" },
        { "HudSize", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Size", "HUD \345\260\272\345\257\270", "Overall scale of the main HUD.", "\344\270\273HUD\346\225\264\344\275\223\347\274\251\346\224\276\343\200\202", "1.2~2.0 fits most users.", "1.2~2.0 \351\200\202\345\220\210\345\244\247\345\244\232\346\225\260\344\272\272\343\200\202" },
        { "FixedHudXOffset", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Horizontal Offset", "HUD \345\267\246\345\217\263\345\201\217\347\247\273", "Moves the main HUD left/right on its own plane, in meters.", "\346\214\211\344\270\273 HUD \345\271\263\351\235\242\347\232\204\345\267\246\345\217\263\346\226\271\345\220\221\347\247\273\345\212\250\357\274\214\345\215\225\344\275\215\344\270\272\347\261\263\343\200\202", "Negative moves left, positive moves right.", "\350\264\237\346\225\260\345\220\221\345\267\246\357\274\214\346\255\243\346\225\260\345\220\221\345\217\263\343\200\202" },
        { "FixedHudYOffset", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Vertical Offset", "HUD \344\270\212\344\270\213\345\201\217\347\247\273", "Moves the main HUD up/down on its own plane, in meters.", "\346\214\211\344\270\273 HUD \345\271\263\351\235\242\347\232\204\344\270\212\344\270\213\346\226\271\345\220\221\347\247\273\345\212\250\357\274\214\345\215\225\344\275\215\344\270\272\347\261\263\343\200\202", "The default 0.25 cancels the built-in -0.25m vertical shift.", "\351\273\230\350\256\244 0.25 \344\274\232\346\212\265\346\266\210\345\206\205\347\275\256\347\232\204 -0.25 \347\261\263\351\253\230\345\272\246\344\277\256\346\255\243\343\200\202" },
        { "TopHudCurvature", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Curvature", "HUD \346\233\262\347\216\207", "Controls the curvature of the main SteamVR HUD overlay.", "\346\216\247\345\210\266\344\270\273 HUD \347\232\204 SteamVR \350\246\206\347\233\226\345\261\202\346\233\262\347\216\207\343\200\202", "0 is flat; higher values bend the panel more.", "0 \344\270\272\345\271\263\351\235\242\357\274\214\346\225\260\345\200\274\350\266\212\345\244\247\345\274\257\346\233\262\350\266\212\346\230\216\346\230\276\343\200\202" },
        { "HudFollowHmdMovement", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Follow HMD Movement", "HUD \350\267\237\351\232\217HMD\344\270\212\344\270\213", "When enabled, the main HUD uses the full HMD pose instead of yaw-only placement.", "\345\274\200\345\220\257\345\220\216\344\270\273 HUD \344\275\277\347\224\250\345\256\214\346\225\264 HMD \345\247\277\346\200\201\357\274\214\344\270\215\345\206\215\345\217\252\350\267\237\351\232\217\345\267\246\345\217\263\346\226\271\345\220\221\343\200\202", "Use this if you want the HUD to follow head pitch and vertical movement.", "\346\203\263\350\256\251 HUD \350\267\237\351\232\217\346\212\254\345\244\264\344\275\216\345\244\264\345\222\214\344\270\212\344\270\213\347\247\273\345\212\250\346\227\266\345\274\200\345\220\257\343\200\202" },
        { "HudAlwaysVisible", "HUD (Main)", "HUD\357\274\210\344\270\273\347\225\214\351\235\242\357\274\211", "HUD Always Visible", "HUD \346\200\273\346\230\257\345\217\257\350\247\201", "Keep HUD visible even when not toggled by gameplay.", "\345\215\263\344\275\277\346\234\252\350\242\253\346\270\270\346\210\217\351\200\273\350\276\221\346\230\276\347\244\272\344\271\237\345\247\213\347\273\210\346\230\276\347\244\272HUD\343\200\202", "Disable if you prefer a minimal view.", "\350\213\245\346\203\263\346\236\201\347\256\200\350\247\206\351\207\216\345\217\257\345\205\263\351\227\255\343\200\202" },
        { "LeftWristHudEnabled", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Enable Status HUD (Off-hand)", "\345\220\257\347\224\250\347\212\266\346\200\201HUD\357\274\210\345\211\257\346\211\213\357\274\211", "Shows a small wrist-style HUD on the off-hand controller using a SteamVR overlay.", "\345\234\250\345\211\257\346\211\213\346\211\213\346\237\204\344\270\212\347\224\250SteamVR\350\246\206\347\233\226\345\261\202\346\230\276\347\244\272\344\270\200\344\270\252\350\205\225\350\241\250\345\274\217\345\260\217HUD\343\200\202", "Displays HP and quick item status (throwable/med/pills or adrenaline).", "\346\230\276\347\244\272\347\224\237\345\221\275\345\200\274\344\270\216\345\205\263\351\224\256\347\211\251\345\223\201\347\212\266\346\200\201\357\274\210\346\212\225\346\216\267\347\211\251/\345\214\273\347\226\227\346\247\275/\350\215\257\347\211\207\346\210\226\350\202\276\344\270\212\350\205\272\347\264\240\357\274\211\343\200\202" },
        { "LeftWristHudWidthMeters", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Status HUD Width (meters)", "\347\212\266\346\200\201HUD\345\256\275\345\272\246\357\274\210\347\261\263\357\274\211", "Physical width of the Status HUD overlay quad (meters).", "\347\212\266\346\200\201HUD\350\246\206\347\233\226\345\261\202\345\271\263\351\235\242\347\232\204\347\211\251\347\220\206\345\256\275\345\272\246\357\274\210\347\261\263\357\274\211\343\200\202", "Bigger = easier to read, but can feel intrusive.", "\350\266\212\345\244\247\350\266\212\345\256\271\346\230\223\347\234\213\346\270\205\357\274\214\344\275\206\344\271\237\346\233\264\346\230\276\347\234\274\343\200\202" },
        { "LeftWristHudXOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Status HUD X Offset", "\347\212\266\346\200\201HUD X\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "LeftWristHudYOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Status HUD Y Offset", "\347\212\266\346\200\201HUD Y\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "LeftWristHudZOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Status HUD Z Offset", "\347\212\266\346\200\201HUD Z\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "LeftWristHudAngleOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Status HUD Angle Offset (pitch,yaw,roll)", "\347\212\266\346\200\201HUD\350\247\222\345\272\246\345\201\217\347\247\273 (\344\277\257\344\273\260,\345\201\217\350\210\252,\347\277\273\346\273\232)", "Additional rotation for the Status HUD overlay (degrees).", "\347\212\266\346\200\201HUD\350\246\206\347\233\226\345\261\202\347\232\204\351\242\235\345\244\226\346\227\213\350\275\254\357\274\210\345\272\246\357\274\211\343\200\202", "Adjust so it faces your eyes naturally.", "\350\260\203\345\210\260\347\234\213\350\265\267\346\235\245\345\203\217\350\264\264\345\234\250\346\211\213\350\205\225\344\270\212\343\200\201\350\207\252\347\204\266\346\234\235\345\220\221\347\234\274\347\235\233\345\215\263\345\217\257\343\200\202" },
        { "RightAmmoHudEnabled", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Enable Ammo HUD (Gun-hand)", "\345\220\257\347\224\250\345\274\271\350\215\257HUD\357\274\210\346\214\201\346\236\252\346\211\213\357\274\211", "Shows a compact ammo HUD on the gun-hand controller using a SteamVR overlay.", "\345\234\250\346\214\201\346\236\252\346\211\213\346\211\213\346\237\204\344\270\212\347\224\250SteamVR\350\246\206\347\233\226\345\261\202\346\230\276\347\244\272\344\270\200\344\270\252\347\247\221\346\212\200\346\204\237\345\274\271\350\215\257\346\241\206\343\200\202", "Displays clip/reserve and upgraded ammo when available.", "\346\230\276\347\244\272\345\274\271\345\214\243/\345\244\207\345\274\271\357\274\214\345\271\266\345\234\250\346\234\211\347\211\271\346\256\212\345\255\220\345\274\271\346\227\266\346\230\276\347\244\272\345\211\251\344\275\231\351\207\217\343\200\202" },
        { "RightAmmoHudWidthMeters", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Ammo HUD Width (meters)", "\345\274\271\350\215\257HUD\345\256\275\345\272\246\357\274\210\347\261\263\357\274\211", "Physical width of the ammo HUD overlay quad (meters).", "\345\274\271\350\215\257HUD\350\246\206\347\233\226\345\261\202\345\271\263\351\235\242\347\232\204\347\211\251\347\220\206\345\256\275\345\272\246\357\274\210\347\261\263\357\274\211\343\200\202", "Increase if numbers are too small.", "\345\246\202\346\236\234\346\225\260\345\255\227\345\244\252\345\260\217\345\260\261\350\260\203\345\244\247\343\200\202" },
        { "RightAmmoHudXOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Ammo HUD X Offset", "\345\274\271\350\215\257HUD X\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "RightAmmoHudYOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Ammo HUD Y Offset", "\345\274\271\350\215\257HUD Y\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "RightAmmoHudZOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Ammo HUD Z Offset", "\345\274\271\350\215\257HUD Z\345\201\217\347\247\273", "Overlay translation in controller local space (meters).", "\350\246\206\347\233\226\345\261\202\345\234\250\346\211\213\346\237\204\346\234\254\345\234\260\345\235\220\346\240\207\347\263\273\344\270\255\347\232\204\345\271\263\347\247\273\357\274\210\347\261\263\357\274\211\343\200\202", "Uses the same axis convention as other overlay offsets (ScopeOverlay*).", "\344\270\216\345\205\266\344\273\226\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273\357\274\210ScopeOverlay*\357\274\211\344\275\277\347\224\250\347\233\270\345\220\214\345\235\220\346\240\207\347\272\246\345\256\232\343\200\202" },
        { "RightAmmoHudAngleOffset", "HUD (Hand)", "HUD\357\274\210\346\211\213\346\237\204\357\274\211", "Ammo HUD Angle Offset (pitch,yaw,roll)", "\345\274\271\350\215\257HUD\350\247\222\345\272\246\345\201\217\347\247\273 (\344\277\257\344\273\260,\345\201\217\350\210\252,\347\277\273\346\273\232)", "Additional rotation for the ammo HUD overlay (degrees).", "\345\274\271\350\215\257HUD\350\246\206\347\233\226\345\261\202\347\232\204\351\242\235\345\244\226\346\227\213\350\275\254\357\274\210\345\272\246\357\274\211\343\200\202", "Adjust so it sits like a weapon-side panel.", "\350\260\203\345\210\260\345\203\217\350\264\264\345\234\250\346\255\246\345\231\250\346\227\201\350\276\271\347\232\204\345\260\217\345\261\217\345\271\225\345\215\263\345\217\257\343\200\202" },
        { "HideArms", "Hands / Debug", "\346\211\213\351\203\250 / \350\260\203\350\257\225", "Hide Arms", "\351\232\220\350\227\217\346\211\213\350\207\202", "Hides in-game arm models while keeping weapons.", "\351\232\220\350\227\217\346\270\270\346\210\217\344\270\255\347\232\204\346\211\213\350\207\202\346\250\241\345\236\213\357\274\214\344\273\205\344\277\235\347\225\231\346\255\246\345\231\250\343\200\202", "", "" },
        { "RequireSecondaryAttackForItemSwitch", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Require Alt-Fire for Item Switch", "\345\210\207\346\215\242\347\211\251\345\223\201\351\234\200\345\211\257\346\224\273\351\224\256", "Prevents accidental item switches unless secondary attack is held.", "\351\234\200\350\246\201\346\214\211\344\275\217\345\211\257\346\224\273\345\207\273\351\224\256\346\211\215\345\210\207\346\215\242\347\211\251\345\223\201\357\274\214\351\201\277\345\205\215\350\257\257\350\247\246\343\200\202", "", "" },
        { "VoiceRecordCombo", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Voice Chat Combo", "\350\257\255\351\237\263\350\201\212\345\244\251\347\273\204\345\220\210\351\224\256", "VR action combination that triggers voice chat (format: Action+Action).", "\350\247\246\345\217\221\350\257\255\351\237\263\350\201\212\345\244\251\347\232\204VR\345\212\250\344\275\234\347\273\204\345\220\210\357\274\210\346\240\274\345\274\217\357\274\232\345\212\250\344\275\234+\345\212\250\344\275\234\357\274\211\343\200\202", "Set to \"false\" to disable.", "\350\256\276\344\270\272 \"false\" \345\217\257\347\246\201\347\224\250\343\200\202" },
        { "QuickTurnCombo", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Quick Turn Combo", "\345\277\253\351\200\237\350\275\254\350\272\253\347\273\204\345\220\210\351\224\256", "Action combo that triggers a quick 180\302\260 turn.", "\350\247\246\345\217\221\345\277\253\351\200\237180\302\260\350\275\254\350\272\253\347\232\204\345\212\250\344\275\234\347\273\204\345\220\210\343\200\202", "Use VR action names joined with +.", "\344\275\277\347\224\250VR\345\212\250\344\275\234\345\220\215\345\271\266\347\224\250 + \350\277\236\346\216\245\343\200\202" },
        { "ViewmodelAdjustEnabled", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Enable Weapon Position Adjustment", "\345\220\257\347\224\250\346\255\246\345\231\250\344\275\215\347\275\256\350\260\203\346\225\264", "Allows saving manual weapon model offsets in VR.", "\345\205\201\350\256\270\345\234\250VR\344\270\255\344\277\235\345\255\230\346\255\246\345\231\250\346\250\241\345\236\213\347\232\204\344\275\215\347\275\256\345\201\217\347\247\273\343\200\202", "", "" },
        { "ViewmodelAdjustCombo", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Weapon Position Adjust Combo", "\346\255\246\345\231\250\344\275\215\347\275\256\350\260\203\346\225\264\347\273\204\345\220\210\351\224\256", "Action combo to toggle weapon position adjustment mode.", "\347\224\250\344\272\216\345\210\207\346\215\242\346\255\246\345\231\250\344\275\215\347\275\256\350\260\203\346\225\264\346\250\241\345\274\217\347\232\204\347\273\204\345\220\210\345\212\250\344\275\234\343\200\202", "Set to \"false\" to disable if you never edit offsets.", "\350\213\245\344\270\215\351\234\200\350\246\201\345\217\257\350\256\276\344\270\272 \"false\" \347\246\201\347\224\250\343\200\202" },
        { "ViewmodelAdjustMoveSpeed", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Viewmodel Adjust Move Speed", "\350\247\206\346\250\241\350\260\203\346\225\264\347\247\273\345\212\250\351\200\237\345\272\246", "Multiplier for weapon model translation while moving the left controller in adjustment mode.", "\347\247\273\345\212\250\346\211\213\346\237\204\346\227\266\350\247\206\346\250\241\345\201\217\347\247\273\347\232\204\345\200\215\347\216\207\343\200\202", "Lower is slower; higher is faster.", "\350\260\203\344\275\216\346\233\264\346\205\242\357\274\214\350\260\203\351\253\230\346\233\264\345\277\253\343\200\202" },
        { "ViewmodelAdjustRotateSpeed", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Viewmodel Adjust Rotate Speed", "\350\247\206\346\250\241\350\260\203\346\225\264\346\227\213\350\275\254\351\200\237\345\272\246", "Multiplier for weapon model rotation. Also controls left-stick pitch/yaw rotation in adjustment mode.", "\350\260\203\346\225\264\346\250\241\345\274\217\344\270\213\345\267\246\346\221\207\346\235\206\345\222\214\346\211\213\346\237\204\346\227\213\350\275\254\345\257\271\350\247\206\346\250\241\350\247\222\345\272\246\347\232\204\345\200\215\347\216\207\343\200\202", "Left stick only rotates the viewmodel in adjustment mode; it no longer moves the player.", "\345\267\246\346\221\207\346\235\206\345\234\250\350\260\203\346\225\264\346\250\241\345\274\217\344\270\213\345\217\252\346\227\213\350\275\254\350\247\206\346\250\241\357\274\214\344\270\215\345\206\215\347\247\273\345\212\250\344\272\272\347\211\251\343\200\202" },
        { "ViewmodelDisableMoveBob", "Interaction / Combos", "\344\272\244\344\272\222 / \347\273\204\345\220\210\351\224\256", "Disable Weapon Move Bob", "\347\246\201\347\224\250\346\255\246\345\231\250\347\247\273\345\212\250\346\231\203\345\212\250", "Disables movement bob/sway on the first-person weapon model.", "\347\246\201\347\224\250\347\254\254\344\270\200\344\272\272\347\247\260\346\255\246\345\231\250\346\250\241\345\236\213\351\232\217\347\247\273\345\212\250\344\272\247\347\224\237\347\232\204\346\231\203\345\212\250/\346\221\206\345\212\250\343\200\202", "When enabled, the weapon model appears more stable while moving.", "\345\274\200\345\220\257\345\220\216\357\274\214\347\247\273\345\212\250\346\227\266\346\255\246\345\231\250\346\250\241\345\236\213\344\274\232\346\233\264\347\250\263\345\256\232\343\200\202" },
        { "AutoRepeatSemiAutoFire", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Hold-to-Fire for Semi-Auto", "\345\215\225\345\217\221\346\236\252\351\225\277\346\214\211\350\277\236\345\217\221", "Converts a held primary-fire input into pulses so semi-auto / single-shot guns keep firing while you hold the trigger.", "\346\212\212\346\214\201\347\273\255\346\214\211\344\275\217\347\232\204\344\270\273\345\274\200\347\201\253\350\276\223\345\205\245\345\217\230\346\210\220\342\200\234\347\202\271\345\260\204\350\204\211\345\206\262\342\200\235\357\274\214\350\256\251\345\215\212\350\207\252\345\212\250/\345\215\225\345\217\221\346\255\246\345\231\250\345\234\250\346\214\211\344\275\217\346\211\263\346\234\272\346\227\266\344\271\237\350\203\275\350\277\236\347\273\255\345\260\204\345\207\273\343\200\202", "Does not affect full-auto weapons. Use AutoRepeatSemiAutoFireHz to adjust click rate.", "\344\270\215\345\275\261\345\223\215\345\205\250\350\207\252\345\212\250\346\255\246\345\231\250\343\200\202\345\217\257\347\224\250 AutoRepeatSemiAutoFireHz \350\260\203\346\225\264\342\200\234\350\277\236\347\202\271\342\200\235\351\242\221\347\216\207\343\200\202" },
        { "AutoRepeatSemiAutoFireHz", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Auto-Repeat Rate (Hz)", "\350\277\236\347\202\271\351\242\221\347\216\207 (Hz)", "How many fire pulses per second to generate while holding the trigger.", "\346\214\211\344\275\217\346\211\263\346\234\272\346\227\266\346\257\217\347\247\222\347\224\237\346\210\220\345\244\232\345\260\221\346\254\241\345\274\200\347\201\253\350\204\211\345\206\262\343\200\202", "Higher can feel snappier, but the weapon's real fire rate still limits shots.", "\350\260\203\351\253\230\344\274\232\346\233\264\342\200\234\350\267\237\346\211\213\342\200\235\357\274\214\344\275\206\345\256\236\351\231\205\345\260\204\351\200\237\344\273\215\345\217\227\346\255\246\345\231\250\346\234\254\350\272\253\351\231\220\345\210\266\343\200\202" },
        { "AutoRepeatSprayPushEnabled", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Auto Spray-Push", "\350\207\252\345\212\250 Spray-Push", "While hold-to-fire is active, automatically applies the extra spray/push input assist used by pump/chrome shotguns and AWP/scout.", "\345\234\250\351\225\277\346\214\211\350\277\236\345\217\221\346\277\200\346\264\273\346\227\266\357\274\214\350\207\252\345\212\250\351\231\204\345\270\246 pump/chrome \351\234\260\345\274\271\346\236\252\345\222\214 AWP/scout \344\275\277\347\224\250\347\232\204 spray/push \350\276\223\345\205\245\350\276\205\345\212\251\343\200\202", "Only matters when Hold-to-Fire for Semi-Auto is enabled.", "\344\273\205\345\234\250\342\200\234\345\215\225\345\217\221\346\236\252\351\225\277\346\214\211\350\277\236\345\217\221\342\200\235\345\274\200\345\220\257\346\227\266\346\211\215\346\234\211\346\204\217\344\271\211\343\200\202" },
        { "HitSoundEnabled", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Hit Sound Feedback", "\345\221\275\344\270\255\351\237\263\346\225\210\345\217\215\351\246\210", "Plays a short cue immediately when your shot trace hits an infected target.", "\345\275\223\346\234\254\345\234\260\345\260\204\345\207\273\350\275\250\350\277\271\345\221\275\344\270\255\346\204\237\346\237\223\350\200\205\347\233\256\346\240\207\346\227\266\357\274\214\347\253\213\345\210\273\346\222\255\346\224\276\347\237\255\344\277\203\345\221\275\344\270\255\346\217\220\347\244\272\351\237\263\343\200\202", "Uses the same local hit detection as the projected hit icon.", "\345\222\214\345\221\275\344\270\255\344\275\215\347\275\256\345\233\276\346\240\207\344\275\277\347\224\250\345\220\214\344\270\200\345\245\227\346\234\254\345\234\260\345\221\275\344\270\255\346\243\200\346\265\213\343\200\202" },
        { "HitSoundSpec", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Hit Sound", "\345\221\275\344\270\255\351\237\263\346\225\210", "Sound spec for non-lethal hits. Supports alias:, file:, game:, gamesound:, cmd:, or a direct audio file path.", "\346\231\256\351\200\232\345\221\275\344\270\255\351\237\263\346\225\210\351\205\215\347\275\256\343\200\202\346\224\257\346\214\201 alias:\343\200\201file:\343\200\201game:\343\200\201gamesound:\343\200\201cmd:\357\274\214\344\271\237\346\224\257\346\214\201\347\233\264\346\216\245\345\241\253\345\206\231\351\237\263\351\242\221\346\226\207\344\273\266\350\267\257\345\276\204\343\200\202", "Direct file paths may be absolute, or relative to the VR folder. Example: hit.mp3 resolves to VR\\hit.mp3", "\346\226\207\344\273\266\350\267\257\345\276\204\345\217\257\347\224\250\347\273\235\345\257\271\350\267\257\345\276\204\357\274\214\344\271\237\345\217\257\347\233\270\345\257\271 VR \347\233\256\345\275\225\343\200\202\347\244\272\344\276\213\357\274\232hit.mp3 \344\274\232\350\247\243\346\236\220\345\210\260 VR\\hit.mp3" },
        { "HitSoundVolume", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Feedback Sound Volume", "\345\217\215\351\246\210\351\237\263\351\207\217", "Unified volume multiplier for hit, kill, and headshot sounds. Adjusting this slider updates all three together.", "\347\273\237\344\270\200\346\216\247\345\210\266\345\221\275\344\270\255\343\200\201\345\207\273\346\235\200\343\200\201\347\210\206\345\244\264\344\270\211\347\247\215\346\217\220\347\244\272\351\237\263\347\232\204\351\237\263\351\207\217\345\200\215\347\216\207\343\200\202\350\260\203\346\225\264\350\277\231\344\270\252\346\273\221\346\235\206\344\274\232\345\220\214\346\227\266\345\206\231\345\205\245\344\270\211\344\270\252\351\205\215\347\275\256\351\241\271\343\200\202", "1.0 keeps source loudness unchanged. This slider is clamped to 0.5~2.0 in the config tool.", "1.0 \350\241\250\347\244\272\344\277\235\346\214\201\347\264\240\346\235\220\345\216\237\345\247\213\345\223\215\345\272\246\343\200\202\351\205\215\347\275\256\345\267\245\345\205\267\351\207\214\350\277\231\344\270\252\346\273\221\346\235\206\347\232\204\350\214\203\345\233\264\351\231\220\345\210\266\344\270\272 0.5~2.0\343\200\202" },
        { "KillSoundEnabled", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Kill Sound Feedback", "\345\207\273\346\235\200\351\237\263\346\225\210\345\217\215\351\246\210", "Plays a short sound when your local kill counter increases.", "\345\275\223\346\234\254\345\234\260\345\207\273\346\235\200\350\256\241\346\225\260\345\242\236\345\212\240\346\227\266\346\222\255\346\224\276\347\237\255\344\277\203\346\217\220\347\244\272\351\237\263\343\200\202", "Headshots can use a separate sound via KillSoundHeadshotSpec.", "\347\210\206\345\244\264\345\217\257\351\200\232\350\277\207 KillSoundHeadshotSpec \344\275\277\347\224\250\345\217\246\344\270\200\347\247\215\351\237\263\346\225\210\343\200\202" },
        { "KillSoundNormalSpec", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Normal Kill Sound", "\346\231\256\351\200\232\345\207\273\346\235\200\351\237\263\346\225\210", "Sound spec for normal kills. Supports alias:, file:, game:, gamesound:, cmd:, or a direct audio file path.", "\346\231\256\351\200\232\345\207\273\346\235\200\351\237\263\346\225\210\351\205\215\347\275\256\343\200\202\346\224\257\346\214\201 alias:\343\200\201file:\343\200\201game:\343\200\201gamesound:\343\200\201cmd:\357\274\214\344\271\237\346\224\257\346\214\201\347\233\264\346\216\245\345\241\253\345\206\231\351\237\263\351\242\221\346\226\207\344\273\266\350\267\257\345\276\204\343\200\202", "Direct file paths may be absolute, or relative to the VR folder.", "\346\226\207\344\273\266\350\267\257\345\276\204\345\217\257\347\224\250\347\273\235\345\257\271\350\267\257\345\276\204\357\274\214\344\271\237\345\217\257\347\233\270\345\257\271 VR \347\233\256\345\275\225\343\200\202" },
        { "KillSoundHeadshotSpec", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Headshot Kill Sound", "\347\210\206\345\244\264\345\207\273\346\235\200\351\237\263\346\225\210", "Sound spec used when the confirmed kill followed a recent head hit.", "\345\275\223\347\241\256\350\256\244\345\207\273\346\235\200\346\235\245\350\207\252\346\234\200\350\277\221\344\270\200\346\254\241\345\244\264\351\203\250\345\221\275\344\270\255\346\227\266\344\275\277\347\224\250\347\232\204\351\237\263\346\225\210\351\205\215\347\275\256\343\200\202", "Direct file paths may be absolute, or relative to the VR folder. Example: headshot.mp3 resolves to VR\\headshot.mp3", "\346\226\207\344\273\266\350\267\257\345\276\204\345\217\257\347\224\250\347\273\235\345\257\271\350\267\257\345\276\204\357\274\214\344\271\237\345\217\257\347\233\270\345\257\271 VR \347\233\256\345\275\225\343\200\202\347\244\272\344\276\213\357\274\232headshot.mp3 \344\274\232\350\247\243\346\236\220\345\210\260 VR\\headshot.mp3" },
        { "KillSoundVolume", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Kill Sound Volume", "\345\207\273\346\235\200\351\237\263\351\207\217", "Volume multiplier for normal kill sounds.", "\346\231\256\351\200\232\345\207\273\346\235\200\351\237\263\346\225\210\347\232\204\351\237\263\351\207\217\345\200\215\347\216\207\343\200\202", "Useful when the kill cue should sit lower than the hit cue or headshot cue.", "\351\200\202\345\220\210\345\234\250\344\275\240\346\203\263\350\256\251\346\231\256\351\200\232\345\207\273\346\235\200\345\274\261\344\272\216\345\221\275\344\270\255\346\210\226\347\210\206\345\244\264\346\217\220\347\244\272\346\227\266\350\260\203\346\225\264\343\200\202" },
        { "HeadshotSoundVolume", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Headshot Volume", "\347\210\206\345\244\264\351\237\263\351\207\217", "Volume multiplier used when a confirmed kill is matched to a recent head hit.", "\345\275\223\347\241\256\350\256\244\345\207\273\346\235\200\345\214\271\351\205\215\345\210\260\346\234\200\350\277\221\344\270\200\346\254\241\345\244\264\351\203\250\345\221\275\344\270\255\346\227\266\344\275\277\347\224\250\347\232\204\351\237\263\351\207\217\345\200\215\347\216\207\343\200\202", "Set above 1.0 if you want the headshot cue to pop harder than the normal kill cue.", "\345\246\202\346\236\234\346\203\263\350\256\251\347\210\206\345\244\264\346\217\220\347\244\272\346\233\264\342\200\234\347\202\270\342\200\235\357\274\214\345\217\257\344\273\245\350\256\276\345\210\260 1.0 \344\273\245\344\270\212\343\200\202" },
        { "KillIndicatorEnabled", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Hit / Kill Icons", "\345\221\275\344\270\255 / \345\207\273\346\235\200\345\233\276\346\240\207", "Projects animated hit and kill icons onto the HUD at the traced impact / confirmed kill position.", "\346\212\212\345\212\250\346\200\201\345\221\275\344\270\255\345\222\214\345\207\273\346\235\200\345\233\276\346\240\207\346\212\225\345\275\261\345\210\260 HUD \344\270\212\357\274\214\345\271\266\350\267\237\351\232\217\345\221\275\344\270\255\347\202\271 / \347\241\256\350\256\244\345\207\273\346\235\200\344\275\215\347\275\256\346\230\276\347\244\272\343\200\202", "Uses Source materials, so animated VMT/VTF feedback packs can play directly.", "\347\233\264\346\216\245\344\275\277\347\224\250 Source \346\235\220\350\264\250\357\274\214\345\270\246\345\212\250\347\224\273\347\232\204 VMT/VTF \345\217\215\351\246\210\347\264\240\346\235\220\345\217\257\344\273\245\347\233\264\346\216\245\346\222\255\346\224\276\343\200\202" },
        { "KillIndicatorMaterialBaseSpec", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Kill Icon Material Base", "\345\207\273\346\235\200\345\233\276\346\240\207\346\235\220\350\264\250\347\233\256\345\275\225", "Base material path or folder. The mod will use /hit, /kill, and /headshot under it.", "\346\235\220\350\264\250\345\237\272\347\241\200\350\267\257\345\276\204\346\210\226\347\233\256\345\275\225\343\200\202\346\250\241\347\273\204\344\274\232\350\207\252\345\212\250\344\275\277\347\224\250\345\205\266\344\270\255\347\232\204 /hit\343\200\201/kill \345\222\214 /headshot \346\235\220\350\264\250\343\200\202", "Supports engine material paths like overlays/2965700751 or an absolute folder path ending in materials\\...", "\346\224\257\346\214\201 overlays/2965700751 \350\277\231\347\261\273\346\235\220\350\264\250\350\267\257\345\276\204\357\274\214\344\271\237\346\224\257\346\214\201\344\273\245 materials\\... \347\273\223\345\260\276\347\232\204\347\273\235\345\257\271\347\233\256\345\275\225\350\267\257\345\276\204\343\200\202" },
        { "BlockFireOnFriendlyAimEnabled", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "Friendly-fire Aim Guard ", "\347\246\201\346\255\242\345\220\221\351\230\237\345\217\213\345\274\200\347\201\253", "Suppresses firing when your aim line is on a teammate.", "\345\275\223\347\236\204\345\207\206\347\272\277\346\214\207\345\220\221\351\230\237\345\217\213\346\227\266\346\212\221\345\210\266\345\274\200\347\201\253\343\200\202", "This is the startup default; you can still toggle it at runtime via SteamVR binding.", "\350\277\231\346\230\257\345\220\257\345\212\250\346\227\266\347\232\204\351\273\230\350\256\244\345\200\274\357\274\233\350\277\220\350\241\214\344\270\255\344\273\215\345\217\257\347\224\250 SteamVR \347\273\221\345\256\232\345\274\200\345\205\263\345\210\207\346\215\242\343\200\202" },
        { "MotionGestureSwingThreshold", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Swing Gesture Threshold", "\346\214\245\345\212\250\345\210\244\345\256\232\351\230\210\345\200\274", "Velocity needed to detect a swing gesture.", "\345\210\244\345\256\232\346\214\245\345\212\250\346\211\213\345\212\277\346\211\200\351\234\200\347\232\204\351\200\237\345\272\246\351\230\210\345\200\274\343\200\202", "Increase to reduce false swings.", "\346\217\220\351\253\230\345\217\257\345\207\217\345\260\221\350\257\257\345\210\244\343\200\202" },
        { "MotionGestureDownSwingThreshold", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Down Swing Threshold", "\344\270\213\345\212\210\345\210\244\345\256\232\351\230\210\345\200\274", "Velocity threshold for downward swing recognition.", "\345\210\244\345\256\232\344\270\213\345\212\210\345\212\250\344\275\234\347\232\204\351\200\237\345\272\246\351\230\210\345\200\274\343\200\202", "", "" },
        { "MotionGestureJumpThreshold", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Jump Gesture Threshold", "\350\267\263\350\267\203\346\211\213\345\212\277\351\230\210\345\200\274", "Vertical velocity required to trigger jump gesture.", "\350\247\246\345\217\221\350\267\263\350\267\203\346\211\213\345\212\277\346\211\200\351\234\200\347\232\204\345\236\202\347\233\264\351\200\237\345\272\246\343\200\202", "", "" },
        { "MotionGestureCooldown", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Gesture Cooldown", "\346\211\213\345\212\277\345\206\267\345\215\264", "Minimum seconds between repeated gesture activations.", "\351\207\215\345\244\215\346\211\213\345\212\277\350\247\246\345\217\221\344\271\213\351\227\264\347\232\204\346\234\200\345\260\217\351\227\264\351\232\224\357\274\210\347\247\222\357\274\211\343\200\202", "", "" },
        { "MotionGestureHoldDuration", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Gesture Hold Duration", "\346\211\213\345\212\277\346\214\211\344\275\217\346\227\266\351\225\277", "Seconds a gesture must be held to count as intentional.", "\346\211\213\345\212\277\351\234\200\344\277\235\346\214\201\347\232\204\347\247\222\346\225\260\357\274\214\350\266\205\350\277\207\346\211\215\345\210\244\345\256\232\344\270\272\346\234\211\346\225\210\343\200\202", "", "" },
        { "InventoryGestureRange", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Inventory Gesture Activation Range", "\351\201\223\345\205\267\351\224\232\347\202\271\346\212\223\345\217\226\347\232\204\346\234\211\346\225\210\350\214\203\345\233\264", "Distance from the inventory anchor within which grabbing is allowed (meters).", "\346\211\213\351\203\250\351\235\240\350\277\221\351\201\223\345\205\267\346\240\217\345\244\232\350\277\221\346\227\266\357\274\214\345\205\201\350\256\270\350\247\246\345\217\221\347\211\251\345\223\201\346\212\223\345\217\226\357\274\210\347\261\263\357\274\211\343\200\202", "Grab is triggered by pressing the grip when the hand is inside this range.", "\346\211\213\350\277\233\345\205\245\350\214\203\345\233\264\345\220\216\346\214\211\344\270\213\346\217\241\351\224\256\345\215\263\345\217\257\346\212\223\345\217\226\351\201\223\345\205\267\343\200\202" },
        { "ShowInventoryAnchors", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Show Inventory Grab Zones", "\346\230\276\347\244\272\351\201\223\345\205\267\346\240\217\346\212\223\345\217\226\345\214\272\345\237\237\357\274\210\344\270\215\347\206\237\346\202\211\344\275\215\347\275\256\346\227\266\345\273\272\350\256\256\345\274\200\345\220\257\357\274\211", "Draws visible grab zones for inventory anchors.", "\345\234\250\344\270\226\347\225\214\344\270\255\347\273\230\345\210\266\345\217\257\350\247\206\345\214\226\347\232\204\351\201\223\345\205\267\346\240\217\346\212\223\345\217\226\345\214\272\345\237\237\343\200\202", "Recommended for learning anchor positions; can be disabled once familiar.", "\347\206\237\346\202\211\345\220\204\351\224\232\347\202\271\344\275\215\347\275\256\345\220\216\345\217\257\345\205\263\351\227\255\344\273\245\345\207\217\345\260\221\350\247\206\350\247\211\345\271\262\346\211\260\343\200\202" },
        { "ScopeEnabled", "Optics", "\345\205\211\345\255\246", "Enable Gun Scope", "\345\220\257\347\224\250\346\236\252\346\242\260\347\236\204\345\207\206\351\225\234", "Renders a gun-mounted scope view to an overlay.", "\345\260\206\346\236\252\346\242\260\347\236\204\345\207\206\351\225\234\347\224\273\351\235\242\346\270\262\346\237\223\345\210\260\344\270\200\344\270\252\350\246\206\347\233\226\345\261\202\344\270\212\343\200\202", "If disabled, scope rendering and overlay are skipped.", "\345\205\263\351\227\255\345\220\216\345\260\206\344\270\215\345\206\215\346\270\262\346\237\223\347\236\204\345\207\206\351\225\234\345\217\212\345\205\266\350\246\206\347\233\226\345\261\202\343\200\202" },
        { "ScopeRTTSize", "Optics", "\345\205\211\345\255\246", "Scope Render Texture Size", "\347\236\204\345\207\206\351\225\234\346\270\262\346\237\223\345\210\206\350\276\250\347\216\207", "Render target size (pixels) for the scope view.", "\347\236\204\345\207\206\351\225\234\347\224\273\351\235\242\346\270\262\346\237\223\347\233\256\346\240\207\345\260\272\345\257\270\357\274\210\345\203\217\347\264\240\357\274\211\343\200\202", "Higher is sharper but costs more GPU time.", "\350\266\212\351\253\230\350\266\212\346\270\205\346\231\260\357\274\214\344\275\206GPU\345\274\200\351\224\200\346\233\264\345\244\247\343\200\202" },
        { "ScopeReticleAlpha", "Optics", "\345\205\211\345\255\246", "Scope Reticle Alpha", "\347\236\204\345\207\206\351\225\234\345\207\206\346\230\237\351\200\217\346\230\216\345\272\246", "Opacity scale for the reticle drawn inside the scope RTT.", "\350\260\203\346\225\264\347\236\204\345\207\206\351\225\234 RTT \345\206\205\351\203\250\345\207\206\346\230\237\347\232\204\344\270\215\351\200\217\346\230\216\345\272\246\343\200\202", "0 hides the reticle; 1 keeps the default opacity.", "0 \351\232\220\350\227\217\345\207\206\346\230\237\357\274\2331 \344\277\235\346\214\201\351\273\230\350\256\244\344\270\215\351\200\217\346\230\216\345\272\246\343\200\202" },
        { "ScopeDefaultFov", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Default Scope FOV", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4" "FOV", "Default magnification FOV for weapons without a saved scope profile.", "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8D\x95\xE7\x8B\xAC\xE4\xBF\x9D\xE5\xAD\x98\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\x85\x8D\xE7\xBD\xAE\xE7\x9A\x84\xE6\xAD\xA6\xE5\x99\xA8\xE4\xBC\x9A\xE4\xBD\xBF\xE7\x94\xA8\xE8\xBF\x99\xE4\xB8\xAA\xE9\xBB\x98\xE8\xAE\xA4" "FOV\xE3\x80\x82", "Smaller FOV means higher magnification. Per-weapon values are saved to scope_adjustments.txt.", "FOV\xE8\xB6\x8A\xE5\xB0\x8F\xE5\x80\x8D\xE7\x8E\x87\xE8\xB6\x8A\xE9\xAB\x98\xEF\xBC\x9B\xE6\xAF\x8F\xE6\x8A\x8A\xE6\xAD\xA6\xE5\x99\xA8\xE7\x9A\x84\xE5\xAE\x9E\xE9\x99\x85\xE5\x80\xBC\xE4\xBC\x9A\xE5\x86\x99\xE5\x85\xA5 scope_adjustments.txt\xE3\x80\x82" },
        { "ScopeMagnificationFovRange", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Scope Magnification FOV Range", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x80\x8D\xE7\x8E\x87" "FOV\xE8\x8C\x83\xE5\x9B\xB4", "Minimum and maximum FOV used by per-weapon magnification adjustment.", "\xE6\xAF\x8F\xE6\x8A\x8A\xE6\xAD\xA6\xE5\x99\xA8\xE7\x8B\xAC\xE7\xAB\x8B\xE5\x80\x8D\xE7\x8E\x87\xE8\xB0\x83\xE6\x95\xB4\xE4\xBD\xBF\xE7\x94\xA8\xE7\x9A\x84\xE6\x9C\x80\xE5\xB0\x8F/\xE6\x9C\x80\xE5\xA4\xA7" "FOV\xE3\x80\x82", "Format: min,max. Use + left-stick vertical changes the current weapon value.", "\xE6\xA0\xBC\xE5\xBC\x8F\xEF\xBC\x9A\xE6\x9C\x80\xE5\xB0\x8F,\xE6\x9C\x80\xE5\xA4\xA7\xE3\x80\x82\xE5\xBC\x80\xE9\x95\x9C\xE6\x97\xB6\xE6\x8C\x89\xE4\xBD\x8Fuse\xE5\xB9\xB6\xE4\xB8\x8A\xE4\xB8\x8B\xE6\x8E\xA8\xE5\x8A\xA8\xE5\xB7\xA6\xE6\x91\x87\xE6\x9D\x86\xE8\xB0\x83\xE6\x95\xB4\xE5\xBD\x93\xE5\x89\x8D\xE6\xAD\xA6\xE5\x99\xA8\xE5\x80\x8D\xE7\x8E\x87\xE3\x80\x82" },
        { "ScopeMagnificationAdjustSpeed", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Scope Magnification Adjust Speed", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x80\x8D\xE7\x8E\x87\xE8\xB0\x83\xE6\x95\xB4\xE9\x80\x9F\xE5\xBA\xA6", "FOV change speed for Use + left-stick vertical adjustment while the scope is active.", "\xE5\xBC\x80\xE9\x95\x9C\xE6\x97\xB6\xE6\x8C\x89\xE4\xBD\x8Fuse\xE5\xB9\xB6\xE4\xB8\x8A\xE4\xB8\x8B\xE6\x8E\xA8\xE5\x8A\xA8\xE5\xB7\xA6\xE6\x91\x87\xE6\x9D\x86\xE8\xB0\x83\xE6\x95\xB4\xE5\x80\x8D\xE7\x8E\x87\xE7\x9A\x84\xE9\x80\x9F\xE5\xBA\xA6\xE3\x80\x82", "Degrees per second at full stick deflection.", "\xE6\x91\x87\xE6\x9D\x86\xE6\x8E\xA8\xE6\xBB\xA1\xE6\x97\xB6\xE6\xAF\x8F\xE7\xA7\x92\xE6\x94\xB9\xE5\x8F\x98\xE7\x9A\x84" "FOV\xE8\xA7\x92\xE5\xBA\xA6\xE3\x80\x82" },
        { "ScopeSizeRange", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Scope Size Range", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE8\x8C\x83\xE5\x9B\xB4", "Minimum and maximum scope overlay width for per-weapon size adjustment.", "\xE6\xAF\x8F\xE6\x8A\x8A\xE6\xAD\xA6\xE5\x99\xA8\xE7\x8B\xAC\xE7\xAB\x8B\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE8\xB0\x83\xE6\x95\xB4\xE4\xBD\xBF\xE7\x94\xA8\xE7\x9A\x84\xE6\x9C\x80\xE5\xB0\x8F/\xE6\x9C\x80\xE5\xA4\xA7\xE5\xAE\xBD\xE5\xBA\xA6\xE3\x80\x82", "Format: min,max in meters. Use + left-stick horizontal changes the current weapon value.", "\xE6\xA0\xBC\xE5\xBC\x8F\xEF\xBC\x9A\xE6\x9C\x80\xE5\xB0\x8F,\xE6\x9C\x80\xE5\xA4\xA7\xEF\xBC\x8C\xE5\x8D\x95\xE4\xBD\x8D\xE7\xB1\xB3\xE3\x80\x82\xE5\xBC\x80\xE9\x95\x9C\xE6\x97\xB6\xE6\x8C\x89\xE4\xBD\x8Fuse\xE5\xB9\xB6\xE5\xB7\xA6\xE5\x8F\xB3\xE6\x8E\xA8\xE5\x8A\xA8\xE5\xB7\xA6\xE6\x91\x87\xE6\x9D\x86\xE8\xB0\x83\xE6\x95\xB4\xE5\xBD\x93\xE5\x89\x8D\xE6\xAD\xA6\xE5\x99\xA8\xE5\xA4\xA7\xE5\xB0\x8F\xE3\x80\x82" },
        { "ScopeSizeAdjustSpeed", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Scope Size Adjust Speed", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE8\xB0\x83\xE6\x95\xB4\xE9\x80\x9F\xE5\xBA\xA6", "Scope overlay width change speed for Use + left-stick horizontal adjustment.", "\xE6\x8C\x89\xE4\xBD\x8Fuse\xE5\xB9\xB6\xE5\xB7\xA6\xE5\x8F\xB3\xE6\x8E\xA8\xE5\x8A\xA8\xE5\xB7\xA6\xE6\x91\x87\xE6\x9D\x86\xE8\xB0\x83\xE6\x95\xB4\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\xA4\xA7\xE5\xB0\x8F\xE7\x9A\x84\xE9\x80\x9F\xE5\xBA\xA6\xE3\x80\x82", "Meters per second at full stick deflection.", "\xE6\x91\x87\xE6\x9D\x86\xE6\x8E\xA8\xE6\xBB\xA1\xE6\x97\xB6\xE6\xAF\x8F\xE7\xA7\x92\xE6\x94\xB9\xE5\x8F\x98\xE7\x9A\x84\xE7\xB1\xB3\xE6\x95\xB0\xE3\x80\x82" },
        { "ScopeOffsetAdjustMoveSpeed", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Scope Offset Adjust Move Speed", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE4\xBD\x8D\xE7\xBD\xAE\xE8\xB0\x83\xE6\x95\xB4\xE9\x80\x9F\xE5\xBA\xA6", "Multiplier for moving the scope overlay with the viewmodel-adjust shortcut while scoped.", "\xE5\xBC\x80\xE9\x95\x9C\xE6\x97\xB6\xE6\x8C\x89\xE4\xBD\x8F\xE8\xA7\x86\xE6\xA8\xA1\xE8\xB0\x83\xE6\x95\xB4\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE\xE5\xB9\xB6\xE7\xA7\xBB\xE5\x8A\xA8\xE6\x89\x8B\xE6\x9F\x84\xE8\xB0\x83\xE6\x95\xB4\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x81\x8F\xE7\xA7\xBB\xE7\x9A\x84\xE5\x80\x8D\xE7\x8E\x87\xE3\x80\x82", "While scoped, this shortcut edits scope offset instead of weapon viewmodel offset.", "\xE5\xBC\x80\xE9\x95\x9C\xE6\x97\xB6\xE8\xAF\xA5\xE5\xBF\xAB\xE6\x8D\xB7\xE9\x94\xAE\xE6\x94\xB9\xE4\xB8\xBA\xE8\xB0\x83\xE6\x95\xB4\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE5\x81\x8F\xE7\xA7\xBB\xEF\xBC\x8C\xE4\xB8\x8D\xE5\x86\x8D\xE8\xB0\x83\xE6\x95\xB4\xE6\xAD\xA6\xE5\x99\xA8\xE8\xA7\x86\xE6\xA8\xA1\xE5\x81\x8F\xE7\xA7\xBB\xE3\x80\x82" },
        { "ScopeDefaultOverlayWidthMeters", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Default Scope Size", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4\xE5\xA4\xA7\xE5\xB0\x8F", "Default scope overlay width for weapons without a saved scope profile.", "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8D\x95\xE7\x8B\xAC\xE4\xBF\x9D\xE5\xAD\x98\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\x85\x8D\xE7\xBD\xAE\xE7\x9A\x84\xE6\xAD\xA6\xE5\x99\xA8\xE4\xBC\x9A\xE4\xBD\xBF\xE7\x94\xA8\xE8\xBF\x99\xE4\xB8\xAA\xE9\xBB\x98\xE8\xAE\xA4\xE5\xA4\xA7\xE5\xB0\x8F\xE3\x80\x82", "Per-weapon values are saved to scope_adjustments.txt.", "\xE6\xAF\x8F\xE6\x8A\x8A\xE6\xAD\xA6\xE5\x99\xA8\xE7\x9A\x84\xE5\xAE\x9E\xE9\x99\x85\xE5\x80\xBC\xE4\xBC\x9A\xE5\x86\x99\xE5\x85\xA5 scope_adjustments.txt\xE3\x80\x82" },
        { "ScopeDefaultOverlayOffset", "Optics", "\xE5\x85\x89\xE5\xAD\xA6", "Default Scope Offset", "\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\xBB\x98\xE8\xAE\xA4\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x81\x8F\xE7\xA7\xBB", "Default scope overlay position offset for weapons without a saved scope profile.", "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8D\x95\xE7\x8B\xAC\xE4\xBF\x9D\xE5\xAD\x98\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE9\x85\x8D\xE7\xBD\xAE\xE7\x9A\x84\xE6\xAD\xA6\xE5\x99\xA8\xE4\xBC\x9A\xE4\xBD\xBF\xE7\x94\xA8\xE8\xBF\x99\xE4\xB8\xAA\xE9\xBB\x98\xE8\xAE\xA4\xE4\xBD\x8D\xE7\xBD\xAE\xE5\x81\x8F\xE7\xA7\xBB\xE3\x80\x82", "Format: x,y,z in meters relative to the scope overlay parent.", "\xE6\xA0\xBC\xE5\xBC\x8F\xEF\xBC\x9Ax,y,z\xEF\xBC\x8C\xE5\x8D\x95\xE4\xBD\x8D\xE7\xB1\xB3\xEF\xBC\x8C\xE7\x9B\xB8\xE5\xAF\xB9\xE7\x9E\x84\xE5\x87\x86\xE9\x95\x9C\xE8\xA6\x86\xE7\x9B\x96\xE5\xB1\x82\xE7\x88\xB6\xE7\xBA\xA7\xE3\x80\x82" },
        { "ScopeAimSensitivityFovReductionRate", "Optics", "\345\205\211\345\255\246", "Realtime FOV Sensitivity Reduction Rate", "\345\256\236\346\227\266 FOV \347\201\265\346\225\217\345\272\246\351\231\215\344\275\216\351\200\237\347\216\207", "Controls how strongly controller/mouse aim sensitivity is reduced as realtime scope FOV gets smaller.", "\345\256\236\346\227\266 FOV \350\266\212\345\260\217\357\274\214\347\236\204\345\207\206\347\201\265\346\225\217\345\272\246\350\207\252\345\212\250\351\231\215\344\275\216\357\274\233\350\277\231\344\270\252\346\225\260\345\200\274\346\216\247\345\210\266\351\231\215\344\275\216\345\274\272\345\272\246\343\200\202", "0 disables reduction. Higher values reduce sensitivity faster at low FOV.", "0 \350\241\250\347\244\272\344\270\215\351\231\215\344\275\216\343\200\202\346\225\260\345\200\274\350\266\212\345\244\247\357\274\214\344\275\216 FOV \344\270\213\347\201\265\346\225\217\345\272\246\351\231\215\345\276\227\350\266\212\345\277\253\343\200\202" },
        { "ScopeZNear", "Optics", "\345\205\211\345\255\246", "Scope Camera Near Plane", "\347\236\204\345\207\206\351\225\234\350\277\221\350\243\201\345\211\252\351\235\242", "Near clip distance for the scope camera.", "\347\236\204\345\207\206\351\225\234\347\233\270\346\234\272\347\232\204\350\277\221\350\243\201\345\211\252\350\267\235\347\246\273\343\200\202", "Increase if the scope camera clips through nearby geometry.", "\345\246\202\346\236\234\350\277\221\345\244\204\345\207\240\344\275\225\344\275\223\350\242\253\350\243\201\345\211\252/\347\251\277\346\250\241\357\274\214\345\217\257\351\200\202\345\275\223\350\260\203\345\244\247\343\200\202" },
        { "ScopeOverlayAngleOffset", "Optics", "\345\205\211\345\255\246", "Scope Overlay Angle Offset (pitch,yaw,roll)", "\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\350\247\222\345\272\246\345\201\217\347\247\273 (\344\277\257\344\273\260,\345\201\217\350\210\252,\347\277\273\346\273\232)", "Additional rotation for the scope overlay quad (degrees).", "\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\345\271\263\351\235\242\347\232\204\351\242\235\345\244\226\346\227\213\350\275\254\357\274\210\345\272\246\357\274\211\343\200\202", "Use this to make the scope face your eye correctly.", "\347\224\250\344\272\216\350\256\251\347\236\204\345\207\206\351\225\234\345\271\263\351\235\242\346\255\243\347\241\256\346\234\235\345\220\221\347\234\274\347\235\233\343\200\202" },
        { "ScopeRequireLookThrough", "Optics", "\345\205\211\345\255\246", "Require Looking Through Scope", "\351\234\200\350\246\201\350\264\264\350\277\221\347\236\204\345\207\206\351\225\234\346\211\215\346\230\276\347\244\272", "Only shows the scope overlay when your eye is close and aligned.", "\344\273\205\345\234\250\347\234\274\347\235\233\351\235\240\350\277\221\345\271\266\345\257\271\345\207\206\346\227\266\346\211\215\346\230\276\347\244\272\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\343\200\202", "When disabled, the scope stays hidden until Toggle Scope is pressed.", "\345\205\263\351\227\255\345\220\216\344\270\215\345\206\215\350\207\252\345\212\250\346\230\276\347\244\272\357\274\233\345\217\252\350\203\275\351\200\232\350\277\207\40\123\164\145\141\155\126\122\40\347\232\204\40\124\157\147\147\154\145\40\123\143\157\160\145\40\347\273\221\345\256\232\347\202\271\346\214\211\345\210\207\346\215\242\343\200\202" },
        { "ScopeLookThroughDistanceMeters", "Optics", "\345\205\211\345\255\246", "Look-Through Distance", "\350\264\264\350\277\221\350\267\235\347\246\273", "Max eye-to-scope distance to count as looking through (meters).", "\347\234\274\347\235\233\345\210\260\347\236\204\345\207\206\351\225\234\347\232\204\346\234\200\345\244\247\350\267\235\347\246\273\357\274\210\347\261\263\357\274\211\357\274\214\345\260\217\344\272\216\350\257\245\345\200\274\346\211\215\347\256\227\345\234\250\347\234\213\351\225\234\343\200\202", "Larger is more forgiving.", "\350\266\212\345\244\247\350\266\212\345\256\275\346\235\276\343\200\202" },
        { "ScopeLookThroughAngleDeg", "Optics", "\345\205\211\345\255\246", "Look-Through Angle", "\350\264\264\350\277\221\350\247\222\345\272\246", "Max angle off the scope axis to count as looking through (degrees).", "\350\247\206\347\272\277\345\201\217\347\246\273\347\236\204\345\207\206\351\225\234\350\275\264\347\272\277\347\232\204\346\234\200\345\244\247\350\247\222\345\272\246\357\274\210\345\272\246\357\274\211\343\200\202", "Smaller is stricter; larger is more forgiving.", "\350\266\212\345\260\217\350\266\212\344\270\245\346\240\274\357\274\214\350\266\212\345\244\247\350\266\212\345\256\275\346\235\276\343\200\202" },
        { "ScopeOverlayAlwaysVisible", "Optics", "\345\205\211\345\255\246", "Scope Overlay Always Visible", "\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\345\247\213\347\273\210\345\217\257\350\247\201", "Keeps the scope overlay visible even when not looking through.", "\345\215\263\344\275\277\346\262\241\346\234\211\350\264\264\350\277\221\347\236\204\345\207\206\351\225\234\357\274\214\344\271\237\344\277\235\346\214\201\350\246\206\347\233\226\345\261\202\345\217\257\350\247\201\343\200\202", "Useful for aligning/previewing the overlay.", "\347\224\250\344\272\216\345\257\271\351\275\220/\351\242\204\350\247\210\350\246\206\347\233\226\345\261\202\344\275\215\347\275\256\343\200\202" },
        { "ScopeOverlayIdleAlpha", "Optics", "\345\205\211\345\255\246", "Scope Idle Alpha", "\347\236\204\345\207\206\351\225\234\351\227\262\347\275\256\351\200\217\346\230\216\345\272\246", "Overlay alpha when not looking through (0~1).", "\346\234\252\350\264\264\350\277\221\346\227\266\350\246\206\347\233\226\345\261\202\351\200\217\346\230\216\345\272\246\357\274\2100~1\357\274\211\343\200\202", "0 = invisible, 1 = fully opaque.", "0=\345\256\214\345\205\250\351\200\217\346\230\216\357\274\2141=\345\256\214\345\205\250\344\270\215\351\200\217\346\230\216\343\200\202" },
        { "CustomAction1Command", "Custom Actions", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234", "Custom Action 1 Command", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\2341\346\214\207\344\273\244", "Console command", "\345\217\257\345\241\253\346\216\247\345\210\266\345\217\260\346\214\207\344\273\244", "Mapped to VR custom action slot 1.", "\345\257\271\345\272\224VR\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234\346\247\2751\343\200\202" },
        { "CustomAction2Command", "Custom Actions", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234", "Custom Action 2 Command", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\2342\346\214\207\344\273\244", "", "", "", "" },
        { "CustomAction3Command", "Custom Actions", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234", "Custom Action 3 Command", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\2343\346\214\207\344\273\244", "", "", "", "" },
        { "CustomAction4Command", "Custom Actions", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234", "Custom Action 4 Command", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\2344\346\214\207\344\273\244", "", "", "", "" },
        { "CustomAction5Command", "Custom Actions", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234", "Custom Action 5 Command", "\350\207\252\345\256\232\344\271\211\345\212\250\344\275\2345\346\214\207\344\273\244", "Defaults to vr_afk_fall_guard_hold. Bind SteamVR Custom Action 5 to a controller button and hold it while falling to arm the remote-server idle trigger.", "\351\273\230\350\256\244\344\270\272 vr_afk_fall_guard_hold\343\200\202\345\234\250 SteamVR \344\270\255\346\212\212\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234 5 \347\273\221\345\256\232\345\210\260\346\211\213\346\237\204\346\214\211\351\224\256\357\274\214\344\270\213\350\220\275\346\227\266\351\225\277\346\214\211\345\217\257\346\232\202\346\227\266\345\274\200\345\220\257\350\277\234\347\250\213\346\234\215\345\212\241\345\231\250\351\227\262\347\275\256\350\247\246\345\217\221\346\243\200\346\265\213\343\200\202", "Release the button to stop detection immediately. Replacing this command restores the slot for another custom action.", "\346\235\276\345\274\200\346\214\211\351\224\256\345\215\263\345\210\273\345\201\234\346\255\242\346\243\200\346\265\213\343\200\202\345\246\202\346\236\234\346\212\212\350\257\245\346\214\207\344\273\244\346\224\271\346\210\220\345\205\266\344\273\226\345\206\205\345\256\271\357\274\214\350\207\252\345\256\232\344\271\211\345\212\250\344\275\234 5 \344\274\232\346\201\242\345\244\215\344\270\272\346\231\256\351\200\232\350\207\252\345\256\232\344\271\211\346\247\275\344\275\215\343\200\202" },
        { "AntiAliasing", "Performance", "\346\200\247\350\203\275", "Anti-Aliasing Level", "\346\212\227\351\224\257\351\275\277\347\272\247\345\210\253", "Controls the anti-aliasing level used by the VR rendering path. Takes effect after restarting the game.", "\346\216\247\345\210\266 VR \346\270\262\346\237\223\350\267\257\345\276\204\344\275\277\347\224\250\347\232\204\346\212\227\351\224\257\351\275\277\347\272\247\345\210\253\343\200\202\xE9\x87\x8D\xE5\x90\xAF\xE6\xB8\xB8\xE6\x88\x8F\xE5\x90\x8E\xE7\x94\x9F\xE6\x95\x88\xE3\x80\x82", "0 disables anti-aliasing. Raise it only if your GPU has headroom.", "0 \350\241\250\347\244\272\345\205\263\351\227\255\346\212\227\351\224\257\351\275\277\343\200\202\345\217\252\346\234\211\345\234\250\346\230\276\345\215\241\344\275\231\351\207\217\345\205\205\350\266\263\346\227\266\345\206\215\346\217\220\351\253\230\343\200\202" },
        { "ThirdPersonVRCameraOffset", "Camera / Third Person", "\347\233\270\346\234\272 / \347\254\254\344\270\211\344\272\272\347\247\260", "Third-Person Camera Offset", "\347\254\254\344\270\211\344\272\272\347\247\260\347\233\270\346\234\272\345\201\217\347\247\273", "Distance offset applied to the third-person VR camera.", "\345\272\224\347\224\250\345\210\260\347\254\254\344\270\211\344\272\272\347\247\260 VR \347\233\270\346\234\272\347\232\204\350\267\235\347\246\273\345\201\217\347\247\273\343\200\202", "Positive values usually move the camera farther back.", "\346\255\243\345\200\274\351\200\232\345\270\270\344\274\232\346\212\212\347\233\270\346\234\272\346\213\211\345\276\227\346\233\264\351\235\240\345\220\216\343\200\202" },
        { "D3DAimLineOverlayEnabled", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "Enable Aim-Line", "\345\220\257\347\224\250\347\236\204\345\207\206\347\272\277", "Draws a screen-space aim line overlay using the D3D layer.", "\344\275\277\347\224\250 D3D \345\261\202\347\273\230\345\210\266\345\261\217\345\271\225\347\251\272\351\227\264\347\232\204\347\236\204\345\207\206\347\272\277\350\246\206\347\233\226\345\261\202\343\200\202", "Useful if you want a flat overlay instead of only world-space helpers.", "\345\246\202\346\236\234\344\275\240\346\203\263\344\275\277\347\224\250\345\271\263\351\235\242\350\246\206\347\233\226\345\261\202\357\274\214\350\200\214\344\270\215\345\217\252\344\276\235\350\265\226\344\270\226\347\225\214\347\251\272\351\227\264\350\276\205\345\212\251\347\272\277\357\274\214\345\217\257\344\273\245\345\274\200\345\220\257\343\200\202" },
        { "AimLineOnlyWhenLaserSight", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "Show Aim Line Only With Laser Sight", "\344\273\205\345\234\250\346\277\200\345\205\211\347\236\204\345\207\206\345\274\200\345\220\257\346\227\266\346\230\276\347\244\272\347\236\204\345\207\206\347\272\277", "When enabled, the VR aim line is hidden unless your firearm has the in-game laser sight upgrade active.", "\345\274\200\345\220\257\345\220\216\357\274\214\351\231\244\351\235\236\346\236\252\346\242\260\345\267\262\346\277\200\346\264\273\346\270\270\346\210\217\345\206\205\347\232\204\346\277\200\345\205\211\347\236\204\345\207\206\345\215\207\347\272\247\357\274\214\345\220\246\345\210\231\351\232\220\350\227\217VR\347\236\204\345\207\206\347\272\277\343\200\202", "Throwable trajectory arcs are not affected.", "\346\212\225\346\216\267\347\211\251\346\212\233\347\211\251\347\272\277\344\270\215\345\217\227\345\275\261\345\223\215\343\200\202" },
        { "D3DAimLineOverlayWidthPixels", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "D3D Aim-Line Width", "D3D \347\236\204\345\207\206\347\272\277\345\256\275\345\272\246", "Pixel width of the D3D aim-line overlay.", "D3D \347\236\204\345\207\206\347\272\277\350\246\206\347\233\226\345\261\202\347\232\204\345\203\217\347\264\240\345\256\275\345\272\246\343\200\202", "Increase only if the line is too thin to see clearly.", "\345\217\252\346\234\211\345\234\250\347\236\204\345\207\206\347\272\277\345\244\252\347\273\206\346\227\266\345\206\215\350\260\203\345\244\247\343\200\202" },
        { "D3DAimLineOverlayOutlinePixels", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "D3D Aim-Line Outline Width", "D3D \347\236\204\345\207\206\347\272\277\346\217\217\350\276\271\345\256\275\345\272\246", "Pixel width of the outline around the D3D aim-line.", "D3D \347\236\204\345\207\206\347\272\277\345\244\226\346\217\217\350\276\271\347\232\204\345\203\217\347\264\240\345\256\275\345\272\246\343\200\202", "Use a small outline to keep the line visible on bright scenes.", "\344\272\256\345\234\272\346\231\257\344\270\213\345\217\257\347\224\250\345\260\221\351\207\217\346\217\217\350\276\271\346\235\245\346\217\220\351\253\230\345\217\257\350\247\201\346\200\247\343\200\202" },
        { "D3DAimLineOverlayEndpointPixels", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "D3D Aim-Line Endpoint Size", "D3D \347\236\204\345\207\206\347\272\277\347\253\257\347\202\271\345\244\247\345\260\217", "Pixel size of the endpoint marker on the D3D aim-line overlay.", "D3D \347\236\204\345\207\206\347\272\277\350\246\206\347\233\226\345\261\202\347\253\257\347\202\271\346\240\207\350\256\260\347\232\204\345\203\217\347\264\240\345\244\247\345\260\217\343\200\202", "Raise it if you want the hit point marker to stand out more.", "\345\246\202\346\236\234\345\270\214\346\234\233\350\220\275\347\202\271\346\240\207\350\256\260\346\233\264\351\206\222\347\233\256\357\274\214\345\217\257\344\273\245\350\260\203\345\244\247\343\200\202" },
        { "D3DAimLineOverlayColor", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "D3D Aim-Line Color", "D3D \347\236\204\345\207\206\347\272\277\351\242\234\350\211\262", "RGBA color for the D3D aim-line overlay.", "D3D \347\236\204\345\207\206\347\272\277\350\246\206\347\233\226\345\261\202\344\275\277\347\224\250\347\232\204 RGBA \351\242\234\350\211\262\343\200\202", "Use a bright color with some alpha for readability.", "\345\273\272\350\256\256\344\275\277\347\224\250\351\253\230\344\272\256\351\242\234\350\211\262\345\271\266\344\277\235\347\225\231\344\270\200\347\202\271\351\200\217\346\230\216\345\272\246\343\200\202" },
        { "D3DAimLineOverlayOutlineColor", "Aim Assist", "\350\276\205\345\212\251\347\236\204\345\207\206", "D3D Aim-Line Outline Color", "D3D \347\236\204\345\207\206\347\272\277\346\217\217\350\276\271\351\242\234\350\211\262", "RGBA color for the D3D aim-line outline.", "D3D \347\236\204\345\207\206\347\272\277\346\217\217\350\276\271\344\275\277\347\224\250\347\232\204 RGBA \351\242\234\350\211\262\343\200\202", "Keep it subtle so the main line remains the focus.", "\345\273\272\350\256\256\351\242\234\350\211\262\344\275\216\350\260\203\344\270\200\344\272\233\357\274\214\351\201\277\345\205\215\346\212\242\344\270\273\347\272\277\347\232\204\350\247\206\350\247\211\347\204\246\347\202\271\343\200\202" },
        { "MotionGesturePushThreshold", "Motion Gestures", "\345\212\250\344\275\234\346\211\213\345\212\277", "Push Gesture Threshold", "\346\216\250\346\211\213\345\212\277\351\230\210\345\200\274", "Velocity threshold required to recognize a push gesture.", "\350\257\206\345\210\253\346\216\250\346\211\213\345\212\277\346\211\200\351\234\200\347\232\204\351\200\237\345\272\246\351\230\210\345\200\274\343\200\202", "Increase it if forward hand motions trigger too easily.", "\345\246\202\346\236\234\345\220\221\345\211\215\346\214\245\346\211\213\350\277\207\344\272\216\345\256\271\346\230\223\350\257\257\350\247\246\357\274\214\345\260\261\346\212\212\345\256\203\350\260\203\351\253\230\343\200\202" },
        { "InventoryBodyOriginOffset", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Body Origin Offset (x,y,z)", "\350\272\253\344\275\223\345\216\237\347\202\271\345\201\217\347\247\273 (x,y,z)", "Offsets the body-relative origin used to place inventory anchors.", "\350\260\203\346\225\264\347\224\250\344\272\216\346\224\276\347\275\256\347\211\251\345\223\201\346\240\217\351\224\232\347\202\271\347\232\204\350\272\253\344\275\223\347\233\270\345\257\271\345\216\237\347\202\271\343\200\202", "Use this if all anchor positions feel consistently shifted.", "\345\246\202\346\236\234\346\211\200\346\234\211\351\224\232\347\202\271\346\225\264\344\275\223\351\203\275\345\201\217\344\272\206\357\274\214\347\224\250\350\277\231\344\270\252\347\273\237\344\270\200\344\277\256\346\255\243\343\200\202" },
        { "InventoryAnchorColor", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Inventory Anchor Color", "\347\211\251\345\223\201\346\240\217\351\224\232\347\202\271\351\242\234\350\211\262", "RGBA color used when drawing visible inventory anchors.", "\346\230\276\347\244\272\347\211\251\345\223\201\346\240\217\351\224\232\347\202\271\346\227\266\344\275\277\347\224\250\347\232\204 RGBA \351\242\234\350\211\262\343\200\202", "Only matters when inventory anchors are visible.", "\344\273\205\345\234\250\346\230\276\347\244\272\351\224\232\347\202\271\346\227\266\347\224\237\346\225\210\343\200\202" },
        { "InventoryHudMarkerDistance", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Inventory HUD Marker Distance", "\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\350\267\235\347\246\273", "Distance used when placing inventory HUD markers relative to the player.", "\347\233\270\345\257\271\347\216\251\345\256\266\346\224\276\347\275\256\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\346\227\266\344\275\277\347\224\250\347\232\204\350\267\235\347\246\273\343\200\202", "Increase it if markers feel too close to your face.", "\345\246\202\346\236\234\346\240\207\350\256\260\347\246\273\350\204\270\345\244\252\350\277\221\357\274\214\345\217\257\344\273\245\350\260\203\345\244\247\343\200\202" },
        { "InventoryHudMarkerUpOffset", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Inventory HUD Marker Up Offset", "\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\344\270\212\347\247\273\345\201\217\347\247\273", "Vertical offset applied to inventory HUD markers.", "\345\272\224\347\224\250\345\210\260\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\347\232\204\345\236\202\347\233\264\345\201\217\347\247\273\343\200\202", "Negative values move the markers lower.", "\350\264\237\345\200\274\344\274\232\346\212\212\346\240\207\350\256\260\345\276\200\344\270\213\347\247\273\343\200\202" },
        { "InventoryHudMarkerSeparation", "Inventory / Anchors", "\347\211\251\345\223\201\346\240\217 / \351\224\232\347\202\271", "Inventory HUD Marker Separation", "\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\351\227\264\350\267\235", "Controls spacing between inventory HUD markers.", "\346\216\247\345\210\266\347\211\251\345\223\201\346\240\217 HUD \346\240\207\350\256\260\344\271\213\351\227\264\347\232\204\351\227\264\350\267\235\343\200\202", "Raise it if markers overlap each other.", "\345\246\202\346\236\234\346\240\207\350\256\260\344\272\222\347\233\270\351\207\215\345\217\240\357\274\214\345\260\261\346\212\212\345\256\203\350\260\203\345\244\247\343\200\202" },
        { "ScopeCameraOffset", "Optics", "\345\205\211\345\255\246", "Scope Camera Offset (x,y,z)", "\347\236\204\345\207\206\351\225\234\347\233\270\346\234\272\345\201\217\347\247\273 (x,y,z)", "Offsets the rendered scope camera relative to the weapon or controller.", "\350\260\203\346\225\264\347\236\204\345\207\206\351\225\234\346\270\262\346\237\223\347\233\270\346\234\272\347\233\270\345\257\271\346\255\246\345\231\250\346\210\226\346\216\247\345\210\266\345\231\250\347\232\204\344\275\215\347\275\256\343\200\202", "Use this when the rendered scope view feels misaligned.", "\345\246\202\346\236\234\347\236\204\345\207\206\351\225\234\347\224\273\351\235\242\344\275\215\347\275\256\344\270\215\345\257\271\357\274\214\345\217\257\344\273\245\347\224\250\345\256\203\346\240\241\346\255\243\343\200\202" },
        { "ScopeCameraAngleOffset", "Optics", "\345\205\211\345\255\246", "Scope Camera Angle Offset (pitch,yaw,roll)", "\347\236\204\345\207\206\351\225\234\347\233\270\346\234\272\350\247\222\345\272\246\345\201\217\347\247\273 (\344\277\257\344\273\260,\345\201\217\350\210\252,\347\277\273\346\273\232)", "Additional rotation applied to the rendered scope camera.", "\351\242\235\345\244\226\345\272\224\347\224\250\345\210\260\347\236\204\345\207\206\351\225\234\346\270\262\346\237\223\347\233\270\346\234\272\347\232\204\346\227\213\350\275\254\343\200\202", "Useful when the scope picture is tilted or not centered.", "\345\275\223\351\225\234\345\206\205\347\224\273\351\235\242\346\255\252\346\226\234\346\210\226\344\270\215\345\261\205\344\270\255\346\227\266\345\276\210\346\234\211\347\224\250\343\200\202" },
        { "ScopeStabilizationEnabled", "Optics", "\345\205\211\345\255\246", "Enable Scope Stabilization", "\345\220\257\347\224\250\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232", "Applies smoothing to scoped aiming to reduce visible jitter.", "\345\257\271\345\274\200\351\225\234\347\236\204\345\207\206\345\272\224\347\224\250\345\271\263\346\273\221\345\244\204\347\220\206\357\274\214\345\207\217\345\260\221\345\217\257\350\247\201\346\212\226\345\212\250\343\200\202", "Useful for high magnification scopes where tiny hand motion is amplified.", "\351\253\230\345\200\215\347\216\207\347\236\204\345\207\206\351\225\234\344\270\213\345\276\256\345\260\217\346\211\213\346\212\226\344\274\232\350\242\253\346\224\276\345\244\247\357\274\214\350\277\231\344\270\252\351\200\211\351\241\271\345\260\244\345\205\266\346\234\211\347\224\250\343\200\202" },
        { "ScopeStabilizationMinCutoff", "Optics", "\345\205\211\345\255\246", "Scope Stabilization Min Cutoff", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232\346\234\200\345\260\217\346\210\252\346\255\242\351\242\221\347\216\207", "Base cutoff used by the scope stabilization filter.", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232\346\273\244\346\263\242\345\231\250\344\275\277\347\224\250\347\232\204\345\237\272\347\241\200\346\210\252\346\255\242\351\242\221\347\216\207\343\200\202", "Lower values smooth more but respond slower.", "\346\225\260\345\200\274\350\266\212\344\275\216\350\266\212\345\271\263\346\273\221\357\274\214\344\275\206\345\223\215\345\272\224\344\271\237\350\266\212\346\205\242\343\200\202" },
        { "ScopeStabilizationBeta", "Optics", "\345\205\211\345\255\246", "Scope Stabilization Beta", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232 Beta", "Dynamic responsiveness factor for the scope stabilization filter.", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232\346\273\244\346\263\242\345\231\250\347\232\204\345\212\250\346\200\201\345\223\215\345\272\224\347\263\273\346\225\260\343\200\202", "Higher values track fast motion more aggressively.", "\346\225\260\345\200\274\350\266\212\351\253\230\357\274\214\345\277\253\351\200\237\345\212\250\344\275\234\347\232\204\350\267\237\351\232\217\344\274\232\346\233\264\347\247\257\346\236\201\343\200\202" },
        { "ScopeStabilizationDCutoff", "Optics", "\345\205\211\345\255\246", "Scope Stabilization D Cutoff", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232\345\257\274\346\225\260\346\210\252\346\255\242\351\242\221\347\216\207", "Derivative cutoff used by the scope stabilization filter.", "\347\236\204\345\207\206\351\225\234\347\250\263\345\256\232\346\273\244\346\263\242\345\231\250\344\275\277\347\224\250\347\232\204\345\257\274\346\225\260\346\210\252\346\255\242\351\242\221\347\216\207\343\200\202", "Adjust only if you need to fine-tune filter behavior.", "\345\217\252\346\234\211\345\234\250\351\234\200\350\246\201\347\273\206\350\260\203\346\273\244\346\263\242\350\241\214\344\270\272\346\227\266\345\206\215\346\224\271\345\256\203\343\200\202" },
        { "MouseModeScopedViewmodelAnchorOffset", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse-Mode Scoped Anchor Offset (x,y,z)", "\351\224\256\351\274\240\346\250\241\345\274\217\345\274\200\351\225\234\351\224\232\347\202\271\345\201\217\347\247\273 (x,y,z)", "Viewmodel anchor offset used while scoped in mouse mode.", "\351\224\256\351\274\240\346\250\241\345\274\217\345\274\200\351\225\234\346\227\266\344\275\277\347\224\250\347\232\204\346\255\246\345\231\250\346\250\241\345\236\213\351\224\232\347\202\271\345\201\217\347\247\273\343\200\202", "Use this to line up scoped weapons separately from hip-fire.", "\347\224\250\344\272\216\346\212\212\345\274\200\351\225\234\346\227\266\347\232\204\346\255\246\345\231\250\344\275\215\347\275\256\344\270\216\350\205\260\345\260\204\347\212\266\346\200\201\345\210\206\345\274\200\346\240\241\346\255\243\343\200\202" },
        { "MouseModeHmdAimSensitivity", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse-Mode HMD Aim Sensitivity", "\351\224\256\351\274\240\346\250\241\345\274\217\345\244\264\346\230\276\347\236\204\345\207\206\347\201\265\346\225\217\345\272\246", "Sensitivity multiplier applied when mouse-mode aiming is driven from the HMD.", "\351\224\256\351\274\240\346\250\241\345\274\217\347\224\261\345\244\264\346\230\276\351\251\261\345\212\250\347\236\204\345\207\206\346\227\266\344\275\277\347\224\250\347\232\204\347\201\265\346\225\217\345\272\246\345\200\215\347\216\207\343\200\202", "Raise it only if headset-driven aiming feels too sluggish.", "\345\217\252\346\234\211\345\234\250\345\244\264\346\230\276\347\236\204\345\207\206\346\230\216\346\230\276\345\201\217\346\205\242\346\227\266\345\206\215\346\217\220\351\253\230\343\200\202" },
        { "MouseModeScopeOverlayOffset", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse-Mode Scope Overlay Offset (x,y,z)", "\351\224\256\351\274\240\346\250\241\345\274\217\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\345\201\217\347\247\273 (x,y,z)", "Offset for the scope overlay when using mouse mode.", "\351\224\256\351\274\240\346\250\241\345\274\217\344\270\213\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\347\232\204\344\275\215\347\275\256\345\201\217\347\247\273\343\200\202", "Use this to align the overlay with scoped weapons in mouse mode.", "\347\224\250\344\272\216\346\212\212\351\224\256\351\274\240\346\250\241\345\274\217\344\270\213\347\232\204\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\345\222\214\346\255\246\345\231\250\345\257\271\351\275\220\343\200\202" },
        { "MouseModeScopeOverlayAngleOffset", "Input / Mouse Mode", "\350\276\223\345\205\245 / \351\224\256\351\274\240\346\250\241\345\274\217", "Mouse-Mode Scope Overlay Angle Offset", "\351\224\256\351\274\240\346\250\241\345\274\217\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\350\247\222\345\272\246\345\201\217\347\247\273", "Angular offset for the scope overlay when using mouse mode.", "\351\224\256\351\274\240\346\250\241\345\274\217\344\270\213\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\347\232\204\350\247\222\345\272\246\345\201\217\347\247\273\343\200\202", "Adjust this if the scope overlay appears rotated incorrectly.", "\345\246\202\346\236\234\347\236\204\345\207\206\351\225\234\350\246\206\347\233\226\345\261\202\350\247\222\345\272\246\344\270\215\345\257\271\357\274\214\345\217\257\344\273\245\347\224\250\345\256\203\346\240\241\346\255\243\343\200\202" },
        { "AutoRepeatSprayPushDelayTicks", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Spray-Push Delay Ticks", "Spray-Push \345\273\266\350\277\237 Tick", "Delay before the automatic spray-push assist is applied.", "\350\207\252\345\212\250 spray-push \350\276\205\345\212\251\350\247\246\345\217\221\345\211\215\347\255\211\345\276\205\347\232\204 Tick \346\225\260\343\200\202", "Increase it if the assist happens too early for your weapon timing.", "\345\246\202\346\236\234\350\276\205\345\212\251\350\247\246\345\217\221\345\276\227\345\244\252\346\227\251\357\274\214\345\217\257\344\273\245\346\212\212\345\256\203\350\260\203\345\244\247\343\200\202" },
        { "AutoRepeatSprayPushHoldTicks", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Spray-Push Hold Ticks", "Spray-Push \344\277\235\346\214\201 Tick", "How long the automatic spray-push assist stays active once triggered.", "\350\207\252\345\212\250 spray-push \350\276\205\345\212\251\350\247\246\345\217\221\345\220\216\344\277\235\346\214\201\347\224\237\346\225\210\347\232\204 Tick \346\225\260\343\200\202", "Use a low value unless your weapon timing needs more hold time.", "\351\231\244\351\235\236\346\255\246\345\231\250\350\212\202\345\245\217\347\241\256\345\256\236\351\234\200\350\246\201\357\274\214\345\220\246\345\210\231\345\273\272\350\256\256\344\277\235\346\214\201\350\276\203\344\275\216\346\225\260\345\200\274\343\200\202" },
        { "HitIndicatorEnabled", "Weapons / Fire", "\346\255\246\345\231\250 / \345\274\200\347\201\253", "Hit Indicator", "\345\221\275\344\270\255\346\214\207\347\244\272\345\231\250", "Shows a visual hit indicator when your shot connects.", "\345\275\223\344\275\240\347\232\204\345\260\204\345\207\273\345\221\275\344\270\255\346\227\266\346\230\276\347\244\272\350\247\206\350\247\211\345\221\275\344\270\255\346\217\220\347\244\272\343\200\202", "Useful if you want feedback without relying only on sound.", "\345\246\202\346\236\234\344\275\240\344\270\215\346\203\263\345\217\252\344\276\235\350\265\226\351\237\263\346\225\210\345\217\215\351\246\210\357\274\214\345\217\257\344\273\245\345\274\200\345\220\257\345\256\203\343\200\202" },
        { "FlashlightEnhancementEnabled", "General", "\351\200\232\347\224\250", "Flashlight Enhancement", "\346\211\213\347\224\265\345\242\236\345\274\272", "Enhances the flashlight by forcing a wider beam, brighter output, and longer third-person range on the client.", "\345\234\250\345\256\242\346\210\267\347\253\257\345\274\272\345\210\266\344\275\277\347\224\250\346\233\264\345\256\275\347\232\204\347\205\247\345\260\204\350\247\222\343\200\201\346\233\264\344\272\256\347\232\204\344\272\256\345\272\246\345\222\214\346\233\264\350\277\234\347\232\204\347\254\254\344\270\211\344\272\272\347\247\260\346\211\213\347\224\265\350\214\203\345\233\264\346\235\245\345\242\236\345\274\272\346\211\213\347\224\265\346\225\210\346\236\234\343\200\202", "Applies r_flashlight_3rd_person_range=300, r_flashlightbrightness=0.5, and r_flashlightfov=80 through VEngineCvar.", "\351\200\232\350\277\207 VEngineCvar \345\206\231\345\205\245 r_flashlight_3rd_person_range=300\343\200\201r_flashlightbrightness=0.5 \345\222\214 r_flashlightfov=80\343\200\202" },
        { "AutoFlashlightEnabled", "General", "\351\200\232\347\224\250", "Automatic Flashlight", "\350\207\252\345\212\250\346\211\213\347\224\265\347\255\222", "Automatically manages flashlight usage using the rendered VR eye image, not engine light probes.", "Uses rendered VR eye brightness, not game light sampling.", "Manual flashlight input pauses automation briefly to avoid fighting your input.", "Manual input temporarily pauses automation." },
        { "AutoFlashlightDarkThreshold", "General", "\351\200\232\347\224\250", "Auto Flashlight Dark Threshold", "Auto Flashlight Dark Threshold", "Turns the flashlight on after the center of the rendered eye image stays below this luma value.", "Turns on when the rendered eye center stays darker than this value.", "Lower values make auto-on less sensitive. Range is 0-255.", "Lower = less sensitive. Range 0-255." },
        { "AutoFlashlightBrightThreshold", "General", "\351\200\232\347\224\250", "Auto Flashlight Bright Threshold", "Auto Flashlight Bright Threshold", "Turns the flashlight off after peripheral rendered-eye brightness stays above this luma value.", "Turns off when peripheral rendered-eye brightness stays above this value.", "Uses peripheral brightness so the flashlight beam itself does not immediately turn the light off.", "Uses peripheral brightness to avoid self-shutoff." },
    };

    constexpr int kCfgOptionTextSpecCount = (int)(sizeof(kCfgOptionTextSpecs) / sizeof(kCfgOptionTextSpecs[0]));

    struct CfgOverlayLine
    {
        std::string raw;
        std::string key;
    };

    struct CfgOverlayState
    {
        std::atomic<bool> workerStarted{ false };
        vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
        vr::VROverlayHandle_t backHandle = vr::k_ulOverlayHandleInvalid;
        vr::VROverlayHandle_t menuButtonHandle = vr::k_ulOverlayHandleInvalid;
        bool visible = false;
        bool prevF8 = false;
        bool dirty = true;
        bool needsUpload = true;
        bool useChinese = true;
        int selected = 0;
        int scroll = 0;
        std::string status; // Set by CfgLoad/CfgToggleOpen so it follows the current UI language.
        std::string configPath;
        std::vector<CfgOverlayLine> lines;
        std::unordered_map<std::string, std::string> values;
        std::vector<int> visibleSpecIndexes;
        std::vector<uint8_t> rgba;
        std::vector<uint8_t> menuButtonRgba;
        bool menuButtonNeedsUpload = true;
        bool menuButtonRenderedChinese = true;
        bool hasUnsavedEdits = false;
        bool configWriteTimeValid = false;
        std::filesystem::file_time_type configWriteTime{};
        uint32_t lastConfigStatMs = 0;

        // The config editor is a world-fixed panel. When opened, capture the current HMD
        // pose once, place the panel in front of it, then keep that absolute transform.
        // Re-capture only when the panel is reopened or its distance is changed.
        bool fixedPlacementValid = false;
        bool fixedPlacementApplied = false;
        vr::ETrackingUniverseOrigin fixedPlacementOrigin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t fixedPlacementTransform{};
        float appliedOverlayDistanceMeters = -1.0f;
        float appliedOverlaySizeMeters = -1.0f;
        bool panelOverlayShown = false;
        bool baseOverlaysInputBlocked = false;
        bool menuButtonBaseInputBlocked = false;
        bool menuButtonHovered = false;
        bool menuButtonManualSelectPrev = false;
        int hoveredItem = -1;
        uint32_t hoverSelectionSuppressedUntilMs = 0;
        std::string componentEditKey;
        int componentEditIndex = 0;
        bool keyboardActive = false;
        std::string keyboardEditKey;
        vr::VROverlayHandle_t keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
        uint64_t keyboardUserValue = 0;
        uint32_t keyboardOpenMs = 0;
        std::mutex mutex;
    };

    CfgOverlayState g_CfgOverlay;

    static void CfgApplyOverlayPlacement(CfgOverlayState& s, vr::IVROverlay* ov = nullptr);
    static void CfgInvalidateFixedPlacement(CfgOverlayState& s);
    static void CfgOpenPanelFromMenuButton(CfgOverlayState& s, vr::IVROverlay* ov = nullptr);
    static vr::HmdMatrix34_t CfgMul34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b);
    static bool CfgGetCurrentHmdAbsolutePose(vr::ETrackingUniverseOrigin& origin, vr::HmdMatrix34_t& hmdAbs);

    static std::wstring CfgUtf8ToWide(const char* s)
    {
        if (!s || !*s)
            return std::wstring();

        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (len <= 0)
        {
            codePage = CP_ACP;
            flags = 0;
            len = MultiByteToWideChar(codePage, flags, s, -1, nullptr, 0);
        }
        if (len <= 0)
            return std::wstring();

        std::wstring out((size_t)len - 1, L'\0');
        MultiByteToWideChar(codePage, flags, s, -1, out.data(), len);
        return out;
    }

    static std::wstring CfgUtf8ToWide(const std::string& s)
    {
        return CfgUtf8ToWide(s.c_str());
    }

    static std::string CfgTrim(std::string s)
    {
        auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    }

    static std::string CfgLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    static int CfgFindSpecIndex(const std::string& key)
    {
        static std::unordered_map<std::string, int> s_index = []()
            {
                std::unordered_map<std::string, int> m;
                for (int i = 0; i < kCfgOptionSpecCount; ++i)
                    m.emplace(kCfgOptionSpecs[i].key, i);
                return m;
            }();

        auto it = s_index.find(key);
        return it == s_index.end() ? -1 : it->second;
    }


    static const CfgOptionTextSpec* CfgFindTextSpec(const char* key)
    {
        if (!key)
            return nullptr;

        static std::unordered_map<std::string, int> s_textIndex = []()
            {
                std::unordered_map<std::string, int> m;
                for (int i = 0; i < kCfgOptionTextSpecCount; ++i)
                    m.emplace(kCfgOptionTextSpecs[i].key, i);
                return m;
            }();

        auto it = s_textIndex.find(key);
        return it == s_textIndex.end() ? nullptr : &kCfgOptionTextSpecs[it->second];
    }

    static const char* CfgGroupText(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const CfgOptionTextSpec* t = CfgFindTextSpec(spec.key);
        if (!s.useChinese && t && t->groupEnUtf8 && *t->groupEnUtf8)
            return t->groupEnUtf8;
        if (s.useChinese && t && t->groupZhUtf8 && *t->groupZhUtf8)
            return t->groupZhUtf8;
        return spec.groupZhUtf8 ? spec.groupZhUtf8 : "";
    }

    static const char* CfgTitleText(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const CfgOptionTextSpec* t = CfgFindTextSpec(spec.key);
        if (!s.useChinese && t && t->titleEnUtf8 && *t->titleEnUtf8)
            return t->titleEnUtf8;
        if (s.useChinese && t && t->titleZhUtf8 && *t->titleZhUtf8)
            return t->titleZhUtf8;
        return spec.titleZhUtf8 ? spec.titleZhUtf8 : spec.key;
    }

    static const char* CfgDescText(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const CfgOptionTextSpec* t = CfgFindTextSpec(spec.key);
        if (!s.useChinese && t && t->descEnUtf8 && *t->descEnUtf8)
            return t->descEnUtf8;
        if (s.useChinese && t && t->descZhUtf8 && *t->descZhUtf8)
            return t->descZhUtf8;
        return "";
    }

    static const char* CfgTipText(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const CfgOptionTextSpec* t = CfgFindTextSpec(spec.key);
        if (!s.useChinese && t && t->tipEnUtf8 && *t->tipEnUtf8)
            return t->tipEnUtf8;
        if (s.useChinese && t && t->tipZhUtf8 && *t->tipZhUtf8)
            return t->tipZhUtf8;
        return "";
    }

    static bool CfgIsBoolText(const std::string& v)
    {
        const std::string t = CfgLower(CfgTrim(v));
        return t == "true" || t == "false" || t == "1" || t == "0" ||
            t == "yes" || t == "no" || t == "on" || t == "off" ||
            t == "enable" || t == "disable" || t == "enabled" || t == "disabled";
    }

    static bool CfgBoolValue(const std::string& v)
    {
        const std::string t = CfgLower(CfgTrim(v));
        return t == "true" || t == "1" || t == "yes" || t == "on" || t == "enable" || t == "enabled";
    }

    static bool CfgSystemPrefersChinese()
    {
        auto isChineseLang = [](LANGID id) -> bool
            {
                return PRIMARYLANGID(id) == LANG_CHINESE;
            };

        if (isChineseLang(GetUserDefaultUILanguage()) ||
            isChineseLang(GetUserDefaultLangID()) ||
            isChineseLang(GetSystemDefaultUILanguage()) ||
            isChineseLang(GetSystemDefaultLangID()))
        {
            return true;
        }

        wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0)
        {
            const wchar_t c0 = localeName[0];
            const wchar_t c1 = localeName[1];
            if ((c0 == L'z' || c0 == L'Z') && (c1 == L'h' || c1 == L'H'))
                return true;
        }

        return false;
    }

    static void CfgReadLanguageValue(CfgOverlayState& s)
    {
        // Language selection is no longer stored in config.txt.
        // Chinese Windows locales use Chinese; every other locale uses English.
        s.useChinese = CfgSystemPrefersChinese();
    }

    static bool CfgIsLanguageOption(const CfgOptionSpec& spec)
    {
        (void)spec;
        return false;
    }

    static bool CfgTryFloat(const std::string& v, float& out)
    {
        std::string t = CfgTrim(v);
        if (t.empty())
            return false;
        char* end = nullptr;
        out = std::strtof(t.c_str(), &end);
        return end && *end == '\0' && end != t.c_str();
    }

    static bool CfgTryInt(const std::string& v, int& out)
    {
        std::string t = CfgTrim(v);
        if (t.empty())
            return false;
        char* end = nullptr;
        const long parsed = std::strtol(t.c_str(), &end, 10);
        if (!(end && *end == '\0' && end != t.c_str()))
            return false;
        out = (int)parsed;
        return true;
    }

    static float CfgClampFloatToSpec(const CfgOptionSpec& spec, float v)
    {
        if (spec.maxValue > spec.minValue)
            v = (std::clamp)(v, spec.minValue, spec.maxValue);
        return v;
    }

    static int CfgClampIntToSpec(const CfgOptionSpec& spec, int v)
    {
        if (spec.maxValue > spec.minValue)
            v = (std::clamp)(v, (int)std::lround(spec.minValue), (int)std::lround(spec.maxValue));
        return v;
    }

    static float CfgStepForFloat(const CfgOptionSpec& spec, float current)
    {
        const float range = spec.maxValue - spec.minValue;
        if (range > 0.0f)
        {
            if (range <= 0.05f) return 0.001f;
            if (range <= 0.5f) return 0.01f;
            if (range <= 2.0f) return 0.05f;
            if (range <= 10.0f) return 0.1f;
            if (range <= 80.0f) return 1.0f;
            if (range <= 400.0f) return 5.0f;
            return 50.0f;
        }

        const float absV = std::fabs(current);
        if (absV >= 1000.0f) return 50.0f;
        if (absV >= 100.0f) return 5.0f;
        if (absV >= 10.0f) return 1.0f;
        if (absV > 0.0f && absV < 1.0f) return 0.01f;
        return 0.1f;
    }

    static int CfgStepPrecision(float step)
    {
        if (step >= 1.0f)
            return 0;
        int precision = 0;
        float scaled = step;
        while (precision < 4 && std::fabs(scaled - std::round(scaled)) > 0.0001f)
        {
            scaled *= 10.0f;
            ++precision;
        }
        return precision;
    }

    static std::string CfgTrimTrailingZeros(std::string s)
    {
        const size_t dot = s.find('.');
        if (dot == std::string::npos)
            return s;
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        if (s == "-0")
            s = "0";
        return s;
    }

    static std::string CfgFormatFloat(float v, float step)
    {
        const float snapped = (step > 0.0f) ? std::round(v / step) * step : v;
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "%.*f", CfgStepPrecision(step), snapped);
        return CfgTrimTrailingZeros(buf);
    }

    static int CfgComponentCount(const CfgOptionSpec& spec)
    {
        if (spec.type == CfgOptionType::Color)
            return 4;
        if (spec.type == CfgOptionType::Vec3)
            return 3;
        return 0;
    }

    static const char* CfgComponentLabel(const CfgOptionSpec& spec, int index)
    {
        static const char* kVecLabels[] = { "X", "Y", "Z" };
        static const char* kColorLabels[] = { "R", "G", "B", "A" };
        if (spec.type == CfgOptionType::Color)
            return (index >= 0 && index < 4) ? kColorLabels[index] : "?";
        if (spec.type == CfgOptionType::Vec3)
            return (index >= 0 && index < 3) ? kVecLabels[index] : "?";
        return "?";
    }

    static bool CfgParseComponentValues(const CfgOptionSpec& spec, const std::string& text, std::vector<float>& out)
    {
        const int count = CfgComponentCount(spec);
        out.assign((size_t)count, 0.0f);
        if (count <= 0)
            return false;

        std::string source = CfgTrim(text);
        if (source.empty())
            source = spec.defaultValue ? spec.defaultValue : "";

        std::stringstream ss(source);
        std::string part;
        int index = 0;
        bool ok = true;
        while (std::getline(ss, part, ',') && index < count)
        {
            float parsed = 0.0f;
            if (!CfgTryFloat(part, parsed))
            {
                parsed = 0.0f;
                ok = false;
            }
            out[(size_t)index++] = parsed;
        }

        if (index < count)
            ok = false;

        if (!ok && spec.defaultValue && source != spec.defaultValue)
            return CfgParseComponentValues(spec, spec.defaultValue, out);

        return ok;
    }

    static float CfgComponentStep(const CfgOptionSpec& spec, int index, float current)
    {
        (void)index;
        if (spec.type == CfgOptionType::Color)
            return 1.0f;
        return CfgStepForFloat(spec, current);
    }

    static std::string CfgFormatComponentValues(const CfgOptionSpec& spec, const std::vector<float>& values)
    {
        const int count = CfgComponentCount(spec);
        std::string result;
        for (int i = 0; i < count; ++i)
        {
            if (i > 0)
                result += ",";

            const float v = (i < (int)values.size()) ? values[(size_t)i] : 0.0f;
            if (spec.type == CfgOptionType::Color)
            {
                const int iv = (std::clamp)((int)std::lround(v), 0, 255);
                result += std::to_string(iv);
            }
            else
            {
                const float step = CfgComponentStep(spec, i, v);
                result += CfgFormatFloat(CfgClampFloatToSpec(spec, v), step);
            }
        }
        return result;
    }

    static int CfgSelectedComponentIndex(CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const int count = CfgComponentCount(spec);
        if (count <= 0)
            return 0;
        if (s.componentEditKey != spec.key)
        {
            s.componentEditKey = spec.key;
            s.componentEditIndex = 0;
        }
        s.componentEditIndex = (std::clamp)(s.componentEditIndex, 0, count - 1);
        return s.componentEditIndex;
    }

    static void CfgSetSelectedComponentIndex(CfgOverlayState& s, const CfgOptionSpec& spec, int index)
    {
        const int count = CfgComponentCount(spec);
        if (count <= 0)
            return;
        s.componentEditKey = spec.key;
        s.componentEditIndex = (std::clamp)(index, 0, count - 1);
        s.dirty = true;
    }

    static void CfgMarkEdited(CfgOverlayState& s);
    static std::string CfgValueFor(const CfgOverlayState& s, const CfgOptionSpec& spec);
    static bool CfgIsComponentEditable(const CfgOptionSpec& spec);
    static void CfgRebuildVisibleIndexes(CfgOverlayState& s);

    static void CfgAdjustComponentValue(CfgOverlayState& s, const CfgOptionSpec& spec, int dir)
    {
        if (!CfgIsComponentEditable(spec))
            return;

        std::vector<float> values;
        CfgParseComponentValues(spec, CfgValueFor(s, spec), values);
        const int index = CfgSelectedComponentIndex(s, spec);
        if (index < 0 || index >= (int)values.size())
            return;

        const float current = values[(size_t)index];
        const float step = CfgComponentStep(spec, index, current);
        float next = current + (float)dir * step;
        if (spec.type == CfgOptionType::Color)
            next = (std::clamp)(next, 0.0f, 255.0f);
        else
            next = CfgClampFloatToSpec(spec, next);

        values[(size_t)index] = next;
        const std::string value = CfgFormatComponentValues(spec, values);
        s.values[spec.key] = value;
        s.status = std::string(spec.key) + ": " + CfgComponentLabel(spec, index) + " = " +
            (spec.type == CfgOptionType::Color ? std::to_string((int)std::lround(next)) : CfgFormatFloat(next, step));
        CfgRebuildVisibleIndexes(s);
        CfgMarkEdited(s);
    }

    static std::string CfgGetModuleDir()
    {
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return {};
        std::string s(path, n);
        const size_t slash = s.find_last_of("\\/");
        return slash == std::string::npos ? std::string() : s.substr(0, slash);
    }

    static std::string CfgDefaultConfigPath()
    {
        const std::string dir = CfgGetModuleDir();
        return dir.empty() ? std::string("vr\\config.txt") : dir + "\\vr\\config.txt";
    }

    static bool CfgReadConfigWriteTime(CfgOverlayState& s, std::filesystem::file_time_type& outTime)
    {
        if (s.configPath.empty())
            s.configPath = CfgDefaultConfigPath();

        try
        {
            outTime = std::filesystem::last_write_time(s.configPath);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    static void CfgRefreshConfigWriteTime(CfgOverlayState& s)
    {
        std::filesystem::file_time_type writeTime{};
        if (CfgReadConfigWriteTime(s, writeTime))
        {
            s.configWriteTime = writeTime;
            s.configWriteTimeValid = true;
        }
        else
        {
            s.configWriteTimeValid = false;
        }
    }

    static void CfgMarkEdited(CfgOverlayState& s)
    {
        s.hasUnsavedEdits = true;
        s.dirty = true;
    }

    static std::string CfgValueFor(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const auto it = s.values.find(spec.key);
        if (it != s.values.end())
            return it->second;
        return spec.defaultValue ? spec.defaultValue : "";
    }

    static float CfgFloatValue(const CfgOverlayState& s, const char* key, float fallback)
    {
        if (!key || !*key)
            return fallback;

        const int specIndex = CfgFindSpecIndex(key);
        std::string value;
        auto it = s.values.find(key);
        if (it != s.values.end())
            value = it->second;
        else if (specIndex >= 0)
            value = kCfgOptionSpecs[specIndex].defaultValue ? kCfgOptionSpecs[specIndex].defaultValue : "";

        float parsed = fallback;
        if (!CfgTryFloat(value, parsed) || !std::isfinite(parsed))
            parsed = fallback;

        if (specIndex >= 0)
            parsed = CfgClampFloatToSpec(kCfgOptionSpecs[specIndex], parsed);
        return parsed;
    }

    static bool CfgIsKeyboardEditable(const CfgOptionSpec& spec)
    {
        return spec.type == CfgOptionType::String;
    }

    static bool CfgIsComponentEditable(const CfgOptionSpec& spec)
    {
        return spec.type == CfgOptionType::Vec3 || spec.type == CfgOptionType::Color;
    }

    static bool CfgIsAdjustable(const CfgOptionSpec& spec)
    {
        return spec.type == CfgOptionType::Bool ||
            spec.type == CfgOptionType::Float ||
            spec.type == CfgOptionType::Int ||
            CfgIsComponentEditable(spec) ||
            CfgIsKeyboardEditable(spec);
    }

    static void CfgParseLine(CfgOverlayLine& cl)
    {
        cl.key.clear();
        std::string t = cl.raw;
        if (!t.empty() && (t[0] == '#' || t[0] == '/'))
            return;

        const size_t eq = t.find('=');
        if (eq == std::string::npos)
            return;

        cl.key = CfgTrim(t.substr(0, eq));
    }

    static bool CfgStartsWith(const char* value, const char* prefix)
    {
        if (!value || !prefix)
            return false;
        while (*prefix)
        {
            if (*value++ != *prefix++)
                return false;
        }
        return true;
    }

    static bool CfgIsEnabled(const CfgOverlayState& s, const char* key, bool defVal = false)
    {
        if (!key || !*key)
            return defVal;
        const int specIndex = CfgFindSpecIndex(key);
        std::string value;
        auto it = s.values.find(key);
        if (it != s.values.end())
            value = it->second;
        else if (specIndex >= 0)
            value = kCfgOptionSpecs[specIndex].defaultValue ? kCfgOptionSpecs[specIndex].defaultValue : "";
        else
            return defVal;
        return CfgBoolValue(value);
    }

    static bool CfgHitKillIndicatorsEnabled(const CfgOverlayState& s)
    {
        return CfgIsEnabled(s, "KillIndicatorEnabled", false) || CfgIsEnabled(s, "HitIndicatorEnabled", false);
    }

    static bool CfgIsSpecVisible(const CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        const char* key = spec.key;
        if (!key || !*key)
            return false;

        // Match the desktop ConfigTool dependency rules: child options disappear when
        // the feature toggle that owns them is disabled.
        if (std::strcmp(key, "HitIndicatorEnabled") == 0)
            return false;

        if (std::strcmp(key, "AimLineOnlyWhenLaserSight") == 0)
            return CfgIsEnabled(s, "D3DAimLineOverlayEnabled", false);

        if (CfgStartsWith(key, "MouseMode") && std::strcmp(key, "MouseModeEnabled") != 0)
            return CfgIsEnabled(s, "MouseModeEnabled", false);

        if (CfgStartsWith(key, "LeftWristHud") && std::strcmp(key, "LeftWristHudEnabled") != 0)
            return CfgIsEnabled(s, "LeftWristHudEnabled", false);

        if (CfgStartsWith(key, "RightAmmoHud") && std::strcmp(key, "RightAmmoHudEnabled") != 0)
            return CfgIsEnabled(s, "RightAmmoHudEnabled", false);


        if (CfgStartsWith(key, "D3DAimLineOverlay") && std::strcmp(key, "D3DAimLineOverlayEnabled") != 0)
            return CfgIsEnabled(s, "D3DAimLineOverlayEnabled", false);

        if (CfgStartsWith(key, "Scope") && std::strcmp(key, "ScopeEnabled") != 0)
        {
            if (!CfgIsEnabled(s, "ScopeEnabled", false))
                return false;

            if ((std::strcmp(key, "ScopeLookThroughDistanceMeters") == 0 ||
                std::strcmp(key, "ScopeLookThroughAngleDeg") == 0) &&
                !CfgIsEnabled(s, "ScopeRequireLookThrough", true))
                return false;

            if (CfgStartsWith(key, "ScopeStabilization") && std::strcmp(key, "ScopeStabilizationEnabled") != 0)
                return CfgIsEnabled(s, "ScopeStabilizationEnabled", true);
        }

        if (std::strcmp(key, "SnapTurnAngle") == 0)
            return CfgIsEnabled(s, "SnapTurning", false);

        if (std::strcmp(key, "ViewmodelAdjustCombo") == 0 ||
            std::strcmp(key, "ViewmodelAdjustMoveSpeed") == 0 ||
            std::strcmp(key, "ViewmodelAdjustRotateSpeed") == 0)
            return CfgIsEnabled(s, "ViewmodelAdjustEnabled", false);

        if (CfgStartsWith(key, "QueuedRender") ||
            std::strcmp(key, "QueuedSubmitUseRenderPoseToken") == 0)
            return CfgIsEnabled(s, "AutoMatQueueMode", false);


        if (std::strcmp(key, "AutoRepeatSemiAutoFireHz") == 0 ||
            std::strcmp(key, "AutoRepeatSprayPushEnabled") == 0)
            return CfgIsEnabled(s, "AutoRepeatSemiAutoFire", false);

        if (std::strcmp(key, "AutoRepeatSprayPushDelayTicks") == 0 ||
            std::strcmp(key, "AutoRepeatSprayPushHoldTicks") == 0)
            return CfgIsEnabled(s, "AutoRepeatSemiAutoFire", false) && CfgIsEnabled(s, "AutoRepeatSprayPushEnabled", false);


        if (std::strcmp(key, "HitSoundSpec") == 0)
            return CfgIsEnabled(s, "HitSoundEnabled", false);

        if (std::strcmp(key, "HitSoundVolume") == 0)
            return CfgIsEnabled(s, "HitSoundEnabled", false) || CfgIsEnabled(s, "KillSoundEnabled", false);

        if (std::strcmp(key, "KillSoundNormalSpec") == 0 ||
            std::strcmp(key, "KillSoundHeadshotSpec") == 0 ||
            std::strcmp(key, "KillSoundVolume") == 0 ||
            std::strcmp(key, "HeadshotSoundVolume") == 0)
            return CfgIsEnabled(s, "KillSoundEnabled", false);

        if (std::strcmp(key, "FeedbackSoundSpatialBlend") == 0 ||
            std::strcmp(key, "FeedbackSoundSpatialRange") == 0)
            return CfgIsEnabled(s, "HitSoundEnabled", false) || CfgIsEnabled(s, "KillSoundEnabled", false);

        if (CfgStartsWith(key, "KillIndicator") && std::strcmp(key, "KillIndicatorEnabled") != 0)
            return CfgHitKillIndicatorsEnabled(s);

        if (std::strcmp(key, "AimLineThickness") == 0 ||
            std::strcmp(key, "AimLineColor") == 0)
            return CfgIsEnabled(s, "AimLineEnabled", true);


        return true;
    }

    static bool CfgIsSelectableRow(const CfgOverlayState& s, int item)
    {
        return item >= 0 && item < (int)s.visibleSpecIndexes.size();
    }

    static void CfgRebuildVisibleIndexes(CfgOverlayState& s)
    {
        std::string selectedKey;
        if (CfgIsSelectableRow(s, s.selected))
            selectedKey = kCfgOptionSpecs[s.visibleSpecIndexes[s.selected]].key;

        s.visibleSpecIndexes.clear();
        s.visibleSpecIndexes.reserve(kCfgOptionSpecCount);
        for (int i = 0; i < kCfgOptionSpecCount; ++i)
        {
            if (CfgIsSpecVisible(s, kCfgOptionSpecs[i]))
                s.visibleSpecIndexes.push_back(i);
        }

        if (!selectedKey.empty())
        {
            for (int i = 0; i < (int)s.visibleSpecIndexes.size(); ++i)
            {
                if (selectedKey == kCfgOptionSpecs[s.visibleSpecIndexes[i]].key)
                {
                    s.selected = i;
                    break;
                }
            }
        }

        if (s.selected >= (int)s.visibleSpecIndexes.size())
            s.selected = (std::max)(0, (int)s.visibleSpecIndexes.size() - 1);
        if (s.selected < 0)
            s.selected = 0;
        if (s.scroll > s.selected)
            s.scroll = s.selected;
        s.scroll = (std::clamp)(s.scroll, 0, (std::max)(0, (int)s.visibleSpecIndexes.size() - 1));
    }

    static void CfgEnsureSelectedVisible(CfgOverlayState& s)
    {
        const int total = (int)s.visibleSpecIndexes.size();
        if (total <= 0)
        {
            s.selected = 0;
            s.scroll = 0;
            return;
        }

        s.selected = (std::clamp)(s.selected, 0, total - 1);
        if (s.selected < s.scroll)
            s.scroll = s.selected;
        if (s.selected >= s.scroll + kCfgOverlayRowsVisible)
            s.scroll = s.selected - kCfgOverlayRowsVisible + 1;
        s.scroll = (std::clamp)(s.scroll, 0, (std::max)(0, total - 1));
    }

    static void CfgMoveSelection(CfgOverlayState& s, int delta)
    {
        const int total = (int)s.visibleSpecIndexes.size();
        if (total <= 0)
        {
            s.selected = 0;
            s.scroll = 0;
            return;
        }
        s.selected = (std::clamp)(s.selected + delta, 0, total - 1);
        CfgEnsureSelectedVisible(s);
        s.dirty = true;
    }

    static void CfgPageSelection(CfgOverlayState& s, int dir)
    {
        const int total = (int)s.visibleSpecIndexes.size();
        if (total <= 0)
        {
            s.selected = 0;
            s.scroll = 0;
            return;
        }

        const int step = (std::max)(1, kCfgOverlayRowsVisible - 1);
        const int oldScroll = s.scroll;
        s.scroll = (std::clamp)(s.scroll + dir * step, 0, (std::max)(0, total - 1));
        s.selected = s.scroll;
        CfgEnsureSelectedVisible(s);
        if (s.scroll != oldScroll)
            s.dirty = true;
    }

    static void CfgLoad(CfgOverlayState& s)
    {
        CfgReadLanguageValue(s);
        s.configPath = CfgDefaultConfigPath();
        s.lines.clear();
        s.values.clear();

        std::ifstream in(s.configPath, std::ios::binary);
        if (!in.good())
        {
            s.status = (s.useChinese ? "\346\227\240\346\263\225\346\211\223\345\274\200\351\205\215\347\275\256\346\226\207\344\273\266\357\274\232" : "Cannot open config file: ") + s.configPath;
            CfgRebuildVisibleIndexes(s);
            s.dirty = true;
            return;
        }

        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            CfgOverlayLine cl;
            cl.raw = line;
            CfgParseLine(cl);
            if (!cl.key.empty())
            {
                const size_t eq = line.find('=');
                std::string val = (eq == std::string::npos) ? std::string() : line.substr(eq + 1);
                while (!val.empty() && std::isspace((unsigned char)val.front()))
                    val.erase(val.begin());
                if (CfgFindSpecIndex(cl.key) >= 0)
                    s.values[cl.key] = val;
            }
            s.lines.push_back(cl);
        }

        CfgRebuildVisibleIndexes(s);
        CfgRefreshConfigWriteTime(s);
        s.hasUnsavedEdits = false;
        s.status = s.useChinese
            ? "\345\267\262\346\211\223\345\274\200\343\200\202\347\224\250 VR \346\216\247\345\210\266\345\231\250\345\260\204\347\272\277\347\202\271\345\207\273\346\214\211\351\222\256\343\200\202"
            : "Opened. Point and click with the VR controller laser.";
        s.dirty = true;
    }

    static void CfgSave(CfgOverlayState& s)
    {
        if (s.configPath.empty())
            s.configPath = CfgDefaultConfigPath();

        try
        {
            std::filesystem::path parent = std::filesystem::path(s.configPath).parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);
        }
        catch (...)
        {
        }


        for (int specIndex = 0; specIndex < kCfgOptionSpecCount; ++specIndex)
        {
            const CfgOptionSpec& spec = kCfgOptionSpecs[specIndex];
            const std::string val = CfgValueFor(s, spec);
            bool written = false;

            for (int i = (int)s.lines.size() - 1; i >= 0; --i)
            {
                if (s.lines[i].key == spec.key)
                {
                    s.lines[i].raw = std::string(spec.key) + "=" + val;
                    written = true;
                    break;
                }
            }

            if (!written)
            {
                CfgOverlayLine cl;
                cl.key = spec.key;
                cl.raw = std::string(spec.key) + "=" + val;
                s.lines.push_back(cl);
            }
        }

        std::ofstream out(s.configPath, std::ios::binary | std::ios::trunc);
        if (!out.good())
        {
            s.status = (s.useChinese ? "\344\277\235\345\255\230\345\244\261\350\264\245\357\274\232" : "Save failed: ") + s.configPath;
            s.dirty = true;
            return;
        }

        for (const CfgOverlayLine& line : s.lines)
            out << line.raw << "\n";

        CfgRefreshConfigWriteTime(s);
        s.hasUnsavedEdits = false;
        s.status = s.useChinese ? "\345\267\262\344\277\235\345\255\230 config.txt\357\274\214\347\216\260\346\234\211\347\203\255\345\212\240\350\275\275\351\200\273\350\276\221\344\274\232\350\207\252\345\212\250\347\224\237\346\225\210\343\200\202" : "Saved config.txt. Existing hot-reload logic will apply it.";
        s.dirty = true;
    }

    static void CfgAdjustSelected(CfgOverlayState& s, int dir)
    {
        if (s.visibleSpecIndexes.empty())
            return;

        const int item = (std::clamp)(s.selected, 0, (int)s.visibleSpecIndexes.size() - 1);
        const CfgOptionSpec& spec = kCfgOptionSpecs[s.visibleSpecIndexes[item]];
        std::string value = CfgValueFor(s, spec);

        if (spec.type == CfgOptionType::Bool)
        {
            const bool next = !CfgBoolValue(value);
            value = next ? "true" : "false";
            s.values[spec.key] = value;
            if (CfgIsLanguageOption(spec))
                s.useChinese = next;
            s.status = std::string(spec.key) + " = " + value;
            CfgRebuildVisibleIndexes(s);
            CfgMarkEdited(s);
            return;
        }

        if (spec.type == CfgOptionType::Int)
        {
            int v = 0;
            if (!CfgTryInt(value, v))
                CfgTryInt(spec.defaultValue ? spec.defaultValue : "0", v);
            v = CfgClampIntToSpec(spec, v + dir);
            value = std::to_string(v);
            s.values[spec.key] = value;
            s.status = std::string(spec.key) + " = " + value;
            CfgRebuildVisibleIndexes(s);
            CfgMarkEdited(s);
            return;
        }

        if (spec.type == CfgOptionType::Float)
        {
            float f = 0.0f;
            if (!CfgTryFloat(value, f))
                CfgTryFloat(spec.defaultValue ? spec.defaultValue : "0", f);
            const float step = CfgStepForFloat(spec, f);
            f = CfgClampFloatToSpec(spec, f + (float)dir * step);
            value = CfgFormatFloat(f, step);
            s.values[spec.key] = value;
            s.status = std::string(spec.key) + " = " + value;
            CfgRebuildVisibleIndexes(s);
            CfgMarkEdited(s);
            return;
        }

        if (CfgIsComponentEditable(spec))
        {
            CfgAdjustComponentValue(s, spec, dir);
            return;
        }

        if (CfgIsKeyboardEditable(spec))
        {
            s.status = std::string(s.useChinese ? "Click Edit to open VR keyboard: " : "Click Edit to open the VR keyboard: ") + spec.key;
            s.dirty = true;
            return;
        }

        s.status = std::string(s.useChinese ? "This option is not editable in the VR panel: " : "This option is not editable in the VR panel: ") + spec.key;
        s.dirty = true;
    }

    struct CfgRgb
    {
        uint8_t r, g, b;
    };

    struct CfgGdiSurface
    {
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        void* bits = nullptr;
        HFONT titleFont = nullptr;
        HFONT headerFont = nullptr;
        HFONT normalFont = nullptr;
        HFONT smallFont = nullptr;
        HFONT boldFont = nullptr;

        ~CfgGdiSurface()
        {
            if (titleFont) DeleteObject(titleFont);
            if (headerFont) DeleteObject(headerFont);
            if (normalFont) DeleteObject(normalFont);
            if (smallFont) DeleteObject(smallFont);
            if (boldFont) DeleteObject(boldFont);
            if (dc) DeleteDC(dc);
            if (bmp) DeleteObject(bmp);
        }
    };

    static HFONT CfgMakeFont(int px, int weight)
    {
        return CreateFontW(
            -px, 0, 0, 0, weight,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Microsoft YaHei UI");
    }

    static bool CfgCreateGdiSurface(CfgGdiSurface& g)
    {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = kCfgOverlayW;
        bmi.bmiHeader.biHeight = -kCfgOverlayH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        g.dc = CreateCompatibleDC(nullptr);
        g.bmp = CreateDIBSection(g.dc, &bmi, DIB_RGB_COLORS, &g.bits, nullptr, 0);
        if (!g.dc || !g.bmp || !g.bits)
            return false;

        SelectObject(g.dc, g.bmp);
        SetBkMode(g.dc, TRANSPARENT);
        g.titleFont = CfgMakeFont(34, FW_BOLD);
        g.headerFont = CfgMakeFont(20, FW_BOLD);
        g.normalFont = CfgMakeFont(20, FW_NORMAL);
        g.smallFont = CfgMakeFont(14, FW_NORMAL);
        g.boldFont = CfgMakeFont(20, FW_BOLD);
        return g.titleFont && g.headerFont && g.normalFont && g.smallFont && g.boldFont;
    }

    static COLORREF CfgColorRef(CfgRgb c)
    {
        return RGB(c.r, c.g, c.b);
    }

    static void CfgGdiFill(CfgGdiSurface& g, int x, int y, int w, int h, CfgRgb c)
    {
        RECT rc{ x, y, x + w, y + h };
        HBRUSH brush = CreateSolidBrush(CfgColorRef(c));
        FillRect(g.dc, &rc, brush);
        DeleteObject(brush);
    }

    static void CfgGdiFrame(CfgGdiSurface& g, int x, int y, int w, int h, CfgRgb c, int thickness = 1)
    {
        CfgGdiFill(g, x, y, w, thickness, c);
        CfgGdiFill(g, x, y + h - thickness, w, thickness, c);
        CfgGdiFill(g, x, y, thickness, h, c);
        CfgGdiFill(g, x + w - thickness, y, thickness, h, c);
    }

    static void CfgGdiTextW(CfgGdiSurface& g, const RECT& rc, const std::wstring& text, HFONT font, CfgRgb c, UINT extraFlags = 0)
    {
        if (text.empty())
            return;
        HFONT oldFont = (HFONT)SelectObject(g.dc, font);
        SetTextColor(g.dc, CfgColorRef(c));
        RECT copy = rc;
        DrawTextW(g.dc, text.c_str(), (int)text.size(), &copy, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | extraFlags);
        SelectObject(g.dc, oldFont);
    }

    static void CfgGdiText(CfgGdiSurface& g, int x, int y, int w, int h, const char* textUtf8, HFONT font, CfgRgb c, UINT extraFlags = 0)
    {
        RECT rc{ x, y, x + w, y + h };
        CfgGdiTextW(g, rc, CfgUtf8ToWide(textUtf8), font, c, extraFlags);
    }

    static void CfgGdiText(CfgGdiSurface& g, int x, int y, int w, int h, const std::string& textUtf8, HFONT font, CfgRgb c, UINT extraFlags = 0)
    {
        RECT rc{ x, y, x + w, y + h };
        CfgGdiTextW(g, rc, CfgUtf8ToWide(textUtf8), font, c, extraFlags);
    }


    static void CfgGdiTextWrap(CfgGdiSurface& g, int x, int y, int w, int h, const char* textUtf8, HFONT font, CfgRgb c)
    {
        if (!textUtf8 || !*textUtf8)
            return;
        HFONT oldFont = (HFONT)SelectObject(g.dc, font);
        SetTextColor(g.dc, CfgColorRef(c));
        RECT rc{ x, y, x + w, y + h };
        const std::wstring text = CfgUtf8ToWide(textUtf8);
        DrawTextW(g.dc, text.c_str(), (int)text.size(), &rc, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(g.dc, oldFont);
    }

    static void CfgGdiButton(CfgGdiSurface& g, int x, int y, int w, int h, const char* labelUtf8, bool enabled = true)
    {
        CfgGdiFill(g, x, y, w, h, enabled ? CfgRgb{ 46, 56, 76 } : CfgRgb{ 44, 46, 52 });
        CfgGdiFrame(g, x, y, w, h, enabled ? CfgRgb{ 136, 162, 205 } : CfgRgb{ 90, 94, 104 }, 2);
        CfgGdiText(g, x + 4, y, w - 8, h, labelUtf8, g.boldFont, enabled ? CfgRgb{ 238, 243, 248 } : CfgRgb{ 140, 144, 150 }, DT_CENTER);
    }

    static void CfgConvertGdiToRgba(CfgOverlayState& s, const CfgGdiSurface& g)
    {
        if (s.rgba.size() != (size_t)kCfgOverlayW * kCfgOverlayH * 4)
            s.rgba.resize((size_t)kCfgOverlayW * kCfgOverlayH * 4);

        const uint8_t* src = static_cast<const uint8_t*>(g.bits);
        uint8_t* dst = s.rgba.data();
        const size_t pixels = (size_t)kCfgOverlayW * kCfgOverlayH;
        for (size_t i = 0; i < pixels; ++i)
        {
            dst[i * 4 + 0] = src[i * 4 + 2];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 0];
            dst[i * 4 + 3] = 255;
        }
    }

    static void CfgConvertGdiRectToRgba(std::vector<uint8_t>& out, const CfgGdiSurface& g, int srcX, int srcY, int w, int h)
    {
        if (!g.bits || w <= 0 || h <= 0)
            return;

        out.resize((size_t)w * (size_t)h * 4);
        const uint8_t* src0 = reinterpret_cast<const uint8_t*>(g.bits);
        const int srcStride = kCfgOverlayW * 4;

        for (int y = 0; y < h; ++y)
        {
            const uint8_t* row = src0 + (size_t)(srcY + y) * (size_t)srcStride + (size_t)srcX * 4;
            uint8_t* dst = out.data() + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t b = row[x * 4 + 0];
                const uint8_t gch = row[x * 4 + 1];
                const uint8_t r = row[x * 4 + 2];
                dst[x * 4 + 0] = r;
                dst[x * 4 + 1] = gch;
                dst[x * 4 + 2] = b;
                dst[x * 4 + 3] = 255;
            }
        }
    }

    static void CfgDrawGroupHeader(CfgGdiSurface& g, int y, const char* groupUtf8)
    {
        CfgGdiFill(g, kCfgRowsX, y + 15, 22, 4, { 70, 74, 86 });
        CfgGdiText(g, kCfgRowsX + 30, y, 260, kCfgOverlayGroupH, groupUtf8, g.headerFont, { 206, 210, 220 });
        CfgGdiFill(g, kCfgRowsX + 300, y + 15, kCfgRowsW - 300, 4, { 70, 74, 86 });
    }

    static void CfgDrawCheckbox(CfgGdiSurface& g, int x, int y, bool checked, bool selected)
    {
        const CfgRgb box = selected ? CfgRgb{ 38, 82, 140 } : CfgRgb{ 32, 46, 66 };
        CfgGdiFill(g, x, y, 30, 30, box);
        CfgGdiFrame(g, x, y, 30, 30, { 92, 122, 170 }, 2);
        if (checked)
        {
            CfgGdiFill(g, x + 6, y + 14, 7, 7, { 120, 190, 255 });
            CfgGdiFill(g, x + 12, y + 18, 5, 5, { 120, 190, 255 });
            CfgGdiFill(g, x + 17, y + 8, 7, 16, { 120, 190, 255 });
        }
    }

    static void CfgDrawSlider(CfgGdiSurface& g, int x, int y, int w, int h, float t, bool selected)
    {
        t = (std::clamp)(t, 0.0f, 1.0f);
        CfgGdiFill(g, x, y, w, h, selected ? CfgRgb{ 34, 52, 80 } : CfgRgb{ 26, 34, 48 });
        CfgGdiFrame(g, x, y, w, h, { 68, 86, 116 }, 1);
        const int fillW = (int)std::lround((float)w * t);
        if (fillW > 0)
            CfgGdiFill(g, x, y, fillW, h, { 42, 86, 150 });
        const int knobX = x + (int)std::lround((float)(w - 10) * t);
        CfgGdiFill(g, knobX, y - 4, 10, h + 8, { 80, 150, 240 });
    }

    static void CfgRender(CfgOverlayState& s)
    {
        CfgGdiSurface g;
        if (!CfgCreateGdiSurface(g))
        {
            s.status = s.useChinese ? "GDI \346\270\262\346\237\223\345\210\235\345\247\213\345\214\226\345\244\261\350\264\245\343\200\202" : "GDI render initialization failed.";
            return;
        }

        CfgGdiFill(g, 0, 0, kCfgOverlayW, kCfgOverlayH, { 14, 16, 22 });
        CfgGdiFill(g, 0, 0, kCfgOverlayW, 82, { 30, 35, 48 });
        CfgGdiText(g, 26, 14, 520, 54, s.useChinese ? "L4D2VR \351\205\215\347\275\256" : "L4D2VR Config", g.titleFont, { 242, 246, 255 });
        CfgGdiButton(g, kCfgPagePrevX, kCfgTopButtonY, kCfgPageButtonW, kCfgPageButtonH, s.useChinese ? "\344\270\212\344\270\200\351\241\265" : "Prev");
        CfgGdiButton(g, kCfgPageNextX, kCfgTopButtonY, kCfgPageButtonW, kCfgPageButtonH, s.useChinese ? "\344\270\213\344\270\200\351\241\265" : "Next");
        CfgGdiButton(g, kCfgLangX, kCfgTopButtonY, kCfgLangW, kCfgTopButtonH, s.useChinese ? "\344\270\255\346\226\207" : "EN");
        CfgGdiButton(g, kCfgReloadX, kCfgTopButtonY, kCfgReloadW, kCfgTopButtonH, s.useChinese ? "\351\207\215\350\275\275" : "Reload");
        CfgGdiButton(g, kCfgSaveX, kCfgTopButtonY, kCfgSaveW, kCfgTopButtonH, s.useChinese ? "\344\277\235\345\255\230" : "Save");
        CfgGdiButton(g, kCfgCloseX, kCfgTopButtonY, kCfgCloseW, kCfgTopButtonH, s.useChinese ? "\345\205\263\351\227\255" : "Close");

        CfgGdiText(g, 26, 84, 1228, 24, std::string(s.useChinese ? "\351\205\215\347\275\256\346\226\207\344\273\266\357\274\232" : "Config: ") + s.configPath, g.smallFont, { 150, 162, 180 });
        CfgGdiFill(g, 26, 112, 560, 36, { 24, 36, 56 });
        CfgGdiFrame(g, 26, 112, 560, 36, { 68, 86, 116 }, 1);
        CfgGdiText(g, 40, 112, 520, 36, s.useChinese ? "Text rows: Edit opens VR keyboard. Vec3/Color: select component then -/+." : "Text rows: Edit opens VR keyboard. Vec3/Color: select component then -/+.", g.normalFont, { 112, 125, 145 });

        const int total = (int)s.visibleSpecIndexes.size();
        if (s.selected < s.scroll)
            s.scroll = s.selected;
        if (s.selected >= s.scroll + kCfgOverlayRowsVisible)
            s.scroll = s.selected - kCfgOverlayRowsVisible + 1;
        s.scroll = (std::clamp)(s.scroll, 0, (std::max)(0, total - 1));

        int y = kCfgRowsY;
        int lastDrawnItem = s.scroll;
        for (int item = s.scroll; item < total && y < kCfgRowsBottom; ++item)
        {
            const CfgOptionSpec& spec = kCfgOptionSpecs[s.visibleSpecIndexes[item]];
            const char* prevGroup = nullptr;
            if (item > s.scroll)
                prevGroup = CfgGroupText(s, kCfgOptionSpecs[s.visibleSpecIndexes[item - 1]]);
            const char* curGroup = CfgGroupText(s, spec);
            const bool groupChanged = (item == s.scroll) || !prevGroup || std::strcmp(prevGroup, curGroup) != 0;
            if (groupChanged)
            {
                if (y + kCfgOverlayGroupH > kCfgRowsBottom)
                    break;
                CfgDrawGroupHeader(g, y, curGroup);
                y += kCfgOverlayGroupH;
            }

            if (y + kCfgOverlayRowH > kCfgRowsBottom)
                break;

            const std::string value = CfgValueFor(s, spec);
            const bool selected = item == s.selected;
            const int rowY = y;
            CfgGdiFill(g, kCfgRowsX, rowY, kCfgRowsW, kCfgOverlayRowH - 3, selected ? CfgRgb{ 31, 52, 84 } : CfgRgb{ 18, 21, 28 });
            if (selected)
                CfgGdiFrame(g, kCfgRowsX, rowY, kCfgRowsW, kCfgOverlayRowH - 3, { 76, 130, 220 }, 2);

            const int titleX = 70;
            if (spec.type == CfgOptionType::Bool)
            {
                const bool checked = CfgBoolValue(value);
                CfgDrawCheckbox(g, kCfgRowsX + 12, rowY + 7, checked, selected);
                CfgGdiText(g, titleX, rowY + 3, 420, 26, CfgTitleText(s, spec), selected ? g.boldFont : g.normalFont, { 232, 236, 244 });
                if (selected)
                    CfgGdiText(g, titleX, rowY + 25, 420, 16, spec.key, g.smallFont, { 132, 146, 166 });
                CfgGdiText(g, kCfgValueX, rowY + 4, 150, 34, checked ? (s.useChinese ? "\345\274\200\345\220\257" : "On") : (s.useChinese ? "\345\205\263\351\227\255" : "Off"), g.normalFont, checked ? CfgRgb{ 160, 230, 180 } : CfgRgb{ 145, 150, 158 }, DT_RIGHT);
            }
            else if (spec.type == CfgOptionType::Float || spec.type == CfgOptionType::Int)
            {
                CfgGdiText(g, 42, rowY + 3, 420, 26, CfgTitleText(s, spec), selected ? g.boldFont : g.normalFont, { 232, 236, 244 });
                if (selected)
                    CfgGdiText(g, 42, rowY + 25, 420, 16, spec.key, g.smallFont, { 132, 146, 166 });

                float f = 0.0f;
                if (!CfgTryFloat(value, f))
                    CfgTryFloat(spec.defaultValue ? spec.defaultValue : "0", f);
                const float denom = (spec.maxValue > spec.minValue) ? (spec.maxValue - spec.minValue) : 1.0f;
                const float t = (f - spec.minValue) / denom;
                CfgDrawSlider(g, kCfgSliderX, rowY + 10, kCfgSliderW, kCfgSliderH, t, selected);
                CfgGdiText(g, kCfgSliderX, rowY + 7, kCfgSliderW, 32, value, g.normalFont, { 226, 232, 242 }, DT_CENTER);
            }
            else
            {
                CfgGdiText(g, 42, rowY + 3, 420, 26, CfgTitleText(s, spec), selected ? g.boldFont : g.normalFont, { 232, 236, 244 });
                if (selected)
                    CfgGdiText(g, 42, rowY + 25, 420, 16, spec.key, g.smallFont, { 132, 146, 166 });

                if (CfgIsComponentEditable(spec))
                {
                    const int count = CfgComponentCount(spec);
                    const int activeIndex = selected ? CfgSelectedComponentIndex(s, spec) : (s.componentEditKey == spec.key ? (std::clamp)(s.componentEditIndex, 0, (std::max)(0, count - 1)) : 0);
                    for (int i = 0; i < count; ++i)
                    {
                        const int bx = kCfgComponentX + i * (kCfgComponentButtonW + kCfgComponentGap);
                        const bool active = i == activeIndex;
                        CfgGdiFill(g, bx, rowY + 7, kCfgComponentButtonW, kCfgComponentButtonH, active ? CfgRgb{ 42, 86, 150 } : CfgRgb{ 46, 56, 76 });
                        CfgGdiFrame(g, bx, rowY + 7, kCfgComponentButtonW, kCfgComponentButtonH, active ? CfgRgb{ 150, 190, 255 } : CfgRgb{ 136, 162, 205 }, 2);
                        CfgGdiText(g, bx + 4, rowY + 7, kCfgComponentButtonW - 8, kCfgComponentButtonH, CfgComponentLabel(spec, i), g.boldFont, { 238, 243, 248 }, DT_CENTER);
                    }

                    CfgGdiText(g, kCfgComponentValueX, rowY + 4, 300, 34, value, g.normalFont, { 185, 210, 190 });
                    CfgGdiButton(g, kCfgMinusX, rowY + 7, kCfgAdjustButtonW, 30, "-");
                    CfgGdiButton(g, kCfgPlusX, rowY + 7, kCfgAdjustButtonW, 30, "+");
                }
                else if (CfgIsKeyboardEditable(spec))
                {
                    const bool editing = s.keyboardActive && s.keyboardEditKey == spec.key;
                    CfgGdiText(g, kCfgSliderX, rowY + 4, 590, 34, value, g.normalFont, editing ? CfgRgb{ 255, 230, 150 } : CfgRgb{ 185, 210, 190 });
                    CfgGdiButton(g, kCfgStringEditX, rowY + 7, kCfgStringEditW, 30, editing ? (s.useChinese ? "Editing" : "Editing") : (s.useChinese ? "Edit" : "Edit"));
                }
                else
                {
                    CfgGdiText(g, kCfgSliderX, rowY + 4, 590, 34, value, g.normalFont, { 185, 210, 190 });
                    CfgGdiButton(g, 1100, rowY + 7, 120, 30, s.useChinese ? "Read-only" : "Read-only", false);
                }
            }

            y += kCfgOverlayRowH;
            lastDrawnItem = item;
        }

        const int helpY = kCfgOverlayH - 146;
        CfgGdiFill(g, 0, helpY, kCfgOverlayW, 146, { 30, 35, 48 });
        CfgGdiFill(g, 26, helpY + 12, 1228, 1, { 68, 78, 100 });

        if (total > 0 && s.selected >= 0 && s.selected < total)
        {
            const CfgOptionSpec& selSpec = kCfgOptionSpecs[s.visibleSpecIndexes[s.selected]];
            std::string header = std::string(CfgTitleText(s, selSpec)) + "  [" + selSpec.key + "]";
            CfgGdiText(g, 26, helpY + 18, 760, 26, header, g.boldFont, { 234, 238, 246 });
            const char* desc = CfgDescText(s, selSpec);
            const char* tip = CfgTipText(s, selSpec);
            CfgGdiTextWrap(g, 26, helpY + 46, 820, 42, desc, g.smallFont, { 194, 204, 222 });
            if (tip && *tip)
            {
                CfgGdiText(g, 870, helpY + 18, 90, 24, s.useChinese ? "\345\273\272\350\256\256" : "Tip", g.boldFont, { 160, 196, 255 });
                CfgGdiTextWrap(g, 870, helpY + 46, 300, 42, tip, g.smallFont, { 180, 214, 188 });
            }
        }

        CfgGdiText(g, 26, kCfgOverlayH - 43, 860, 34, s.status, g.smallFont, { 226, 232, 242 });
        const std::string counter = total > 0 ? (std::to_string(s.selected + 1) + "/" + std::to_string(total)) : "0/0";
        CfgGdiText(g, 1010, kCfgOverlayH - 43, 240, 34, counter, g.normalFont, { 164, 180, 205 }, DT_RIGHT);

        (void)lastDrawnItem;
        CfgConvertGdiToRgba(s, g);
        s.dirty = false;
        s.needsUpload = true;
    }

    static bool CfgHasLocalPlayerForMenuButton()
    {
        if (!g_Game || !g_Game->m_EngineClient || !g_Game->m_ClientEntityList)
            return false;

#ifdef _MSC_VER
        __try
        {
            const int playerIndex = g_Game->m_EngineClient->GetLocalPlayer();
            if (playerIndex <= 0)
                return false;
            return g_Game->GetClientEntity(playerIndex) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        const int playerIndex = g_Game->m_EngineClient->GetLocalPlayer();
        if (playerIndex <= 0)
            return false;
        return g_Game->GetClientEntity(playerIndex) != nullptr;
#endif
    }

    static bool CfgIsPauseMenuActive()
    {
        static bool s_seenStableGameplayFrame = false;

        if (!g_Game || !g_Game->m_EngineClient)
        {
            s_seenStableGameplayFrame = false;
            return false;
        }

#ifdef _MSC_VER
        __try
        {
            const bool inGame = g_Game->m_EngineClient->IsInGame();
            if (!inGame)
            {
                s_seenStableGameplayFrame = false;
                return false;
            }

            const bool hasLocalPlayer = CfgHasLocalPlayerForMenuButton();
            if (!hasLocalPlayer)
            {
                s_seenStableGameplayFrame = false;
                return false;
            }

            const bool cursorVisible = g_Game->m_VguiSurface && g_Game->m_VguiSurface->IsCursorVisible();
            if (!cursorVisible)
            {
                s_seenStableGameplayFrame = true;
                return false;
            }

            if (!s_seenStableGameplayFrame)
                return false;

            const bool paused = g_Game->m_EngineClient->IsPaused();
            return paused || cursorVisible;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_seenStableGameplayFrame = false;
            return false;
        }
#else
        const bool inGame = g_Game->m_EngineClient->IsInGame();
        if (!inGame)
        {
            s_seenStableGameplayFrame = false;
            return false;
        }

        const bool hasLocalPlayer = CfgHasLocalPlayerForMenuButton();
        if (!hasLocalPlayer)
        {
            s_seenStableGameplayFrame = false;
            return false;
        }

        const bool cursorVisible = g_Game->m_VguiSurface && g_Game->m_VguiSurface->IsCursorVisible();
        if (!cursorVisible)
        {
            s_seenStableGameplayFrame = true;
            return false;
        }

        if (!s_seenStableGameplayFrame)
            return false;

        const bool paused = g_Game->m_EngineClient->IsPaused();
        return paused || cursorVisible;
#endif
    }

    static bool CfgIsValidOverlayHandle(vr::VROverlayHandle_t h)
    {
        return h != vr::k_ulOverlayHandleInvalid;
    }

    static void CfgConfigurePanelOverlay(CfgOverlayState& s, vr::IVROverlay* ov, vr::VROverlayHandle_t h, bool active)
    {
        (void)s;
        if (!ov || !CfgIsValidOverlayHandle(h))
            return;

        ov->SetOverlayAlpha(h, 1.0f);
        ov->SetOverlaySortOrder(h, active ? kCfgOverlaySortOrderActive : kCfgOverlaySortOrderBase);
        ov->SetOverlayInputMethod(h, active ? vr::VROverlayInputMethod_Mouse : vr::VROverlayInputMethod_None);
        ov->SetOverlayFlag(h, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        ov->SetOverlayFlag(h, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        vr::HmdVector2_t mouseScale{ (float)kCfgOverlayW, (float)kCfgOverlayH };
        ov->SetOverlayMouseScale(h, &mouseScale);
    }

    static void CfgHidePanelOverlays(CfgOverlayState& s, vr::IVROverlay* ov)
    {
        if (!ov)
            ov = vr::VROverlay();
        if (!ov)
            return;

        if (CfgIsValidOverlayHandle(s.handle))
            ov->HideOverlay(s.handle);
        if (CfgIsValidOverlayHandle(s.backHandle))
            ov->HideOverlay(s.backHandle);
        s.panelOverlayShown = false;
    }

    static void CfgSetBaseOverlaysBlocked(CfgOverlayState& s, bool blocked, vr::IVROverlay* ov = nullptr)
    {
        if (!g_Game || !g_Game->m_VR)
            return;
        if (!ov)
            ov = vr::VROverlay();
        if (!ov)
            return;

        auto setInput = [&](vr::VROverlayHandle_t h, vr::VROverlayInputMethod method)
            {
                if (h != vr::k_ulOverlayHandleInvalid)
                    ov->SetOverlayInputMethod(h, method);
            };

        auto suppress = [&](vr::VROverlayHandle_t h)
            {
                if (h == vr::k_ulOverlayHandleInvalid)
                    return;
                ov->SetOverlayInputMethod(h, vr::VROverlayInputMethod_None);
                ov->SetOverlaySortOrder(h, 1u);
                ov->HideOverlay(h);
            };

        if (blocked)
        {
            // Keep the config panel modal. The normal L4D2VR HUD overlays are interactive mouse
            // overlays too, so they can visually cover the laser cursor and steal the hit test.
            suppress(g_Game->m_VR->m_MainMenuHandle);
            suppress(g_Game->m_VR->m_HUDTopHandle);
            for (vr::VROverlayHandle_t h : g_Game->m_VR->m_HUDBottomHandles)
                suppress(h);
            s.baseOverlaysInputBlocked = true;
        }
        else if (s.baseOverlaysInputBlocked)
        {
            setInput(g_Game->m_VR->m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
            setInput(g_Game->m_VR->m_HUDTopHandle, vr::VROverlayInputMethod_Mouse);
            for (vr::VROverlayHandle_t h : g_Game->m_VR->m_HUDBottomHandles)
                setInput(h, vr::VROverlayInputMethod_Mouse);
            s.baseOverlaysInputBlocked = false;
        }
    }

    static void CfgSetYawOnlyTransform(vr::HmdMatrix34_t& out, const Vector& position, float yawRadians)
    {
        const float c = std::cos(yawRadians);
        const float sn = std::sin(yawRadians);

        out = {};
        out.m[0][0] = c;
        out.m[0][1] = 0.0f;
        out.m[0][2] = sn;
        out.m[0][3] = position.x;
        out.m[1][0] = 0.0f;
        out.m[1][1] = 1.0f;
        out.m[1][2] = 0.0f;
        out.m[1][3] = position.y;
        out.m[2][0] = -sn;
        out.m[2][1] = 0.0f;
        out.m[2][2] = c;
        out.m[2][3] = position.z;
    }

    static void CfgSetHmdFollowTransform(vr::HmdMatrix34_t& out, const Vector& position, const vr::HmdMatrix34_t& hmdAbs)
    {
        out = {};
        out.m[0][0] = hmdAbs.m[0][0];
        out.m[0][1] = hmdAbs.m[0][1];
        out.m[0][2] = hmdAbs.m[0][2];
        out.m[0][3] = position.x;
        out.m[1][0] = hmdAbs.m[1][0];
        out.m[1][1] = hmdAbs.m[1][1];
        out.m[1][2] = hmdAbs.m[1][2];
        out.m[1][3] = position.y;
        out.m[2][0] = hmdAbs.m[2][0];
        out.m[2][1] = hmdAbs.m[2][1];
        out.m[2][2] = hmdAbs.m[2][2];
        out.m[2][3] = position.z;
    }

    static bool CfgNormalizeVector(Vector& v)
    {
        const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (!(len > 0.0001f) || !std::isfinite(len))
            return false;

        v.x /= len;
        v.y /= len;
        v.z /= len;
        return true;
    }

    static bool CfgBuildHmdFacingTransform(float distanceMeters, float xOffsetMeters, float yOffsetMeters, bool followHmdMovement,
        vr::ETrackingUniverseOrigin& origin, vr::HmdMatrix34_t& out)
    {
        origin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t hmdAbs{};
        if (!CfgGetCurrentHmdAbsolutePose(origin, hmdAbs))
            return false;

        Vector hmdPosition = { hmdAbs.m[0][3], hmdAbs.m[1][3], hmdAbs.m[2][3] };
        Vector hmdForwardYaw = { -hmdAbs.m[0][2], 0.0f, -hmdAbs.m[2][2] };
        if (!CfgNormalizeVector(hmdForwardYaw))
            hmdForwardYaw = { 0.0f, 0.0f, -1.0f };

        Vector hmdForwardFull = { -hmdAbs.m[0][2], -hmdAbs.m[1][2], -hmdAbs.m[2][2] };
        Vector hmdRightFull = { hmdAbs.m[0][0], hmdAbs.m[1][0], hmdAbs.m[2][0] };
        Vector hmdUpFull = { hmdAbs.m[0][1], hmdAbs.m[1][1], hmdAbs.m[2][1] };

        const float yawRadians = std::atan2(hmdAbs.m[0][2], hmdAbs.m[2][2]);
        const float c = std::cos(yawRadians);
        const float sn = std::sin(yawRadians);
        Vector forward = followHmdMovement ? hmdForwardFull : hmdForwardYaw;
        Vector right = followHmdMovement ? hmdRightFull : Vector(c, 0.0f, -sn);
        Vector up = followHmdMovement ? hmdUpFull : Vector(0.0f, 1.0f, 0.0f);

        if (!CfgNormalizeVector(forward))
            forward = hmdForwardYaw;
        CfgNormalizeVector(right);
        CfgNormalizeVector(up);

        Vector position = hmdPosition + forward * distanceMeters;
        position += right * xOffsetMeters;
        position += up * yOffsetMeters;

        if (followHmdMovement)
            CfgSetHmdFollowTransform(out, position, hmdAbs);
        else
            CfgSetYawOnlyTransform(out, position, yawRadians);
        return true;
    }

    static bool CfgBuildYawOnlyHmdFacingTransform(float distanceMeters, float yOffsetMeters,
        vr::ETrackingUniverseOrigin& origin, vr::HmdMatrix34_t& out)
    {
        return CfgBuildHmdFacingTransform(distanceMeters, 0.0f, yOffsetMeters, false, origin, out);
    }

    static bool CfgBuildCurrentPauseHudTransform(vr::ETrackingUniverseOrigin& origin, vr::HmdMatrix34_t& out,
        float& hudWidthMeters, float& hudHeightMeters)
    {
        if (!g_Game || !g_Game->m_VR)
            return false;

        VR* vrState = g_Game->m_VR;
        const float distanceMeters = vrState->m_HudDistance + vrState->m_FixedHudDistanceOffset;
        const float xOffsetMeters = vrState->m_FixedHudXOffset;
        const float yOffsetMeters = -0.25f + vrState->m_FixedHudYOffset;
        if (!CfgBuildHmdFacingTransform(distanceMeters, xOffsetMeters, yOffsetMeters, vrState->m_HudFollowHmdMovement, origin, out))
            return false;

        int windowWidth = 0;
        int windowHeight = 0;
        if (g_Game->m_MaterialSystem && g_Game->m_MaterialSystem->GetRenderContext())
            g_Game->m_MaterialSystem->GetRenderContext()->GetWindowSize(windowWidth, windowHeight);

        hudWidthMeters = (std::max)(0.1f, vrState->m_HudSize);
        const float aspectHeightOverWidth =
            (windowWidth > 0 && windowHeight > 0)
            ? ((float)windowHeight / (float)windowWidth)
            : (9.0f / 16.0f);
        hudHeightMeters = hudWidthMeters * (std::clamp)(aspectHeightOverWidth, 0.25f, 1.5f);
        return true;
    }

    static void CfgSetMenuButtonTransform(vr::IVROverlay* ov, vr::VROverlayHandle_t h)
    {
        if (!ov || !CfgIsValidOverlayHandle(h))
            return;

        vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t hudTransform{};
        float hudWidthMeters = 1.3f;
        float hudHeightMeters = 0.73f;

        if (CfgBuildCurrentPauseHudTransform(origin, hudTransform, hudWidthMeters, hudHeightMeters))
        {
            vr::HmdMatrix34_t rel{};
            rel.m[0][0] = 1.0f;
            rel.m[1][1] = 1.0f;
            rel.m[2][2] = 1.0f;

            // Attach to the pause HUD plane, upper-right, and move a little toward the player.
            // This prevents the pause HUD overlay from winning the SteamVR ray hit test.
            rel.m[0][3] = hudWidthMeters * 0.5f - 0.22f;
            rel.m[1][3] = hudHeightMeters * 0.5f - 0.09f;
            rel.m[2][3] = -0.05f;

            vr::HmdMatrix34_t mat = CfgMul34(hudTransform, rel);
            ov->SetOverlayTransformAbsolute(h, origin, &mat);
            return;
        }

        vr::HmdMatrix34_t fallback{};
        if (CfgBuildYawOnlyHmdFacingTransform(1.20f, 0.42f, origin, fallback))
        {
            ov->SetOverlayTransformAbsolute(h, origin, &fallback);
            return;
        }

        vr::HmdMatrix34_t mat{};
        mat.m[0][0] = 1.0f;
        mat.m[1][1] = 1.0f;
        mat.m[2][2] = 1.0f;
        mat.m[0][3] = 0.78f;
        mat.m[1][3] = 1.42f;
        mat.m[2][3] = -1.20f;
        ov->SetOverlayTransformAbsolute(h, origin, &mat);
    }

    static void CfgRenderMenuButton(CfgOverlayState& s)
    {
        const bool menuChinese = CfgSystemPrefersChinese();

        CfgGdiSurface g;
        if (!CfgCreateGdiSurface(g))
            return;

        CfgGdiFill(g, 0, 0, kCfgMenuButtonW, kCfgMenuButtonH, { 30, 35, 48 });
        CfgGdiFrame(g, 0, 0, kCfgMenuButtonW, kCfgMenuButtonH, { 136, 162, 205 }, 3);
        CfgGdiText(g, 14, 6, kCfgMenuButtonW - 28, 40,
            menuChinese ? "VR \351\205\215\347\275\256" : "VR Config",
            g.headerFont, { 238, 243, 248 }, DT_CENTER);
        CfgGdiText(g, 14, 45, kCfgMenuButtonW - 28, 30,
            menuChinese ? "\346\211\223\345\274\200 L4D2VR \351\235\242\346\235\277" : "Open L4D2VR panel",
            g.smallFont, { 180, 198, 226 }, DT_CENTER);

        CfgConvertGdiRectToRgba(s.menuButtonRgba, g, 0, 0, kCfgMenuButtonW, kCfgMenuButtonH);
        s.menuButtonNeedsUpload = true;
        s.menuButtonRenderedChinese = menuChinese;
    }

    static bool CfgEnsureMenuButtonOverlay(CfgOverlayState& s)
    {
        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return false;

        if (s.menuButtonHandle == vr::k_ulOverlayHandleInvalid)
        {
            vr::EVROverlayError err = ov->CreateOverlay("l4d2vr.config.menu_button", "L4D2VR Config Button", &s.menuButtonHandle);
            if (err != vr::VROverlayError_None || s.menuButtonHandle == vr::k_ulOverlayHandleInvalid)
                return false;

            ov->SetOverlayWidthInMeters(s.menuButtonHandle, 0.34f);
            ov->SetOverlayAlpha(s.menuButtonHandle, 0.6f);
            ov->SetOverlaySortOrder(s.menuButtonHandle, kCfgMenuButtonSortOrder);
            ov->SetOverlayInputMethod(s.menuButtonHandle, vr::VROverlayInputMethod_Mouse);
            ov->SetOverlayFlag(s.menuButtonHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
            vr::HmdVector2_t mouseScale{ (float)kCfgMenuButtonW, (float)kCfgMenuButtonH };
            ov->SetOverlayMouseScale(s.menuButtonHandle, &mouseScale);
            CfgSetMenuButtonTransform(ov, s.menuButtonHandle);
            s.menuButtonNeedsUpload = true;
        }

        if (s.menuButtonRgba.empty() || s.menuButtonRenderedChinese != CfgSystemPrefersChinese())
            CfgRenderMenuButton(s);

        return true;
    }

    static void CfgSetMenuButtonInputGuard(CfgOverlayState& s, bool blocked, vr::IVROverlay* ov = nullptr)
    {
        if (!g_Game || !g_Game->m_VR || s.baseOverlaysInputBlocked)
            return;
        if (!ov)
            ov = vr::VROverlay();
        if (!ov)
            return;

        auto setInput = [&](vr::VROverlayHandle_t h, vr::VROverlayInputMethod method)
            {
                if (CfgIsValidOverlayHandle(h))
                    ov->SetOverlayInputMethod(h, method);
            };

        auto setInteractive = [&](vr::VROverlayHandle_t h, bool enabled)
            {
                if (CfgIsValidOverlayHandle(h))
                    ov->SetOverlayFlag(h, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, enabled);
            };

        if (blocked)
        {
            setInput(g_Game->m_VR->m_MainMenuHandle, vr::VROverlayInputMethod_None);
            setInput(g_Game->m_VR->m_HUDTopHandle, vr::VROverlayInputMethod_None);
            setInteractive(g_Game->m_VR->m_MainMenuHandle, false);
            setInteractive(g_Game->m_VR->m_HUDTopHandle, false);
            for (vr::VROverlayHandle_t h : g_Game->m_VR->m_HUDBottomHandles)
            {
                setInput(h, vr::VROverlayInputMethod_None);
                setInteractive(h, false);
            }
            s.menuButtonBaseInputBlocked = true;
        }
        else if (s.menuButtonBaseInputBlocked)
        {
            setInput(g_Game->m_VR->m_MainMenuHandle, vr::VROverlayInputMethod_Mouse);
            setInput(g_Game->m_VR->m_HUDTopHandle, vr::VROverlayInputMethod_Mouse);
            for (vr::VROverlayHandle_t h : g_Game->m_VR->m_HUDBottomHandles)
                setInput(h, vr::VROverlayInputMethod_Mouse);
            s.menuButtonBaseInputBlocked = false;
        }
    }

    static void CfgHideMenuButton(CfgOverlayState& s)
    {
        if (vr::IVROverlay* ov = vr::VROverlay())
        {
            CfgSetMenuButtonInputGuard(s, false, ov);
            if (s.menuButtonHandle != vr::k_ulOverlayHandleInvalid)
                ov->HideOverlay(s.menuButtonHandle);
        }
        s.menuButtonHovered = false;
        s.menuButtonManualSelectPrev = false;
    }

    static bool CfgAnyControllerIntersectsOverlay(vr::VROverlayHandle_t h)
    {
        if (!g_Game || !g_Game->m_VR || !CfgIsValidOverlayHandle(h))
            return false;

        return g_Game->m_VR->CheckOverlayIntersectionForController(h, vr::TrackedControllerRole_LeftHand) ||
            g_Game->m_VR->CheckOverlayIntersectionForController(h, vr::TrackedControllerRole_RightHand);
    }

    static bool CfgReadDigitalActionHeld(vr::VRActionHandle_t& actionHandle)
    {
        if (!g_Game || !g_Game->m_VR || actionHandle == vr::k_ulInvalidActionHandle)
            return false;

        vr::InputDigitalActionData_t data{};
        if (!g_Game->m_VR->GetDigitalActionData(actionHandle, data))
            return false;
        return data.bState;
    }

    static bool CfgMenuButtonManualOpenPressed(CfgOverlayState& s)
    {
        if (!g_Game || !g_Game->m_VR)
            return false;

        VR* vrState = g_Game->m_VR;
        const bool held =
            CfgReadDigitalActionHeld(vrState->m_MenuSelect) ||
            CfgReadDigitalActionHeld(vrState->m_ActionPrimaryAttack) ||
            CfgReadDigitalActionHeld(vrState->m_ActionUse);
        const bool pressed = held && !s.menuButtonManualSelectPrev;
        s.menuButtonManualSelectPrev = held;
        return pressed;
    }

    static void CfgOpenPanelFromMenuButton(CfgOverlayState& s, vr::IVROverlay* ov)
    {
        if (!ov)
            ov = vr::VROverlay();

        s.visible = true;
        CfgLoad(s);
        CfgInvalidateFixedPlacement(s);
        CfgApplyOverlayPlacement(s, ov);
        s.status = s.useChinese
            ? "\345\267\262\346\211\223\345\274\200\343\200\202\347\224\250 VR \346\216\247\345\210\266\345\231\250\345\260\204\347\272\277\347\202\271\345\207\273\346\214\211\351\222\256\343\200\202"
            : "Opened. Point and click with the VR controller laser.";
        s.dirty = true;
        CfgSetMenuButtonInputGuard(s, false, ov);
        if (ov && CfgIsValidOverlayHandle(s.menuButtonHandle))
            ov->HideOverlay(s.menuButtonHandle);
        s.menuButtonHovered = false;
        s.menuButtonManualSelectPrev = false;
    }

    static void CfgPollMenuButtonEvents(CfgOverlayState& s)
    {
        if (s.menuButtonHandle == vr::k_ulOverlayHandleInvalid)
            return;

        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return;

        vr::VREvent_t ev{};
        while (ov->PollNextOverlayEvent(s.menuButtonHandle, &ev, sizeof(ev)))
        {
            if (ev.eventType == vr::VREvent_MouseButtonDown)
            {
                CfgOpenPanelFromMenuButton(s, ov);
                break;
            }
        }

        if (!s.visible && s.menuButtonHovered && CfgMenuButtonManualOpenPressed(s))
            CfgOpenPanelFromMenuButton(s, ov);
    }

    static void CfgUpdateMenuButton(CfgOverlayState& s)
    {
        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return;

        const bool shouldShowButton = !s.visible && CfgIsPauseMenuActive();
        if (!shouldShowButton)
        {
            CfgHideMenuButton(s);
            return;
        }

        if (!CfgEnsureMenuButtonOverlay(s))
            return;

        CfgSetMenuButtonTransform(ov, s.menuButtonHandle);
        ov->SetOverlayAlpha(s.menuButtonHandle, 1.0f);
        ov->SetOverlaySortOrder(s.menuButtonHandle, kCfgMenuButtonSortOrder);
        ov->SetOverlayInputMethod(s.menuButtonHandle, vr::VROverlayInputMethod_Mouse);
        ov->SetOverlayFlag(s.menuButtonHandle, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);

        if (s.menuButtonNeedsUpload && !s.menuButtonRgba.empty())
        {
            vr::EVROverlayError err = ov->SetOverlayRaw(
                s.menuButtonHandle,
                s.menuButtonRgba.data(),
                kCfgMenuButtonW,
                kCfgMenuButtonH,
                4);
            if (err == vr::VROverlayError_None)
                s.menuButtonNeedsUpload = false;
        }

        ov->ShowOverlay(s.menuButtonHandle);

        // Do not let the pause HUD steal the ray when the controller is aimed at this button.
        s.menuButtonHovered = CfgAnyControllerIntersectsOverlay(s.menuButtonHandle);
        CfgSetMenuButtonInputGuard(s, s.menuButtonHovered, ov);

        CfgPollMenuButtonEvents(s);
    }

    static float CfgOverlayDistanceMeters(const CfgOverlayState& s)
    {
        float fallback = 1.35f;
        if (g_Game && g_Game->m_VR)
            fallback = g_Game->m_VR->m_ConfigOverlayDistanceMeters;
        return (std::clamp)(CfgFloatValue(s, "ConfigOverlayDistanceMeters", fallback), 0.6f, 3.0f);
    }

    static float CfgOverlaySizeMeters(const CfgOverlayState& s)
    {
        float fallback = 2.05f;
        if (g_Game && g_Game->m_VR)
            fallback = g_Game->m_VR->m_ConfigOverlaySizeMeters;
        return (std::clamp)(CfgFloatValue(s, "ConfigOverlaySizeMeters", fallback), 0.8f, 4.0f);
    }

    static void CfgInvalidateFixedPlacement(CfgOverlayState& s)
    {
        s.fixedPlacementValid = false;
        s.fixedPlacementApplied = false;
        s.appliedOverlayDistanceMeters = -1.0f;
    }

    static vr::HmdMatrix34_t CfgMul34(const vr::HmdMatrix34_t& a, const vr::HmdMatrix34_t& b)
    {
        vr::HmdMatrix34_t out{};
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
                out.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c];
            out.m[r][3] = a.m[r][0] * b.m[0][3] + a.m[r][1] * b.m[1][3] + a.m[r][2] * b.m[2][3] + a.m[r][3];
        }
        return out;
    }

    static vr::HmdMatrix34_t CfgPanelRelativeToHmd(float distanceMeters)
    {
        vr::HmdMatrix34_t mat{};
        mat.m[0][0] = 1.0f;
        mat.m[1][1] = 1.0f;
        mat.m[2][2] = 1.0f;
        mat.m[0][3] = 0.0f;
        mat.m[1][3] = -0.08f;
        mat.m[2][3] = -distanceMeters;
        return mat;
    }

    static bool CfgGetCurrentHmdAbsolutePose(vr::ETrackingUniverseOrigin& origin, vr::HmdMatrix34_t& hmdAbs)
    {
        origin = vr::TrackingUniverseStanding;
        if (vr::IVRCompositor* comp = vr::VRCompositor())
            origin = comp->GetTrackingSpace();
        else if (g_Game && g_Game->m_VR && g_Game->m_VR->m_Compositor)
            origin = g_Game->m_VR->m_Compositor->GetTrackingSpace();

        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
        if (vr::IVRSystem* sys = vr::VRSystem())
        {
            sys->GetDeviceToAbsoluteTrackingPose(origin, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);
            const vr::TrackedDevicePose_t& hmd = poses[vr::k_unTrackedDeviceIndex_Hmd];
            if (hmd.bPoseIsValid)
            {
                hmdAbs = hmd.mDeviceToAbsoluteTracking;
                return true;
            }
        }

        if (g_Game && g_Game->m_VR)
        {
            const vr::TrackedDevicePose_t& hmd = g_Game->m_VR->m_Poses[vr::k_unTrackedDeviceIndex_Hmd];
            if (hmd.bPoseIsValid)
            {
                hmdAbs = hmd.mDeviceToAbsoluteTracking;
                return true;
            }
        }

        return false;
    }

    static void CfgBuildFixedPlacement(CfgOverlayState& s, float distanceMeters)
    {
        vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
        vr::HmdMatrix34_t transform{};
        if (CfgBuildYawOnlyHmdFacingTransform(distanceMeters, -0.08f, origin, transform))
        {
            s.fixedPlacementOrigin = origin;
            s.fixedPlacementTransform = transform;
        }
        else
        {
            // Safe fallback: fixed in standing space instead of HMD-relative. Keep it upright.
            s.fixedPlacementOrigin = origin;
            s.fixedPlacementTransform = CfgPanelRelativeToHmd(distanceMeters);
            s.fixedPlacementTransform.m[1][3] = 1.25f;
        }

        s.fixedPlacementValid = true;
        s.fixedPlacementApplied = false;
        s.appliedOverlayDistanceMeters = distanceMeters;
    }

    static void CfgApplyOverlayPlacement(CfgOverlayState& s, vr::IVROverlay* ov)
    {
        if (!CfgIsValidOverlayHandle(s.handle) && !CfgIsValidOverlayHandle(s.backHandle))
            return;

        if (!ov)
            ov = vr::VROverlay();
        if (!ov)
            return;

        const float distanceMeters = CfgOverlayDistanceMeters(s);
        const float sizeMeters = CfgOverlaySizeMeters(s);

        if (g_Game && g_Game->m_VR)
        {
            g_Game->m_VR->m_ConfigOverlayDistanceMeters = distanceMeters;
            g_Game->m_VR->m_ConfigOverlaySizeMeters = sizeMeters;
        }

        if (std::fabs(s.appliedOverlaySizeMeters - sizeMeters) > 0.0001f)
        {
            if (CfgIsValidOverlayHandle(s.handle))
                ov->SetOverlayWidthInMeters(s.handle, sizeMeters);
            if (CfgIsValidOverlayHandle(s.backHandle))
                ov->SetOverlayWidthInMeters(s.backHandle, sizeMeters);
            s.appliedOverlaySizeMeters = sizeMeters;
        }

        if (!s.fixedPlacementValid || std::fabs(s.appliedOverlayDistanceMeters - distanceMeters) > 0.0001f)
            CfgBuildFixedPlacement(s, distanceMeters);

        if (!s.fixedPlacementApplied)
        {
            bool ok = true;
            if (CfgIsValidOverlayHandle(s.handle))
                ok = ov->SetOverlayTransformAbsolute(s.handle, s.fixedPlacementOrigin, &s.fixedPlacementTransform) == vr::VROverlayError_None && ok;
            if (CfgIsValidOverlayHandle(s.backHandle))
                ok = ov->SetOverlayTransformAbsolute(s.backHandle, s.fixedPlacementOrigin, &s.fixedPlacementTransform) == vr::VROverlayError_None && ok;
            if (ok)
                s.fixedPlacementApplied = true;
        }
    }

    static bool CfgEnsureOverlay(CfgOverlayState& s)
    {
        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return false;

        if (CfgIsValidOverlayHandle(s.handle) && CfgIsValidOverlayHandle(s.backHandle))
        {
            CfgApplyOverlayPlacement(s, ov);
            return true;
        }

        if (!CfgIsValidOverlayHandle(s.handle))
        {
            vr::EVROverlayError err = ov->CreateOverlay("l4d2vr.config.overlay.front", "L4D2VR Config", &s.handle);
            if (err != vr::VROverlayError_None || !CfgIsValidOverlayHandle(s.handle))
            {
                s.status = "CreateOverlay failed: " + std::to_string((int)err);
                return false;
            }
        }

        if (!CfgIsValidOverlayHandle(s.backHandle))
        {
            vr::EVROverlayError err = ov->CreateOverlay("l4d2vr.config.overlay.back", "L4D2VR Config Back", &s.backHandle);
            if (err != vr::VROverlayError_None || !CfgIsValidOverlayHandle(s.backHandle))
            {
                s.status = "CreateOverlay(back) failed: " + std::to_string((int)err);
                return false;
            }
        }

        CfgConfigurePanelOverlay(s, ov, s.handle, true);
        CfgConfigurePanelOverlay(s, ov, s.backHandle, false);
        CfgLoad(s);
        CfgInvalidateFixedPlacement(s);
        CfgApplyOverlayPlacement(s, ov);
        return true;
    }

    static void CfgSetNumericValueFromSlider(CfgOverlayState& s, const CfgOptionSpec& spec, int mx)
    {
        if (spec.type != CfgOptionType::Float && spec.type != CfgOptionType::Int)
            return;
        const float t = (std::clamp)((float)(mx - kCfgSliderX) / (float)(std::max)(1, kCfgSliderW), 0.0f, 1.0f);
        if (spec.type == CfgOptionType::Int)
        {
            int v = (int)std::lround(spec.minValue + (spec.maxValue - spec.minValue) * t);
            v = CfgClampIntToSpec(spec, v);
            const std::string value = std::to_string(v);
            s.values[spec.key] = value;
            s.status = std::string(spec.key) + " = " + value;
        }
        else
        {
            const float current = 0.0f;
            const float step = CfgStepForFloat(spec, current);
            float f = spec.minValue + (spec.maxValue - spec.minValue) * t;
            f = CfgClampFloatToSpec(spec, f);
            const std::string value = CfgFormatFloat(f, step);
            s.values[spec.key] = value;
            s.status = std::string(spec.key) + " = " + value;
        }
        CfgRebuildVisibleIndexes(s);
        CfgMarkEdited(s);
        if (std::strcmp(spec.key, "ConfigOverlayDistanceMeters") == 0)
            CfgInvalidateFixedPlacement(s);
    }

    static uint32_t CfgNowMs()
    {
        return static_cast<uint32_t>(GetTickCount());
    }

    static bool CfgHoverSelectionSuppressed(const CfgOverlayState& s, uint32_t nowMs = CfgNowMs())
    {
        return s.hoverSelectionSuppressedUntilMs != 0 &&
            static_cast<int32_t>(nowMs - s.hoverSelectionSuppressedUntilMs) < 0;
    }

    static void CfgSuppressHoverSelectionAfterScroll(CfgOverlayState& s)
    {
        s.hoverSelectionSuppressedUntilMs = CfgNowMs() + kCfgHoverSelectSuppressAfterScrollMs;
        if (s.hoveredItem != -1)
        {
            s.hoveredItem = -1;
            s.dirty = true;
        }
    }

    static int CfgHitTestRowItem(const CfgOverlayState& s, int my)
    {
        const int total = (int)s.visibleSpecIndexes.size();
        int y = kCfgRowsY;
        for (int item = s.scroll; item < total && y < kCfgRowsBottom; ++item)
        {
            const CfgOptionSpec& spec = kCfgOptionSpecs[s.visibleSpecIndexes[item]];
            const char* prevGroup = nullptr;
            if (item > s.scroll)
                prevGroup = CfgGroupText(s, kCfgOptionSpecs[s.visibleSpecIndexes[item - 1]]);
            const char* curGroup = CfgGroupText(s, spec);
            const bool groupChanged = (item == s.scroll) || !prevGroup || std::strcmp(prevGroup, curGroup) != 0;
            if (groupChanged)
            {
                if (my >= y && my < y + kCfgOverlayGroupH)
                    return -1;
                y += kCfgOverlayGroupH;
            }

            if (my >= y && my < y + kCfgOverlayRowH)
                return item;

            y += kCfgOverlayRowH;
        }
        return -1;
    }

    static bool CfgSelectHoveredRow(CfgOverlayState& s, int my)
    {
        if (CfgHoverSelectionSuppressed(s))
        {
            if (s.hoveredItem != -1)
            {
                s.hoveredItem = -1;
                s.dirty = true;
            }
            return false;
        }

        s.hoverSelectionSuppressedUntilMs = 0;
        const int item = CfgHitTestRowItem(s, my);
        if (!CfgIsSelectableRow(s, item))
        {
            s.hoveredItem = -1;
            return false;
        }

        s.hoveredItem = item;
        if (s.selected != item)
        {
            s.selected = item;
            CfgEnsureSelectedVisible(s);
            s.dirty = true;
        }
        return true;
    }


    static void CfgBeginStringEdit(CfgOverlayState& s, const CfgOptionSpec& spec)
    {
        if (!CfgIsKeyboardEditable(spec))
            return;

        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
        {
            s.status = "VR keyboard is unavailable.";
            s.dirty = true;
            return;
        }

        const std::string currentValue = CfgValueFor(s, spec);
        const std::string description = std::string(CfgTitleText(s, spec)) + " [" + spec.key + "]";
        const uint64_t userValue = (static_cast<uint64_t>(GetTickCount()) << 16) ^ static_cast<uint64_t>((uintptr_t)spec.key & 0xFFFFu);
        vr::EVROverlayError err = ov->ShowKeyboardForOverlay(
            s.handle,
            vr::k_EGamepadTextInputModeNormal,
            vr::k_EGamepadTextInputLineModeSingleLine,
            static_cast<uint32_t>(vr::KeyboardFlag_Modal),
            description.c_str(),
            MAX_STR_LEN - 1,
            currentValue.c_str(),
            userValue);

        if (err != vr::VROverlayError_None)
        {
            s.status = std::string("ShowKeyboardForOverlay failed: ") + std::to_string((int)err);
            s.keyboardActive = false;
            s.keyboardEditKey.clear();
            s.keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
            s.dirty = true;
            return;
        }

        s.keyboardActive = true;
        s.keyboardEditKey = spec.key;
        s.keyboardEventHandle = s.handle;
        s.keyboardUserValue = userValue;
        s.keyboardOpenMs = static_cast<uint32_t>(GetTickCount());
        s.status = std::string("Editing with VR keyboard: ") + spec.key;
        s.dirty = true;
    }

    static void CfgCommitKeyboardText(CfgOverlayState& s)
    {
        if (!s.keyboardActive || s.keyboardEditKey.empty())
            return;

        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return;

        char text[MAX_STR_LEN] = {};
        const uint32_t copied = ov->GetKeyboardText(text, (uint32_t)sizeof(text));
        (void)copied;

        const int specIndex = CfgFindSpecIndex(s.keyboardEditKey);
        if (specIndex < 0)
        {
            s.keyboardActive = false;
            s.keyboardEditKey.clear();
            s.keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
            s.keyboardUserValue = 0;
            s.dirty = true;
            return;
        }

        const CfgOptionSpec& spec = kCfgOptionSpecs[specIndex];
        std::string value = text;
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        s.values[spec.key] = value;
        s.status = std::string(spec.key) + " = " + value;
        s.keyboardActive = false;
        s.keyboardEditKey.clear();
        s.keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
        s.keyboardUserValue = 0;
        CfgRebuildVisibleIndexes(s);
        CfgMarkEdited(s);
    }

    static void CfgCancelKeyboardText(CfgOverlayState& s)
    {
        if (!s.keyboardActive)
            return;
        s.keyboardActive = false;
        s.keyboardEditKey.clear();
        s.keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
        s.keyboardUserValue = 0;
        s.status = "VR keyboard closed.";
        s.dirty = true;
    }

    static void CfgHandleClick(CfgOverlayState& s, int mx, int my)
    {
        if (my >= kCfgTopButtonY && my <= kCfgTopButtonY + kCfgTopButtonH)
        {
            if (mx >= kCfgPagePrevX && mx <= kCfgPagePrevX + kCfgPageButtonW)
            {
                CfgPageSelection(s, -1);
                return;
            }
            if (mx >= kCfgPageNextX && mx <= kCfgPageNextX + kCfgPageButtonW)
            {
                CfgPageSelection(s, +1);
                return;
            }
            if (mx >= kCfgLangX && mx <= kCfgLangX + kCfgLangW)
            {
                s.useChinese = !s.useChinese;
                CfgRebuildVisibleIndexes(s);
                s.status = s.useChinese ? "\350\257\255\350\250\200\357\274\232\344\270\255\346\226\207\357\274\210\344\270\264\346\227\266\345\210\207\346\215\242\357\274\214\344\270\213\346\254\241\346\211\223\345\274\200\346\214\211\347\263\273\347\273\237\350\257\255\350\250\200\357\274\211\343\200\202" : "Language: English (temporary; next open follows system locale).";
                s.dirty = true;
                return;
            }
            if (mx >= kCfgReloadX && mx <= kCfgReloadX + kCfgReloadW)
            {
                CfgLoad(s);
                CfgApplyOverlayPlacement(s);
                return;
            }
            if (mx >= kCfgSaveX && mx <= kCfgSaveX + kCfgSaveW)
            {
                CfgSave(s);
                CfgApplyOverlayPlacement(s);
                return;
            }
            if (mx >= kCfgCloseX && mx <= kCfgCloseX + kCfgCloseW)
            {
                if (s.keyboardActive)
                {
                    if (vr::IVROverlay* ov = vr::VROverlay())
                        ov->HideKeyboard();
                    s.keyboardActive = false;
                    s.keyboardEditKey.clear();
                    s.keyboardEventHandle = vr::k_ulOverlayHandleInvalid;
                }
                s.visible = false;
                s.hoverSelectionSuppressedUntilMs = 0;
                s.hoveredItem = -1;
                s.status = s.useChinese ? "\345\267\262\345\205\263\351\227\255\343\200\202\346\214\211 F8 \345\217\257\351\207\215\346\226\260\346\211\223\345\274\200\343\200\202" : "Closed. Press F8 to reopen.";
                s.dirty = true;
                return;
            }
        }

        const int item = CfgHitTestRowItem(s, my);
        if (!CfgIsSelectableRow(s, item))
            return;

        s.selected = item;
        CfgEnsureSelectedVisible(s);

        const CfgOptionSpec& spec = kCfgOptionSpecs[s.visibleSpecIndexes[item]];
        if (spec.type == CfgOptionType::Bool)
        {
            CfgAdjustSelected(s, +1);
        }
        else if ((spec.type == CfgOptionType::Float || spec.type == CfgOptionType::Int) &&
            mx >= kCfgSliderX && mx <= kCfgSliderX + kCfgSliderW)
        {
            CfgSetNumericValueFromSlider(s, spec, mx);
        }
        else if (CfgIsComponentEditable(spec))
        {
            const int count = CfgComponentCount(spec);
            bool componentClicked = false;
            for (int i = 0; i < count; ++i)
            {
                const int bx = kCfgComponentX + i * (kCfgComponentButtonW + kCfgComponentGap);
                if (mx >= bx && mx <= bx + kCfgComponentButtonW)
                {
                    CfgSetSelectedComponentIndex(s, spec, i);
                    componentClicked = true;
                    break;
                }
            }

            if (mx >= kCfgMinusX && mx <= kCfgMinusX + kCfgAdjustButtonW)
                CfgAdjustComponentValue(s, spec, -1);
            else if (mx >= kCfgPlusX && mx <= kCfgPlusX + kCfgAdjustButtonW)
                CfgAdjustComponentValue(s, spec, +1);
            else if (!componentClicked)
                s.status = std::string("Select a component, then click -/+ to adjust: ") + spec.key;
            s.dirty = true;
        }
        else if (CfgIsKeyboardEditable(spec))
        {
            if (mx >= kCfgSliderX || mx >= kCfgStringEditX)
                CfgBeginStringEdit(s, spec);
            else
            {
                s.status = std::string("Click Edit to change: ") + spec.key;
                s.dirty = true;
            }
        }
        else if (CfgIsAdjustable(spec))
        {
            // Clicking the left/right side of a numeric row still gives coarse adjustment,
            // useful when a controller ray is too imprecise for the slider.
            CfgAdjustSelected(s, mx < kCfgSliderX ? -1 : +1);
        }
        else
        {
            s.status = std::string("This option is not editable in the VR panel: ") + spec.key;
            s.dirty = true;
        }
    }

    static void CfgReloadIfConfigFileChanged(CfgOverlayState& s)
    {
        if (s.hasUnsavedEdits)
            return;

        const uint32_t nowMs = static_cast<uint32_t>(GetTickCount());
        if (s.lastConfigStatMs != 0 && (nowMs - s.lastConfigStatMs) < 250u)
            return;
        s.lastConfigStatMs = nowMs;

        std::filesystem::file_time_type writeTime{};
        if (!CfgReadConfigWriteTime(s, writeTime))
            return;

        if (!s.configWriteTimeValid)
        {
            s.configWriteTime = writeTime;
            s.configWriteTimeValid = true;
            return;
        }

        if (writeTime != s.configWriteTime)
        {
            CfgLoad(s);
            CfgApplyOverlayPlacement(s);
        }
    }

    static bool CfgUploadPanelTexture(CfgOverlayState& s, vr::IVROverlay* ov)
    {
        if (!ov)
            ov = vr::VROverlay();
        if (!ov || !CfgIsValidOverlayHandle(s.handle) || s.rgba.empty())
            return false;

        const bool canSwapOverlay = !s.keyboardActive;
        const bool hasBack = canSwapOverlay && CfgIsValidOverlayHandle(s.backHandle);
        vr::VROverlayHandle_t target = hasBack ? s.backHandle : s.handle;
        vr::VROverlayHandle_t old = s.handle;

        CfgApplyOverlayPlacement(s, ov);
        CfgConfigurePanelOverlay(s, ov, target, true);
        if (hasBack)
            CfgConfigurePanelOverlay(s, ov, old, false);

        vr::EVROverlayError err = ov->SetOverlayRaw(target, s.rgba.data(), kCfgOverlayW, kCfgOverlayH, 4);
        if (err != vr::VROverlayError_None)
        {
            s.status = "SetOverlayRaw failed: " + std::to_string((int)err);
            return false;
        }

        // Upload to the hidden/back overlay, show it above the old copy, then swap handles.
        // Do not hide the old copy immediately; with an opaque panel it remains invisible under
        // the new copy and prevents the one-frame blank seen when SetOverlayRaw hits a visible overlay.
        ov->ShowOverlay(target);
        if (hasBack)
            std::swap(s.handle, s.backHandle);

        s.panelOverlayShown = true;
        s.needsUpload = false;
        return true;
    }

    static void CfgShowPanelIfNeeded(CfgOverlayState& s, vr::IVROverlay* ov)
    {
        if (!ov)
            ov = vr::VROverlay();
        if (!ov || !CfgIsValidOverlayHandle(s.handle))
            return;

        CfgConfigurePanelOverlay(s, ov, s.handle, true);
        if (CfgIsValidOverlayHandle(s.backHandle))
            CfgConfigurePanelOverlay(s, ov, s.backHandle, false);
        if (!s.panelOverlayShown)
        {
            ov->ShowOverlay(s.handle);
            s.panelOverlayShown = true;
        }
    }

    static void CfgPollOverlayEvents(CfgOverlayState& s)
    {
        if (s.handle == vr::k_ulOverlayHandleInvalid)
            return;

        vr::IVROverlay* ov = vr::VROverlay();
        if (!ov)
            return;

        vr::VREvent_t ev{};
        while (ov->PollNextOverlayEvent(s.handle, &ev, sizeof(ev)))
        {
            switch (ev.eventType)
            {
            case vr::VREvent_MouseMove:
            {
                int mx = (int)std::lround(ev.data.mouse.x);
                int my = kCfgOverlayH - (int)std::lround(ev.data.mouse.y);
                mx = (std::clamp)(mx, 0, kCfgOverlayW - 1);
                my = (std::clamp)(my, 0, kCfgOverlayH - 1);
                (void)mx;
                CfgSelectHoveredRow(s, my);
                break;
            }
            case vr::VREvent_MouseButtonDown:
            {
                int mx = (int)std::lround(ev.data.mouse.x);
                int my = kCfgOverlayH - (int)std::lround(ev.data.mouse.y);
                mx = (std::clamp)(mx, 0, kCfgOverlayW - 1);
                my = (std::clamp)(my, 0, kCfgOverlayH - 1);
                CfgSelectHoveredRow(s, my);
                CfgHandleClick(s, mx, my);
                break;
            }
            case vr::VREvent_ScrollDiscrete:
            {
                const float ydelta = ev.data.scroll.ydelta;
                if (std::fabs(ydelta) > 0.01f)
                {
                    CfgSuppressHoverSelectionAfterScroll(s);
                    CfgMoveSelection(s, (ydelta > 0.0f) ? -3 : 3);
                }
                break;
            }
            case vr::VREvent_KeyboardDone:
                CfgCommitKeyboardText(s);
                break;
            case vr::VREvent_KeyboardClosed:
                CfgCancelKeyboardText(s);
                break;
            default:
                break;
            }
        }

        if (s.keyboardActive &&
            CfgIsValidOverlayHandle(s.keyboardEventHandle) &&
            s.keyboardEventHandle != s.handle)
        {
            vr::VREvent_t kev{};
            while (ov->PollNextOverlayEvent(s.keyboardEventHandle, &kev, sizeof(kev)))
            {
                if (kev.eventType == vr::VREvent_KeyboardDone)
                    CfgCommitKeyboardText(s);
                else if (kev.eventType == vr::VREvent_KeyboardClosed)
                    CfgCancelKeyboardText(s);
            }
        }
    }

    static void CfgOverlayThreadMain()
    {
        while (true)
        {
            Sleep(33);

            CfgOverlayState& s = g_CfgOverlay;
            const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8 && !s.prevF8)
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.visible = !s.visible;
                s.hoverSelectionSuppressedUntilMs = 0;
                s.hoveredItem = -1;
                if (s.visible)
                {
                    CfgLoad(s);
                    CfgInvalidateFixedPlacement(s);
                    CfgApplyOverlayPlacement(s);
                }
                s.status = s.visible
                    ? (s.useChinese ? "\345\267\262\346\211\223\345\274\200\343\200\202\347\224\250 VR \346\216\247\345\210\266\345\231\250\345\260\204\347\272\277\347\202\271\345\207\273\346\214\211\351\222\256\343\200\202" : "Opened. Point and click with the VR controller laser.")
                    : (s.useChinese ? "\345\267\262\345\205\263\351\227\255\343\200\202\346\214\211 F8 \345\217\257\351\207\215\346\226\260\346\211\223\345\274\200\343\200\202" : "Closed. Press F8 to reopen.");
                s.dirty = true;
            }
            s.prevF8 = f8;

            std::lock_guard<std::mutex> lock(s.mutex);
            if (!CfgEnsureOverlay(s))
                continue;

            vr::IVROverlay* ov = vr::VROverlay();
            if (!ov)
                continue;

            CfgUpdateMenuButton(s);

            if (!s.visible)
            {
                CfgHidePanelOverlays(s, ov);
                CfgSetBaseOverlaysBlocked(s, false, ov);
                continue;
            }

            CfgHideMenuButton(s);
            CfgSetBaseOverlaysBlocked(s, true, ov);
            CfgReloadIfConfigFileChanged(s);
            CfgApplyOverlayPlacement(s, ov);
            CfgPollOverlayEvents(s);

            if (s.dirty)
                CfgRender(s);

            if (s.needsUpload)
                CfgUploadPanelTexture(s, ov);
            else
                CfgShowPanelIfNeeded(s, ov);
        }
    }
}

void L4D2VRConfigOverlay_StartWorker()
{
    bool expected = false;
    if (!g_CfgOverlay.workerStarted.compare_exchange_strong(expected, true))
        return;

    std::thread([]()
        {
            CfgOverlayThreadMain();
        }).detach();
}
