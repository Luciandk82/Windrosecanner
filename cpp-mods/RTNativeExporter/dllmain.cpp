#define NOMINMAX

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <windows.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <thread>

using namespace RC;

static std::filesystem::path resolve_data_dir()
{
    std::filesystem::path candidates[] = {
        "../../../windrose_plus_data",
        "windrose_plus_data",
    };

    for (auto& p : candidates)
    {
        try
        {
            if (std::filesystem::exists(p)) return p;
        }
        catch (...) {}
    }

    try { std::filesystem::create_directories("windrose_plus_data"); } catch (...) {}
    return "windrose_plus_data";
}

static void file_log(const std::string& s)
{
    try
    {
        auto out_dir = resolve_data_dir() / "native_rt_export";
        std::filesystem::create_directories(out_dir);
        std::ofstream f(out_dir / "rt_native_exporter.log", std::ios::app);
        f << s << "\n";
    }
    catch (...) {}
}

class RTNativeExporter : public CppUserModBase
{
public:
    RTNativeExporter() : CppUserModBase()
    {
        ModName = STR("RTNativeExporter");
        ModVersion = STR("0.1.0");
    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter on_unreal_init\n"));
        file_log("RTNativeExporter on_unreal_init");
        file_log("Phase 1 lifecycle OK");
    }

    auto on_update() -> void override
    {
        m_frame_count++;

        if (!m_logged_update && m_frame_count > 60)
        {
            m_logged_update = true;
            Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter on_update OK\n"));
            file_log("RTNativeExporter on_update OK");
        }
    }

private:
    int m_frame_count = 0;
    bool m_logged_update = false;
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new RTNativeExporter();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
