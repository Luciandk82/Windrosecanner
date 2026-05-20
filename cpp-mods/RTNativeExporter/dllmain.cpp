#define NOMINMAX

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <windows.h>
#include <Psapi.h>
#include <tlhelp32.h>

#include <fstream>
#include <thread>
#include <chrono>
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

/* removed stale phase text by PHASE 9J CLEANROOM */
}






















































































































































































static void scan_render_targets()
{
    // PHASE 9J CLEANROOM: legacy scan_render_targets disabled.
    // Phase 9J runs from constructor only and does not use old Phase 7/8/9H scan paths.
}


// BEGIN PHASE9J_CLEANROOM_TG_DUMP

static uint8_t phase9j_u8(uint8_t* base, size_t off)
{
    uint8_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint16_t phase9j_u16(uint8_t* base, size_t off)
{
    uint16_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint32_t phase9j_u32(uint8_t* base, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static int32_t phase9j_i32(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static float phase9j_f32(uint8_t* base, size_t off)
{
    float v = 0.0f;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase9j_ptr(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static bool phase9j_probably_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xffffffffffffffffULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static bool phase9j_small_count(uint32_t v)
{
    return v > 0 && v < 20000000U;
}

static bool phase9j_name_hits_tg(const std::string& name, const std::string& path, const std::string& cls)
{
    return name.find("TerrainGenerator") != std::string::npos ||
           path.find("TerrainGenerator") != std::string::npos ||
           cls.find("TerrainGenerator") != std::string::npos ||
           name.find("VolumizationSubsystem") != std::string::npos ||
           path.find("VolumizationSubsystem") != std::string::npos ||
           cls.find("VolumizationSubsystem") != std::string::npos ||
           path.find("R5TerrainGenerator") != std::string::npos ||
           path.find("Volumization") != std::string::npos;
}

static std::string phase9j_hex_bytes(uint8_t* p, size_t n)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t b = 0;
        std::memcpy(&b, p + i, 1);
        out.push_back(hex[(b >> 4) & 0xf]);
        out.push_back(hex[b & 0xf]);
    }
    return out;
}

struct Phase9JTGCandidate
{
    uintptr_t base = 0;
    std::string source;
    std::string owner;
    int score = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uintptr_t height_base = 0;
    uint32_t height_count = 0;
    uintptr_t island_df_base = 0;
    uint32_t island_df_count = 0;
    uintptr_t biomedf_base = 0;
    uint32_t biomedf_count = 0;
    uintptr_t biomeid_base = 0;
    uint32_t biomeid_count = 0;
    uintptr_t subbiome_base = 0;
    uint32_t subbiome_count = 0;
    uintptr_t biomeweights_owner = 0;
    uint32_t biomeweights_count = 0;
};

static Phase9JTGCandidate phase9j_eval_tg_candidate(uintptr_t base_addr, const std::string& source, const std::string& owner)
{
    Phase9JTGCandidate c;
    c.base = base_addr;
    c.source = source;
    c.owner = owner;
    if (!phase9j_probably_ptr(base_addr)) return c;

    uint8_t* base = reinterpret_cast<uint8_t*>(base_addr);
    c.width = phase9j_u32(base, 0x10);
    c.height = phase9j_u32(base, 0x14);
    c.height_base = phase9j_ptr(base, 0x118);
    c.height_count = phase9j_u32(base, 0x120);
    c.island_df_base = phase9j_ptr(base, 0x140);
    c.island_df_count = phase9j_u32(base, 0x148);
    c.biomedf_base = phase9j_ptr(base, 0x150);
    c.biomedf_count = phase9j_u32(base, 0x158);
    c.biomeid_base = phase9j_ptr(base, 0x160);
    c.biomeid_count = phase9j_u32(base, 0x168);
    c.subbiome_base = phase9j_ptr(base, 0x170);
    c.subbiome_count = phase9j_u32(base, 0x178);
    c.biomeweights_owner = phase9j_ptr(base, 0x180);
    c.biomeweights_count = phase9j_u32(base, 0x188);

    if (c.width > 0 && c.width < 100000 && c.height > 0 && c.height < 100000) c.score += 3;
    if (phase9j_probably_ptr(c.height_base) && phase9j_small_count(c.height_count)) c.score += 3;
    if (phase9j_probably_ptr(c.island_df_base) && phase9j_small_count(c.island_df_count)) c.score += 5;
    if (phase9j_probably_ptr(c.biomedf_base) && c.biomedf_count > 0 && c.biomedf_count < 4096) c.score += 6;
    if (phase9j_probably_ptr(c.biomeid_base) && phase9j_small_count(c.biomeid_count)) c.score += 3;
    if (phase9j_probably_ptr(c.subbiome_base) && phase9j_small_count(c.subbiome_count)) c.score += 3;
    if (phase9j_probably_ptr(c.biomeweights_owner) && c.biomeweights_count > 0 && c.biomeweights_count < 4096) c.score += 6;

    return c;
}

static void phase9j_write_samples(std::ofstream& out, int run, int delay_seconds, const std::string& label, uintptr_t base, uint32_t count, uint32_t elem_size, uint32_t max_rows)
{
    if (!phase9j_probably_ptr(base) || count == 0 || elem_size == 0) return;
    uint32_t n = count < max_rows ? count : max_rows;
    for (uint32_t i = 0; i < n; ++i)
    {
        uintptr_t addr = base + static_cast<uintptr_t>(i) * elem_size;
        uint8_t* p = reinterpret_cast<uint8_t*>(addr);
        uint32_t u32 = 0;
        int32_t i32 = 0;
        float f32 = 0.0f;
        if (elem_size >= 4)
        {
            std::memcpy(&u32, p, 4);
            std::memcpy(&i32, p, 4);
            std::memcpy(&f32, p, 4);
        }
        out << run << "," << delay_seconds << ",\"" << label << "\"," << i
            << ",\"0x" << std::hex << addr << std::dec << "\"," << u32 << "," << i32 << "," << f32
            << ",\"" << phase9j_hex_bytes(p, elem_size > 32 ? 32 : elem_size) << "\"\n";
    }
}

static void write_phase9j_tg_datastructure_dump_snapshot(int run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream summary(out / "phase9j_summary.txt", std::ios::app);
    std::ofstream candidates_csv(out / "phase9j_tg_candidates.csv", std::ios::app);
    std::ofstream fields_csv(out / "phase9j_tg_fields.csv", std::ios::app);
    std::ofstream island_csv(out / "phase9j_island_dfmap_u16.csv", std::ios::app);
    std::ofstream biomedf_csv(out / "phase9j_biomedf_records.csv", std::ios::app);
    std::ofstream biomedf_payload_csv(out / "phase9j_biomedf_payload_u16.csv", std::ios::app);
    std::ofstream weights_csv(out / "phase9j_biomeweights_records.csv", std::ios::app);
    std::ofstream weights_payload_csv(out / "phase9j_biomeweights_payload_i32.csv", std::ios::app);
    std::ofstream samples_csv(out / "phase9j_core_array_samples.csv", std::ios::app);
    std::ofstream notes(out / "phase9j_notes.txt", std::ios::app);

    if (run == 1)
    {
        candidates_csv << "run,delay_seconds,base,source,owner,score,width,height,height_base,height_count,island_df_base,island_df_count,biomedf_base,biomedf_count,biomeid_base,biomeid_count,subbiome_base,subbiome_count,biomeweights_owner,biomeweights_count\n";
        fields_csv << "run,delay_seconds,best_base,offset,name,raw_hex,u32,i32,f32,ptr\n";
        island_csv << "run,delay_seconds,best_base,index,value\n";
        biomedf_csv << "run,delay_seconds,best_base,index,record_addr,x,y,w,h,payload,payload_count,raw20\n";
        biomedf_payload_csv << "run,delay_seconds,best_base,record_index,payload_index,value\n";
        weights_csv << "run,delay_seconds,best_base,index,record_addr,payload,payload_count,raw_f8\n";
        weights_payload_csv << "run,delay_seconds,best_base,record_index,payload_index,value\n";
        samples_csv << "run,delay_seconds,label,index,addr,u32,i32,f32,raw\n";
    }

    struct ObjInfo { UObject* obj; std::string name; std::string path; std::string cls; };
    std::vector<ObjInfo> objects;
    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;
            scanned++;
            std::string name = obj_name(o);
            std::string path = obj_path(o);
            std::string cls = class_name(o);
            if (phase9j_name_hits_tg(name, path, cls))
                objects.push_back({o, name, path, cls});
            return RC::LoopAction::Continue;
        }
    );

    std::vector<Phase9JTGCandidate> candidates;
    for (const auto& obj : objects)
    {
        uintptr_t obj_addr = reinterpret_cast<uintptr_t>(obj.obj);
        std::string owner = obj.cls + "|" + obj.name + "|" + obj.path;
        candidates.push_back(phase9j_eval_tg_candidate(obj_addr, "object_base", owner));

        uint8_t* b = reinterpret_cast<uint8_t*>(obj_addr);
        const size_t ptr_offsets[] = {0x30,0x40,0x50,0x60,0x70,0x80,0x90,0xa0,0xb0,0xc0,0xd0,0xe0,0xf0,0x100,0x108,0x110,0x118,0x120,0x128,0x130,0x138,0x140,0x148,0x150,0x158,0x160,0x168,0x170,0x178,0x180,0x188,0x190,0x198,0x1a0,0x1a8,0x1b0,0x1b8,0x1c0,0x1c8,0x1d0,0x1d8,0x1e0,0x1e8,0x1f0,0x1f8,0x200,0x208,0x210,0x218,0x220,0x228,0x230,0x238,0x240,0x248,0x250,0x258,0x260,0x268,0x270,0x278,0x280,0x288,0x290,0x298,0x2a0,0x2a8,0x2b0,0x2b8,0x2c0,0x2c8,0x2d0,0x2d8,0x2e0,0x2e8,0x2f0,0x2f8,0x300,0x308,0x310,0x318,0x320,0x328,0x330,0x338,0x340,0x348,0x350,0x358,0x360};
        for (size_t off : ptr_offsets)
        {
            uintptr_t ptr = phase9j_ptr(b, off);
            if (!phase9j_probably_ptr(ptr)) continue;
            candidates.push_back(phase9j_eval_tg_candidate(ptr, "object_ptr_0x" + std::to_string(static_cast<unsigned long long>(off)), owner));
        }
    }

    Phase9JTGCandidate best;
    for (const auto& c : candidates)
    {
        candidates_csv << run << "," << delay_seconds << ",\"0x" << std::hex << c.base << std::dec << "\",\"" << c.source << "\",\"" << c.owner << "\"," << c.score << "," << c.width << "," << c.height
                       << ",\"0x" << std::hex << c.height_base << std::dec << "\"," << c.height_count
                       << ",\"0x" << std::hex << c.island_df_base << std::dec << "\"," << c.island_df_count
                       << ",\"0x" << std::hex << c.biomedf_base << std::dec << "\"," << c.biomedf_count
                       << ",\"0x" << std::hex << c.biomeid_base << std::dec << "\"," << c.biomeid_count
                       << ",\"0x" << std::hex << c.subbiome_base << std::dec << "\"," << c.subbiome_count
                       << ",\"0x" << std::hex << c.biomeweights_owner << std::dec << "\"," << c.biomeweights_count << "\n";
        if (c.score > best.score) best = c;
    }

    int island_rows = 0, biomedf_rows = 0, biomedf_payload_rows = 0, weights_rows = 0, weights_payload_rows = 0;

    if (best.score > 0 && phase9j_probably_ptr(best.base))
    {
        uint8_t* b = reinterpret_cast<uint8_t*>(best.base);
        struct F { size_t off; const char* name; } fields[] = {
            {0x10,"width"},{0x14,"height"},{0x118,"Height_base"},{0x120,"Height_count"},{0x140,"IslandDFMap_base"},{0x148,"IslandDFMap_count"},{0x150,"BiomeDFMap_base"},{0x158,"BiomeDFMap_count"},{0x160,"BiomeID_base"},{0x168,"BiomeID_count"},{0x170,"SubBiomeID_base"},{0x178,"SubBiomeID_count"},{0x180,"BiomeWeights_owner"},{0x188,"BiomeWeights_count"},{0x290,"PackedHeightmapA_base"},{0x2a0,"PackedHeightmapA_count"},{0x330,"PackedHeightmapB_width"},{0x334,"PackedHeightmapB_height"},{0x338,"PackedHeightmapB_base"},{0x340,"PackedHeightmapB_count"}
        };
        for (const auto& f : fields)
        {
            uintptr_t raw = phase9j_ptr(b, f.off);
            fields_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\",\"0x" << std::hex << f.off << std::dec << "\",\"" << f.name << "\",\"0x" << std::hex << raw << std::dec << "\"," << phase9j_u32(b,f.off) << "," << phase9j_i32(b,f.off) << "," << phase9j_f32(b,f.off) << ",\"0x" << std::hex << raw << std::dec << "\"\n";
        }

        if (phase9j_probably_ptr(best.island_df_base) && best.island_df_count > 0 && best.island_df_count < 5000000)
        {
            uint32_t n = best.island_df_count < 200000 ? best.island_df_count : 200000;
            uint16_t* arr = reinterpret_cast<uint16_t*>(best.island_df_base);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint16_t v = 0;
                std::memcpy(&v, arr + i, sizeof(v));
                island_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\"," << i << "," << v << "\n";
                island_rows++;
            }
        }

        if (phase9j_probably_ptr(best.biomedf_base) && best.biomedf_count > 0 && best.biomedf_count < 4096)
        {
            uint32_t n = best.biomedf_count < 512 ? best.biomedf_count : 512;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t rec = best.biomedf_base + static_cast<uintptr_t>(i) * 0x20ULL;
                uint8_t* rb = reinterpret_cast<uint8_t*>(rec);
                int32_t x = phase9j_i32(rb, 0x0);
                int32_t y = phase9j_i32(rb, 0x4);
                uint16_t w = phase9j_u16(rb, 0x8);
                uint16_t h = phase9j_u16(rb, 0xa);
                uintptr_t payload = phase9j_ptr(rb, 0x10);
                uint32_t payload_count = phase9j_u32(rb, 0x18);
                biomedf_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\"," << i << ",\"0x" << std::hex << rec << std::dec << "\"," << x << "," << y << "," << w << "," << h << ",\"0x" << std::hex << payload << std::dec << "\"," << payload_count << ",\"" << phase9j_hex_bytes(rb, 0x20) << "\"\n";
                biomedf_rows++;
                if (phase9j_probably_ptr(payload) && payload_count > 0 && payload_count < 2000000)
                {
                    uint32_t pn = payload_count < 4096 ? payload_count : 4096;
                    uint16_t* pa = reinterpret_cast<uint16_t*>(payload);
                    for (uint32_t j = 0; j < pn; ++j)
                    {
                        uint16_t v = 0;
                        std::memcpy(&v, pa + j, sizeof(v));
                        biomedf_payload_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\"," << i << "," << j << "," << v << "\n";
                        biomedf_payload_rows++;
                    }
                }
            }
        }

        if (phase9j_probably_ptr(best.biomeweights_owner) && best.biomeweights_count > 0 && best.biomeweights_count < 4096)
        {
            uint32_t n = best.biomeweights_count < 512 ? best.biomeweights_count : 512;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t rec = best.biomeweights_owner + static_cast<uintptr_t>(i) * 0xf8ULL;
                uint8_t* rb = reinterpret_cast<uint8_t*>(rec);
                uintptr_t payload = phase9j_ptr(rb, 0xe8);
                uint32_t payload_count = phase9j_u32(rb, 0xf0);
                weights_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\"," << i << ",\"0x" << std::hex << rec << std::dec << "\",\"0x" << std::hex << payload << std::dec << "\"," << payload_count << ",\"" << phase9j_hex_bytes(rb, 0xf8) << "\"\n";
                weights_rows++;
                if (phase9j_probably_ptr(payload) && payload_count > 0 && payload_count < 2000000)
                {
                    uint32_t pn = payload_count < 4096 ? payload_count : 4096;
                    int32_t* pa = reinterpret_cast<int32_t*>(payload);
                    for (uint32_t j = 0; j < pn; ++j)
                    {
                        int32_t v = 0;
                        std::memcpy(&v, pa + j, sizeof(v));
                        weights_payload_csv << run << "," << delay_seconds << ",\"0x" << std::hex << best.base << std::dec << "\"," << i << "," << j << "," << v << "\n";
                        weights_payload_rows++;
                    }
                }
            }
        }

        phase9j_write_samples(samples_csv, run, delay_seconds, "Height_f32", best.height_base, best.height_count, 4, 1024);
        phase9j_write_samples(samples_csv, run, delay_seconds, "BiomeID_u8", best.biomeid_base, best.biomeid_count, 1, 2048);
        phase9j_write_samples(samples_csv, run, delay_seconds, "SubBiomeID_u8", best.subbiome_base, best.subbiome_count, 1, 2048);
        phase9j_write_samples(samples_csv, run, delay_seconds, "PackedHeightmapA_u16", phase9j_ptr(b, 0x298), phase9j_u32(b, 0x2a0), 2, 2048);
        phase9j_write_samples(samples_csv, run, delay_seconds, "PackedHeightmapB_u16", phase9j_ptr(b, 0x338), phase9j_u32(b, 0x340), 2, 2048);
    }

    notes << "\n===== PHASE 9J CLEANROOM NOTES RUN " << run << " =====\n";
    notes << "Cleanroom build: old generated phase blocks removed; old runtime scan disabled; only Phase 9J constructor start is active.\n";
    notes << "Ghidra-derived TG offsets: IslandDFMap 0x140/0x148 u16, BiomeDFMap 0x150/0x158 stride 0x20, BiomeWeights 0x180/0x188 stride 0xF8, Height 0x118/0x120, BiomeID 0x160/0x168, SubBiomeID 0x170/0x178.\n";
    notes << "Read-only: no ProcessEvent, no GPU readback, no texture writes.\n";

    summary << "\n===== PHASE 9J CLEANROOM RUN " << run << " DELAY " << delay_seconds << "s =====\n";
    summary << "scanned_objects=" << scanned << "\n";
    summary << "terrain_named_objects=" << objects.size() << "\n";
    summary << "candidate_rows=" << candidates.size() << "\n";
    summary << "best_score=" << best.score << "\n";
    summary << "best_base=0x" << std::hex << best.base << std::dec << "\n";
    summary << "best_owner=" << best.owner << "\n";
    summary << "island_rows=" << island_rows << "\n";
    summary << "biomedf_rows=" << biomedf_rows << "\n";
    summary << "biomedf_payload_rows=" << biomedf_payload_rows << "\n";
    summary << "weights_rows=" << weights_rows << "\n";
    summary << "weights_payload_rows=" << weights_payload_rows << "\n";
    if (best.score >= 10 && (island_rows > 0 || biomedf_rows > 0 || weights_rows > 0))
        summary << "DECISION=tg_datastructure_dumped\n";
    else
        summary << "DECISION=no_valid_tg_datastructure_dumped\n";

    file_log("Phase 9J cleanroom done run=" + std::to_string(run) +
             " best_score=" + std::to_string(best.score) +
             " terrain_named_objects=" + std::to_string(objects.size()) +
             " candidates=" + std::to_string(candidates.size()) +
             " island_rows=" + std::to_string(island_rows) +
             " biomedf_rows=" + std::to_string(biomedf_rows) +
             " weights_rows=" + std::to_string(weights_rows));
}

