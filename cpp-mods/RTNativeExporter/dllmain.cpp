#define NOMINMAX

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <windows.h>

#include <fstream>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <cstdint>

using namespace RC;
using namespace RC::Unreal;

static std::filesystem::path resolve_data_dir()
{
    std::filesystem::path candidates[] = {
        "../../../windrose_plus_data",
        "windrose_plus_data",
    };

    for (auto& p : candidates)
    {
        try { if (std::filesystem::exists(p)) return p; } catch (...) {}
    }

    try { std::filesystem::create_directories("windrose_plus_data"); } catch (...) {}
    return "windrose_plus_data";
}

static std::filesystem::path out_dir()
{
    auto p = resolve_data_dir() / "native_rt_export";
    try { std::filesystem::create_directories(p); } catch (...) {}
    return p;
}

static void file_log(const std::string& s)
{
    try
    {
        std::ofstream f(out_dir() / "rt_native_exporter.log", std::ios::app);
        f << s << "\n";
    }
    catch (...) {}
}

static std::string wide_to_utf8(std::wstring_view w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);

    for (char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                }
                else out += c;
        }
    }

    return out;
}

static std::string obj_name(UObject* o)
{
    if (!o) return "";
    try { return wide_to_utf8(o->GetName()); } catch (...) { return ""; }
}

static std::string obj_path(UObject* o)
{
    if (!o) return "";
    try { return wide_to_utf8(o->GetPathName()); } catch (...) { return ""; }
}

static std::string class_name(UObject* o)
{
    if (!o) return "";
    try
    {
        auto* c = o->GetClassPrivate();
        return c ? wide_to_utf8(c->GetName()) : "";
    }
    catch (...) { return ""; }
}











