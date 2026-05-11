// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <cwctype>
#include <cctype>
#include <cstdlib>
#include <system_error>
#include <shellapi.h>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "sdk.h"

namespace
{
    constexpr wchar_t kDesiredVrWindowTitle[] = L"Left 4 Dead 2 VR - Vulkan";
    constexpr wchar_t kUiFontFixFileName[] = L"UI Font Fix.vpk";

    struct WindowSearchContext
    {
        DWORD processId = 0;
        HWND hwnd = nullptr;
    };

    BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
        if (!ctx || !IsWindow(hwnd))
            return TRUE;

        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId != ctx->processId)
            return TRUE;

        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        if ((style & WS_VISIBLE) == 0)
            return TRUE;

        ctx->hwnd = hwnd;
        return FALSE;
    }

    HWND FindCurrentProcessMainWindow()
    {
        WindowSearchContext ctx;
        ctx.processId = GetCurrentProcessId();
        EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    bool EnsureDesiredWindowTitle(HWND hwnd)
    {
        if (!IsWindow(hwnd))
            return false;

        wchar_t currentTitle[256] = {};
        if (GetWindowTextW(hwnd, currentTitle, _countof(currentTitle)) > 0 &&
            lstrcmpW(currentTitle, kDesiredVrWindowTitle) == 0)
        {
            return true;
        }

        return SetWindowTextW(hwnd, kDesiredVrWindowTitle) != FALSE;
    }

    bool ForceWindowForeground(HWND hwnd)
    {
        if (!IsWindow(hwnd))
            return false;

        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        else
            ShowWindow(hwnd, SW_SHOW);

        HWND fgWindow = GetForegroundWindow();
        DWORD fgThreadId = fgWindow ? GetWindowThreadProcessId(fgWindow, nullptr) : 0;
        DWORD curThreadId = GetCurrentThreadId();
        const bool needAttach = (fgThreadId != 0 && fgThreadId != curThreadId);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, TRUE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);

        if (needAttach)
            AttachThreadInput(fgThreadId, curThreadId, FALSE);

        return GetForegroundWindow() == hwnd;
    }

    DWORD WINAPI FocusGameWindowWorker(LPVOID)
    {
        constexpr int kMaxTries = 30;
        constexpr DWORD kRetryDelayMs = 500;
        constexpr int kSuccessStabilityTries = 3;

        int stableForegroundCount = 0;
        for (int i = 0; i < kMaxTries; ++i)
        {
            HWND window = FindCurrentProcessMainWindow();
            if (window && ForceWindowForeground(window))
            {
                ++stableForegroundCount;
                if (stableForegroundCount >= kSuccessStabilityTries)
                    return 0;
            }
            else
            {
                stableForegroundCount = 0;
            }

            Sleep(kRetryDelayMs);
        }

        return 0;
    }

    DWORD WINAPI MaintainWindowTitleWorker(LPVOID)
    {
        constexpr int kMaxRetries = 60;
        constexpr DWORD kRetryDelayMs = 1000;

        for (int i = 0; i < kMaxRetries; ++i)
        {
            HWND window = FindCurrentProcessMainWindow();
            if (window)
                EnsureDesiredWindowTitle(window);

            Sleep(kRetryDelayMs);
        }

        return 0;
    }

    bool IsNoHmdLaunchArgPresent()
    {
        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return false;

        bool noHmdEnabled = false;
        for (int i = 0; i < nArgs; ++i)
        {
            if (_wcsicmp(szArglist[i], L"-nohmd") == 0)
            {
                noHmdEnabled = true;
                break;
            }
        }

        LocalFree(szArglist);
        return noHmdEnabled;
    }

    struct LaunchArgumentState
    {
        bool hasHeapSize = false;
        bool hasWidth = false;
        bool hasHeight = false;
    };

    bool IsExactLaunchArg(const wchar_t* value, const wchar_t* expected)
    {
        return value && expected && _wcsicmp(value, expected) == 0;
    }

    bool IsLaunchArgName(const wchar_t* value, const wchar_t* expected)
    {
        if (!value || !expected)
            return false;

        if (_wcsicmp(value, expected) == 0)
            return true;

        const size_t expectedLen = wcslen(expected);
        return _wcsnicmp(value, expected, expectedLen) == 0 && value[expectedLen] == L'=';
    }

    bool IsBigFronLaunchArgPresent()
    {
        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return false;

        bool bigFronEnabled = false;
        for (int i = 0; i < nArgs; ++i)
        {
            if (IsLaunchArgName(szArglist[i], L"-bigfonts"))
            {
                bigFronEnabled = true;
                break;
            }
        }

        LocalFree(szArglist);
        return bigFronEnabled;
    }

    LaunchArgumentState ReadLaunchArgumentState()
    {
        LaunchArgumentState state;

        int nArgs = 0;
        LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
        if (!szArglist)
            return state;

        for (int i = 0; i < nArgs; ++i)
        {
            if (IsLaunchArgName(szArglist[i], L"-heapsize"))
                state.hasHeapSize = true;
            else if (IsExactLaunchArg(szArglist[i], L"-w"))
                state.hasWidth = true;
            else if (IsExactLaunchArg(szArglist[i], L"-h"))
                state.hasHeight = true;
        }

        LocalFree(szArglist);
        return state;
    }

    std::filesystem::path GetGameRootPath()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        return std::filesystem::path(exePath).parent_path();
    }

    std::filesystem::path GetLaunchArgumentNoticePath()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return {};

        return gameRootPath / L"left4dead2" / L"cfg" / L"l4d2vr_launch_argument_notice.txt";
    }

    bool FileExistsNoThrow(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    bool DeleteFileNoThrow(const std::filesystem::path& path)
    {
        if (!FileExistsNoThrow(path))
            return false;

        // A readonly VPK can make std::filesystem::remove silently fail through error_code.
        // Clear attributes first so the cleanup path is deterministic.
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);

        std::error_code ec;
        const bool removed = std::filesystem::remove(path, ec);
        return removed && !FileExistsNoThrow(path);
    }

    std::filesystem::path GetLeft4NekoDllPath()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return {};

        return gameRootPath / L"bin" / L"left4neko.dll";
    }

    bool ShouldUseChineseLaunchArgumentPrompt()
    {
        const LANGID uiLang = GetUserDefaultUILanguage();
        if (PRIMARYLANGID(uiLang) == LANG_CHINESE)
            return true;

        wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(localeName, _countof(localeName)) > 0)
            return _wcsnicmp(localeName, L"zh", 2) == 0;

        return false;
    }

    bool LaunchArgumentNoticeAlreadyShown(const std::filesystem::path& noticePath)
    {
        if (!FileExistsNoThrow(noticePath))
            return false;

        std::wifstream input(noticePath);
        if (!input.is_open())
            return true;

        std::wstring line;
        while (std::getline(input, line))
        {
            if (line.find(L"LaunchArgumentNoticeVersion=2") != std::wstring::npos)
                return true;
        }

        return false;
    }

    void MarkLaunchArgumentNoticeShown(const std::filesystem::path& noticePath)
    {
        if (noticePath.empty())
            return;

        std::error_code ec;
        std::filesystem::create_directories(noticePath.parent_path(), ec);

        std::wofstream output(noticePath, std::ios::trunc);
        if (!output.is_open())
            return;

        output << L"LaunchArgumentNoticeShown=1\n";
        output << L"LaunchArgumentNoticeVersion=2\n";
        output << L"CheckedArgs=-heapsize,-w,-h,left4neko.dll\n";
    }

    void ShowLaunchArgumentNoticeIfNeeded()
    {
        const std::filesystem::path noticePath = GetLaunchArgumentNoticePath();
        if (LaunchArgumentNoticeAlreadyShown(noticePath))
            return;

        const LaunchArgumentState state = ReadLaunchArgumentState();
        const bool left4NekoExists = FileExistsNoThrow(GetLeft4NekoDllPath());
        const bool missingHeapSize = !state.hasHeapSize && !left4NekoExists;
        const bool hasResolutionLaunchArgs = state.hasWidth || state.hasHeight;
        if (!missingHeapSize && !hasResolutionLaunchArgs)
            return;

        const bool useChinese = ShouldUseChineseLaunchArgumentPrompt();
        const wchar_t* title = useChinese ? L"L4D2VR \u542f\u52a8\u53c2\u6570\u63d0\u793a" : L"L4D2VR Launch Options";

        std::wstring message;
        if (useChinese)
        {
            message = L"\u68c0\u6d4b\u5230\u5f53\u524d\u542f\u52a8\u53c2\u6570\u53ef\u80fd\u4e0d\u9002\u5408 L4D2VR\uff1a\n\n";
            if (missingHeapSize)
                message += L"- \u672a\u68c0\u6d4b\u5230 -heapsize\uff0c\u4e14\u672a\u68c0\u6d4b\u5230 bin\\left4neko.dll\u3002\u5efa\u8bae\u5728 Steam \u542f\u52a8\u53c2\u6570\u4e2d\u52a0\u5165 -heapsize 524288\u3002\n";
            if (hasResolutionLaunchArgs)
                message += L"- \u68c0\u6d4b\u5230 -w \u6216 -h\u3002\u5efa\u8bae\u4ece Steam \u542f\u52a8\u53c2\u6570\u4e2d\u5220\u9664\u5b83\u4eec\uff0c\u907f\u514d\u8986\u76d6\u63d2\u4ef6\u7ba1\u7406\u7684\u7a97\u53e3\u5206\u8fa8\u7387\u3002\n";
            message += L"\n\u6b64\u63d0\u793a\u53ea\u663e\u793a\u4e00\u6b21\u3002";
        }
        else
        {
            message = L"The current launch options may not be suitable for L4D2VR:\n\n";
            if (missingHeapSize)
                message += L"- -heapsize was not detected, and bin\\left4neko.dll was not found. Add -heapsize 524288 to the Steam launch options.\n";
            if (hasResolutionLaunchArgs)
                message += L"- -w or -h was detected. Remove them from the Steam launch options to avoid overriding the window resolution managed by the plugin.\n";
            message += L"\nThis notice will only be shown once.";
        }

        HWND owner = FindCurrentProcessMainWindow();
        MessageBoxW(owner, message.c_str(), title, MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        MarkLaunchArgumentNoticeShown(noticePath);
    }

    bool ReplaceConfigValueInLine(std::wstring& line, const wchar_t* key, const wchar_t* expectedValue)
    {
        if (line.find(key) == std::wstring::npos)
            return false;

        const size_t firstQuote = line.find(L'"');
        if (firstQuote == std::wstring::npos)
            return false;

        const size_t secondQuote = line.find(L'"', firstQuote + 1);
        if (secondQuote == std::wstring::npos)
            return false;

        const size_t valueStartQuote = line.find(L'"', secondQuote + 1);
        if (valueStartQuote == std::wstring::npos)
            return false;

        const size_t valueEndQuote = line.find(L'"', valueStartQuote + 1);
        if (valueEndQuote == std::wstring::npos)
            return false;

        const std::wstring currentValue = line.substr(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1);
        if (currentValue == expectedValue)
            return false;

        line.replace(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1, expectedValue);
        return true;
    }

    bool ExtractConfigValueFromLine(const std::wstring& line, const wchar_t* key, std::wstring& outValue)
    {
        if (line.find(key) == std::wstring::npos)
            return false;

        const size_t firstQuote = line.find(L'"');
        if (firstQuote == std::wstring::npos)
            return false;

        const size_t secondQuote = line.find(L'"', firstQuote + 1);
        if (secondQuote == std::wstring::npos)
            return false;

        const size_t valueStartQuote = line.find(L'"', secondQuote + 1);
        if (valueStartQuote == std::wstring::npos)
            return false;

        const size_t valueEndQuote = line.find(L'"', valueStartQuote + 1);
        if (valueEndQuote == std::wstring::npos)
            return false;

        outValue = line.substr(valueStartQuote + 1, valueEndQuote - valueStartQuote - 1);
        return true;
    }

    std::wstring TrimWhitespace(const std::wstring& value)
    {
        size_t start = 0;
        while (start < value.size() && std::iswspace(value[start]))
            ++start;

        size_t end = value.size();
        while (end > start && std::iswspace(value[end - 1]))
            --end;

        return value.substr(start, end - start);
    }

    std::string TrimAsciiWhitespace(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
            --end;

        return value.substr(start, end - start);
    }

    struct VideoCfgDesiredSetting
    {
        const wchar_t* key;
        const wchar_t* value;
    };

    constexpr VideoCfgDesiredSetting kDesiredVideoCfgSettings[] =
    {
        { L"\"setting.mat_antialias\"", L"1" },
        { L"\"setting.mat_vsync\"", L"0" },
        { L"\"setting.mat_queue_mode\"", L"0" },
        { L"\"setting.fullscreen\"", L"0" },
        { L"\"setting.nowindowborder\"", L"1" },
    };

    void InsertVideoCfgLineBeforeClosingBrace(std::vector<std::wstring>& lines, const wchar_t* key, const wchar_t* value)
    {
        std::wstring text = L"\t";
        text += key;
        text += L"\t\t\"";
        text += value;
        text += L"\"";

        for (auto it = lines.begin(); it != lines.end(); ++it)
        {
            if (TrimWhitespace(*it) == L"}")
            {
                lines.insert(it, text);
                return;
            }
        }

        lines.push_back(text);
    }

    bool EnsureVideoCfgSettings()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return false;

        const std::filesystem::path videoCfgPath = gameRootPath / L"left4dead2" / L"cfg" / L"video.txt";
        if (!std::filesystem::exists(videoCfgPath))
            return false;

        std::wifstream input(videoCfgPath);
        if (!input.is_open())
            return false;

        std::vector<std::wstring> lines;
        lines.reserve(64);

        bool found[_countof(kDesiredVideoCfgSettings)] = {};
        std::wstring line;
        bool changed = false;
        while (std::getline(input, line))
        {
            for (size_t i = 0; i < _countof(kDesiredVideoCfgSettings); ++i)
            {
                const VideoCfgDesiredSetting& desired = kDesiredVideoCfgSettings[i];
                if (line.find(desired.key) != std::wstring::npos)
                {
                    found[i] = true;
                    changed |= ReplaceConfigValueInLine(line, desired.key, desired.value);
                    break;
                }
            }

            lines.push_back(line);
        }
        input.close();

        for (size_t i = 0; i < _countof(kDesiredVideoCfgSettings); ++i)
        {
            if (!found[i])
            {
                InsertVideoCfgLineBeforeClosingBrace(
                    lines,
                    kDesiredVideoCfgSettings[i].key,
                    kDesiredVideoCfgSettings[i].value);
                changed = true;
            }
        }

        if (!changed)
            return false;

        std::wofstream output(videoCfgPath, std::ios::trunc);
        if (!output.is_open())
            return false;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }

        return true;
    }

    void ApplyRuntimeVideoSettings()
    {
        if (!g_Game || !g_Game->m_Initialized)
            return;

        // The engine can rewrite video.txt after our early startup pass, especially after
        // display-mode changes. Apply the runtime ConVars as well so the current session
        // stays clamped while the worker keeps the persisted file corrected for next launch.
        g_Game->SetConVarInt("mat_antialias", 1);
        g_Game->SetConVarInt("mat_vsync", 0);
    }

    DWORD WINAPI MaintainVideoCfgSettingsWorker(LPVOID)
    {
        // Source may load defaults, change display mode, then save video.txt again after
        // DllMain's early pass. Keep checking briefly during startup instead of relying on
        // one file write at DLL load time.
        constexpr int kAttempts = 120;
        for (int i = 0; i < kAttempts; ++i)
        {
            ApplyRuntimeVideoSettings();
            EnsureVideoCfgSettings();

            Sleep(i < 20 ? 500 : 1000);
        }

        return 0;
    }

    std::string StripQuotes(std::string value)
    {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            return value.substr(1, value.size() - 2);
        return value;
    }

    bool AutoexecHasCrosshairOne(const std::filesystem::path& autoexecPath)
    {
        std::ifstream input(autoexecPath);
        if (!input.is_open())
            return false;

        std::string line;
        while (std::getline(input, line))
        {
            const size_t commentPos = line.find("//");
            if (commentPos != std::string::npos)
                line = line.substr(0, commentPos);

            std::istringstream iss(line);
            std::string cmd;
            std::string value;
            if (!(iss >> cmd >> value))
                continue;

            cmd = StripQuotes(cmd);
            value = StripQuotes(value);
            if (_stricmp(cmd.c_str(), "crosshair") == 0 && value == "1")
                return true;
        }

        return false;
    }

    void EnsureNoHmdAutoexecCrosshair()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const std::filesystem::path autoexecPath = gameRootPath / L"left4dead2" / L"cfg" / L"autoexec.cfg";

        if (!std::filesystem::exists(autoexecPath))
        {
            std::error_code ec;
            std::filesystem::create_directories(autoexecPath.parent_path(), ec);

            std::ofstream createFile(autoexecPath, std::ios::trunc);
            if (createFile.is_open())
                createFile << "crosshair 1\n";
            return;
        }

        if (AutoexecHasCrosshairOne(autoexecPath))
            return;

        std::ofstream appendFile(autoexecPath, std::ios::app);
        if (!appendFile.is_open())
            return;

        appendFile << "\ncrosshair 1\n";
    }

    void EnsureNoHmdVideoCfgDesktopResolution()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return;

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        int width = 0;
        int height = 0;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        {
            width = static_cast<int>(dm.dmPelsWidth);
            height = static_cast<int>(dm.dmPelsHeight);
        }
        else
        {
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
        }

        if (width <= 0 || height <= 0)
            return;

        const std::wstring widthValue = std::to_wstring(width);
        const std::wstring heightValue = std::to_wstring(height);

        std::filesystem::path videoCfgPath(exePath);
        videoCfgPath = videoCfgPath.parent_path() / L"left4dead2" / L"cfg" / L"video.txt";
        if (!std::filesystem::exists(videoCfgPath))
            return;

        std::wifstream input(videoCfgPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(64);

        std::wstring line;
        bool changed = false;
        bool foundRes = false;
        bool foundHeight = false;

        while (std::getline(input, line))
        {
            if (line.find(L"\"setting.defaultres\"") != std::wstring::npos)
                foundRes = true;
            if (line.find(L"\"setting.defaultresheight\"") != std::wstring::npos)
                foundHeight = true;

            changed |= ReplaceConfigValueInLine(line, L"\"setting.defaultres\"", widthValue.c_str());
            changed |= ReplaceConfigValueInLine(line, L"\"setting.defaultresheight\"", heightValue.c_str());
            lines.push_back(line);
        }
        input.close();

        auto insertLineBeforeClosingBrace = [&lines](const std::wstring& text)
            {
                for (auto it = lines.begin(); it != lines.end(); ++it)
                {
                    if (TrimWhitespace(*it) == L"}")
                    {
                        lines.insert(it, text);
                        return;
                    }
                }
                lines.push_back(text);
            };

        if (!foundRes)
        {
            insertLineBeforeClosingBrace(L"\t\"setting.defaultres\"\t\t\"" + widthValue + L"\"");
            changed = true;
        }

        if (!foundHeight)
        {
            insertLineBeforeClosingBrace(L"\t\"setting.defaultresheight\"\t\t\"" + heightValue + L"\"");
            changed = true;
        }

        if (!changed)
            return;

        std::wofstream output(videoCfgPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }
    }

    bool TryGetCurrentDisplayResolution(int& width, int& height)
    {
        width = 0;
        height = 0;

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
        {
            width = static_cast<int>(dm.dmPelsWidth);
            height = static_cast<int>(dm.dmPelsHeight);
        }
        else
        {
            width = GetSystemMetrics(SM_CXSCREEN);
            height = GetSystemMetrics(SM_CYSCREEN);
        }

        return width > 0 && height > 0;
    }

    bool TryExtractFirstQuotedString(const std::wstring& line, std::wstring& outValue)
    {
        const size_t start = line.find(L'"');
        if (start == std::wstring::npos)
            return false;

        const size_t end = line.find(L'"', start + 1);
        if (end == std::wstring::npos)
            return false;

        outValue = line.substr(start + 1, end - start - 1);
        return true;
    }

    bool TryExtractSecondQuotedString(const std::wstring& line, std::wstring& outValue)
    {
        size_t pos = line.find(L'"');
        if (pos == std::wstring::npos)
            return false;
        pos = line.find(L'"', pos + 1);
        if (pos == std::wstring::npos)
            return false;
        pos = line.find(L'"', pos + 1);
        if (pos == std::wstring::npos)
            return false;

        const size_t end = line.find(L'"', pos + 1);
        if (end == std::wstring::npos)
            return false;

        outValue = line.substr(pos + 1, end - pos - 1);
        return true;
    }

    bool IsUiFontFixAddonEntryLine(const std::wstring& line)
    {
        std::wstring firstQuoted;
        if (!TryExtractFirstQuotedString(line, firstQuoted))
            return false;

        for (wchar_t& ch : firstQuoted)
        {
            if (ch == L'/')
                ch = L'\\';
        }

        const wchar_t* suffix = kUiFontFixFileName;
        const size_t suffixLen = wcslen(suffix);
        if (firstQuoted.size() < suffixLen)
            return false;

        return _wcsicmp(firstQuoted.c_str() + firstQuoted.size() - suffixLen, suffix) == 0;
    }

    bool IsAddonListEntryLine(const std::wstring& line)
    {
        std::wstring firstQuoted;
        std::wstring secondQuoted;
        return TryExtractFirstQuotedString(line, firstQuoted) && TryExtractSecondQuotedString(line, secondQuoted);
    }

    void WriteAddonListLines(const std::filesystem::path& addonListPath, const std::vector<std::wstring>& lines)
    {
        std::error_code ec;
        std::filesystem::create_directories(addonListPath.parent_path(), ec);

        std::wofstream output(addonListPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << L'\n';
        }
    }

    void EnsureUiFontFixAddonListFirst(const std::filesystem::path& addonListPath)
    {
        constexpr wchar_t kUiFontFixEntry[] = L"\t\"UI Font Fix.vpk\"\t\t\"1\"";

        if (!FileExistsNoThrow(addonListPath))
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        std::wifstream input(addonListPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(128);

        std::wstring line;
        while (std::getline(input, line))
            lines.push_back(line);
        input.close();

        int openBraceIndex = -1;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (TrimWhitespace(lines[i]) == L"{")
            {
                openBraceIndex = static_cast<int>(i);
                break;
            }
        }

        if (openBraceIndex < 0)
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        int firstEntryIndex = -1;
        int existingUiFontFixIndex = -1;
        std::vector<std::wstring> filteredLines;
        filteredLines.reserve(lines.size() + 1);

        for (size_t i = 0; i < lines.size(); ++i)
        {
            const bool isAddonEntry = static_cast<int>(i) > openBraceIndex && IsAddonListEntryLine(lines[i]);
            if (isAddonEntry && firstEntryIndex < 0)
                firstEntryIndex = static_cast<int>(i);

            if (IsUiFontFixAddonEntryLine(lines[i]))
            {
                if (existingUiFontFixIndex < 0)
                    existingUiFontFixIndex = static_cast<int>(i);
                continue;
            }

            filteredLines.push_back(lines[i]);
        }

        const bool alreadyFirst = (existingUiFontFixIndex >= 0) &&
            (firstEntryIndex < 0 || existingUiFontFixIndex == firstEntryIndex);
        if (alreadyFirst)
        {
            std::wstring firstValue;
            const bool enabled = TryExtractSecondQuotedString(lines[existingUiFontFixIndex], firstValue) && firstValue == L"1";
            if (enabled)
                return;
        }

        int filteredOpenBraceIndex = -1;
        for (size_t i = 0; i < filteredLines.size(); ++i)
        {
            if (TrimWhitespace(filteredLines[i]) == L"{")
            {
                filteredOpenBraceIndex = static_cast<int>(i);
                break;
            }
        }

        if (filteredOpenBraceIndex < 0)
        {
            WriteAddonListLines(addonListPath, {
                L"\"AddonList\"",
                L"{",
                kUiFontFixEntry,
                L"}"
                });
            return;
        }

        filteredLines.insert(filteredLines.begin() + filteredOpenBraceIndex + 1, kUiFontFixEntry);
        WriteAddonListLines(addonListPath, filteredLines);
    }

    void RemoveUiFontFixAddonListEntry(const std::filesystem::path& addonListPath)
    {
        if (!FileExistsNoThrow(addonListPath))
            return;

        std::wifstream input(addonListPath);
        if (!input.is_open())
            return;

        std::vector<std::wstring> lines;
        lines.reserve(128);

        std::wstring line;
        bool changed = false;
        while (std::getline(input, line))
        {
            if (IsUiFontFixAddonEntryLine(line))
            {
                changed = true;
                continue;
            }

            lines.push_back(line);
        }
        input.close();

        if (!changed)
            return;

        WriteAddonListLines(addonListPath, lines);
    }

    bool IsMatSetVideoModeLine(const std::string& line)
    {
        std::string activePart = line;
        const size_t commentPos = activePart.find("//");
        if (commentPos != std::string::npos)
            activePart = activePart.substr(0, commentPos);

        activePart = TrimAsciiWhitespace(activePart);
        if (activePart.empty())
            return false;

        std::istringstream iss(activePart);
        std::string cmd;
        if (!(iss >> cmd))
            return false;

        cmd = StripQuotes(cmd);
        return _stricmp(cmd.c_str(), "mat_setvideomode") == 0;
    }

    void EnsureAutoexecMatSetVideoMode(const std::filesystem::path& autoexecPath, int width, int height)
    {
        if (width <= 0 || height <= 0)
            return;

        const std::string line0 = "mat_setvideomode " + std::to_string(width) + " " + std::to_string(height) + " 0";
        const std::string line1 = "mat_setvideomode " + std::to_string(width) + " " + std::to_string(height) + " 1";

        std::vector<std::string> lines;
        bool foundLine0 = false;
        bool foundLine1 = false;
        int matSetVideoModeLineCount = 0;

        if (FileExistsNoThrow(autoexecPath))
        {
            std::ifstream input(autoexecPath);
            if (input.is_open())
            {
                std::string line;
                while (std::getline(input, line))
                {
                    const std::string trimmed = TrimAsciiWhitespace(line);
                    if (IsMatSetVideoModeLine(line))
                    {
                        ++matSetVideoModeLineCount;
                        if (_stricmp(trimmed.c_str(), line0.c_str()) == 0)
                            foundLine0 = true;
                        else if (_stricmp(trimmed.c_str(), line1.c_str()) == 0)
                            foundLine1 = true;
                        continue;
                    }

                    lines.push_back(line);
                }
            }
        }

        if (foundLine0 && foundLine1 && matSetVideoModeLineCount == 2)
            return;

        if (!lines.empty() && !TrimAsciiWhitespace(lines.back()).empty())
            lines.push_back(std::string());

        lines.push_back(line0);
        lines.push_back(line1);

        std::error_code ec;
        std::filesystem::create_directories(autoexecPath.parent_path(), ec);

        std::ofstream output(autoexecPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << '\n';
        }
    }

    void RemoveAutoexecMatSetVideoMode(const std::filesystem::path& autoexecPath)
    {
        if (!FileExistsNoThrow(autoexecPath))
            return;

        std::ifstream input(autoexecPath);
        if (!input.is_open())
            return;

        std::vector<std::string> lines;
        lines.reserve(128);

        std::string line;
        bool changed = false;
        while (std::getline(input, line))
        {
            if (IsMatSetVideoModeLine(line))
            {
                changed = true;
                continue;
            }

            lines.push_back(line);
        }
        input.close();

        if (!changed)
            return;

        std::ofstream output(autoexecPath, std::ios::trunc);
        if (!output.is_open())
            return;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            output << lines[i];
            if (i + 1 < lines.size())
                output << '\n';
        }
    }

    void RemoveUiFontFixVpkAndConfig(const std::filesystem::path& gameRootPath)
    {
        if (gameRootPath.empty())
            return;

        const std::filesystem::path left4dead2Path = gameRootPath / L"left4dead2";
        const std::filesystem::path targetVpkPath = left4dead2Path / L"addons" / kUiFontFixFileName;

        DeleteFileNoThrow(targetVpkPath);
        RemoveUiFontFixAddonListEntry(left4dead2Path / L"addonlist.txt");
        RemoveAutoexecMatSetVideoMode(left4dead2Path / L"cfg" / L"autoexec.cfg");
    }

    void EnsureVrConfigTxtFromSample()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const std::filesystem::path vrPath = gameRootPath / L"vr";
        const std::filesystem::path configPath = vrPath / L"config.txt";
        if (FileExistsNoThrow(configPath))
            return;

        const std::filesystem::path samplePath = vrPath / L"config.sample";
        if (!FileExistsNoThrow(samplePath))
            return;

        std::error_code ec;
        std::filesystem::create_directories(vrPath, ec);
        ec.clear();
        std::filesystem::copy_file(samplePath, configPath, std::filesystem::copy_options::none, ec);
    }

    void EnsureUiFontFixVpkAndConfig()
    {
        const std::filesystem::path gameRootPath = GetGameRootPath();
        if (gameRootPath.empty())
            return;

        const bool bigFronEnabled = IsBigFronLaunchArgPresent();
        if (!bigFronEnabled)
        {
            RemoveUiFontFixVpkAndConfig(gameRootPath);
            return;
        }

        const std::filesystem::path sourceVpkPath = gameRootPath / L"vr" / kUiFontFixFileName;
        if (!FileExistsNoThrow(sourceVpkPath))
            return;

        const std::filesystem::path left4dead2Path = gameRootPath / L"left4dead2";
        const std::filesystem::path addonsPath = left4dead2Path / L"addons";
        const std::filesystem::path targetVpkPath = addonsPath / kUiFontFixFileName;

        if (!FileExistsNoThrow(targetVpkPath))
        {
            std::error_code ec;
            std::filesystem::create_directories(addonsPath, ec);
            std::filesystem::copy_file(sourceVpkPath, targetVpkPath, std::filesystem::copy_options::none, ec);
            if (!FileExistsNoThrow(targetVpkPath))
                return;
        }

        EnsureUiFontFixAddonListFirst(left4dead2Path / L"addonlist.txt");

        int width = 0;
        int height = 0;
        if (!TryGetCurrentDisplayResolution(width, height))
            return;

        EnsureAutoexecMatSetVideoMode(left4dead2Path / L"cfg" / L"autoexec.cfg", width, height);
    }
}