static void start_phase9j_tg_datastructure_dump()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("PHASE 9J CLEANROOM BUILD active - TG datastructure dump started");

    std::thread([]()
    {
        const int delays[] = {180, 360};
        int previous = 0;
        for (int i = 0; i < 2; ++i)
        {
            int target = delays[i];
            int delta = target - previous;
            previous = target;
            if (delta > 0)
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            write_phase9j_tg_datastructure_dump_snapshot(i + 1, target);
        }
        file_log("PHASE 9J CLEANROOM BUILD finished");
    }).detach();
}

// END PHASE9J_CLEANROOM_TG_DUMP


class RTNativeExporter : public CppUserModBase
{
public:
    RTNativeExporter() : CppUserModBase()
    {
        ModName = STR("RTNativeExporter");
        ModVersion = STR("1.35.0");
        start_phase9j_tg_datastructure_dump();

    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter v1.35.0 on_unreal_init\n"));
        file_log("RTNativeExporter v1.35.0 on_unreal_init");

        start_phase9j_tg_datastructure_dump();
        file_log("PHASE 9J START CALL EXECUTED");

    }

    auto on_update() -> void override
    {
        // PHASE 9J CLEANROOM: on_update intentionally disabled.
        // No legacy periodic scans or old phase callbacks are allowed in this build.
        return;

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