static bool text_contains_any(const std::string& path)
{
    static const std::vector<std::string> tokens = {
        "RT_",
        "Landscape",
        "Biome",
        "SubBiome",
        "DistanceField",
        "Voronoi",
        "Map",
        "Capture",
        "Fog",
        "Terrain",
        "Volum",
        "Height",
        "RenderTarget",
        "Genlandia",
        "Transient"
    };

    for (const auto& t : tokens)
    {
        if (path.find(t) != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

static bool name_contains_target(const std::string& path)
{
    return text_contains_any(path);
}

static std::string outer_chain(UObject* o, int max_depth = 8)
{
    std::string out;

    UObject* cur = nullptr;

    try
    {
        cur = o ? o->GetOuterPrivate() : nullptr;
    }
    catch (...)
    {
        cur = nullptr;
    }

    int depth = 0;

    while (cur && depth < max_depth)
    {
        if (!out.empty())
        {
            out += " <- ";
        }

        out += obj_path(cur);

        try
        {
            cur = cur->GetOuterPrivate();
        }
        catch (...)
        {
            cur = nullptr;
        }

        depth++;
    }

    return out;
}

static bool is_rt_class_name(const std::string& cls)
{
    return cls.find("TextureRenderTarget2D") != std::string::npos ||
           cls.find("TextureRenderTarget2DArray") != std::string::npos;
}


template <typename T>
static bool read_prop_value(UObject* o, const wchar_t* prop_name, T& out)
{
    if (!o) return false;

    try
    {
        FProperty* p = o->GetPropertyByNameInChain(prop_name);
        if (!p) return false;

        T* v = p->ContainerPtrToValuePtr<T>(o);
        if (!v) return false;

        out = *v;
        return true;
    }
    catch (...) { return false; }
}

static UObject* read_object_prop(UObject* o, const wchar_t* prop_name)
{
    if (!o) return nullptr;

    try
    {
        FProperty* p = o->GetPropertyByNameInChain(prop_name);
        if (!p) return nullptr;

        UObject** pp = p->ContainerPtrToValuePtr<UObject*>(o);
        if (!pp) return nullptr;

        return *pp;
    }
    catch (...) { return nullptr; }
}

static std::string read_prop_as_json(UObject* o, const wchar_t* prop_name)
{
    int32_t i32 = 0;
    uint32_t u32 = 0;
    bool b = false;

    if (read_prop_value<int32_t>(o, prop_name, i32)) return std::to_string(i32);
    if (read_prop_value<uint32_t>(o, prop_name, u32)) return std::to_string(u32);
    if (read_prop_value<bool>(o, prop_name, b)) return b ? "true" : "false";

    UObject* ref = read_object_prop(o, prop_name);
    if (ref)
    {
        return std::string("{\"object\":\"") + json_escape(obj_path(ref)) +
               "\",\"class\":\"" + json_escape(class_name(ref)) + "\"}";
    }

    return "null";
}

static void write_prop(std::ofstream& json, UObject* o, const char* json_name, const wchar_t* prop_name, bool comma=true)
{
    json << "    \"" << json_name << "\":" << read_prop_as_json(o, prop_name);
    if (comma) json << ",";
    json << "\n";
}



static bool is_map_capture_target(const std::string& path)
{
    return path.find("RT_MapCapture") != std::string::npos ||
           path.find("RT_MapFog") != std::string::npos;
}

static void write_pixel_export_probe(UObject* o)
{
    if (!o) return;

    std::string path = obj_path(o);
    if (!is_map_capture_target(path)) return;

    auto out = out_dir();

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    std::string safe_name = obj_name(o);
    for (char& c : safe_name)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    }

    std::ofstream f(out / (safe_name + "_pixel_probe.txt"), std::ios::app);
    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";
    f << "note=Phase4 confirms candidate for native pixel export. Actual ReadPixels not enabled yet.\n";
    f << "\n";

    file_log("Phase 4 pixel probe candidate: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
}



static bool function_name_interesting(const std::string& n)
{
    static const std::vector<std::string> tokens = {
        "RenderTarget",
        "Resource",
        "Read",
        "Pixel",
        "Surface",
        "Texture",
        "Size",
        "Export",
        "Construct",
        "Update",
        "Resolve"
    };

    for (const auto& t : tokens)
    {
        if (n.find(t) != std::string::npos) return true;
    }

    return false;
}

static void write_resource_access_probe(UObject* o)
{
    if (!o) return;

    std::string path = obj_path(o);
    if (!is_map_capture_target(path)) return;

    auto out = out_dir();

    std::string safe_name = obj_name(o);
    for (char& c : safe_name)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    }

    std::ofstream f(out / (safe_name + "_resource_probe.txt"), std::ios::app);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

    UObject* source = read_object_prop(o, STR("Source"));
    UObject* platform = read_object_prop(o, STR("PlatformData"));
    UObject* resource = read_object_prop(o, STR("Resource"));

    f << "Source=" << obj_path(source) << " class=" << class_name(source) << "\n";
    f << "PlatformData=" << obj_path(platform) << " class=" << class_name(platform) << "\n";
    f << "Resource=" << obj_path(resource) << " class=" << class_name(resource) << "\n";

    f << "class_children_interesting=skipped\n";
    f << "note2=Skipped FField iteration because GetNext is private in current UE4SS headers.\n";

    f << "note=Phase5A is a safe native resource/function probe only. No ReadPixels call yet.\n";
    f << "\n";

    file_log("Phase 5A resource probe candidate: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
}

static void scan_render_targets()
{
    file_log("Phase 5A scan_render_targets started");
    Output::send<LogLevel::Verbose>(STR("[RTN] Phase 5A scan_render_targets started\n"));

    auto out = out_dir();
    std::ofstream json(out / "rt_native_runtime_scan.json");

    json << "{\n";
    json << "  \"version\": 2,\n";
    json << "  \"note\": \"Native UE4SS RT metadata scan. Pixel export not enabled yet.\",\n";
    json << "  \"targets\": [\n";

    bool first = true;
    int found = 0;
    int scanned = 0;

    UObjectGlobals::ForEachUObject([&](UObject* o, int32, int32) -> RC::LoopAction {
        if (!o) return RC::LoopAction::Continue;

        scanned++;

        std::string path = obj_path(o);
        if (!name_contains_target(path)) return RC::LoopAction::Continue;

        std::string cls = class_name(o);

        if (!is_rt_class_name(cls))
        {
            return RC::LoopAction::Continue;
        }

        int32_t sx_tmp = 0;
        int32_t sy_tmp = 0;
        read_prop_value<int32_t>(o, STR("SizeX"), sx_tmp);
        read_prop_value<int32_t>(o, STR("SizeY"), sy_tmp);

        if (!text_contains_any(path) && sx_tmp <= 1 && sy_tmp <= 1)
        {
            return RC::LoopAction::Continue;
        }

        found++;

        write_pixel_export_probe(o);
        write_resource_access_probe(o);

        if (!first) json << ",\n";
        first = false;

        json << "  {\n";
        json << "    \"name\":\"" << json_escape(obj_name(o)) << "\",\n";
        json << "    \"path\":\"" << json_escape(path) << "\",\n";
        json << "    \"class\":\"" << json_escape(cls) << "\",\n";
        json << "    \"address\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\",\n";
        json << "    \"outer_chain\":\"" << json_escape(outer_chain(o)) << "\",\n";
        json << "    \"outer_chain\":\"" << json_escape(outer_chain(o)) << "\",\n";

        write_prop(json, o, "SizeX", STR("SizeX"));
        write_prop(json, o, "SizeY", STR("SizeY"));
        write_prop(json, o, "Slices", STR("Slices"));
        write_prop(json, o, "OverrideFormat", STR("OverrideFormat"));
        write_prop(json, o, "RenderTargetFormat", STR("RenderTargetFormat"));
        write_prop(json, o, "PixelFormat", STR("PixelFormat"));
        write_prop(json, o, "Format", STR("Format"));
        write_prop(json, o, "SRGB", STR("SRGB"));
        write_prop(json, o, "bHDR", STR("bHDR"));
        write_prop(json, o, "bForceLinearGamma", STR("bForceLinearGamma"));
        write_prop(json, o, "Filter", STR("Filter"));
        write_prop(json, o, "LODGroup", STR("LODGroup"));
        write_prop(json, o, "CompressionSettings", STR("CompressionSettings"));
        write_prop(json, o, "NeverStream", STR("NeverStream"));
        write_prop(json, o, "Source", STR("Source"));
        write_prop(json, o, "PlatformData", STR("PlatformData"));
        write_prop(json, o, "Resource", STR("Resource"), false);

        json << "  }";

        return RC::LoopAction::Continue;
    });

    json << "\n  ],\n";
    json << "  \"scanned_objects\":" << scanned << ",\n";
    json << "  \"found_targets\":" << found << ",\n";
    json << "  \"pixel_export_enabled\": false\n";
    json << "}\n";

    json.close();

    std::ofstream done(out / "rt_native_runtime_scan_done");
    done << "ok\n";
    done.close();

    file_log("Phase 5A scan_render_targets done. found=" + std::to_string(found) + " scanned=" + std::to_string(scanned));
    Output::send<LogLevel::Verbose>(STR("[RTN] Phase 5A done. found={} scanned={}\n"), found, scanned);
}

class RTNativeExporter : public CppUserModBase
{
public:
    RTNativeExporter() : CppUserModBase()
    {
        ModName = STR("RTNativeExporter");
        ModVersion = STR("0.5.0");
    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter v0.5 on_unreal_init\n"));
        file_log("RTNativeExporter v0.5 on_unreal_init");
    }

    auto on_update() -> void override
    {
        m_frame_count++;

        // Retry-style native scan: wait a bit for world/assets, then scan every ~30 sec up to 12 times.
        if (m_attempts_done >= 12) return;
        if (m_frame_count < 300) return;
        if ((m_frame_count - 300) % 1800 != 0) return;

        m_attempts_done++;

        file_log("Native scan attempt " + std::to_string(m_attempts_done));
        scan_render_targets();
    }

private:
    int m_frame_count = 0;
    int m_attempts_done = 0;
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new RTNativeExporter();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}