DWORD WINAPI InitL4D2VR(HMODULE hModule)
{
#ifdef _DEBUG
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    // Make sure -insecure is used
    LPWSTR* szArglist;
    int nArgs;
    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    bool insecureEnabled = false;
    for (int i = 0; i < nArgs; ++i)
    {
        if (wcscmp(szArglist[i], L"-insecure") == 0)
            insecureEnabled = true;
    }
    LocalFree(szArglist);

    EnsureVrConfigTxtFromSample();
    EnsureUiFontFixVpkAndConfig();

    if (IsNoHmdLaunchArgPresent())
    {
        EnsureNoHmdVideoCfgDesktopResolution();
        EnsureNoHmdAutoexecCrosshair();
        return 0;
    }

    ShowLaunchArgumentNoticeIfNeeded();

    CreateThread(nullptr, 0, FocusGameWindowWorker, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, MaintainWindowTitleWorker, nullptr, 0, nullptr);

    // First pass before engine interfaces are ready. A later worker repeats this because
    // the Source material system can save video.txt again after a display-mode change.
    EnsureVideoCfgSettings();

    g_Game = new Game();

    ApplyRuntimeVideoSettings();
    CreateThread(nullptr, 0, MaintainVideoCfgSettingsWorker, nullptr, 0, nullptr);

    return 0;
}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitL4D2VR, hModule, 0, NULL);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        Game::UninstallVertexFormatWarningFilter();
        break;
    }
    return TRUE;
}
