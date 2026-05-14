#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <string>
#include <chrono>
#include <thread>

static std::filesystem::path OutDir()
{
    return std::filesystem::current_path() / "windrose_plus_data" / "native_rt_export";
}

static void Log(const std::string& s)
{
    std::filesystem::create_directories(OutDir());
    std::ofstream f(OutDir() / "rt_native_exporter.log", std::ios::app);
    f << s << "\n";
}

DWORD WINAPI MainThread(LPVOID)
{
    try
    {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        Log("RTNativeExporter loaded");
        Log("Current path=" + std::filesystem::current_path().string());
        Log("Phase 1 skeleton OK");
    }
    catch (...)
    {
        Log("RTNativeExporter exception");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }

    return TRUE;
}
