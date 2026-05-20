#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <String/StringType.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

using namespace RC;

static std::filesystem::path wme_mod_root()
{
    auto cwd = std::filesystem::current_path();

    auto p1 = cwd / "Mods" / "WindroseMapExtractor";
    if (std::filesystem::exists(p1))
        return p1;

    auto p2 = cwd / "ue4ss" / "Mods" / "WindroseMapExtractor";
    return p2;
}

static std::filesystem::path wme_logs_dir()
{
    auto p = wme_mod_root() / "Logs";
    std::filesystem::create_directories(p);
    return p;
}

static void wme_log(const std::string& msg)
{
    auto log_path = wme_logs_dir() / "windrose_map_extractor.log";
    std::ofstream f(log_path, std::ios::app);
    f << msg << "\n";
    f.flush();
}

static void wme_write_heartbeat(const std::string& msg)
{
    auto path = wme_logs_dir() / "heartbeat.txt";
    std::ofstream f(path, std::ios::app);
    f << msg << "\n";
    f.flush();
}

static void wme_start_worker()
{
    static std::atomic<bool> started{false};

    bool expected = false;
    if (!started.compare_exchange_strong(expected, true))
    {
        Output::send<LogLevel::Verbose>(STR("[WME] worker already started\n"));
        return;
    }

    Output::send<LogLevel::Verbose>(STR("[WME] worker starting\n"));
    wme_log("WindroseMapExtractor v1.1 worker starting");

    std::thread([]()
    {
        Output::send<LogLevel::Verbose>(STR("[WME] worker thread entered\n"));
        wme_log("worker thread entered");

        for (int i = 1; i <= 3; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(30));

            std::ostringstream ss;
            ss << "heartbeat " << i << " after " << (i * 30) << " seconds";

            Output::send<LogLevel::Verbose>(STR("[WME] heartbeat\n"));
            wme_log(ss.str());
            wme_write_heartbeat(ss.str());
        }

        Output::send<LogLevel::Verbose>(STR("[WME] object count snapshot starting\n"));
        wme_log("object count snapshot starting");

        int scanned = 0;

        RC::Unreal::UObjectGlobals::ForEachUObject(
            [&](RC::Unreal::UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
            {
                if (!o) return RC::LoopAction::Continue;

                scanned++;

                if ((scanned % 25000) == 0)
                {
                    Output::send<LogLevel::Verbose>(STR("[WME] object count progress\n"));
                }

                if (scanned >= 300000)
                {
                    Output::send<LogLevel::Verbose>(STR("[WME] object count limit hit\n"));
                    return RC::LoopAction::Break;
                }

                return RC::LoopAction::Continue;
            }
        );

        {
            auto path = wme_logs_dir() / "object_count.txt";
            std::ofstream f(path, std::ios::app);
            f << "scanned_objects=" << scanned << "\n";
            f.flush();
        }

        std::ostringstream done;
        done << "object count snapshot done scanned_objects=" << scanned;
        wme_log(done.str());
        Output::send<LogLevel::Verbose>(STR("[WME] object count snapshot done\n"));

        wme_log("worker finished");
        Output::send<LogLevel::Verbose>(STR("[WME] worker finished\n"));
    }).detach();

    Output::send<LogLevel::Verbose>(STR("[WME] worker detached\n"));
}

class WindroseMapExtractor : public CppUserModBase
{
public:
    WindroseMapExtractor() : CppUserModBase()
    {
        ModName = STR("WindroseMapExtractor");
        ModVersion = STR("1.1.0");
    }

    ~WindroseMapExtractor() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[WME] WindroseMapExtractor v1.1 on_unreal_init\n"));
        wme_log("WindroseMapExtractor v1.1 on_unreal_init");
        wme_start_worker();
    }

    auto on_update() -> void override
    {
        return;
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new WindroseMapExtractor();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
