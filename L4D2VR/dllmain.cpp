// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <cwctype>
#include <system_error>
#include <shellapi.h>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "sdk.h"

namespace
{
    constexpr wchar_t kDesiredVrWindowTitle[] = L"Left 4 Dead 2 VR - Vulkan";

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

    std::filesystem::path GetLaunchArgumentNoticePath()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        std::filesystem::path noticePath(exePath);
        return noticePath.parent_path() / L"left4dead2" / L"cfg" / L"l4d2vr_launch_argument_notice.txt";
    }

    bool FileExistsNoThrow(const std::filesystem::path& path)
    {
        if (path.empty())
            return false;

        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }

    std::filesystem::path GetLeft4NekoDllPath()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return {};

        std::filesystem::path gameRootPath(exePath);
        return gameRootPath.parent_path() / L"bin" / L"left4neko.dll";
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

    void EnsureVideoCfgSettings()
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return;

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
        while (std::getline(input, line))
        {
            changed |= ReplaceConfigValueInLine(line, L"\"setting.mat_antialias\"", L"1");
            changed |= ReplaceConfigValueInLine(line, L"\"setting.mat_vsync\"", L"0");
            changed |= ReplaceConfigValueInLine(line, L"\"setting.mat_queue_mode\"", L"0");
            changed |= ReplaceConfigValueInLine(line, L"\"setting.fullscreen\"", L"0");
            changed |= ReplaceConfigValueInLine(line, L"\"setting.nowindowborder\"", L"1");
            lines.push_back(line);
        }
        input.close();

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
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
            return;

        std::filesystem::path autoexecPath(exePath);
        autoexecPath = autoexecPath.parent_path() / L"left4dead2" / L"cfg" / L"autoexec.cfg";

        if (!std::filesystem::exists(autoexecPath))
        {
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

    if (IsNoHmdLaunchArgPresent())
    {
        EnsureNoHmdVideoCfgDesktopResolution();
        EnsureNoHmdAutoexecCrosshair();
        return 0;
    }

    ShowLaunchArgumentNoticeIfNeeded();

    CreateThread(nullptr, 0, FocusGameWindowWorker, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, MaintainWindowTitleWorker, nullptr, 0, nullptr);
    EnsureVideoCfgSettings();

    g_Game = new Game();

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
