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

    file_log("Phase 7A resource probe candidate: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
}


// BEGIN PHASE5B_NATIVE_MEMORY_PROBE
static bool mem_readable(const void* a, size_t len)
{
    if (!a || len == 0) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    auto start = reinterpret_cast<uintptr_t>(a);
    auto end = start + len;
    auto region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    auto region_end = region_start + mbi.RegionSize;

    return end >= start && start >= region_start && end <= region_end;
}

static bool looks_like_ptr(uint64_t v)
{
    return v > 0x10000ULL && v < 0x7FFFFFFFFFFFULL;
}

static void write_native_memory_probe(UObject* o)
{
    if (!o) return;

    std::string path = obj_path(o);
    if (!is_map_capture_target(path)) return;

    std::string safe_name = obj_name(o);
    for (char& c : safe_name)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    }

    auto out = out_dir();
    std::ofstream f(out / (safe_name + "_native_memory_probe.txt"), std::ios::app);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);

    if (!mem_readable(base, 0x400))
    {
        f << "object_memory_readable=false\n\n";
        return;
    }

    f << "object_memory_readable=true\n";

    uint64_t vtable = 0;
    if (mem_readable(base, sizeof(uint64_t)))
    {
        vtable = *reinterpret_cast<uint64_t*>(base);
    }

    f << "vtable=0x" << std::hex << vtable << std::dec << "\n";
    f << "pointer_candidates:\n";

    for (size_t off = 0; off < 0x400; off += 8)
    {
        if (!mem_readable(base + off, sizeof(uint64_t))) continue;

        uint64_t val = *reinterpret_cast<uint64_t*>(base + off);
        if (!looks_like_ptr(val)) continue;

        bool target_readable = mem_readable(reinterpret_cast<void*>(val), 16);

        f << "  off=0x" << std::hex << off
          << " val=0x" << val
          << " readable=" << std::dec << (target_readable ? 1 : 0)
          << "\n";
    }

    f << "note=Phase5B only scans native memory pointer candidates. No method call and no ReadPixels yet.\n\n";

    file_log("Phase 7A native memory probe: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
}
// END PHASE5B_NATIVE_MEMORY_PROBE



// BEGIN PHASE5C_DEEP_POINTER_PROBE
static void dump_ptr_block(std::ofstream& f, const char* label, uint8_t* base, size_t off)
{
    if (!mem_readable(base + off, sizeof(uint64_t))) return;

    uint64_t ptr = *reinterpret_cast<uint64_t*>(base + off);

    f << label << " off=0x" << std::hex << off
      << " ptr=0x" << ptr
      << " readable=" << std::dec << (mem_readable(reinterpret_cast<void*>(ptr), 0x100) ? 1 : 0)
      << "\n";

    if (!looks_like_ptr(ptr)) return;
    uint8_t* p = reinterpret_cast<uint8_t*>(ptr);
    if (!mem_readable(p, 0x100)) return;

    f << "  qwords:\n";
    for (size_t i = 0; i < 0x100; i += 8)
    {
        if (!mem_readable(p + i, 8)) continue;
        uint64_t v = *reinterpret_cast<uint64_t*>(p + i);
        f << "    +0x" << std::hex << i << " = 0x" << v << std::dec;

        if (v == 2048 || v == 2048ULL)
        {
            f << "  <-- 2048";
        }

        if (v == 4194304 || v == 16777216)
        {
            f << "  <-- possible buffer size";
        }

        f << "\n";
    }

    f << "  dwords:\n";
    for (size_t i = 0; i < 0x100; i += 4)
    {
        if (!mem_readable(p + i, 4)) continue;
        uint32_t v = *reinterpret_cast<uint32_t*>(p + i);
        if (v == 2048 || v == 1024 || v == 512 || v == 256 || v == 4096 || v == 4194304 || v == 16777216)
        {
            f << "    +0x" << std::hex << i << " = " << std::dec << v << "  <-- interesting\n";
        }
    }

    f << "\n";
}

static void write_deep_pointer_probe(UObject* o)
{
    if (!o) return;

    std::string path = obj_path(o);
    if (!is_map_capture_target(path)) return;

    std::string safe_name = obj_name(o);
    for (char& c : safe_name)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
    }

    auto out = out_dir();
    std::ofstream f(out / (safe_name + "_deep_pointer_probe.txt"), std::ios::app);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!mem_readable(base, 0x500))
    {
        f << "object_memory_readable=false\n\n";
        return;
    }

    f << "deep_pointer_blocks:\n";

    if (path.find("RT_MapCapture") != std::string::npos)
    {
        size_t offs[] = {0x20, 0x50, 0x80, 0xa0, 0x130, 0x300, 0x308, 0x310, 0x320, 0x330, 0x378};
        for (size_t off : offs) dump_ptr_block(f, "MapCapture", base, off);
    }
    else if (path.find("RT_MapFog") != std::string::npos)
    {
        size_t offs[] = {0x20, 0x50, 0x80, 0xa0, 0x130, 0x180, 0x188, 0x190, 0x1a0, 0x1b0, 0x300, 0x308, 0x320, 0x330, 0x360, 0x3c0};
        for (size_t off : offs) dump_ptr_block(f, "MapFog", base, off);
    }

    f << "note=Phase5C deep pointer probe. No method call and no ReadPixels yet.\n\n";

    file_log("Phase 7A deep pointer probe: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
}
// END PHASE5C_DEEP_POINTER_PROBE



// BEGIN PHASE5D_TARGETED_CHAIN_PROBE
static void dump_chain_level(std::ofstream& f, const std::string& label, uint8_t* ptr, int level)
{
    if (!ptr || !mem_readable(ptr, 0x400)) {
        f << label << " level=" << level << " readable=0\n";
        return;
    }

    f << label << " level=" << level << " ptr=0x" << std::hex << reinterpret_cast<uintptr_t>(ptr) << std::dec << "\n";

    for (size_t off = 0; off < 0x400; off += 8)
    {
        if (!mem_readable(ptr + off, 8)) continue;

        uint64_t q = *reinterpret_cast<uint64_t*>(ptr + off);
        uint32_t lo = static_cast<uint32_t>(q & 0xffffffff);
        uint32_t hi = static_cast<uint32_t>((q >> 32) & 0xffffffff);

        bool interesting =
            q == 2048 || q == 4096 || q == 4194304 || q == 16777216 ||
            lo == 2048 || hi == 2048 ||
            lo == 4096 || hi == 4096 ||
            lo == 4194304 || hi == 4194304 ||
            lo == 16777216 || hi == 16777216 ||
            looks_like_ptr(q);

        if (!interesting) continue;

        f << "  +0x" << std::hex << off
          << " q=0x" << q
          << " lo=" << std::dec << lo
          << " hi=" << hi;

        if (q == 2048 || lo == 2048 || hi == 2048) f << " <-- 2048";
        if (q == 16777216 || lo == 16777216 || hi == 16777216) f << " <-- 16MB";
        if (q == 4194304 || lo == 4194304 || hi == 4194304) f << " <-- 4MP";

        if (looks_like_ptr(q))
        {
            f << " ptr_readable=" << (mem_readable(reinterpret_cast<void*>(q), 0x80) ? 1 : 0);
        }

        f << "\n";
    }
}

static void write_targeted_chain_probe(UObject* o)
{
    if (!o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos)
    {
        file_log("Phase 7A skip non-MapCapture: " + path);
        return;
    }

    file_log("Phase 7A processing MapCapture: " + path);

    auto out = out_dir();
    std::ofstream f(out / "RT_MapCapture_targeted_chain_probe.txt", std::ios::app);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!mem_readable(base, 0x500))
    {
        f << "object_memory_readable=false\n\n";
        return;
    }

    size_t root_offsets[] = {0x20, 0x50, 0x80, 0xa0, 0x130, 0x300, 0x308, 0x310, 0x320, 0x330, 0x378};

    for (size_t root_off : root_offsets)
    {
        if (!mem_readable(base + root_off, 8)) continue;

        uint64_t p1v = *reinterpret_cast<uint64_t*>(base + root_off);
        f << "\nROOT off=0x" << std::hex << root_off << " p1=0x" << p1v << std::dec
          << " readable=" << (mem_readable(reinterpret_cast<void*>(p1v), 0x80) ? 1 : 0) << "\n";

        if (!looks_like_ptr(p1v)) continue;

        uint8_t* p1 = reinterpret_cast<uint8_t*>(p1v);
        dump_chain_level(f, "  L1", p1, 1);

        if (!mem_readable(p1, 0x400)) continue;

        for (size_t off2 = 0; off2 < 0x400; off2 += 8)
        {
            if (!mem_readable(p1 + off2, 8)) continue;

            uint64_t p2v = *reinterpret_cast<uint64_t*>(p1 + off2);
            if (!looks_like_ptr(p2v)) continue;
            if (!mem_readable(reinterpret_cast<void*>(p2v), 0x80)) continue;

            uint8_t* p2 = reinterpret_cast<uint8_t*>(p2v);

            f << "  L2 from root+0x" << std::hex << root_off << " +0x" << off2
              << " p2=0x" << p2v << std::dec << "\n";

            dump_chain_level(f, "    L2", p2, 2);
        }
    }

    f << "\nnote=Phase5D targeted chain probe for RT_MapCapture only. No ReadPixels and no memory copy export yet.\n\n";

    file_log("Phase 7A targeted chain probe RT_MapCapture size=" + std::to_string(sx) + "x" + std::to_string(sy));
}
// END PHASE5D_TARGETED_CHAIN_PROBE



// BEGIN PHASE6A_ENGINE_METHOD_DISCOVERY
static bool phase6_mem_readable(const void* a, size_t len)
{
    if (!a || len == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(a, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    auto start = reinterpret_cast<uintptr_t>(a);
    auto end = start + len;
    auto region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    auto region_end = region_start + mbi.RegionSize;
    return end >= start && start >= region_start && end <= region_end;
}

static bool phase6_page_scan_ascii(uint8_t* base, size_t size, const char* needle)
{
    if (!base || !needle) return false;
    size_t n = std::strlen(needle);
    if (n == 0 || size < n) return false;

    for (size_t i = 0; i + n <= size; ++i)
    {
        if (std::memcmp(base + i, needle, n) == 0) return true;
    }
    return false;
}

static bool phase6_page_scan_utf16(uint8_t* base, size_t size, const char* needle)
{
    if (!base || !needle) return false;
    size_t n = std::strlen(needle);
    if (n == 0 || size < n * 2) return false;

    for (size_t i = 0; i + (n * 2) <= size; ++i)
    {
        bool ok = true;
        for (size_t j = 0; j < n; ++j)
        {
            if (base[i + j * 2] != static_cast<uint8_t>(needle[j]) || base[i + j * 2 + 1] != 0)
            {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static void phase6_scan_module_for_needles(std::ofstream& f, const MODULEENTRY32W& me)
{
    const char* needles[] = {
        "GameThread_GetRenderTargetResource",
        "ReadPixels",
        "ReadLinearColorPixels",
        "RHILockTexture2D",
        "FTextureRenderTargetResource",
        "UTextureRenderTarget2D",
        "TextureRenderTarget2D",
        "FRenderTarget",
        "RenderTargetResource",
        "RHITexture",
        "FRHITexture",
        "LockTexture2D",
        "RHIMapStagingSurface",
        "ReadSurfaceData"
    };

    uint8_t* module_base = reinterpret_cast<uint8_t*>(me.modBaseAddr);
    size_t module_size = static_cast<size_t>(me.modBaseSize);

    f << "module=";
    for (wchar_t c : std::wstring(me.szModule))
    {
        if (c == 0) break;
        f << static_cast<char>(c < 128 ? c : '?');
    }
    f << " base=0x" << std::hex << reinterpret_cast<uintptr_t>(module_base)
      << " size=0x" << module_size << std::dec << "\n";

    uintptr_t cur = reinterpret_cast<uintptr_t>(module_base);
    uintptr_t end = cur + module_size;

    while (cur < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi))) break;

        uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t region_end = region_start + mbi.RegionSize;
        uintptr_t scan_start = cur > region_start ? cur : region_start;
        uintptr_t scan_end = region_end < end ? region_end : end;

        bool readable =
            mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

        if (readable && scan_end > scan_start)
        {
            uint8_t* scan_ptr = reinterpret_cast<uint8_t*>(scan_start);
            size_t scan_len = static_cast<size_t>(scan_end - scan_start);

            if (scan_len > 0 && scan_len <= 64 * 1024 * 1024)
            {
                for (const char* needle : needles)
                {
                    bool ascii_hit = phase6_page_scan_ascii(scan_ptr, scan_len, needle);
                    bool utf16_hit = phase6_page_scan_utf16(scan_ptr, scan_len, needle);

                    if (ascii_hit || utf16_hit)
                    {
                        f << "  HIT needle=" << needle
                          << " ascii=" << (ascii_hit ? 1 : 0)
                          << " utf16=" << (utf16_hit ? 1 : 0)
                          << " region=0x" << std::hex << scan_start << "-0x" << scan_end << std::dec
                          << "\n";
                    }
                }
            }
        }

        if (region_end <= cur) break;
        cur = region_end;
    }
}

static void phase6_engine_method_discovery()
{
    auto out = out_dir();
    std::ofstream f(out / "phase6_engine_method_discovery.txt", std::ios::out);

    f << "Phase 7A engine method discovery\n";
    f << "Targets: GameThread_GetRenderTargetResource, ReadPixels, ReadLinearColorPixels, RHILockTexture2D, FTextureRenderTargetResource\n";
    f << "Mode: scan loaded module memory for ASCII/UTF16 method/type strings. No calls. No ReadPixels. No GPU access.\n\n";

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
    {
        f << "CreateToolhelp32Snapshot failed\n";
        return;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (!Module32FirstW(snap, &me))
    {
        f << "Module32FirstW failed\n";
        CloseHandle(snap);
        return;
    }

    do
    {
        phase6_scan_module_for_needles(f, me);
    }
    while (Module32NextW(snap, &me));

    CloseHandle(snap);

    f << "\nDone.\n";
    file_log("Phase 7A engine method discovery wrote phase6_engine_method_discovery.txt");
}
// END PHASE6A_ENGINE_METHOD_DISCOVERY



// BEGIN PHASE6B_STRING_CONTEXT_SCANNER
static void phase6b_dump_bytes(std::ofstream& f, uint8_t* addr, size_t len)
{
    if (!addr || !phase6_mem_readable(addr, len)) return;

    for (size_t i = 0; i < len; i += 16)
    {
        f << "    0x" << std::hex << reinterpret_cast<uintptr_t>(addr + i) << ": ";
        for (size_t j = 0; j < 16 && i + j < len; ++j)
        {
            unsigned int b = addr[i + j];
            if (b < 16) f << "0";
            f << b << " ";
        }
        f << std::dec << "\n";
    }
}

static void phase6b_scan_context()
{
    auto out = out_dir();
    std::ofstream f(out / "phase6b_string_context.txt", std::ios::out);

    const char* needles[] = {
        "GameThread_GetRenderTargetResource",
        "ReadPixels",
        "ReadLinearColorPixels",
        "RHILockTexture2D",
        "FTextureRenderTargetResource",
        "UTextureRenderTarget2D",
        "RHITexture",
        "FRHITexture",
        "RHIMapStagingSurface",
        "ReadSurfaceData"
    };

    f << "Phase 7A string context scanner\n";
    f << "Goal: locate actual string addresses inside WindroseServer-Win64-Shipping.exe and dump nearby memory.\n";
    f << "No function calls. No ReadPixels. No GPU access.\n\n";

    HMODULE hExe = GetModuleHandleW(L"WindroseServer-Win64-Shipping.exe");
    if (!hExe)
    {
        hExe = GetModuleHandleW(nullptr);
    }
    if (!hExe)
    {
        f << "GetModuleHandleW(nullptr) failed\n";
        return;
    }

    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hExe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>((uint8_t*)hExe + dos->e_lfanew);

    uint8_t* base = reinterpret_cast<uint8_t*>(hExe);
    size_t size = nt->OptionalHeader.SizeOfImage;

    f << "exe_base=0x" << std::hex << reinterpret_cast<uintptr_t>(base)
      << " exe_size=0x" << size << std::dec << "\n\n";

    for (const char* needle : needles)
    {
        size_t n = std::strlen(needle);
        int hits = 0;

        f << "NEEDLE " << needle << "\n";

        uintptr_t cur = reinterpret_cast<uintptr_t>(base);
        uintptr_t end = cur + size;

        while (cur < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi))) break;

            uintptr_t rs = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t re = rs + mbi.RegionSize;
            uintptr_t ss = cur > rs ? cur : rs;
            uintptr_t se = re < end ? re : end;

            bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (readable && se > ss)
            {
                uint8_t* ptr = reinterpret_cast<uint8_t*>(ss);
                size_t len = static_cast<size_t>(se - ss);

                for (size_t i = 0; i + n <= len; ++i)
                {
                    bool ascii = std::memcmp(ptr + i, needle, n) == 0;

                    bool utf16 = false;
                    if (i + n * 2 <= len)
                    {
                        utf16 = true;
                        for (size_t j = 0; j < n; ++j)
                        {
                            if (ptr[i + j * 2] != static_cast<uint8_t>(needle[j]) || ptr[i + j * 2 + 1] != 0)
                            {
                                utf16 = false;
                                break;
                            }
                        }
                    }

                    if (ascii || utf16)
                    {
                        uint8_t* hit = ptr + i;
                        f << "  hit=" << hits
                          << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(hit)
                          << " rva=0x" << (reinterpret_cast<uintptr_t>(hit) - reinterpret_cast<uintptr_t>(base))
                          << " ascii=" << std::dec << (ascii ? 1 : 0)
                          << " utf16=" << (utf16 ? 1 : 0)
                          << "\n";

                        uintptr_t ctx_start = reinterpret_cast<uintptr_t>(hit) > 128 ? reinterpret_cast<uintptr_t>(hit) - 128 : reinterpret_cast<uintptr_t>(hit);
                        phase6b_dump_bytes(f, reinterpret_cast<uint8_t*>(ctx_start), 256);

                        f << "\n";
                        hits++;
                        if (hits >= 20) break;
                    }
                }
            }

            if (hits >= 20) break;
            if (re <= cur) break;
            cur = re;
        }

        f << "  total_limited_hits=" << hits << "\n\n";
    }

    file_log("Phase 7A wrote phase6b_string_context.txt");
}
// END PHASE6B_STRING_CONTEXT_SCANNER



// BEGIN PHASE6C_VTABLE_DIAGNOSTIC
static std::string phase6c_module_for_addr(uint64_t addr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return "";

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    if (!Module32FirstW(snap, &me))
    {
        CloseHandle(snap);
        return "";
    }

    do
    {
        uint64_t base = reinterpret_cast<uint64_t>(me.modBaseAddr);
        uint64_t end = base + static_cast<uint64_t>(me.modBaseSize);

        if (addr >= base && addr < end)
        {
            std::string out;
            for (wchar_t c : std::wstring(me.szModule))
            {
                if (!c) break;
                out.push_back(static_cast<char>(c < 128 ? c : '?'));
            }
            CloseHandle(snap);
            return out;
        }
    }
    while (Module32NextW(snap, &me));

    CloseHandle(snap);
    return "";
}

static bool phase6c_is_executable_ptr(uint64_t addr)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    DWORD p = mbi.Protect & 0xff;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static void phase6c_dump_near_bytes(std::ofstream& f, uint64_t addr)
{
    if (addr < 32) return;

    uint8_t* p = reinterpret_cast<uint8_t*>(addr - 32);
    if (!phase6_mem_readable(p, 96)) return;

    f << "    bytes_around:\n";
    for (size_t i = 0; i < 96; i += 16)
    {
        f << "      0x" << std::hex << reinterpret_cast<uintptr_t>(p + i) << ": ";
        for (size_t j = 0; j < 16; ++j)
        {
            unsigned int b = p[i + j];
            if (b < 16) f << "0";
            f << b << " ";
        }
        f << std::dec << "\n";
    }
}

static void write_phase6c_vtable_diagnostic(UObject* o)
{
    static bool done = false;
    if (done || !o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream f(out / "phase6c_vtable_diagnostic.txt", std::ios::out);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    f << "Phase 7A vtable diagnostic\n";
    f << "object=" << path << "\n";
    f << "class=" << class_name(o) << "\n";
    f << "SizeX=" << sx << "\n";
    f << "SizeY=" << sy << "\n";
    f << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!phase6_mem_readable(base, 0x40))
    {
        f << "object_memory_readable=false\n";
        return;
    }

    uint64_t vtable = *reinterpret_cast<uint64_t*>(base);
    f << "vtable=0x" << std::hex << vtable << std::dec << "\n";
    f << "vtable_module=" << phase6c_module_for_addr(vtable) << "\n\n";

    if (!looks_like_ptr(vtable) || !phase6_mem_readable(reinterpret_cast<void*>(vtable), 8 * 160))
    {
        f << "vtable_not_readable\n";
        return;
    }

    auto vt = reinterpret_cast<uint64_t*>(vtable);

    f << "entries:\n";
    for (int i = 0; i < 160; ++i)
    {
        uint64_t fn = vt[i];
        if (!looks_like_ptr(fn)) continue;

        std::string mod = phase6c_module_for_addr(fn);
        bool exec = phase6c_is_executable_ptr(fn);

        f << "  [" << i << "] fn=0x" << std::hex << fn << std::dec
          << " exec=" << (exec ? 1 : 0)
          << " module=" << mod
          << "\n";

        if (exec && (mod.find("WindroseServer-Win64-Shipping.exe") != std::string::npos || mod.find("UE4SS.dll") != std::string::npos))
        {
            phase6c_dump_near_bytes(f, fn);
        }
    }

    f << "\nobject_native_pointer_summary:\n";
    for (size_t off = 0; off < 0x420; off += 8)
    {
        if (!phase6_mem_readable(base + off, 8)) continue;
        uint64_t val = *reinterpret_cast<uint64_t*>(base + off);
        if (!looks_like_ptr(val)) continue;

        std::string mod = phase6c_module_for_addr(val);
        bool exec = phase6c_is_executable_ptr(val);

        if (exec || !mod.empty())
        {
            f << "  off=0x" << std::hex << off
              << " val=0x" << val << std::dec
              << " exec=" << (exec ? 1 : 0)
              << " module=" << mod
              << "\n";
        }
    }

    f << "\nnote=Phase6C only dumps vtable/function pointer diagnostics. No function calls, no ReadPixels, no GPU access.\n";
    file_log("Phase 7A wrote phase6c_vtable_diagnostic.txt");
}
// END PHASE6C_VTABLE_DIAGNOSTIC



// BEGIN PHASE6D_RAW_CANDIDATE_DUMP
static bool phase6d_safe_dump_file(const std::filesystem::path& path, void* ptr, size_t len)
{
    if (!ptr || len == 0) return false;
    if (!phase6_mem_readable(ptr, len)) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(ptr), len);
    return out.good();
}

static void write_phase6d_raw_candidate_dump(UObject* o)
{
    static bool done = false;
    if (done || !o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase6d_raw_candidate_dump.txt", std::ios::out);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    const size_t expected = 2048ULL * 2048ULL * 4ULL;

    log << "Phase 7A raw candidate dump\n";
    log << "object=" << path << "\n";
    log << "class=" << class_name(o) << "\n";
    log << "SizeX=" << sx << "\n";
    log << "SizeY=" << sy << "\n";
    log << "expected_bytes=" << expected << "\n";
    log << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!phase6_mem_readable(base, 0x500))
    {
        log << "object_memory_readable=false\n";
        return;
    }

    struct RootCandidate
    {
        size_t object_off;
        size_t l1_off;
        size_t data_relative_off;
        const char* label;
    };

    RootCandidate candidates[] = {
        {0x20, 0xc0, 0x00, "root20_l1c0_self"},
        {0x20, 0xc0, 0x60, "root20_l1c0_plus60"},
        {0x20, 0xc0, 0xc0, "root20_l1c0_plusc0"},
        {0x20, 0xc0, 0x120, "root20_l1c0_plus120"},
        {0x20, 0xc0, 0x1e0, "root20_l1c0_plus1e0"},
        {0x20, 0xc0, 0x2d0, "root20_l1c0_plus2d0"},
        {0x20, 0xc0, 0x390, "root20_l1c0_plus390"},

        {0x20, 0xd8, 0x00, "root20_l1d8_self"},
        {0x20, 0xd8, 0x230, "root20_l1d8_plus230"},
        {0x20, 0xd8, 0x2a0, "root20_l1d8_plus2a0"},
        {0x20, 0xd8, 0x310, "root20_l1d8_plus310"},
        {0x20, 0xd8, 0x380, "root20_l1d8_plus380"},

        {0x300, 0x00, 0x00, "obj300_self"},
        {0x308, 0x00, 0x00, "obj308_self"},
        {0x3c0, 0x00, 0x00, "obj3c0_self"}
    };

    int dumped = 0;

    for (auto& c : candidates)
    {
        if (!phase6_mem_readable(base + c.object_off, 8))
        {
            log << c.label << " skip object_off_unreadable\n";
            continue;
        }

        uint64_t p1v = *reinterpret_cast<uint64_t*>(base + c.object_off);
        if (!looks_like_ptr(p1v) || !phase6_mem_readable(reinterpret_cast<void*>(p1v), 0x400))
        {
            log << c.label << " skip p1 invalid p1=0x" << std::hex << p1v << std::dec << "\n";
            continue;
        }

        uint8_t* p1 = reinterpret_cast<uint8_t*>(p1v);

        uint64_t p2v = p1v;
        if (c.l1_off != 0 || c.object_off == 0x20)
        {
            if (!phase6_mem_readable(p1 + c.l1_off, 8))
            {
                log << c.label << " skip l1_off_unreadable\n";
                continue;
            }

            p2v = *reinterpret_cast<uint64_t*>(p1 + c.l1_off);
        }

        if (!looks_like_ptr(p2v))
        {
            log << c.label << " skip p2 invalid p2=0x" << std::hex << p2v << std::dec << "\n";
            continue;
        }

        uint8_t* p2 = reinterpret_cast<uint8_t*>(p2v);
        uint8_t* data = p2 + c.data_relative_off;

        bool readable = phase6_mem_readable(data, expected);

        log << c.label
            << " object_off=0x" << std::hex << c.object_off
            << " l1_off=0x" << c.l1_off
            << " p1=0x" << p1v
            << " p2=0x" << p2v
            << " data=0x" << reinterpret_cast<uintptr_t>(data)
            << std::dec
            << " readable_16mb=" << (readable ? 1 : 0)
            << "\n";

        if (!readable) continue;

        std::string filename = std::string("RT_MapCapture_") + c.label + "_2048x2048_rgba_candidate.raw";
        bool ok = phase6d_safe_dump_file(out / filename, data, expected);

        log << "  dump=" << filename << " ok=" << (ok ? 1 : 0) << "\n";

        if (ok) dumped++;
        if (dumped >= 4) break;
    }

    log << "\ndumped_count=" << dumped << "\n";
    log << "note=Phase6D only copies readable 16MB CPU memory candidates. No function calls, no ReadPixels, no GPU API.\n";

    file_log("Phase 7A raw candidate dump done count=" + std::to_string(dumped));
}
// END PHASE6D_RAW_CANDIDATE_DUMP



// BEGIN PHASE6E_SMALL_CANDIDATE_DUMPS
static bool phase6e_dump_small(const std::filesystem::path& path, void* ptr, size_t len)
{
    if (!ptr || len == 0) return false;
    if (!phase6_mem_readable(ptr, len)) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(ptr), len);
    return out.good();
}

static void write_phase6e_small_candidate_dumps(UObject* o)
{
    static bool done = false;
    if (done || !o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase6e_small_candidate_dumps.txt", std::ios::out);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    log << "Phase 7A small candidate dumps\n";
    log << "object=" << path << "\n";
    log << "class=" << class_name(o) << "\n";
    log << "SizeX=" << sx << "\n";
    log << "SizeY=" << sy << "\n";
    log << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!phase6_mem_readable(base, 0x500))
    {
        log << "object_memory_readable=false\n";
        return;
    }

    struct Cand { uint64_t addr; const char* label; };

    std::vector<Cand> candidates;

    auto add_ptr = [&](uint64_t addr, const char* label)
    {
        if (looks_like_ptr(addr) && phase6_mem_readable(reinterpret_cast<void*>(addr), 0x1000))
            candidates.push_back({addr, label});
    };

    size_t object_offsets[] = {
        0x20, 0x50, 0x80, 0xa0, 0x130,
        0x180, 0x190, 0x1a0, 0x1b0, 0x1c0, 0x1d0,
        0x300, 0x310, 0x320, 0x330, 0x340,
        0x380, 0x388, 0x3c0, 0x3c8, 0x3d0, 0x3d8, 0x3e0, 0x3e8, 0x3f0, 0x3f8
    };

    for (size_t off : object_offsets)
    {
        if (!phase6_mem_readable(base + off, 8)) continue;
        uint64_t p1 = *reinterpret_cast<uint64_t*>(base + off);

        char label[64];
        std::snprintf(label, sizeof(label), "obj_off_%03llx", (unsigned long long)off);
        add_ptr(p1, _strdup(label));

        if (looks_like_ptr(p1) && phase6_mem_readable(reinterpret_cast<void*>(p1), 0x400))
        {
            uint8_t* p = reinterpret_cast<uint8_t*>(p1);

            for (size_t inner = 0; inner < 0x400; inner += 8)
            {
                if (!phase6_mem_readable(p + inner, 8)) continue;
                uint64_t p2 = *reinterpret_cast<uint64_t*>(p + inner);

                if (!looks_like_ptr(p2)) continue;
                if (!phase6_mem_readable(reinterpret_cast<void*>(p2), 0x1000)) continue;

                char label2[96];
                std::snprintf(label2, sizeof(label2), "obj_%03llx_inner_%03llx", (unsigned long long)off, (unsigned long long)inner);
                add_ptr(p2, _strdup(label2));
            }
        }
    }

    size_t dump_sizes[] = { 4096, 65536, 262144, 1048576 };

    int dumped = 0;
    std::set<uint64_t> seen;

    for (auto& c : candidates)
    {
        if (seen.count(c.addr)) continue;
        seen.insert(c.addr);

        log << "candidate label=" << c.label
            << " addr=0x" << std::hex << c.addr << std::dec
            << "\n";

        for (size_t sz : dump_sizes)
        {
            bool readable = phase6_mem_readable(reinterpret_cast<void*>(c.addr), sz);
            log << "  size=" << sz << " readable=" << (readable ? 1 : 0);

            if (readable)
            {
                std::string filename = std::string("RT_MapCapture_") + c.label + "_" + std::to_string(sz) + ".bin";
                bool ok = phase6e_dump_small(out / filename, reinterpret_cast<void*>(c.addr), sz);
                log << " dump=" << filename << " ok=" << (ok ? 1 : 0);
                if (ok) dumped++;
            }

            log << "\n";
        }

        if (dumped >= 40) break;
    }

    log << "\ndumped_files=" << dumped << "\n";
    log << "note=Phase6E dumps small readable chunks only. No function calls, no ReadPixels, no GPU API.\n";

    file_log("Phase 7A small candidate dumps done files=" + std::to_string(dumped));
}
// END PHASE6E_SMALL_CANDIDATE_DUMPS



// BEGIN PHASE6F_AGGRESSIVE_RESOURCE_DUMP
static bool phase6f_dump_file(const std::filesystem::path& path, void* ptr, size_t len)
{
    if (!ptr || len == 0) return false;
    if (!phase6_mem_readable(ptr, len)) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    out.write(reinterpret_cast<const char*>(ptr), len);
    return out.good();
}

static uint32_t phase6f_u32(uint8_t* p)
{
    return *reinterpret_cast<uint32_t*>(p);
}

static uint64_t phase6f_u64(uint8_t* p)
{
    return *reinterpret_cast<uint64_t*>(p);
}

static bool phase6f_interesting_u32(uint32_t v)
{
    return v == 1 || v == 2 || v == 4 ||
           v == 512 || v == 1024 || v == 2048 || v == 4096 ||
           v == 4194304 || v == 8388608 || v == 16777216 ||
           v == 50331909 || v == 50332420 ||
           v == 16973825 || v == 16973827;
}

static void phase6f_scan_block(std::ofstream& log, const std::string& label, uint8_t* ptr, size_t max_scan)
{
    if (!ptr || !phase6_mem_readable(ptr, 0x1000)) return;

    size_t scan_len = max_scan;
    while (scan_len >= 0x1000 && !phase6_mem_readable(ptr, scan_len))
        scan_len /= 2;

    if (scan_len < 0x1000) scan_len = 0x1000;

    log << "\nSCAN label=" << label
        << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(ptr)
        << " scan_len=" << std::dec << scan_len << "\n";

    for (size_t off = 0; off + 8 <= scan_len; off += 4)
    {
        if (!phase6_mem_readable(ptr + off, 8)) continue;

        uint32_t a = phase6f_u32(ptr + off);
        uint32_t b = phase6f_u32(ptr + off + 4);
        uint64_t q = phase6f_u64(ptr + off);

        bool hit =
            phase6f_interesting_u32(a) ||
            phase6f_interesting_u32(b) ||
            q == 2048ULL ||
            q == 4096ULL ||
            q == 4194304ULL ||
            q == 8388608ULL ||
            q == 16777216ULL ||
            looks_like_ptr(q);

        if (!hit) continue;

        log << "  +0x" << std::hex << off
            << " u32a=" << std::dec << a
            << " u32b=" << b
            << " q=0x" << std::hex << q << std::dec;

        if (a == 2048 || b == 2048 || q == 2048) log << " HIT_2048";
        if (a == 4194304 || b == 4194304 || q == 4194304) log << " HIT_4MP";
        if (a == 16777216 || b == 16777216 || q == 16777216) log << " HIT_16MB";
        if (a == 16973825 || b == 16973825 || a == 16973827 || b == 16973827) log << " HIT_RT_FORMAT";

        if (looks_like_ptr(q))
            log << " ptr_readable=" << (phase6_mem_readable(reinterpret_cast<void*>(q), 0x1000) ? 1 : 0);

        log << "\n";
    }
}

static void write_phase6f_aggressive_resource_dump(UObject* o)
{
    static int done_count = 0;
    if (!o || done_count >= 2) return;

    std::string path = obj_path(o);
    bool is_capture = path.find("RT_MapCapture") != std::string::npos;
    bool is_fog = path.find("RT_MapFog") != std::string::npos;

    if (!is_capture && !is_fog) return;
    done_count++;

    std::string prefix = is_capture ? "RT_MapCapture" : "RT_MapFog";

    auto out = out_dir();
    std::ofstream log(out / (prefix + "_phase6f_aggressive_resource_dump.txt"), std::ios::out);

    int32_t sx = 0;
    int32_t sy = 0;
    read_prop_value<int32_t>(o, STR("SizeX"), sx);
    read_prop_value<int32_t>(o, STR("SizeY"), sy);

    log << "Phase 7A aggressive resource dump\n";
    log << "object=" << path << "\n";
    log << "class=" << class_name(o) << "\n";
    log << "SizeX=" << sx << "\n";
    log << "SizeY=" << sy << "\n";
    log << "object_address=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    if (!phase6_mem_readable(base, 0x500))
    {
        log << "object_memory_readable=false\n";
        return;
    }

    std::vector<std::pair<std::string, uint64_t>> ptrs;

    auto add_ptr = [&](const std::string& label, uint64_t addr)
    {
        if (!looks_like_ptr(addr)) return;
        if (!phase6_mem_readable(reinterpret_cast<void*>(addr), 0x1000)) return;
        ptrs.push_back({label, addr});
    };

    // Object-level pointers.
    for (size_t off = 0; off < 0x420; off += 8)
    {
        if (!phase6_mem_readable(base + off, 8)) continue;
        uint64_t p1 = phase6f_u64(base + off);
        add_ptr("obj_" + std::to_string(off), p1);
    }

    // Known important resource-ish roots from previous phases.
    size_t roots[] = {0x20, 0x80, 0xa0, 0x130, 0x300, 0x310, 0x320, 0x378, 0x380, 0x3d0, 0x3d8};

    for (size_t roff : roots)
    {
        if (!phase6_mem_readable(base + roff, 8)) continue;

        uint64_t p1v = phase6f_u64(base + roff);
        if (!looks_like_ptr(p1v) || !phase6_mem_readable(reinterpret_cast<void*>(p1v), 0x1000)) continue;

        uint8_t* p1 = reinterpret_cast<uint8_t*>(p1v);
        add_ptr("root_" + std::to_string(roff), p1v);

        // L1 pointers
        for (size_t off1 = 0; off1 < 0x800; off1 += 8)
        {
            if (!phase6_mem_readable(p1 + off1, 8)) continue;

            uint64_t p2v = phase6f_u64(p1 + off1);
            if (!looks_like_ptr(p2v) || !phase6_mem_readable(reinterpret_cast<void*>(p2v), 0x1000)) continue;

            add_ptr("root_" + std::to_string(roff) + "_l1_" + std::to_string(off1), p2v);

            uint8_t* p2 = reinterpret_cast<uint8_t*>(p2v);

            // L2 pointers, but only first 0x400 to keep output bounded.
            for (size_t off2 = 0; off2 < 0x400; off2 += 8)
            {
                if (!phase6_mem_readable(p2 + off2, 8)) continue;

                uint64_t p3v = phase6f_u64(p2 + off2);
                if (!looks_like_ptr(p3v) || !phase6_mem_readable(reinterpret_cast<void*>(p3v), 0x1000)) continue;

                add_ptr("root_" + std::to_string(roff) + "_l1_" + std::to_string(off1) + "_l2_" + std::to_string(off2), p3v);
            }
        }
    }

    std::set<uint64_t> seen;
    int dumped = 0;
    int scanned = 0;

    size_t dump_sizes[] = {4096, 65536, 262144, 1048576, 4194304};

    for (auto& it : ptrs)
    {
        const std::string& label = it.first;
        uint64_t addr = it.second;

        if (seen.count(addr)) continue;
        seen.insert(addr);

        uint8_t* p = reinterpret_cast<uint8_t*>(addr);

        phase6f_scan_block(log, label, p, 1048576);
        scanned++;

        bool has_big_hint = false;

        // Quick scan first 1MB for 2048/16MB/format values.
        size_t quick_len = 1048576;
        if (!phase6_mem_readable(p, quick_len)) quick_len = 65536;
        if (!phase6_mem_readable(p, quick_len)) quick_len = 4096;

        for (size_t off = 0; off + 8 <= quick_len; off += 4)
        {
            if (!phase6_mem_readable(p + off, 8)) continue;
            uint32_t a = phase6f_u32(p + off);
            uint32_t b = phase6f_u32(p + off + 4);
            uint64_t q = phase6f_u64(p + off);

            if (a == 2048 || b == 2048 || q == 2048 ||
                a == 16777216 || b == 16777216 || q == 16777216 ||
                a == 4194304 || b == 4194304 || q == 4194304 ||
                a == 16973825 || b == 16973825 || a == 16973827 || b == 16973827)
            {
                has_big_hint = true;
                break;
            }
        }

        if (!has_big_hint && dumped > 20) continue;

        for (size_t sz : dump_sizes)
        {
            if (!phase6_mem_readable(p, sz)) continue;

            std::string safe = label;
            for (char& c : safe)
            {
                if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) c = '_';
            }

            std::string fn = prefix + "_phase6f_" + safe + "_" + std::to_string(sz) + ".bin";
            bool ok = phase6f_dump_file(out / fn, p, sz);

            log << "DUMP label=" << label
                << " addr=0x" << std::hex << addr << std::dec
                << " size=" << sz
                << " file=" << fn
                << " ok=" << (ok ? 1 : 0)
                << "\n";

            if (ok) dumped++;
            if (dumped >= 80) break;
        }

        if (dumped >= 80 || scanned >= 120) break;
    }

    log << "\nunique_ptrs=" << seen.size() << "\n";
    log << "scanned=" << scanned << "\n";
    log << "dumped=" << dumped << "\n";
    log << "note=Phase6F aggressive dump. Still no function calls, no ReadPixels, no GPU API.\n";

    file_log("Phase 7A aggressive resource dump " + prefix + " dumped=" + std::to_string(dumped));
}
// END PHASE6F_AGGRESSIVE_RESOURCE_DUMP



// BEGIN PHASE6G_STRING_NEIGHBORHOOD_PROBE
static bool phase6g_is_ascii_printable(uint8_t c)
{
    return c >= 32 && c <= 126;
}

static std::string phase6g_read_ascii(uint8_t* p, size_t max_len)
{
    std::string out;
    if (!p || !phase6_mem_readable(p, 1)) return out;

    for (size_t i = 0; i < max_len; ++i)
    {
        if (!phase6_mem_readable(p + i, 1)) break;
        uint8_t c = *(p + i);
        if (c == 0) break;
        if (!phase6g_is_ascii_printable(c)) break;
        out.push_back((char)c);
    }
    return out;
}

static std::string phase6g_read_wide_ascii(uint8_t* p, size_t max_chars)
{
    std::string out;
    if (!p || !phase6_mem_readable(p, 2)) return out;

    for (size_t i = 0; i < max_chars; ++i)
    {
        uint8_t* q = p + i * 2;
        if (!phase6_mem_readable(q, 2)) break;
        uint8_t c = q[0];
        uint8_t z = q[1];
        if (c == 0 && z == 0) break;
        if (z != 0 || !phase6g_is_ascii_printable(c)) break;
        out.push_back((char)c);
    }
    return out;
}

static void write_phase6g_string_neighborhood_probe(UObject* o)
{
    file_log("Phase 7A string-neighborhood entered");
    static bool done = false;
    if (done || !o)
    {
        file_log("Phase 7A early return: done or null object");
        return;
    }

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase6g_string_neighborhood_probe.txt", std::ios::out);

    log << "Phase 7A targeted string-neighborhood probe\n";
    log << "trigger_object=" << path << "\n";
    log << "trigger_class=" << class_name(o) << "\n\n";

    const char* needles[] = {
        "R5UTextureUtils.cpp",
        "TextureResource.h",
        "OpenGLTexture.cpp",
        "ReadLinearColor",
        "CanvasForDrawMaterialToRenderTarget",
        "RenderTargetPool.cpp",
        "RHILockTracker.cpp",
        "Render to Texture",
        "Invalid size",
        "DefaultRenderTargetFormat",
        "GetRenderTargetSize",
        "SetRenderTargetSize",
        "SampleRenderTargetValue",
        "LoadRenderTargetValue",
        "SetRenderTargetValue"
    };

    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe)
    {
        log << "GetModuleHandleW(nullptr) failed\n";
        return;
    }

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hExe, &mi, sizeof(mi)))
    {
        log << "GetModuleInformation failed\n";
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    size_t size = static_cast<size_t>(mi.SizeOfImage);

    log << "module_base=0x" << std::hex << reinterpret_cast<uintptr_t>(base)
        << " module_size=0x" << size << std::dec << "\n\n";

    const size_t max_scan = size;
    int total_hits = 0;

    for (const char* needle : needles)
    {
        size_t nlen = std::strlen(needle);
        log << "\n=== NEEDLE " << needle << " ===\n";

        int hits = 0;

        for (size_t off = 0; off + nlen < max_scan; ++off)
        {
            uint8_t* p = base + off;
            if (!phase6_mem_readable(p, nlen)) continue;

            bool match = std::memcmp(p, needle, nlen) == 0;

            if (!match)
            {
                // wide ASCII variant
                bool wmatch = true;
                for (size_t i = 0; i < nlen; ++i)
                {
                    if (!phase6_mem_readable(p + i * 2, 2)) { wmatch = false; break; }
                    if (p[i * 2] != (uint8_t)needle[i] || p[i * 2 + 1] != 0) { wmatch = false; break; }
                }
                match = wmatch;
            }

            if (!match) continue;

            hits++;
            total_hits++;

            log << "hit_" << hits
                << " addr=0x" << std::hex << reinterpret_cast<uintptr_t>(p)
                << " rva=0x" << off << std::dec << "\n";

            // Dump nearby strings.
            size_t start = off > 512 ? off - 512 : 0;
            size_t end = std::min(max_scan, off + 2048);

            log << "nearby_strings:\n";
            for (size_t qoff = start; qoff < end; ++qoff)
            {
                uint8_t* q = base + qoff;
                if (!phase6_mem_readable(q, 8)) continue;

                auto a = phase6g_read_ascii(q, 160);
                if (a.size() >= 8)
                {
                    log << "  ascii rva=0x" << std::hex << qoff << std::dec << " " << a << "\n";
                    qoff += a.size();
                    continue;
                }

                auto w = phase6g_read_wide_ascii(q, 160);
                if (w.size() >= 8)
                {
                    log << "  wide  rva=0x" << std::hex << qoff << std::dec << " " << w << "\n";
                    qoff += w.size() * 2;
                    continue;
                }
            }

            // Search for nearby references to the string address in module memory.
            uint64_t addr = reinterpret_cast<uint64_t>(p);
            log << "xrefs_to_string_addr:\n";

            int xrefs = 0;
            for (size_t x = 0; x + 8 < max_scan; x += 1)
            {
                uint8_t* xp = base + x;
                if (!phase6_mem_readable(xp, 8)) continue;

                uint64_t val = *reinterpret_cast<uint64_t*>(xp);
                if (val != addr) continue;

                xrefs++;
                log << "  xref_" << xrefs
                    << " at=0x" << std::hex << reinterpret_cast<uintptr_t>(xp)
                    << " rva=0x" << x << std::dec << "\n";

                // Dump bytes around xref.
                size_t bx = x > 128 ? x - 128 : 0;
                log << "  bytes_around_xref:\n";
                for (size_t line = 0; line < 256; line += 16)
                {
                    uint8_t* bp = base + bx + line;
                    if (!phase6_mem_readable(bp, 16)) continue;
                    log << "    rva=0x" << std::hex << (bx + line) << " ";
                    for (int k = 0; k < 16; ++k)
                    {
                        unsigned int b = bp[k];
                        if (b < 16) log << "0";
                        log << b << " ";
                    }
                    log << std::dec << "\n";
                }

                if (xrefs >= 20) break;
            }

            if (hits >= 20) break;
        }

        log << "needle_hits=" << hits << "\n";
    }

    log << "\ntotal_hits=" << total_hits << "\n";
    log << "note=Phase6G scans exe memory for known texture/render strings and xrefs. No function calls, no GPU API.\n";

    file_log("Phase 7A string-neighborhood probe done hits=" + std::to_string(total_hits));
}
// END PHASE6G_STRING_NEIGHBORHOOD_PROBE



// BEGIN PHASE6H_FAST_MODULE_STRING_SCAN
static void write_phase6h_fast_module_string_scan(UObject* o)
{
    static bool done = false;
    if (done || !o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase6h_fast_module_string_scan.txt", std::ios::out);

    file_log("Phase 7A fast module string scan entered");

    HMODULE hExe = GetModuleHandleW(L"WindroseServer-Win64-Shipping.exe");
    if (!hExe)
    {
        hExe = GetModuleHandleW(nullptr);
    }

    if (!hExe)
    {
        log << "ERROR no module handle\n";
        file_log("Phase 7A no module handle");
        return;
    }

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hExe, &mi, sizeof(mi)))
    {
        log << "ERROR GetModuleInformation failed\n";
        file_log("Phase 7A GetModuleInformation failed");
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    size_t size = static_cast<size_t>(mi.SizeOfImage);

    log << "Phase 7A fast module string scan\n";
    log << "trigger_object=" << path << "\n";
    log << "module_base=0x" << std::hex << reinterpret_cast<uintptr_t>(base)
        << " module_size=0x" << size << std::dec << "\n";

    const char* needles[] = {
        "R5UTextureUtils.cpp",
        "TextureResource.h",
        "OpenGLTexture.cpp",
        "ReadLinearColor",
        "CanvasForDrawMaterialToRenderTarget",
        "RenderTargetPool.cpp",
        "RHILockTracker.cpp",
        "Render to Texture",
        "Invalid size",
        "DefaultRenderTargetFormat",
        "GetRenderTargetSize",
        "SetRenderTargetSize",
        "SampleRenderTargetValue",
        "LoadRenderTargetValue",
        "SetRenderTargetValue"
    };

    int total_hits = 0;

    for (const char* needle : needles)
    {
        size_t nlen = std::strlen(needle);
        int hits = 0;

        log << "\n=== NEEDLE " << needle << " ===\n";

        for (size_t off = 0; off + nlen < size; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            uint8_t* cur = base + off;

            if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
            {
                off += 0x1000;
                continue;
            }

            uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t region_end = region_start + mbi.RegionSize;
            uintptr_t module_end = reinterpret_cast<uintptr_t>(base) + size;

            if (region_end > module_end) region_end = module_end;

            bool readable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (!readable)
            {
                off = region_end - reinterpret_cast<uintptr_t>(base);
                continue;
            }

            uint8_t* region_ptr = reinterpret_cast<uint8_t*>(region_start);
            size_t region_len = region_end - region_start;

            if (region_len >= nlen)
            {
                for (size_t i = 0; i + nlen <= region_len; ++i)
                {
                    uint8_t* p = region_ptr + i;

                    bool match_ascii = std::memcmp(p, needle, nlen) == 0;
                    bool match_wide = false;

                    if (!match_ascii && i + (nlen * 2) <= region_len)
                    {
                        match_wide = true;
                        for (size_t k = 0; k < nlen; ++k)
                        {
                            if (p[k * 2] != (uint8_t)needle[k] || p[k * 2 + 1] != 0)
                            {
                                match_wide = false;
                                break;
                            }
                        }
                    }

                    if (!match_ascii && !match_wide) continue;

                    uintptr_t addr = reinterpret_cast<uintptr_t>(p);
                    size_t rva = addr - reinterpret_cast<uintptr_t>(base);

                    hits++;
                    total_hits++;

                    log << "hit_" << hits
                        << " addr=0x" << std::hex << addr
                        << " rva=0x" << rva
                        << std::dec
                        << " encoding=" << (match_wide ? "wide" : "ascii")
                        << "\n";

                    size_t before = rva > 192 ? rva - 192 : 0;
                    size_t after = std::min(size, rva + 512);

                    log << "context_strings:\n";

                    for (size_t q = before; q < after; ++q)
                    {
                        uint8_t* qp = base + q;
                        if (!phase6_mem_readable(qp, 8)) continue;

                        std::string a = phase6g_read_ascii(qp, 120);
                        if (a.size() >= 8)
                        {
                            log << "  ascii rva=0x" << std::hex << q << std::dec << " " << a << "\n";
                            q += a.size();
                            continue;
                        }

                        std::string w = phase6g_read_wide_ascii(qp, 120);
                        if (w.size() >= 8)
                        {
                            log << "  wide  rva=0x" << std::hex << q << std::dec << " " << w << "\n";
                            q += w.size() * 2;
                            continue;
                        }
                    }

                    if (hits >= 20 || total_hits >= 120) break;
                }
            }

            if (hits >= 20 || total_hits >= 120) break;

            off = region_end - reinterpret_cast<uintptr_t>(base);
        }

        log << "needle_hits=" << hits << "\n";
        log.flush();

        if (total_hits >= 120) break;
    }

    log << "\ntotal_hits=" << total_hits << "\n";
    log << "note=Phase6H fast scanner. Heavy dumps disabled. No function calls, no GPU API.\n";
    log.flush();

    file_log("Phase 7A fast module string scan done hits=" + std::to_string(total_hits));
}
// END PHASE6H_FAST_MODULE_STRING_SCAN



// BEGIN PHASE6I_TARGETED_XREF_SCAN
static void phase6i_hex_line(std::ofstream& log, uint8_t* base, size_t rva, size_t count)
{
    log << "    rva=0x" << std::hex << rva << " ";
    for (size_t i = 0; i < count; ++i)
    {
        uint8_t* p = base + rva + i;
        if (!phase6_mem_readable(p, 1))
        {
            log << "?? ";
            continue;
        }

        unsigned int b = *p;
        if (b < 16) log << "0";
        log << b << " ";
    }
    log << std::dec << "\n";
}

static void write_phase6i_targeted_xref_scan(UObject* o)
{
    static bool done = false;
    if (done || !o) return;

    std::string path = obj_path(o);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase6i_targeted_xref_scan.txt", std::ios::out);

    file_log("Phase 7A targeted xref scan entered");

    HMODULE hExe = GetModuleHandleW(L"WindroseServer-Win64-Shipping.exe");
    if (!hExe) hExe = GetModuleHandleW(nullptr);

    if (!hExe)
    {
        log << "ERROR no module handle\n";
        return;
    }

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), hExe, &mi, sizeof(mi)))
    {
        log << "ERROR GetModuleInformation failed\n";
        return;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    size_t size = static_cast<size_t>(mi.SizeOfImage);

    log << "Phase 7A targeted xref/code-window scan\n";
    log << "trigger_object=" << path << "\n";
    log << "module_base=0x" << std::hex << reinterpret_cast<uintptr_t>(base)
        << " module_size=0x" << size << std::dec << "\n\n";

    struct Target
    {
        const char* name;
        size_t rva;
    };

    Target targets[] = {
        {"Render_to_Texture_2D", 0x0cfc53e8},
        {"InvalidSizeForConversionToTexture", 0x0cfc5490},
        {"TextureRenderTarget2D", 0x0cfc54d8},
        {"FTextureRenderTarget2DResource", 0x0cfc5508},
        {"TextureRenderTarget2DResource", 0x0cfc5548},

        {"Render_to_Texture_2DArray", 0x0cfc5ba0},
        {"FTextureRenderTarget2DArrayResource", 0x0cfc5c70},
        {"TextureRenderTarget2DArrayResource", 0x0cfc5cc0},

        {"R5UTextureUtils_cpp", 0x0d083500},
        {"TextureResource_h", 0x0cc6c800},
        {"OpenGLTexture_cpp", 0x0d056170},
        {"RHICreateTexture2DFromResource_string", 0x0d0563a0}
    };

    int total_xrefs = 0;

    for (const auto& t : targets)
    {
        if (t.rva >= size) continue;

        uint8_t* target_ptr = base + t.rva;
        uint64_t target_abs = reinterpret_cast<uint64_t>(target_ptr);
        int xrefs = 0;

        log << "\n=== TARGET " << t.name << " ===\n";
        log << "target_rva=0x" << std::hex << t.rva
            << " target_abs=0x" << target_abs << std::dec << "\n";

        log << "target_context:\n";
        size_t ctx_start = t.rva > 128 ? t.rva - 128 : 0;
        for (size_t line = 0; line < 384; line += 16)
        {
            if (ctx_start + line >= size) break;
            phase6i_hex_line(log, base, ctx_start + line, 16);
        }

        // Search direct 64-bit absolute pointer references.
        for (size_t off = 0; off + 8 < size; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            uint8_t* cur = base + off;

            if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
            {
                off += 0x1000;
                continue;
            }

            uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t region_end = region_start + mbi.RegionSize;
            uintptr_t module_end = reinterpret_cast<uintptr_t>(base) + size;

            if (region_end > module_end) region_end = module_end;

            bool readable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (!readable)
            {
                off = region_end - reinterpret_cast<uintptr_t>(base);
                continue;
            }

            uint8_t* region_ptr = reinterpret_cast<uint8_t*>(region_start);
            size_t region_len = region_end - region_start;

            for (size_t i = 0; i + 8 <= region_len; ++i)
            {
                uint8_t* p = region_ptr + i;
                uint64_t val = *reinterpret_cast<uint64_t*>(p);

                if (val != target_abs) continue;

                uintptr_t hit_abs = reinterpret_cast<uintptr_t>(p);
                size_t hit_rva = hit_abs - reinterpret_cast<uintptr_t>(base);

                xrefs++;
                total_xrefs++;

                log << "ABS_XREF_" << xrefs
                    << " hit_rva=0x" << std::hex << hit_rva
                    << " hit_abs=0x" << hit_abs << std::dec << "\n";

                size_t win_start = hit_rva > 192 ? hit_rva - 192 : 0;
                log << "  bytes_around_abs_xref:\n";
                for (size_t line = 0; line < 512; line += 16)
                {
                    if (win_start + line >= size) break;
                    phase6i_hex_line(log, base, win_start + line, 16);
                }

                if (xrefs >= 25) break;
            }

            if (xrefs >= 25) break;
            off = region_end - reinterpret_cast<uintptr_t>(base);
        }

        // Search x64 RIP-relative LEA/MOV references:
        // address after instruction + disp32 == target_abs.
        int riprefs = 0;

        for (size_t off = 0; off + 7 < size; )
        {
            MEMORY_BASIC_INFORMATION mbi{};
            uint8_t* cur = base + off;

            if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
            {
                off += 0x1000;
                continue;
            }

            uintptr_t region_start = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            uintptr_t region_end = region_start + mbi.RegionSize;
            uintptr_t module_end = reinterpret_cast<uintptr_t>(base) + size;

            if (region_end > module_end) region_end = module_end;

            bool readable =
                mbi.State == MEM_COMMIT &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));

            if (!readable)
            {
                off = region_end - reinterpret_cast<uintptr_t>(base);
                continue;
            }

            uint8_t* region_ptr = reinterpret_cast<uint8_t*>(region_start);
            size_t region_len = region_end - region_start;

            for (size_t i = 0; i + 7 <= region_len; ++i)
            {
                uint8_t* p = region_ptr + i;

                // Common patterns:
                // 48 8D 0D xx xx xx xx  lea rcx,[rip+disp]
                // 48 8D 15 xx xx xx xx  lea rdx,[rip+disp]
                // 4C 8D 05 xx xx xx xx  lea r8,[rip+disp]
                // 48 8B 0D xx xx xx xx  mov rcx,[rip+disp]
                bool possible =
                    (p[0] == 0x48 && (p[1] == 0x8D || p[1] == 0x8B) && (p[2] == 0x0D || p[2] == 0x15 || p[2] == 0x05)) ||
                    (p[0] == 0x4C && p[1] == 0x8D && (p[2] == 0x05 || p[2] == 0x0D || p[2] == 0x15));

                if (!possible) continue;

                int32_t disp = *reinterpret_cast<int32_t*>(p + 3);
                uint64_t instr_end = reinterpret_cast<uint64_t>(p + 7);
                uint64_t resolved = instr_end + disp;

                if (resolved != target_abs) continue;

                uintptr_t hit_abs = reinterpret_cast<uintptr_t>(p);
                size_t hit_rva = hit_abs - reinterpret_cast<uintptr_t>(base);

                riprefs++;
                total_xrefs++;

                log << "RIP_XREF_" << riprefs
                    << " hit_rva=0x" << std::hex << hit_rva
                    << " hit_abs=0x" << hit_abs
                    << " opcode=" << (unsigned int)p[0] << " " << (unsigned int)p[1] << " " << (unsigned int)p[2]
                    << std::dec << "\n";

                size_t win_start = hit_rva > 256 ? hit_rva - 256 : 0;
                log << "  bytes_around_rip_xref:\n";
                for (size_t line = 0; line < 768; line += 16)
                {
                    if (win_start + line >= size) break;
                    phase6i_hex_line(log, base, win_start + line, 16);
                }

                if (riprefs >= 25) break;
            }

            if (riprefs >= 25) break;
            off = region_end - reinterpret_cast<uintptr_t>(base);
        }

        log << "target_abs_xrefs=" << xrefs << "\n";
        log << "target_rip_xrefs=" << riprefs << "\n";
        log.flush();
    }

    log << "\ntotal_xrefs=" << total_xrefs << "\n";
    log << "note=Phase6I xref scan only. Heavy dumps disabled. No function calls, no GPU API.\n";
    log.flush();

    file_log("Phase 7A targeted xref scan done total_xrefs=" + std::to_string(total_xrefs));
}
// END PHASE6I_TARGETED_XREF_SCAN






// BEGIN PHASE7A_UE_RUNTIME_READBACK_DISCOVERY
static void write_phase7a_ue_runtime_readback_discovery(UObject* trigger)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string path = obj_path(trigger);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase7a_ue_runtime_readback_discovery.txt", std::ios::out);

    file_log("Phase 7A UE runtime readback discovery entered");

    log << "Phase 7A UE runtime readback discovery\n";
    log << "trigger=" << path << "\n\n";

    const char* needles[] = {
        "KismetRenderingLibrary",
        "ReadRenderTarget",
        "ReadRenderTargetRaw",
        "ReadRenderTargetPixel",
        "ReadRenderTargetRawPixel",
        "ExportRenderTarget",
        "ImportBufferAsTexture2D",
        "RenderTargetCreateStaticTexture2DEditorOnly",
        "BeginDrawCanvasToRenderTarget",
        "EndDrawCanvasToRenderTarget",
        "ClearRenderTarget2D",
        "DrawMaterialToRenderTarget",
        "TextureRenderTarget2D",
        "TextureRenderTarget2DArray",
        "RT_MapCapture",
        "RT_MapFog",
        "RT_LandscapeTable",
        "RT_LandscapeHeights",
        "RT_Biomes",
        "RT_SubBiomes",
        "RT_BiomeDistanceFields"
    };

    int scanned = 0;
    int hits = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o)
            {
                return RC::LoopAction::Continue;
            }

            scanned++;

            std::string name = obj_name(o);
            std::string full = obj_path(o);
            std::string cls = class_name(o);

            bool match = false;

            for (const char* n : needles)
            {
                if (
                    name.find(n) != std::string::npos ||
                    full.find(n) != std::string::npos ||
                    cls.find(n) != std::string::npos
                )
                {
                    match = true;
                    break;
                }
            }

            if (!match)
            {
                return RC::LoopAction::Continue;
            }

            hits++;

            log << "HIT[" << hits << "]\n";
            log << "  name=" << name << "\n";
            log << "  path=" << full << "\n";
            log << "  class=" << cls << "\n";
            log << "  addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";

            if (hits >= 500)
            {
                return RC::LoopAction::Break;
            }

            return RC::LoopAction::Continue;
        }
    );

    log << "scanned_objects=" << scanned << "\n";
    log << "hits=" << hits << "\n";
    log << "note=Discovery only. No ProcessEvent calls, no GPU API, no ReadPixels yet.\n";

    file_log("Phase 7A UE runtime readback discovery done hits=" + std::to_string(hits));
}
// END PHASE7A_UE_RUNTIME_READBACK_DISCOVERY






// BEGIN PHASE7B_UFUNCTION_SIGNATURE_DUMP
static bool phase7b_is_target_function(const std::string& full)
{
    const char* targets[] = {
        "KismetRenderingLibrary:ExportRenderTarget",
        "KismetRenderingLibrary:ReadRenderTarget",
        "KismetRenderingLibrary:ReadRenderTargetPixel",
        "KismetRenderingLibrary:ReadRenderTargetRaw",
        "KismetRenderingLibrary:ReadRenderTargetRawPixel",
        "KismetRenderingLibrary:ReadRenderTargetRawPixelArea",
        "KismetRenderingLibrary:ReadRenderTargetRawUV",
        "KismetRenderingLibrary:ReadRenderTargetRawUVArea",
        "KismetRenderingLibrary:ReadRenderTargetUV",
        "KismetRenderingLibrary:ReadRenderTargetUVArea",
        "KismetRenderingLibrary:BeginDrawCanvasToRenderTarget",
        "KismetRenderingLibrary:EndDrawCanvasToRenderTarget",
        "KismetRenderingLibrary:DrawMaterialToRenderTarget",
        "KismetRenderingLibrary:ClearRenderTarget2D"
    };

    for (const char* t : targets)
    {
        if (full.find(t) != std::string::npos) return true;
    }

    return false;
}

static void write_phase7b_ufunction_signature_dump(UObject* trigger)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string path = obj_path(trigger);
    if (path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase7b_ufunction_signature_dump.txt", std::ios::out);

    file_log("Phase 7B UFunction signature dump entered");

    log << "Phase 7B UFunction signature dump\n";
    log << "trigger=" << path << "\n";
    log << "mode=discovery_only_no_ProcessEvent_no_GPU_no_ReadPixels\n\n";

    int scanned = 0;
    int target_functions = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string name = obj_name(o);
            std::string full = obj_path(o);
            std::string cls = class_name(o);

            if (cls != "Function") return RC::LoopAction::Continue;
            if (!phase7b_is_target_function(full)) return RC::LoopAction::Continue;

            target_functions++;

            log << "FUNCTION[" << target_functions << "]\n";
            log << "  name=" << name << "\n";
            log << "  path=" << full << "\n";
            log << "  class=" << cls << "\n";
            log << "  addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";

            auto* fn = static_cast<UFunction*>(o);
            uint32_t flags = static_cast<uint32_t>(fn->GetFunctionFlags());

            log << "  function_flags_dec=" << flags << "\n";
            log << "  function_flags_hex=0x" << std::hex << flags << std::dec << "\n";
            log << "  note=Phase 7B intentionally does not traverse FProperty/FField yet.\n";
            log << "\n";

            if (target_functions >= 64) return RC::LoopAction::Break;
            return RC::LoopAction::Continue;
        }
    );

    log << "scanned_objects=" << scanned << "\n";
    log << "target_functions=" << target_functions << "\n";
    log << "note=Phase 7B is metadata discovery only. No ProcessEvent calls.\n";

    file_log("Phase 7B UFunction signature dump done functions=" + std::to_string(target_functions));
}
// END PHASE7B_UFUNCTION_SIGNATURE_DUMP






// BEGIN PHASE7C1_CONTEXT_OBJECT_DISCOVERY
static void write_phase7c1_context_object_discovery(UObject* trigger)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase7c1_context_object_discovery.txt", std::ios::out);

    file_log("Phase 7C1B targeted context object discovery entered");

    log << "Phase 7C1B targeted context object discovery\n";
    log << "trigger=" << trigger_path << "\n";
    log << "mode=full_scan_targeted_candidates_only_no_ProcessEvent_no_GPU_no_ReadPixels\n\n";

    int scanned = 0;
    int logged_hits = 0;

    UObject* default_kismet = nullptr;
    UObject* rt_map_capture = nullptr;
    UObject* rt_map_fog = nullptr;
    UObject* rt_landscape_table = nullptr;
    UObject* rt_landscape_heights = nullptr;
    UObject* rt_biomes = nullptr;
    UObject* rt_sub_biomes = nullptr;
    UObject* rt_biome_distance_fields = nullptr;

    UObject* likely_world = nullptr;
    UObject* likely_game_instance = nullptr;
    UObject* likely_engine = nullptr;
    UObject* likely_game_viewport = nullptr;
    UObject* likely_player_controller = nullptr;
    UObject* likely_r5_player_controller = nullptr;
    UObject* likely_island_manager = nullptr;
    UObject* likely_terrain_settings = nullptr;
    UObject* likely_persistent_level = nullptr;

    auto log_hit = [&](const char* label, UObject* o, const std::string& name, const std::string& full, const std::string& cls)
    {
        logged_hits++;
        log << "TARGET_HIT[" << logged_hits << "] " << label << "\n";
        log << "  name=" << name << "\n";
        log << "  path=" << full << "\n";
        log << "  class=" << cls << "\n";
        log << "  addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";
    };

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o)
            {
                return RC::LoopAction::Continue;
            }

            scanned++;

            std::string name = obj_name(o);
            std::string full = obj_path(o);
            std::string cls = class_name(o);

            bool should_log = false;
            const char* label = "unknown";

            if (full == "/Script/Engine.Default__KismetRenderingLibrary" || full.find("Default__KismetRenderingLibrary") != std::string::npos)
            {
                default_kismet = o;
                should_log = true;
                label = "Default__KismetRenderingLibrary";
            }
            else if (full == "/Game/UI/META/FullscreenMap/Assets/RT_MapCapture.RT_MapCapture")
            {
                rt_map_capture = o;
                should_log = true;
                label = "RT_MapCapture";
            }
            else if (full == "/Game/UI/META/FullscreenMap/Assets/RT_MapFog.RT_MapFog")
            {
                rt_map_fog = o;
                should_log = true;
                label = "RT_MapFog";
            }
            else if (full == "/R5TerrainGeneratorAPI/Volumization/RT_LandscapeTable.RT_LandscapeTable")
            {
                rt_landscape_table = o;
                should_log = true;
                label = "RT_LandscapeTable";
            }
            else if (full == "/R5TerrainGeneratorAPI/Volumization/RT_LandscapeHeights.RT_LandscapeHeights")
            {
                rt_landscape_heights = o;
                should_log = true;
                label = "RT_LandscapeHeights";
            }
            else if (full == "/R5TerrainGeneratorAPI/Volumization/RT_Biomes.RT_Biomes")
            {
                rt_biomes = o;
                should_log = true;
                label = "RT_Biomes";
            }
            else if (full == "/R5TerrainGeneratorAPI/Volumization/RT_SubBiomes.RT_SubBiomes")
            {
                rt_sub_biomes = o;
                should_log = true;
                label = "RT_SubBiomes";
            }
            else if (full == "/R5TerrainGeneratorAPI/Volumization/RT_BiomeDistanceFields.RT_BiomeDistanceFields")
            {
                rt_biome_distance_fields = o;
                should_log = true;
                label = "RT_BiomeDistanceFields";
            }
            else if (!likely_world && cls == "World" && full.find("/Game/Maps/") != std::string::npos)
            {
                likely_world = o;
                should_log = true;
                label = "LikelyWorld_GameMaps";
            }
            else if (!likely_world && cls == "World")
            {
                likely_world = o;
                should_log = true;
                label = "LikelyWorld_ClassWorld";
            }
            else if (!likely_persistent_level && cls == "Level" && full.find("PersistentLevel") != std::string::npos)
            {
                likely_persistent_level = o;
                should_log = true;
                label = "LikelyPersistentLevel";
            }
            else if (!likely_game_instance && cls.find("GameInstance") != std::string::npos && cls != "Class")
            {
                likely_game_instance = o;
                should_log = true;
                label = "LikelyGameInstance";
            }
            else if (!likely_engine && cls.find("GameEngine") != std::string::npos && cls != "Class")
            {
                likely_engine = o;
                should_log = true;
                label = "LikelyGameEngine";
            }
            else if (!likely_game_viewport && cls.find("GameViewportClient") != std::string::npos && cls != "Class")
            {
                likely_game_viewport = o;
                should_log = true;
                label = "LikelyGameViewportClient";
            }
            else if (!likely_r5_player_controller && (cls.find("R5PlayerController") != std::string::npos || full.find("R5PlayerController") != std::string::npos) && cls != "Class")
            {
                likely_r5_player_controller = o;
                should_log = true;
                label = "LikelyR5PlayerController";
            }
            else if (!likely_player_controller && cls.find("PlayerController") != std::string::npos && cls != "Class")
            {
                likely_player_controller = o;
                should_log = true;
                label = "LikelyPlayerController";
            }
            else if (!likely_island_manager && (cls.find("R5IslandManager") != std::string::npos || full.find("R5IslandManager") != std::string::npos) && cls != "Class")
            {
                likely_island_manager = o;
                should_log = true;
                label = "LikelyR5IslandManager";
            }
            else if (!likely_terrain_settings && (cls.find("R5TerrainSettings") != std::string::npos || full.find("R5TerrainSettings") != std::string::npos) && cls != "Class")
            {
                likely_terrain_settings = o;
                should_log = true;
                label = "LikelyR5TerrainSettings";
            }

            if (should_log)
            {
                log_hit(label, o, name, full, cls);
            }

            return RC::LoopAction::Continue;
        }
    );

    auto write_candidate = [&](const char* label, UObject* o)
    {
        log << "CANDIDATE " << label << "\n";

        if (!o)
        {
            log << "  found=false\n\n";
            return;
        }

        log << "  found=true\n";
        log << "  name=" << obj_name(o) << "\n";
        log << "  path=" << obj_path(o) << "\n";
        log << "  class=" << class_name(o) << "\n";
        log << "  addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n\n";
    };

    log << "\n===== SELECTED CANDIDATES =====\n";
    write_candidate("Default__KismetRenderingLibrary", default_kismet);
    write_candidate("RT_MapCapture", rt_map_capture);
    write_candidate("RT_MapFog", rt_map_fog);
    write_candidate("RT_LandscapeTable", rt_landscape_table);
    write_candidate("RT_LandscapeHeights", rt_landscape_heights);
    write_candidate("RT_Biomes", rt_biomes);
    write_candidate("RT_SubBiomes", rt_sub_biomes);
    write_candidate("RT_BiomeDistanceFields", rt_biome_distance_fields);
    write_candidate("LikelyWorldContextObject_World", likely_world);
    write_candidate("LikelyPersistentLevel", likely_persistent_level);
    write_candidate("LikelyGameInstance", likely_game_instance);
    write_candidate("LikelyEngine", likely_engine);
    write_candidate("LikelyGameViewportClient", likely_game_viewport);
    write_candidate("LikelyPlayerController", likely_player_controller);
    write_candidate("LikelyR5PlayerController", likely_r5_player_controller);
    write_candidate("LikelyR5IslandManager", likely_island_manager);
    write_candidate("LikelyR5TerrainSettings", likely_terrain_settings);

    log << "scanned_objects=" << scanned << "\n";
    log << "logged_hits=" << logged_hits << "\n";
    log << "note=Phase 7C1B performs a full object scan and logs only targeted candidates. No function invocation happened.\n";

    file_log("Phase 7C1B targeted context object discovery done scanned=" + std::to_string(scanned) + " hits=" + std::to_string(logged_hits));
}
// END PHASE7C1_CONTEXT_OBJECT_DISCOVERY



// BEGIN PHASE7D_EXPORT_RENDER_TARGET_ATTEMPT
struct Phase7D_FString_Lite
{
    wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

struct Phase7D_ExportRenderTarget_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    Phase7D_FString_Lite FilePath;
    Phase7D_FString_Lite FileName;
};

static Phase7D_FString_Lite phase7d_make_fstring_lite(std::wstring& value)
{
    Phase7D_FString_Lite result{};
    result.Data = const_cast<wchar_t*>(value.c_str());
    result.Num = static_cast<int32_t>(value.size() + 1);
    result.Max = static_cast<int32_t>(value.size() + 1);
    return result;
}

static void write_phase7d_export_render_target_attempt(UObject* trigger)
{
    static int attempts = 0;
    static bool success = false;

    if (success || !trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    if (attempts >= 10) return;
    attempts++;

    auto out = out_dir();
    std::ofstream log(out / "phase7d_export_render_target_attempt.txt", std::ios::app);

    file_log("Phase 7D ExportRenderTarget attempt " + std::to_string(attempts) + " entered");

    log << "===== Phase 7D ExportRenderTarget attempt " << attempts << " =====\n";
    log << "trigger=" << trigger_path << "\n";
    log << "mode=controlled_ProcessEvent_ExportRenderTarget_only\n";

    UObject* default_kismet = nullptr;
    UObject* rt_map_capture = nullptr;
    UObject* persistent_level = nullptr;
    UFunction* export_fn = nullptr;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o)
            {
                return RC::LoopAction::Continue;
            }

            scanned++;

            std::string full = obj_path(o);
            std::string cls = class_name(o);

            if (!default_kismet && full.find("/Script/Engine.Default__KismetRenderingLibrary") != std::string::npos)
            {
                default_kismet = o;
            }

            if (!rt_map_capture && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapCapture.RT_MapCapture") != std::string::npos)
            {
                rt_map_capture = o;
            }

            if (!persistent_level && cls == "Level" && full.find("PersistentLevel") != std::string::npos && full.find("/Game/Maps/") != std::string::npos)
            {
                persistent_level = o;
            }

            if (!export_fn && cls == "Function" && full.find("/Script/Engine.KismetRenderingLibrary:ExportRenderTarget") != std::string::npos)
            {
                export_fn = static_cast<UFunction*>(o);
            }

            return RC::LoopAction::Continue;
        }
    );

    log << "scanned_objects=" << scanned << "\n";

    auto log_obj = [&](const char* label, UObject* o)
    {
        log << label << "_found=" << (o ? "true" : "false") << "\n";

        if (o)
        {
            log << label << "_path=" << obj_path(o) << "\n";
            log << label << "_class=" << class_name(o) << "\n";
            log << label << "_addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";
        }
    };

    log_obj("default_kismet", default_kismet);
    log_obj("rt_map_capture", rt_map_capture);
    log_obj("persistent_level", persistent_level);
    log_obj("export_fn", reinterpret_cast<UObject*>(export_fn));

    if (!default_kismet || !rt_map_capture || !export_fn)
    {
        log << "decision=skip_missing_required_object\n\n";
        file_log("Phase 7D attempt " + std::to_string(attempts) + " skipped missing required object");
        return;
    }

    UObject* world_context = persistent_level ? persistent_level : trigger;

    std::wstring export_dir = L"Z:/home/pirat_king/windrose-server/R5/Binaries/Win64/windrose_plus_data/native_rt_export";
    std::wstring export_name = L"phase7d_RT_MapCapture_attempt_" + std::to_wstring(attempts);

    Phase7D_ExportRenderTarget_Params params{};
    params.WorldContextObject = world_context;
    params.TextureRenderTarget = rt_map_capture;
    params.FilePath = phase7d_make_fstring_lite(export_dir);
    params.FileName = phase7d_make_fstring_lite(export_name);

    log << "world_context_path=" << obj_path(world_context) << "\n";
    log << "export_dir=Z:/home/pirat_king/windrose-server/R5/Binaries/Win64/windrose_plus_data/native_rt_export\n";
    log << "export_name=phase7d_RT_MapCapture_attempt_" << attempts << "\n";
    log << "about_to_call_ProcessEvent=true\n";
    log.flush();

    default_kismet->ProcessEvent(export_fn, &params);

    log << "after_ProcessEvent=true\n";

    bool png_exists = std::filesystem::exists(out / ("phase7d_RT_MapCapture_attempt_" + std::to_string(attempts) + ".png"));
    bool hdr_exists = std::filesystem::exists(out / ("phase7d_RT_MapCapture_attempt_" + std::to_string(attempts) + ".hdr"));
    bool exr_exists = std::filesystem::exists(out / ("phase7d_RT_MapCapture_attempt_" + std::to_string(attempts) + ".exr"));

    log << "png_exists=" << (png_exists ? "true" : "false") << "\n";
    log << "hdr_exists=" << (hdr_exists ? "true" : "false") << "\n";
    log << "exr_exists=" << (exr_exists ? "true" : "false") << "\n";

    if (png_exists || hdr_exists || exr_exists)
    {
        success = true;
        log << "result=success_file_created\n\n";
        file_log("Phase 7D ExportRenderTarget SUCCESS attempt " + std::to_string(attempts));
        return;
    }

    log << "result=no_file_detected_after_call\n\n";
    file_log("Phase 7D ExportRenderTarget attempt " + std::to_string(attempts) + " completed no file detected");
}
// END PHASE7D_EXPORT_RENDER_TARGET_ATTEMPT



// BEGIN PHASE7E_COMBINED_EXPORT_STRATEGY
struct Phase7E_FString_Lite
{
    wchar_t* Data;
    int32_t Num;
    int32_t Max;
};

struct Phase7E_ExportRenderTarget_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    Phase7E_FString_Lite FilePath;
    Phase7E_FString_Lite FileName;
};

struct Phase7E_Color
{
    uint8_t B;
    uint8_t G;
    uint8_t R;
    uint8_t A;
};

struct Phase7E_LinearColor
{
    float R;
    float G;
    float B;
    float A;
};

struct Phase7E_ReadPixel_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    int32_t X;
    int32_t Y;
    Phase7E_Color ReturnValue;
};

struct Phase7E_ReadRawPixel_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    int32_t X;
    int32_t Y;
    Phase7E_LinearColor ReturnValue;
};

static Phase7E_FString_Lite phase7e_make_fstring_lite(std::wstring& value)
{
    Phase7E_FString_Lite result{};
    result.Data = const_cast<wchar_t*>(value.c_str());
    result.Num = static_cast<int32_t>(value.size() + 1);
    result.Max = static_cast<int32_t>(value.size() + 1);
    return result;
}

static bool phase7e_any_export_exists(const std::filesystem::path& out, const std::string& base)
{
    return
        std::filesystem::exists(out / (base + ".png")) ||
        std::filesystem::exists(out / (base + ".hdr")) ||
        std::filesystem::exists(out / (base + ".exr"));
}

static void write_phase7e_combined_export_strategy(UObject* trigger)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase7e_combined_export_strategy.txt", std::ios::out);

    file_log("Phase 7E combined export strategy entered");

    log << "Phase 7E combined controlled runtime invocation test\n";
    log << "trigger=" << trigger_path << "\n";
    log << "mode=multi_context_multi_path_ExportRenderTarget_plus_single_pixel_sanity\n";
    log << "note=no_large_TArray_reads_no_raw_dumps\n\n";

    UObject* default_kismet = nullptr;
    UObject* rt_map_capture = nullptr;
    UObject* rt_map_fog = nullptr;
    UObject* persistent_level = nullptr;
    UObject* default_world = nullptr;
    UObject* default_game_instance = nullptr;
    UObject* default_game_engine = nullptr;
    UObject* default_game_viewport = nullptr;
    UObject* default_player_controller = nullptr;

    UFunction* export_fn = nullptr;
    UFunction* read_pixel_fn = nullptr;
    UFunction* read_raw_pixel_fn = nullptr;
    UFunction* read_uv_fn = nullptr;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string full = obj_path(o);
            std::string cls = class_name(o);

            if (!default_kismet && full.find("/Script/Engine.Default__KismetRenderingLibrary") != std::string::npos) default_kismet = o;
            if (!rt_map_capture && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapCapture.RT_MapCapture") != std::string::npos) rt_map_capture = o;
            if (!rt_map_fog && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapFog.RT_MapFog") != std::string::npos) rt_map_fog = o;
            if (!persistent_level && cls == "Level" && full.find("PersistentLevel") != std::string::npos && full.find("/Game/Maps/") != std::string::npos) persistent_level = o;
            if (!default_world && full.find("/Script/Engine.Default__World") != std::string::npos) default_world = o;
            if (!default_game_instance && full.find("/Script/Engine.Default__GameInstance") != std::string::npos) default_game_instance = o;
            if (!default_game_engine && full.find("/Script/Engine.Default__GameEngine") != std::string::npos) default_game_engine = o;
            if (!default_game_viewport && full.find("/Script/Engine.Default__GameViewportClient") != std::string::npos) default_game_viewport = o;
            if (!default_player_controller && full.find("/Script/Engine.Default__PlayerController") != std::string::npos) default_player_controller = o;

            if (cls == "Function")
            {
                if (!export_fn && full.find("/Script/Engine.KismetRenderingLibrary:ExportRenderTarget") != std::string::npos) export_fn = static_cast<UFunction*>(o);
                if (!read_pixel_fn && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetPixel") != std::string::npos) read_pixel_fn = static_cast<UFunction*>(o);
                if (!read_raw_pixel_fn && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetRawPixel") != std::string::npos) read_raw_pixel_fn = static_cast<UFunction*>(o);
                if (!read_uv_fn && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetUV") != std::string::npos) read_uv_fn = static_cast<UFunction*>(o);
            }

            return RC::LoopAction::Continue;
        }
    );

    log << "scanned_objects=" << scanned << "\n";

    auto log_obj = [&](const char* label, UObject* o)
    {
        log << label << "_found=" << (o ? "true" : "false") << "\n";
        if (o)
        {
            log << label << "_path=" << obj_path(o) << "\n";
            log << label << "_class=" << class_name(o) << "\n";
            log << label << "_addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";
        }
    };

    log_obj("default_kismet", default_kismet);
    log_obj("rt_map_capture", rt_map_capture);
    log_obj("rt_map_fog", rt_map_fog);
    log_obj("persistent_level", persistent_level);
    log_obj("default_world", default_world);
    log_obj("default_game_instance", default_game_instance);
    log_obj("default_game_engine", default_game_engine);
    log_obj("default_game_viewport", default_game_viewport);
    log_obj("default_player_controller", default_player_controller);
    log_obj("export_fn", reinterpret_cast<UObject*>(export_fn));
    log_obj("read_pixel_fn", reinterpret_cast<UObject*>(read_pixel_fn));
    log_obj("read_raw_pixel_fn", reinterpret_cast<UObject*>(read_raw_pixel_fn));
    log_obj("read_uv_fn", reinterpret_cast<UObject*>(read_uv_fn));

    if (!default_kismet || !rt_map_capture || !export_fn)
    {
        log << "decision=abort_missing_required_export_objects\n";
        file_log("Phase 7E aborted missing required export objects");
        return;
    }

    struct ContextCandidate
    {
        const char* label;
        UObject* object;
    };

    ContextCandidate contexts[] = {
        {"persistent_level", persistent_level},
        {"rt_map_capture_self", rt_map_capture},
        {"default_world", default_world},
        {"default_game_instance", default_game_instance},
        {"default_game_engine", default_game_engine},
        {"default_game_viewport", default_game_viewport},
        {"default_player_controller", default_player_controller},
        {"default_kismet_self", default_kismet}
    };

    struct PathCandidate
    {
        const char* label;
        const wchar_t* file_path;
        bool append_extension_in_name;
    };

    PathCandidate paths[] = {
        {"wine_z_absolute_no_ext", L"Z:/home/pirat_king/windrose-server/R5/Binaries/Win64/windrose_plus_data/native_rt_export", false},
        {"wine_z_absolute_png_name", L"Z:/home/pirat_king/windrose-server/R5/Binaries/Win64/windrose_plus_data/native_rt_export", true},
        {"linux_absolute_no_ext", L"/home/pirat_king/windrose-server/R5/Binaries/Win64/windrose_plus_data/native_rt_export", false},
        {"relative_no_ext", L"windrose_plus_data/native_rt_export", false}
    };

    int export_attempts = 0;
    int export_successes = 0;

    for (const auto& ctx : contexts)
    {
        if (!ctx.object) continue;

        for (const auto& path_candidate : paths)
        {
            export_attempts++;

            std::string base_name =
                std::string("phase7e_RT_MapCapture_") +
                ctx.label + "_" +
                path_candidate.label;

            std::wstring w_path(path_candidate.file_path);
            std::wstring w_name(base_name.begin(), base_name.end());

            if (path_candidate.append_extension_in_name)
            {
                w_name += L".png";
            }

            Phase7E_ExportRenderTarget_Params params{};
            params.WorldContextObject = ctx.object;
            params.TextureRenderTarget = rt_map_capture;
            params.FilePath = phase7e_make_fstring_lite(w_path);
            params.FileName = phase7e_make_fstring_lite(w_name);

            log << "\nEXPORT_ATTEMPT[" << export_attempts << "]\n";
            log << "  context_label=" << ctx.label << "\n";
            log << "  context_path=" << obj_path(ctx.object) << "\n";
            log << "  path_label=" << path_candidate.label << "\n";
            log << "  base_name=" << base_name << "\n";
            log << "  about_to_call_ProcessEvent=true\n";
            log.flush();

            default_kismet->ProcessEvent(export_fn, &params);

            log << "  after_ProcessEvent=true\n";

            bool exists_no_ext = phase7e_any_export_exists(out, base_name);
            bool exists_png_name_png = phase7e_any_export_exists(out, base_name + ".png");

            log << "  exists_base_png_hdr_exr=" << (exists_no_ext ? "true" : "false") << "\n";
            log << "  exists_pngname_png_hdr_exr=" << (exists_png_name_png ? "true" : "false") << "\n";

            if (exists_no_ext || exists_png_name_png)
            {
                export_successes++;
                log << "  result=success_file_detected\n";
            }
            else
            {
                log << "  result=no_file_detected\n";
            }
        }
    }

    log << "\nPIXEL_SANITY_TESTS\n";

    UObject* pixel_context = persistent_level ? persistent_level : rt_map_capture;

    if (read_pixel_fn && pixel_context)
    {
        Phase7E_ReadPixel_Params p0{};
        p0.WorldContextObject = pixel_context;
        p0.TextureRenderTarget = rt_map_capture;
        p0.X = 1024;
        p0.Y = 1024;

        log << "ReadRenderTargetPixel about_to_call_ProcessEvent=true\n";
        log.flush();
        default_kismet->ProcessEvent(read_pixel_fn, &p0);
        log << "ReadRenderTargetPixel after_ProcessEvent=true\n";
        log << "ReadRenderTargetPixel ReturnValue RGBA="
            << static_cast<int>(p0.ReturnValue.R) << ","
            << static_cast<int>(p0.ReturnValue.G) << ","
            << static_cast<int>(p0.ReturnValue.B) << ","
            << static_cast<int>(p0.ReturnValue.A) << "\n";
    }
    else
    {
        log << "ReadRenderTargetPixel skipped_missing_function_or_context\n";
    }

    if (read_raw_pixel_fn && pixel_context)
    {
        Phase7E_ReadRawPixel_Params p1{};
        p1.WorldContextObject = pixel_context;
        p1.TextureRenderTarget = rt_map_capture;
        p1.X = 1024;
        p1.Y = 1024;

        log << "ReadRenderTargetRawPixel about_to_call_ProcessEvent=true\n";
        log.flush();
        default_kismet->ProcessEvent(read_raw_pixel_fn, &p1);
        log << "ReadRenderTargetRawPixel after_ProcessEvent=true\n";
        log << "ReadRenderTargetRawPixel ReturnValue RGBA="
            << p1.ReturnValue.R << ","
            << p1.ReturnValue.G << ","
            << p1.ReturnValue.B << ","
            << p1.ReturnValue.A << "\n";
    }
    else
    {
        log << "ReadRenderTargetRawPixel skipped_missing_function_or_context\n";
    }

    log << "\nSUMMARY\n";
    log << "export_attempts=" << export_attempts << "\n";
    log << "export_successes=" << export_successes << "\n";
    log << "note=Phase 7E combined test completed. If any files exist, upload them with this log.\n";

    file_log("Phase 7E combined export strategy done attempts=" + std::to_string(export_attempts) + " successes=" + std::to_string(export_successes));
}
// END PHASE7E_COMBINED_EXPORT_STRATEGY



// BEGIN PHASE7F_PIXEL_GRID_SAMPLE
struct Phase7F_Color
{
    uint8_t B;
    uint8_t G;
    uint8_t R;
    uint8_t A;
};

struct Phase7F_ReadPixel_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    int32_t X;
    int32_t Y;
    Phase7F_Color ReturnValue;
};

static void write_phase7f_pixel_grid_sample(UObject* trigger)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    done = true;

    auto out = out_dir();
    std::ofstream log(out / "phase7f_pixel_grid_sample.txt", std::ios::out);

    file_log("Phase 7F pixel grid sample entered");

    log << "Phase 7F pixel grid sample\n";
    log << "trigger=" << trigger_path << "\n";
    log << "mode=ReadRenderTargetPixel_grid_32x32_write_csv_and_ppm\n";
    log << "note=no_large_TArray_no_raw_bin_no_ExportRenderTarget\n\n";

    UObject* default_kismet = nullptr;
    UObject* rt_map_capture = nullptr;
    UObject* persistent_level = nullptr;
    UFunction* read_pixel_fn = nullptr;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string full = obj_path(o);
            std::string cls = class_name(o);

            if (!default_kismet && full.find("/Script/Engine.Default__KismetRenderingLibrary") != std::string::npos)
            {
                default_kismet = o;
            }

            if (!rt_map_capture && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapCapture.RT_MapCapture") != std::string::npos)
            {
                rt_map_capture = o;
            }

            if (!persistent_level && cls == "Level" && full.find("PersistentLevel") != std::string::npos && full.find("/Game/Maps/") != std::string::npos)
            {
                persistent_level = o;
            }

            if (!read_pixel_fn && cls == "Function" && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetPixel") != std::string::npos)
            {
                read_pixel_fn = static_cast<UFunction*>(o);
            }

            return RC::LoopAction::Continue;
        }
    );

    auto log_obj = [&](const char* label, UObject* o)
    {
        log << label << "_found=" << (o ? "true" : "false") << "\n";
        if (o)
        {
            log << label << "_path=" << obj_path(o) << "\n";
            log << label << "_class=" << class_name(o) << "\n";
            log << label << "_addr=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\n";
        }
    };

    log << "scanned_objects=" << scanned << "\n";
    log_obj("default_kismet", default_kismet);
    log_obj("rt_map_capture", rt_map_capture);
    log_obj("persistent_level", persistent_level);
    log_obj("read_pixel_fn", reinterpret_cast<UObject*>(read_pixel_fn));

    if (!default_kismet || !rt_map_capture || !read_pixel_fn)
    {
        log << "decision=abort_missing_required_objects\n";
        file_log("Phase 7F aborted missing required objects");
        return;
    }

    UObject* world_context = persistent_level ? persistent_level : rt_map_capture;

    constexpr int W = 32;
    constexpr int H = 32;
    constexpr int RT_SIZE = 2048;

    std::ofstream csv(out / "phase7f_RT_MapCapture_32x32_pixels.csv", std::ios::out);
    std::ofstream ppm(out / "phase7f_RT_MapCapture_32x32_preview.ppm", std::ios::out);

    csv << "sample_x,sample_y,rt_x,rt_y,r,g,b,a\n";
    ppm << "P3\n" << W << " " << H << "\n255\n";

    int non_black = 0;
    int non_transparent = 0;
    int calls = 0;

    for (int sy = 0; sy < H; ++sy)
    {
        for (int sx = 0; sx < W; ++sx)
        {
            int x = static_cast<int>((static_cast<double>(sx) + 0.5) * static_cast<double>(RT_SIZE) / static_cast<double>(W));
            int y = static_cast<int>((static_cast<double>(sy) + 0.5) * static_cast<double>(RT_SIZE) / static_cast<double>(H));

            Phase7F_ReadPixel_Params params{};
            params.WorldContextObject = world_context;
            params.TextureRenderTarget = rt_map_capture;
            params.X = x;
            params.Y = y;

            default_kismet->ProcessEvent(read_pixel_fn, &params);
            calls++;

            int r = static_cast<int>(params.ReturnValue.R);
            int g = static_cast<int>(params.ReturnValue.G);
            int b = static_cast<int>(params.ReturnValue.B);
            int a = static_cast<int>(params.ReturnValue.A);

            if (r != 0 || g != 0 || b != 0) non_black++;
            if (a != 0) non_transparent++;

            csv << sx << "," << sy << "," << x << "," << y << ","
                << r << "," << g << "," << b << "," << a << "\n";

            ppm << r << " " << g << " " << b << "\n";
        }
    }

    csv.close();
    ppm.close();

    log << "world_context_path=" << obj_path(world_context) << "\n";
    log << "grid_width=" << W << "\n";
    log << "grid_height=" << H << "\n";
    log << "rt_assumed_size=" << RT_SIZE << "\n";
    log << "process_event_calls=" << calls << "\n";
    log << "non_black_samples=" << non_black << "\n";
    log << "non_transparent_samples=" << non_transparent << "\n";
    log << "csv_file=phase7f_RT_MapCapture_32x32_pixels.csv\n";
    log << "ppm_file=phase7f_RT_MapCapture_32x32_preview.ppm\n";
    log << "result=completed\n";

    file_log("Phase 7F pixel grid sample done calls=" + std::to_string(calls) + " non_black=" + std::to_string(non_black));
}
// END PHASE7F_PIXEL_GRID_SAMPLE



// BEGIN PHASE7G_LONG_SAMPLING_CAMPAIGN
struct Phase7G_Color
{
    uint8_t B;
    uint8_t G;
    uint8_t R;
    uint8_t A;
};

struct Phase7G_LinearColor
{
    float R;
    float G;
    float B;
    float A;
};

struct Phase7G_ReadPixel_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    int32_t X;
    int32_t Y;
    Phase7G_Color ReturnValue;
};

struct Phase7G_ReadRawPixel_Params
{
    UObject* WorldContextObject;
    UObject* TextureRenderTarget;
    int32_t X;
    int32_t Y;
    Phase7G_LinearColor ReturnValue;
};

static void write_phase7g_long_sampling_campaign(UObject* trigger, int scan_attempt)
{
    static bool header_written = false;
    static int passes = 0;

    if (!trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    if (scan_attempt < 8) return;
    if (passes >= 20) return;

    passes++;

    auto out = out_dir();
    std::ofstream log(out / "phase7g_long_sampling_campaign.txt", std::ios::app);
    std::ofstream csv(out / "phase7g_long_sampling_campaign.csv", std::ios::app);

    if (!header_written)
    {
        log << "Phase 7G long sampling campaign\n";
        log << "mode=20_passes_over_late_scan_attempts_multi_target_multi_method\n";
        log << "note=no_large_TArray_no_raw_bin_no_ExportRenderTarget\n\n";

        csv << "pass,scan_attempt,target_label,target_path,method,sample_label,x,y,r,g,b,a,raw_r,raw_g,raw_b,raw_a\n";
        header_written = true;
    }

    file_log("Phase 7G long sampling campaign pass " + std::to_string(passes) + " scan_attempt " + std::to_string(scan_attempt));

    UObject* default_kismet = nullptr;
    UObject* rt_map_capture = nullptr;
    UObject* rt_map_fog = nullptr;
    UObject* rt_wetness = nullptr;
    UObject* rt_overlap = nullptr;
    UObject* rt_surface_height = nullptr;
    UObject* persistent_level = nullptr;

    UFunction* read_pixel_fn = nullptr;
    UFunction* read_raw_pixel_fn = nullptr;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string full = obj_path(o);
            std::string cls = class_name(o);

            if (!default_kismet && full.find("/Script/Engine.Default__KismetRenderingLibrary") != std::string::npos)
                default_kismet = o;

            if (!rt_map_capture && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapCapture.RT_MapCapture") != std::string::npos)
                rt_map_capture = o;

            if (!rt_map_fog && full.find("/Game/UI/META/FullscreenMap/Assets/RT_MapFog.RT_MapFog") != std::string::npos)
                rt_map_fog = o;

            if (!rt_wetness && full.find("/R5Nature/Rain/RT_WetnessMask.RT_WetnessMask") != std::string::npos)
                rt_wetness = o;

            if (!rt_overlap && full.find("/R5Nature/Rain/RT_OverlapMask.RT_OverlapMask") != std::string::npos)
                rt_overlap = o;

            if (!rt_surface_height && full.find("/R5Nature/Rain/RT_SurfaceHeight.RT_SurfaceHeight") != std::string::npos)
                rt_surface_height = o;

            if (!persistent_level && cls == "Level" && full.find("PersistentLevel") != std::string::npos && full.find("/Game/Maps/") != std::string::npos)
                persistent_level = o;

            if (cls == "Function")
            {
                if (!read_pixel_fn && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetPixel") != std::string::npos)
                    read_pixel_fn = static_cast<UFunction*>(o);

                if (!read_raw_pixel_fn && full.find("/Script/Engine.KismetRenderingLibrary:ReadRenderTargetRawPixel") != std::string::npos)
                    read_raw_pixel_fn = static_cast<UFunction*>(o);
            }

            return RC::LoopAction::Continue;
        }
    );

    log << "PASS " << passes << " scan_attempt=" << scan_attempt << "\n";
    log << "scanned_objects=" << scanned << "\n";
    log << "default_kismet_found=" << (default_kismet ? "true" : "false") << "\n";
    log << "persistent_level_found=" << (persistent_level ? "true" : "false") << "\n";
    log << "read_pixel_fn_found=" << (read_pixel_fn ? "true" : "false") << "\n";
    log << "read_raw_pixel_fn_found=" << (read_raw_pixel_fn ? "true" : "false") << "\n";

    if (!default_kismet || !persistent_level || !read_pixel_fn)
    {
        log << "decision=skip_missing_required_objects\n\n";
        return;
    }

    struct TargetCandidate
    {
        const char* label;
        UObject* object;
    };

    TargetCandidate targets[] = {
        {"RT_MapCapture", rt_map_capture},
        {"RT_MapFog", rt_map_fog},
        {"RT_WetnessMask", rt_wetness},
        {"RT_OverlapMask", rt_overlap},
        {"RT_SurfaceHeight", rt_surface_height}
    };

    struct SamplePoint
    {
        const char* label;
        int x;
        int y;
    };

    SamplePoint samples[] = {
        {"top_left", 64, 64},
        {"top_center", 1024, 64},
        {"top_right", 1984, 64},
        {"mid_left", 64, 1024},
        {"center", 1024, 1024},
        {"mid_right", 1984, 1024},
        {"bottom_left", 64, 1984},
        {"bottom_center", 1024, 1984},
        {"bottom_right", 1984, 1984},
        {"q1", 512, 512},
        {"q2", 1536, 512},
        {"q3", 512, 1536},
        {"q4", 1536, 1536}
    };

    int total_calls = 0;
    int non_red_pixel = 0;
    int non_black_pixel = 0;

    for (const auto& target : targets)
    {
        if (!target.object)
        {
            log << "target_missing=" << target.label << "\n";
            continue;
        }

        std::string target_path = obj_path(target.object);
        log << "target=" << target.label << " path=" << target_path << "\n";

        for (const auto& sample : samples)
        {
            Phase7G_ReadPixel_Params p0{};
            p0.WorldContextObject = persistent_level;
            p0.TextureRenderTarget = target.object;
            p0.X = sample.x;
            p0.Y = sample.y;

            default_kismet->ProcessEvent(read_pixel_fn, &p0);
            total_calls++;

            int r = static_cast<int>(p0.ReturnValue.R);
            int g = static_cast<int>(p0.ReturnValue.G);
            int b = static_cast<int>(p0.ReturnValue.B);
            int a = static_cast<int>(p0.ReturnValue.A);

            if (!(r == 255 && g == 0 && b == 0 && a == 255)) non_red_pixel++;
            if (r != 0 || g != 0 || b != 0) non_black_pixel++;

            float rr = -999.0f;
            float rg = -999.0f;
            float rb = -999.0f;
            float ra = -999.0f;

            if (read_raw_pixel_fn)
            {
                Phase7G_ReadRawPixel_Params p1{};
                p1.WorldContextObject = persistent_level;
                p1.TextureRenderTarget = target.object;
                p1.X = sample.x;
                p1.Y = sample.y;

                default_kismet->ProcessEvent(read_raw_pixel_fn, &p1);
                total_calls++;

                rr = p1.ReturnValue.R;
                rg = p1.ReturnValue.G;
                rb = p1.ReturnValue.B;
                ra = p1.ReturnValue.A;
            }

            csv << passes << ","
                << scan_attempt << ","
                << target.label << ","
                << target_path << ","
                << "pixel_and_raw" << ","
                << sample.label << ","
                << sample.x << ","
                << sample.y << ","
                << r << ","
                << g << ","
                << b << ","
                << a << ","
                << rr << ","
                << rg << ","
                << rb << ","
                << ra << "\n";
        }
    }

    log << "total_process_event_calls=" << total_calls << "\n";
    log << "non_red_pixel_samples=" << non_red_pixel << "\n";
    log << "non_black_pixel_samples=" << non_black_pixel << "\n";
    log << "csv_file=phase7g_long_sampling_campaign.csv\n";
    log << "pass_result=completed\n\n";

    file_log("Phase 7G pass " + std::to_string(passes) + " completed non_red=" + std::to_string(non_red_pixel));
}
// END PHASE7G_LONG_SAMPLING_CAMPAIGN



// BEGIN PHASE8A_LANDSCAPE_HEIGHTMAP_EXTRACTION


static void write_phase8a_landscape_heightmap_extraction(UObject* trigger, int scan_attempt)
{
    static bool started = false;
    static int passes = 0;

    if (!trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    if (scan_attempt < 8) return;
    if (passes >= 20) return;

    passes++;

    auto out = out_dir();

    std::ofstream log(out / "phase8a_landscape_heightmap_extraction.txt", std::ios::app);
    std::ofstream csv(out / "phase8a_landscape_components.csv", std::ios::app);
    std::ofstream summary(out / "phase8a_summary.txt", std::ios::app);

    if (!started)
    {
        started = true;

        log << "Phase 8A Landscape/Heightmap extraction\n";
        log << "mode=aggressive_multi_pass_landscape_component_scan\n";
        log << "reference=WindrosePlus_terrain_v17_style_pipeline\n";
        log << "goal=authoritative_world_map_reconstruction\n";
        log << "note=no_large_raw_height_dumps_yet\n\n";

        csv << "pass,scan_attempt,class,name,path,addr\n";
    }

    file_log("Phase 8A pass " + std::to_string(passes));

    int scanned = 0;
    int landscape_hits = 0;
    int collision_hits = 0;
    int landscape_proxy_hits = 0;
    int terrain_related_hits = 0;

    log << "===== PASS " << passes << " scan_attempt=" << scan_attempt << " =====\n";

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string full = obj_path(o);

            bool interesting = false;

            if (cls.find("Landscape") != std::string::npos) interesting = true;
            if (cls.find("Heightfield") != std::string::npos) interesting = true;
            if (cls.find("Terrain") != std::string::npos) interesting = true;
            if (name.find("Landscape") != std::string::npos) interesting = true;
            if (name.find("Terrain") != std::string::npos) interesting = true;
            if (full.find("Landscape") != std::string::npos) interesting = true;
            if (full.find("Terrain") != std::string::npos) interesting = true;

            if (!interesting)
                return RC::LoopAction::Continue;

            terrain_related_hits++;

            if (cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos)
                collision_hits++;

            if (cls.find("LandscapeProxy") != std::string::npos)
                landscape_proxy_hits++;

            if (cls.find("Landscape") != std::string::npos)
                landscape_hits++;

            uintptr_t addr = reinterpret_cast<uintptr_t>(o);

            log << "OBJECT\n";
            log << "  class=" << cls << "\n";
            log << "  name=" << name << "\n";
            log << "  path=" << full << "\n";
            log << "  addr=0x" << std::hex << addr << std::dec << "\n";

            csv
                << passes << ","
                << scan_attempt << ","
                << "\"" << cls << "\","
                << "\"" << name << "\","
                << "\"" << full << "\","
                << "\"0x" << std::hex << addr << std::dec << "\"\n";
            log << "  property_probe=disabled_v1.11.2_no_UObject_GetClass_available\n";

            log << "\n";

            return RC::LoopAction::Continue;
        }
    );

    log << "PASS_SUMMARY\n";
    log << "  scanned_objects=" << scanned << "\n";
    log << "  terrain_related_hits=" << terrain_related_hits << "\n";
    log << "  landscape_hits=" << landscape_hits << "\n";
    log << "  collision_hits=" << collision_hits << "\n";
    log << "  landscape_proxy_hits=" << landscape_proxy_hits << "\n";
    log << "\n";

    summary << "pass=" << passes
            << " scan_attempt=" << scan_attempt
            << " scanned=" << scanned
            << " terrain_related=" << terrain_related_hits
            << " landscape=" << landscape_hits
            << " collision=" << collision_hits
            << " proxy=" << landscape_proxy_hits
            << "\n";

    file_log(
        "Phase 8A completed pass=" +
        std::to_string(passes) +
        " terrain_hits=" +
        std::to_string(terrain_related_hits)
    );
}

// END PHASE8A_LANDSCAPE_HEIGHTMAP_EXTRACTION



// BEGIN PHASE8B_TARGETED_LANDSCAPE_RUNTIME_INDEX
static std::string phase8b_json_escape(const std::string& input)
{
    std::string out;
    out.reserve(input.size() + 16);

    for (char c : input)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }

    return out;
}

static std::string phase8b_parent_before_colon_or_dot(const std::string& path)
{
    size_t colon = path.find(':');
    if (colon != std::string::npos)
    {
        return path.substr(0, colon);
    }

    size_t dot = path.rfind('.');
    if (dot != std::string::npos)
    {
        return path.substr(0, dot);
    }

    return path;
}

static int phase8b_extract_last_int_after_token(const std::string& s, const std::string& token)
{
    size_t pos = s.rfind(token);
    if (pos == std::string::npos)
    {
        return -1;
    }

    pos += token.size();

    std::string digits;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
    {
        digits.push_back(s[pos]);
        pos++;
    }

    if (digits.empty())
    {
        return -1;
    }

    try
    {
        return std::stoi(digits);
    }
    catch (...)
    {
        return -1;
    }
}

static void write_phase8b_targeted_landscape_runtime_index(UObject* trigger, int scan_attempt)
{
    static bool done = false;
    if (done || !trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    if (scan_attempt < 12) return;

    done = true;

    auto out = out_dir();

    std::ofstream log(out / "phase8b_targeted_landscape_runtime_index.txt", std::ios::out);
    std::ofstream csv(out / "phase8b_runtime_landscape_components.csv", std::ios::out);
    std::ofstream jsonl(out / "phase8b_runtime_landscape_components.jsonl", std::ios::out);

    file_log("Phase 8B targeted landscape runtime index entered");

    log << "Phase 8B targeted landscape runtime index\n";
    log << "mode=runtime_PersistentLevel_landscape_components_only\n";
    log << "goal=index_real_runtime_landscape_collision_and_render_components_for_heightmap_exporter_track\n";
    log << "note=no_raw_memory_height_dumps_yet\n\n";

    csv << "kind,class,name,path,parent,addr,landscape_id,component_id,is_runtime_persistent,is_default_object\n";

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        std::string parent;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        bool is_runtime_persistent;
        bool is_default_object;
    };

    std::vector<Rec> records;

    int scanned = 0;
    int runtime_collision = 0;
    int runtime_landscape_component = 0;
    int runtime_landscape_actor = 0;
    int runtime_scene_component = 0;
    int runtime_other_landscape = 0;
    int default_skipped = 0;
    int class_skipped = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool is_default = path.find("Default__") != std::string::npos;
            bool is_class = cls == "Class" || cls == "ScriptStruct" || cls == "Enum" || cls == "Function" || cls == "Package";
            bool is_runtime_persistent = path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                                         path.find("PersistentLevel") != std::string::npos;

            bool landscape_related =
                cls.find("Landscape") != std::string::npos ||
                name.find("Landscape") != std::string::npos ||
                path.find("Landscape") != std::string::npos;

            if (!landscape_related)
            {
                return RC::LoopAction::Continue;
            }

            if (is_default)
            {
                default_skipped++;
                return RC::LoopAction::Continue;
            }

            if (is_class)
            {
                class_skipped++;
                return RC::LoopAction::Continue;
            }

            if (!is_runtime_persistent)
            {
                return RC::LoopAction::Continue;
            }

            std::string kind = "other_landscape";
            if (cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos)
            {
                kind = "collision";
                runtime_collision++;
            }
            else if (cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos)
            {
                kind = "landscape_component";
                runtime_landscape_component++;
            }
            else if (cls == "Landscape" || cls == "LandscapeStreamingProxy" || cls == "LandscapeProxy")
            {
                kind = "landscape_actor";
                runtime_landscape_actor++;
            }
            else if (cls == "SceneComponent")
            {
                kind = "scene_component";
                runtime_scene_component++;
            }
            else
            {
                runtime_other_landscape++;
            }

            int landscape_id = phase8b_extract_last_int_after_token(path, "Landscape_");
            int component_id = phase8b_extract_last_int_after_token(path, "LandscapeComponent_");
            if (component_id < 0)
            {
                component_id = phase8b_extract_last_int_after_token(path, "CollisionComponent_");
            }

            Rec r{};
            r.kind = kind;
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.parent = phase8b_parent_before_colon_or_dot(path);
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = landscape_id;
            r.component_id = component_id;
            r.is_runtime_persistent = is_runtime_persistent;
            r.is_default_object = is_default;

            records.push_back(r);

            return RC::LoopAction::Continue;
        }
    );

    std::sort(records.begin(), records.end(), [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        if (a.component_id != b.component_id) return a.component_id < b.component_id;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.path < b.path;
    });

    int min_landscape_id = 999999999;
    int max_landscape_id = -1;
    int min_component_id = 999999999;
    int max_component_id = -1;

    std::map<int, int> landscape_counts;
    std::map<std::string, int> kind_counts;
    std::map<int, int> component_id_counts;

    for (const auto& r : records)
    {
        if (r.landscape_id >= 0)
        {
            min_landscape_id = std::min(min_landscape_id, r.landscape_id);
            max_landscape_id = std::max(max_landscape_id, r.landscape_id);
            landscape_counts[r.landscape_id]++;
        }

        if (r.component_id >= 0)
        {
            min_component_id = std::min(min_component_id, r.component_id);
            max_component_id = std::max(max_component_id, r.component_id);
            component_id_counts[r.component_id]++;
        }

        kind_counts[r.kind]++;

        csv << "\"" << r.kind << "\","
            << "\"" << r.cls << "\","
            << "\"" << r.name << "\","
            << "\"" << r.path << "\","
            << "\"" << r.parent << "\","
            << "\"0x" << std::hex << r.addr << std::dec << "\","
            << r.landscape_id << ","
            << r.component_id << ","
            << (r.is_runtime_persistent ? "true" : "false") << ","
            << (r.is_default_object ? "true" : "false") << "\n";

        jsonl << "{"
              << "\"kind\":\"" << phase8b_json_escape(r.kind) << "\","
              << "\"class\":\"" << phase8b_json_escape(r.cls) << "\","
              << "\"name\":\"" << phase8b_json_escape(r.name) << "\","
              << "\"path\":\"" << phase8b_json_escape(r.path) << "\","
              << "\"parent\":\"" << phase8b_json_escape(r.parent) << "\","
              << "\"addr\":\"0x" << std::hex << r.addr << std::dec << "\","
              << "\"landscape_id\":" << r.landscape_id << ","
              << "\"component_id\":" << r.component_id << ","
              << "\"is_runtime_persistent\":" << (r.is_runtime_persistent ? "true" : "false") << ","
              << "\"is_default_object\":" << (r.is_default_object ? "true" : "false")
              << "}\n";
    }

    log << "SUMMARY\n";
    log << "  scanned_objects=" << scanned << "\n";
    log << "  runtime_records=" << records.size() << "\n";
    log << "  runtime_collision=" << runtime_collision << "\n";
    log << "  runtime_landscape_component=" << runtime_landscape_component << "\n";
    log << "  runtime_landscape_actor=" << runtime_landscape_actor << "\n";
    log << "  runtime_scene_component=" << runtime_scene_component << "\n";
    log << "  runtime_other_landscape=" << runtime_other_landscape << "\n";
    log << "  default_skipped=" << default_skipped << "\n";
    log << "  class_skipped=" << class_skipped << "\n";

    if (min_landscape_id != 999999999)
    {
        log << "  landscape_id_min=" << min_landscape_id << "\n";
        log << "  landscape_id_max=" << max_landscape_id << "\n";
        log << "  unique_landscape_ids=" << landscape_counts.size() << "\n";
    }

    if (min_component_id != 999999999)
    {
        log << "  component_id_min=" << min_component_id << "\n";
        log << "  component_id_max=" << max_component_id << "\n";
        log << "  unique_component_ids=" << component_id_counts.size() << "\n";
    }

    log << "\nKIND_COUNTS\n";
    for (const auto& kv : kind_counts)
    {
        log << "  " << kv.first << "=" << kv.second << "\n";
    }

    log << "\nLANDSCAPE_COUNTS_TOP\n";
    int shown = 0;
    for (const auto& kv : landscape_counts)
    {
        log << "  Landscape_" << kv.first << "=" << kv.second << "\n";
        shown++;
        if (shown >= 80) break;
    }

    log << "\nSAMPLE_RECORDS_FIRST_80\n";
    int sample_count = 0;
    for (const auto& r : records)
    {
        log << "RECORD\n";
        log << "  kind=" << r.kind << "\n";
        log << "  class=" << r.cls << "\n";
        log << "  name=" << r.name << "\n";
        log << "  path=" << r.path << "\n";
        log << "  parent=" << r.parent << "\n";
        log << "  addr=0x" << std::hex << r.addr << std::dec << "\n";
        log << "  landscape_id=" << r.landscape_id << "\n";
        log << "  component_id=" << r.component_id << "\n\n";

        sample_count++;
        if (sample_count >= 80) break;
    }

    log << "OUTPUTS\n";
    log << "  csv=phase8b_runtime_landscape_components.csv\n";
    log << "  jsonl=phase8b_runtime_landscape_components.jsonl\n";
    log << "  txt=phase8b_targeted_landscape_runtime_index.txt\n";

    file_log("Phase 8B done records=" + std::to_string(records.size()) + " collisions=" + std::to_string(runtime_collision));
}
// END PHASE8B_TARGETED_LANDSCAPE_RUNTIME_INDEX



// BEGIN PHASE8C_COMPONENT_LAYOUT_AND_TINY_RAW_PROBES
static uint64_t phase8c_fnv1a64(const uint8_t* data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static bool phase8c_is_probably_readable_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static int32_t phase8c_read_i32_unchecked(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint32_t phase8c_read_u32_unchecked(uint8_t* base, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase8c_read_ptr_unchecked(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static void phase8c_dump_tiny_window(std::ofstream& rawlog, const char* label, UObject* o, size_t base_off, size_t len)
{
    if (!o) return;

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    uint8_t buf[256]{};

    if (len > sizeof(buf)) len = sizeof(buf);

    std::memcpy(buf, base + base_off, len);

    uint8_t minv = 255;
    uint8_t maxv = 0;
    uint64_t sum = 0;

    for (size_t i = 0; i < len; ++i)
    {
        minv = std::min(minv, buf[i]);
        maxv = std::max(maxv, buf[i]);
        sum += buf[i];
    }

    rawlog << "RAW_WINDOW label=" << label
           << " object=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec
           << " offset=" << base_off
           << " len=" << len
           << " min=" << static_cast<int>(minv)
           << " max=" << static_cast<int>(maxv)
           << " avg=" << (len ? (double)sum / (double)len : 0.0)
           << " fnv64=0x" << std::hex << phase8c_fnv1a64(buf, len) << std::dec
           << "\n";

    rawlog << "  hex=";
    for (size_t i = 0; i < len && i < 96; ++i)
    {
        rawlog << std::hex;
        int v = static_cast<int>(buf[i]);
        if (v < 16) rawlog << "0";
        rawlog << v;
        if (i + 1 < len && i + 1 < 96) rawlog << " ";
    }
    rawlog << std::dec << "\n";

    rawlog << "  u16_first32=";
    for (size_t i = 0; i + 1 < len && i < 64; i += 2)
    {
        uint16_t v = 0;
        std::memcpy(&v, buf + i, sizeof(v));
        rawlog << v;
        if (i + 2 < len && i + 2 < 64) rawlog << ",";
    }
    rawlog << "\n";
}

static void write_phase8c_component_layout_and_tiny_raw_probes(UObject* trigger, int scan_attempt)
{
    static int phase8c_runs = 0;
    if (!trigger) return;

    std::string trigger_path = obj_path(trigger);
    if (trigger_path.find("RT_MapCapture") == std::string::npos) return;

    // Phase 8C4: load-aware trigger.
    // Do not run just because a timer attempt was reached.
    // First scan for real runtime landscape components. Only snapshot when they exist.
    if (phase8c_runs >= 5) return;

    auto out = out_dir();

    std::ofstream log(out / "phase8c_component_layout_and_tiny_raw_probes.txt", std::ios::app);
    std::ofstream csv(out / "phase8c_component_layout_candidates.csv", std::ios::app);
    std::ofstream rawlog(out / "phase8c_tiny_raw_probe_windows.txt", std::ios::app);

    file_log("Phase 8C4 CHECK attempt=" + std::to_string(scan_attempt) + " current_runs=" + std::to_string(phase8c_runs));

    log << "\n===== PHASE 8C4 RUN " << phase8c_runs << " ATTEMPT " << scan_attempt << " =====\n";
    log << "Phase 8C4 component layout + tiny raw probes\n";
    log << "mode=targeted_runtime_landscape_metadata_and_small_memory_windows\n";
    log << "goal=find_sectionbase_component_size_candidate_offsets_and_initial_height_raw_signal\n";
    log << "safety=max_8_objects_small_windows_no_full_height_dump\n\n";

    if (phase8c_runs == 1)
    {
        csv << "run,attempt,kind,class,name,path,addr,landscape_id,component_id,"
               "candidate_sectionbase_count,candidate_size_count,candidate_ptr_count,"
               "sample_i32_offsets,sample_size_offsets,sample_ptr_offsets\n";
    }

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        UObject* obj;
    };

    std::vector<Rec> collisions;
    std::vector<Rec> components;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool is_runtime_persistent =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!is_runtime_persistent) return RC::LoopAction::Continue;
            if (path.find("Default__") != std::string::npos) return RC::LoopAction::Continue;

            bool is_collision = cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos;
            bool is_component = (cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos);

            if (!is_collision && !is_component) return RC::LoopAction::Continue;

            auto extract_int_after = [](const std::string& text, const std::string& token) -> int
            {
                size_t pos = text.rfind(token);
                if (pos == std::string::npos) return -1;
                pos += token.size();
                std::string digits;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
                {
                    digits.push_back(text[pos]);
                    pos++;
                }
                if (digits.empty()) return -1;
                try { return std::stoi(digits); } catch (...) { return -1; }
            };

            Rec r{};
            r.kind = is_collision ? "collision" : "landscape_component";
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = extract_int_after(path, "Landscape_");
            r.component_id = extract_int_after(path, is_collision ? "LandscapeHeightfieldCollisionComponent_" : "LandscapeComponent_");
            r.obj = o;

            if (is_collision) collisions.push_back(r);
            if (is_component) components.push_back(r);

            return RC::LoopAction::Continue;
        }
    );

    auto by_landscape_component = [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        return a.component_id < b.component_id;
    };

    std::sort(collisions.begin(), collisions.end(), by_landscape_component);
    std::sort(components.begin(), components.end(), by_landscape_component);

    log << "SUMMARY\n";
    log << "  scanned_objects=" << scanned << "\n";
    log << "  collisions=" << collisions.size() << "\n";
    log << "  landscape_components=" << components.size() << "\n";
    log << "  scan_attempt=" << scan_attempt << "\n";
    log << "  current_runs_before_decision=" << phase8c_runs << "\n\n";

    bool landscape_loaded = (!collisions.empty() && !components.empty());

    bool force_late_snapshot =
        scan_attempt >= 35 &&
        (collisions.size() > 0 || components.size() > 0);

    if (!landscape_loaded && !force_late_snapshot)
    {
        log << "DECISION=skip_not_loaded_yet\n";
        log << "reason=no_runtime_landscape_components_found_yet\n\n";
        file_log("Phase 8C4 skip attempt=" + std::to_string(scan_attempt) + " scanned=" + std::to_string(scanned) + " collisions=" + std::to_string(collisions.size()) + " components=" + std::to_string(components.size()));
        return;
    }

    bool should_snapshot =
        phase8c_runs == 0 ||
        scan_attempt >= 20 ||
        scan_attempt >= 35 ||
        scan_attempt >= 50 ||
        scan_attempt >= 70;

    if (!should_snapshot)
    {
        log << "DECISION=skip_waiting_for_snapshot_window\n\n";
        return;
    }

    phase8c_runs++;

    log << "DECISION=run_snapshot\n";
    log << "phase8c_run=" << phase8c_runs << "\n\n";

    file_log("Phase 8C4 ENTERED attempt=" + std::to_string(scan_attempt) + " run=" + std::to_string(phase8c_runs) + " collisions=" + std::to_string(collisions.size()) + " components=" + std::to_string(components.size()));

    std::vector<Rec> probe_objects;

    auto add_probe = [&](const std::vector<Rec>& src, const char* reason)
    {
        if (src.empty()) return;

        probe_objects.push_back(src.front());

        if (src.size() > 2)
        {
            probe_objects.push_back(src[src.size() / 2]);
        }

        if (src.size() > 1)
        {
            probe_objects.push_back(src.back());
        }

        log << "probe_selection_" << reason << "_added_from=" << src.size() << "\n";
    };

    add_probe(collisions, "collisions");
    add_probe(components, "components");

    // Deduplicate and cap to 8.
    std::vector<Rec> dedup;
    for (const auto& r : probe_objects)
    {
        bool exists = false;
        for (const auto& d : dedup)
        {
            if (d.addr == r.addr)
            {
                exists = true;
                break;
            }
        }

        if (!exists)
        {
            dedup.push_back(r);
        }

        if (dedup.size() >= 8) break;
    }

    log << "probe_objects=" << dedup.size() << "\n\n";

    const size_t max_scan = 0x600;
    const size_t stride = 4;

    for (const auto& r : dedup)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(r.obj);

        std::vector<size_t> section_like_offsets;
        std::vector<size_t> size_like_offsets;
        std::vector<size_t> ptr_like_offsets;

        for (size_t off = 0; off + 8 <= max_scan; off += stride)
        {
            int32_t i32 = phase8c_read_i32_unchecked(base, off);
            uint32_t u32 = phase8c_read_u32_unchecked(base, off);

            // SectionBase values are often world/grid-ish ints; keep broad.
            if ((i32 >= -2000000 && i32 <= 2000000 && i32 != 0) ||
                (i32 >= -32768 && i32 <= 32768 && i32 != 0))
            {
                if (section_like_offsets.size() < 80)
                {
                    section_like_offsets.push_back(off);
                }
            }

            // Unreal landscape component sizes often around 7/15/31/63/127/255 or close.
            if (u32 == 7 || u32 == 15 || u32 == 31 || u32 == 32 || u32 == 63 || u32 == 64 ||
                u32 == 127 || u32 == 128 || u32 == 255 || u32 == 256 ||
                u32 == 511 || u32 == 512 || u32 == 1023 || u32 == 1024)
            {
                if (size_like_offsets.size() < 80)
                {
                    size_like_offsets.push_back(off);
                }
            }

            if (off + sizeof(uintptr_t) <= max_scan)
            {
                uintptr_t ptr = phase8c_read_ptr_unchecked(base, off);
                if (phase8c_is_probably_readable_ptr(ptr))
                {
                    if (ptr_like_offsets.size() < 80)
                    {
                        ptr_like_offsets.push_back(off);
                    }
                }
            }
        }

        auto join_offsets = [](const std::vector<size_t>& v) -> std::string
        {
            std::string out;
            for (size_t i = 0; i < v.size() && i < 20; ++i)
            {
                out += std::to_string(v[i]);
                if (i + 1 < v.size() && i + 1 < 20) out += "|";
            }
            return out;
        };

        csv << phase8c_runs << ","
            << scan_attempt << ","
            << "\"" << r.kind << "\","
            << "\"" << r.cls << "\","
            << "\"" << r.name << "\","
            << "\"" << r.path << "\","
            << "\"0x" << std::hex << r.addr << std::dec << "\","
            << r.landscape_id << ","
            << r.component_id << ","
            << section_like_offsets.size() << ","
            << size_like_offsets.size() << ","
            << ptr_like_offsets.size() << ","
            << "\"" << join_offsets(section_like_offsets) << "\","
            << "\"" << join_offsets(size_like_offsets) << "\","
            << "\"" << join_offsets(ptr_like_offsets) << "\"\n";

        log << "OBJECT\n";
        log << "  kind=" << r.kind << "\n";
        log << "  class=" << r.cls << "\n";
        log << "  name=" << r.name << "\n";
        log << "  path=" << r.path << "\n";
        log << "  addr=0x" << std::hex << r.addr << std::dec << "\n";
        log << "  landscape_id=" << r.landscape_id << "\n";
        log << "  component_id=" << r.component_id << "\n";
        log << "  section_like_offsets_count=" << section_like_offsets.size() << "\n";
        log << "  size_like_offsets_count=" << size_like_offsets.size() << "\n";
        log << "  ptr_like_offsets_count=" << ptr_like_offsets.size() << "\n";
        log << "  section_like_offsets_first20=" << join_offsets(section_like_offsets) << "\n";
        log << "  size_like_offsets_first20=" << join_offsets(size_like_offsets) << "\n";
        log << "  ptr_like_offsets_first20=" << join_offsets(ptr_like_offsets) << "\n\n";

        // Tiny raw windows from object memory only, not pointer-dereferenced data yet.
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x000, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x080, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x100, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x180, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x200, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x300, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x400, 128);
        phase8c_dump_tiny_window(rawlog, r.kind.c_str(), r.obj, 0x500, 128);
        rawlog << "\n";
    }

    log << "OUTPUTS\n";
    log << "  txt=phase8c_component_layout_and_tiny_raw_probes.txt\n";
    log << "  csv=phase8c_component_layout_candidates.csv\n";
    log << "  rawlog=phase8c_tiny_raw_probe_windows.txt\n";
    log << "NEXT\n";
    log << "  If stable, Phase 8D should compare repeated offsets across all 1256 components and identify SectionBaseX/Y and size fields.\n";
    log << "  Full height buffer dereference is still deferred until pointer candidates are narrowed.\n";

    file_log("Phase 8C done probe_objects=" + std::to_string(dedup.size()));
}
// END PHASE8C_COMPONENT_LAYOUT_AND_TINY_RAW_PROBES



// BEGIN PHASE8D_INDEPENDENT_LANDSCAPE_WATCHDOG
static void write_phase8d_independent_landscape_watchdog_snapshot(int watchdog_run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream log(out / "phase8d_independent_landscape_watchdog.txt", std::ios::app);
    std::ofstream csv(out / "phase8d_landscape_runtime_snapshot.csv", std::ios::app);

    if (watchdog_run == 1)
    {
        csv << "run,delay_seconds,kind,class,name,path,addr,landscape_id,component_id\n";
    }

    file_log("Phase 8D watchdog snapshot begin run=" + std::to_string(watchdog_run) + " delay=" + std::to_string(delay_seconds));

    log << "\n===== PHASE 8D WATCHDOG RUN " << watchdog_run << " DELAY " << delay_seconds << "s =====\n";

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        UObject* obj;
    };

    auto extract_int_after = [](const std::string& text, const std::string& token) -> int
    {
        size_t pos = text.rfind(token);
        if (pos == std::string::npos) return -1;

        pos += token.size();

        std::string digits;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        {
            digits.push_back(text[pos]);
            pos++;
        }

        if (digits.empty()) return -1;

        try
        {
            return std::stoi(digits);
        }
        catch (...)
        {
            return -1;
        }
    };

    int scanned = 0;
    int runtime_persistent = 0;
    int collision_count = 0;
    int component_count = 0;
    int actor_count = 0;
    int other_count = 0;

    std::vector<Rec> records;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool is_runtime_persistent =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!is_runtime_persistent) return RC::LoopAction::Continue;

            runtime_persistent++;

            if (path.find("Default__") != std::string::npos) return RC::LoopAction::Continue;

            bool landscape_related =
                cls.find("Landscape") != std::string::npos ||
                name.find("Landscape") != std::string::npos ||
                path.find("Landscape") != std::string::npos;

            if (!landscape_related) return RC::LoopAction::Continue;

            std::string kind = "other_landscape";

            if (cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos)
            {
                kind = "collision";
                collision_count++;
            }
            else if (cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos)
            {
                kind = "landscape_component";
                component_count++;
            }
            else if (cls == "Landscape" || cls == "LandscapeProxy" || cls == "LandscapeStreamingProxy")
            {
                kind = "landscape_actor";
                actor_count++;
            }
            else
            {
                other_count++;
            }

            Rec r{};
            r.kind = kind;
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = extract_int_after(path, "Landscape_");
            r.component_id = extract_int_after(path, "LandscapeHeightfieldCollisionComponent_");

            if (r.component_id < 0)
            {
                r.component_id = extract_int_after(path, "LandscapeComponent_");
            }

            r.obj = o;

            records.push_back(r);

            return RC::LoopAction::Continue;
        }
    );

    std::sort(records.begin(), records.end(), [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        if (a.component_id != b.component_id) return a.component_id < b.component_id;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.path < b.path;
    });

    std::map<int, int> landscape_counts;
    for (const auto& r : records)
    {
        if (r.landscape_id >= 0)
        {
            landscape_counts[r.landscape_id]++;
        }

        csv << watchdog_run << ","
            << delay_seconds << ","
            << "\"" << r.kind << "\","
            << "\"" << r.cls << "\","
            << "\"" << r.name << "\","
            << "\"" << r.path << "\","
            << "\"0x" << std::hex << r.addr << std::dec << "\","
            << r.landscape_id << ","
            << r.component_id << "\n";
    }

    log << "SUMMARY\n";
    log << "  scanned_objects=" << scanned << "\n";
    log << "  runtime_persistent_objects=" << runtime_persistent << "\n";
    log << "  records=" << records.size() << "\n";
    log << "  collision_count=" << collision_count << "\n";
    log << "  landscape_component_count=" << component_count << "\n";
    log << "  landscape_actor_count=" << actor_count << "\n";
    log << "  other_landscape_count=" << other_count << "\n";
    log << "  unique_landscape_ids=" << landscape_counts.size() << "\n";

    if (collision_count > 0 && component_count > 0)
    {
        log << "DECISION=runtime_landscape_loaded\n";
    }
    else
    {
        log << "DECISION=runtime_landscape_not_loaded_yet\n";
    }

    log << "\nLANDSCAPE_COUNTS_FIRST_80\n";
    int shown = 0;
    for (const auto& kv : landscape_counts)
    {
        log << "  Landscape_" << kv.first << "=" << kv.second << "\n";
        shown++;
        if (shown >= 80) break;
    }

    log << "\nSAMPLE_FIRST_40\n";
    int sample = 0;
    for (const auto& r : records)
    {
        log << "RECORD kind=" << r.kind
            << " class=" << r.cls
            << " name=" << r.name
            << " landscape_id=" << r.landscape_id
            << " component_id=" << r.component_id
            << " path=" << r.path
            << "\n";

        sample++;
        if (sample >= 40) break;
    }

    file_log(
        "Phase 8D watchdog snapshot done run=" +
        std::to_string(watchdog_run) +
        " scanned=" +
        std::to_string(scanned) +
        " collisions=" +
        std::to_string(collision_count) +
        " components=" +
        std::to_string(component_count)
    );
}

static void start_phase8d_independent_landscape_watchdog()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("Phase 8D independent watchdog started");

    std::thread([]()
    {
        const int delays[] = {60, 120, 180, 240, 300, 420, 600};

        int previous = 0;

        for (int i = 0; i < 7; ++i)
        {
            int target = delays[i];
            int delta = target - previous;
            previous = target;

            if (delta > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            }

            write_phase8d_independent_landscape_watchdog_snapshot(i + 1, target);
        }

        file_log("Phase 8D independent watchdog finished");
    }).detach();
}
// END PHASE8D_INDEPENDENT_LANDSCAPE_WATCHDOG



// BEGIN PHASE8E_TIMED_LAYOUT_MEMORY_PROBES
static uint64_t phase8e_fnv1a64(const uint8_t* data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

static int phase8e_extract_int_after(const std::string& text, const std::string& token)
{
    size_t pos = text.rfind(token);
    if (pos == std::string::npos) return -1;

    pos += token.size();

    std::string digits;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
    {
        digits.push_back(text[pos]);
        pos++;
    }

    if (digits.empty()) return -1;

    try
    {
        return std::stoi(digits);
    }
    catch (...)
    {
        return -1;
    }
}

static int32_t phase8e_read_i32(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint32_t phase8e_read_u32(uint8_t* base, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase8e_read_ptr(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static bool phase8e_probably_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static void phase8e_dump_object_window(std::ofstream& raw, int run, const char* kind, UObject* o, size_t offset, size_t len)
{
    if (!o) return;
    if (len > 256) len = 256;

    uint8_t* base = reinterpret_cast<uint8_t*>(o);
    uint8_t buf[256]{};
    std::memcpy(buf, base + offset, len);

    uint8_t minv = 255;
    uint8_t maxv = 0;
    uint64_t sum = 0;

    for (size_t i = 0; i < len; ++i)
    {
        minv = std::min(minv, buf[i]);
        maxv = std::max(maxv, buf[i]);
        sum += buf[i];
    }

    raw << "WINDOW run=" << run
        << " kind=" << kind
        << " obj=0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec
        << " offset=" << offset
        << " len=" << len
        << " min=" << static_cast<int>(minv)
        << " max=" << static_cast<int>(maxv)
        << " avg=" << (len ? (double)sum / (double)len : 0.0)
        << " fnv64=0x" << std::hex << phase8e_fnv1a64(buf, len) << std::dec
        << "\n";

    raw << "  u16_first32=";
    for (size_t i = 0; i + 1 < len && i < 64; i += 2)
    {
        uint16_t v = 0;
        std::memcpy(&v, buf + i, sizeof(v));
        raw << v;
        if (i + 2 < len && i + 2 < 64) raw << ",";
    }
    raw << "\n";

    raw << "  i32_first16=";
    for (size_t i = 0; i + 3 < len && i < 64; i += 4)
    {
        int32_t v = 0;
        std::memcpy(&v, buf + i, sizeof(v));
        raw << v;
        if (i + 4 < len && i + 4 < 64) raw << ",";
    }
    raw << "\n";
}

static void write_phase8e_timed_layout_memory_probe_snapshot(int run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream log(out / "phase8e_timed_layout_memory_probes.txt", std::ios::app);
    std::ofstream csv(out / "phase8e_layout_candidates.csv", std::ios::app);
    std::ofstream raw(out / "phase8e_tiny_object_windows.txt", std::ios::app);

    if (run == 1)
    {
        csv << "run,delay_seconds,kind,class,name,path,addr,landscape_id,component_id,"
               "section_like_count,size_like_count,ptr_like_count,"
               "section_offsets_first30,size_offsets_first30,ptr_offsets_first30\n";
    }

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        UObject* obj;
    };

    std::vector<Rec> collisions;
    std::vector<Rec> components;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool runtime =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!runtime) return RC::LoopAction::Continue;
            if (path.find("Default__") != std::string::npos) return RC::LoopAction::Continue;

            bool is_collision = cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos;
            bool is_component = cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos;

            if (!is_collision && !is_component) return RC::LoopAction::Continue;

            Rec r{};
            r.kind = is_collision ? "collision" : "landscape_component";
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = phase8e_extract_int_after(path, "Landscape_");
            r.component_id = is_collision
                ? phase8e_extract_int_after(path, "LandscapeHeightfieldCollisionComponent_")
                : phase8e_extract_int_after(path, "LandscapeComponent_");
            r.obj = o;

            if (is_collision) collisions.push_back(r);
            else components.push_back(r);

            return RC::LoopAction::Continue;
        }
    );

    auto sorter = [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        if (a.component_id != b.component_id) return a.component_id < b.component_id;
        return a.path < b.path;
    };

    std::sort(collisions.begin(), collisions.end(), sorter);
    std::sort(components.begin(), components.end(), sorter);

    log << "\n===== PHASE 8E RUN " << run << " DELAY " << delay_seconds << "s =====\n";
    log << "scanned_objects=" << scanned << "\n";
    log << "collisions=" << collisions.size() << "\n";
    log << "landscape_components=" << components.size() << "\n";

    if (collisions.empty() || components.empty())
    {
        log << "DECISION=skip_not_loaded\n";
        file_log("Phase 8E skip run=" + std::to_string(run));
        return;
    }

    log << "DECISION=probe_loaded_landscape\n";

    std::vector<Rec> probes;

    auto add_pick = [&](const std::vector<Rec>& src)
    {
        if (src.empty()) return;
        probes.push_back(src.front());
        probes.push_back(src[src.size() / 4]);
        probes.push_back(src[src.size() / 2]);
        probes.push_back(src[(src.size() * 3) / 4]);
        probes.push_back(src.back());
    };

    add_pick(collisions);
    add_pick(components);

    std::vector<Rec> dedup;
    for (const auto& r : probes)
    {
        bool exists = false;
        for (const auto& d : dedup)
        {
            if (d.addr == r.addr)
            {
                exists = true;
                break;
            }
        }

        if (!exists) dedup.push_back(r);
        if (dedup.size() >= 12) break;
    }

    log << "probe_objects=" << dedup.size() << "\n";

    const size_t max_scan = 0x900;
    const size_t stride = 4;

    std::map<size_t, int> section_offset_frequency;
    std::map<size_t, int> size_offset_frequency;
    std::map<size_t, int> ptr_offset_frequency;

    auto join_offsets = [](const std::vector<size_t>& values, size_t maxn) -> std::string
    {
        std::string out;
        for (size_t i = 0; i < values.size() && i < maxn; ++i)
        {
            out += std::to_string(values[i]);
            if (i + 1 < values.size() && i + 1 < maxn) out += "|";
        }
        return out;
    };

    for (const auto& r : dedup)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(r.obj);

        std::vector<size_t> section_like;
        std::vector<size_t> size_like;
        std::vector<size_t> ptr_like;

        for (size_t off = 0; off + 8 <= max_scan; off += stride)
        {
            int32_t i32 = phase8e_read_i32(base, off);
            uint32_t u32 = phase8e_read_u32(base, off);

            bool section_candidate =
                i32 != 0 &&
                i32 > -5000000 &&
                i32 < 5000000;

            if (section_candidate)
            {
                if (section_like.size() < 120) section_like.push_back(off);
                section_offset_frequency[off]++;
            }

            bool size_candidate =
                u32 == 1 || u32 == 2 || u32 == 3 || u32 == 4 || u32 == 5 || u32 == 7 ||
                u32 == 8 || u32 == 9 || u32 == 15 || u32 == 16 || u32 == 31 || u32 == 32 ||
                u32 == 63 || u32 == 64 || u32 == 127 || u32 == 128 || u32 == 255 || u32 == 256 ||
                u32 == 511 || u32 == 512 || u32 == 1023 || u32 == 1024;

            if (size_candidate)
            {
                if (size_like.size() < 120) size_like.push_back(off);
                size_offset_frequency[off]++;
            }

            if (off + sizeof(uintptr_t) <= max_scan)
            {
                uintptr_t ptr = phase8e_read_ptr(base, off);
                if (phase8e_probably_ptr(ptr))
                {
                    if (ptr_like.size() < 120) ptr_like.push_back(off);
                    ptr_offset_frequency[off]++;
                }
            }
        }

        csv << run << ","
            << delay_seconds << ","
            << "\"" << r.kind << "\","
            << "\"" << r.cls << "\","
            << "\"" << r.name << "\","
            << "\"" << r.path << "\","
            << "\"0x" << std::hex << r.addr << std::dec << "\","
            << r.landscape_id << ","
            << r.component_id << ","
            << section_like.size() << ","
            << size_like.size() << ","
            << ptr_like.size() << ","
            << "\"" << join_offsets(section_like, 30) << "\","
            << "\"" << join_offsets(size_like, 30) << "\","
            << "\"" << join_offsets(ptr_like, 30) << "\"\n";

        log << "OBJECT kind=" << r.kind
            << " class=" << r.cls
            << " name=" << r.name
            << " landscape_id=" << r.landscape_id
            << " component_id=" << r.component_id
            << " section_count=" << section_like.size()
            << " size_count=" << size_like.size()
            << " ptr_count=" << ptr_like.size()
            << "\n";

        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x000, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x100, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x200, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x300, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x400, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x600, 128);
        phase8e_dump_object_window(raw, run, r.kind.c_str(), r.obj, 0x800, 128);
        raw << "\n";
    }

    auto dump_top = [&](const char* title, const std::map<size_t, int>& freq)
    {
        std::vector<std::pair<size_t, int>> rows(freq.begin(), freq.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b)
        {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

        log << "\n" << title << "\n";
        int shown = 0;
        for (const auto& kv : rows)
        {
            log << "  off=" << kv.first << " count=" << kv.second << "\n";
            shown++;
            if (shown >= 40) break;
        }
    };

    dump_top("TOP_SECTION_LIKE_OFFSETS", section_offset_frequency);
    dump_top("TOP_SIZE_LIKE_OFFSETS", size_offset_frequency);
    dump_top("TOP_PTR_LIKE_OFFSETS", ptr_offset_frequency);

    file_log("Phase 8E done run=" + std::to_string(run) + " probes=" + std::to_string(dedup.size()));
}

static void start_phase8e_timed_layout_memory_probes()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("Phase 8E timed layout memory probes started");

    std::thread([]()
    {
        const int delays[] = {120, 180, 300, 600};
        int previous = 0;

        for (int i = 0; i < 4; ++i)
        {
            int target = delays[i];
            int delta = target - previous;
            previous = target;

            if (delta > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            }

            write_phase8e_timed_layout_memory_probe_snapshot(i + 1, target);
        }

        file_log("Phase 8E timed layout memory probes finished");
    }).detach();
}
// END PHASE8E_TIMED_LAYOUT_MEMORY_PROBES



// BEGIN PHASE8F_OFFSET_VALUE_MATRIX
static int phase8f_extract_int_after(const std::string& text, const std::string& token)
{
    size_t pos = text.rfind(token);
    if (pos == std::string::npos) return -1;

    pos += token.size();

    std::string digits;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
    {
        digits.push_back(text[pos]);
        pos++;
    }

    if (digits.empty()) return -1;

    try
    {
        return std::stoi(digits);
    }
    catch (...)
    {
        return -1;
    }
}

static int32_t phase8f_i32(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uint32_t phase8f_u32(uint8_t* base, size_t off)
{
    uint32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static float phase8f_f32(uint8_t* base, size_t off)
{
    float v = 0.0f;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase8f_ptr(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static bool phase8f_probably_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static void write_phase8f_offset_value_matrix_snapshot(int run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream log(out / "phase8f_offset_value_matrix_summary.txt", std::ios::app);
    std::ofstream csv(out / "phase8f_offset_value_matrix.csv", std::ios::app);

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        UObject* obj;
    };

    std::vector<Rec> rows;

    int scanned = 0;
    int collisions = 0;
    int components = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool runtime =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!runtime) return RC::LoopAction::Continue;
            if (path.find("Default__") != std::string::npos) return RC::LoopAction::Continue;

            bool is_collision = cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos;
            bool is_component = cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos;

            if (!is_collision && !is_component) return RC::LoopAction::Continue;

            Rec r{};
            r.kind = is_collision ? "collision" : "landscape_component";
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = phase8f_extract_int_after(path, "Landscape_");
            r.component_id = is_collision
                ? phase8f_extract_int_after(path, "LandscapeHeightfieldCollisionComponent_")
                : phase8f_extract_int_after(path, "LandscapeComponent_");
            r.obj = o;

            rows.push_back(r);

            if (is_collision) collisions++;
            if (is_component) components++;

            return RC::LoopAction::Continue;
        }
    );

    std::sort(rows.begin(), rows.end(), [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.component_id != b.component_id) return a.component_id < b.component_id;
        return a.path < b.path;
    });

    log << "\n===== PHASE 8F RUN " << run << " DELAY " << delay_seconds << "s =====\n";
    log << "scanned_objects=" << scanned << "\n";
    log << "rows=" << rows.size() << "\n";
    log << "collisions=" << collisions << "\n";
    log << "landscape_components=" << components << "\n";

    if (rows.empty() || collisions == 0 || components == 0)
    {
        log << "DECISION=skip_not_loaded\n";
        file_log("Phase 8F skip run=" + std::to_string(run));
        return;
    }

    log << "DECISION=write_offset_matrix\n";

    const size_t offsets[] = {
        4, 8, 12, 20, 24, 44, 52, 60,
        136, 140, 416, 440,
        580, 588, 596, 612, 616, 624, 632, 636, 640,
        720, 764, 788, 864, 868, 876,
        916, 924, 960, 972, 980,
        1000, 1004,
        1176, 1180, 1208,
        1284, 1288, 1292, 1296, 1300, 1304,
        1328, 1332, 1344, 1348, 1360, 1364,
        1400, 1504, 1524,
        1684, 1688, 1732, 1740, 1748, 1752, 1760,
        1796, 1804, 1812, 1816, 1824,
        1880, 1888, 1944, 1952,
        2000, 2020, 2040, 2060, 2080, 2120
    };

    const size_t offset_count = sizeof(offsets) / sizeof(offsets[0]);

    if (run == 1)
    {
        csv << "run,delay_seconds,kind,class,name,path,addr,landscape_id,component_id";

        for (size_t i = 0; i < offset_count; ++i)
        {
            csv << ",i32_" << offsets[i];
            csv << ",u32_" << offsets[i];
            csv << ",f32_" << offsets[i];
            csv << ",ptr_" << offsets[i];
            csv << ",ptrlike_" << offsets[i];
        }

        csv << "\n";
    }

    std::map<size_t, int> distinct_count_hint;
    std::map<size_t, std::map<int32_t, int>> i32_freq;
    std::map<size_t, std::map<uint32_t, int>> u32_freq;

    for (const auto& r : rows)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(r.obj);

        csv << run << ","
            << delay_seconds << ","
            << "\"" << r.kind << "\","
            << "\"" << r.cls << "\","
            << "\"" << r.name << "\","
            << "\"" << r.path << "\","
            << "\"0x" << std::hex << r.addr << std::dec << "\","
            << r.landscape_id << ","
            << r.component_id;

        for (size_t i = 0; i < offset_count; ++i)
        {
            size_t off = offsets[i];

            int32_t i32 = phase8f_i32(base, off);
            uint32_t u32 = phase8f_u32(base, off);
            float f32 = phase8f_f32(base, off);
            uintptr_t ptr = phase8f_ptr(base, off);
            bool ptrlike = phase8f_probably_ptr(ptr);

            i32_freq[off][i32]++;
            u32_freq[off][u32]++;

            csv << "," << i32
                << "," << u32
                << "," << f32
                << ",0x" << std::hex << ptr << std::dec
                << "," << (ptrlike ? "1" : "0");
        }

        csv << "\n";
    }

    log << "\nOFFSET_VALUE_SUMMARY_TOP\n";

    for (size_t i = 0; i < offset_count; ++i)
    {
        size_t off = offsets[i];

        log << "OFFSET " << off
            << " i32_distinct=" << i32_freq[off].size()
            << " u32_distinct=" << u32_freq[off].size();

        auto emit_top_i32 = [&](const std::map<int32_t, int>& freq)
        {
            std::vector<std::pair<int32_t, int>> v(freq.begin(), freq.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b)
            {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });

            log << " top_i32=";
            for (size_t j = 0; j < v.size() && j < 5; ++j)
            {
                log << v[j].first << ":" << v[j].second;
                if (j + 1 < v.size() && j + 1 < 5) log << "|";
            }
        };

        emit_top_i32(i32_freq[off]);
        log << "\n";
    }

    file_log("Phase 8F done run=" + std::to_string(run) + " rows=" + std::to_string(rows.size()));
}

static void start_phase8f_offset_value_matrix()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("Phase 8F offset value matrix started");

    std::thread([]()
    {
        const int delays[] = {120, 300};
        int previous = 0;

        for (int i = 0; i < 2; ++i)
        {
            int target = delays[i];
            int delta = target - previous;
            previous = target;

            if (delta > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            }

            write_phase8f_offset_value_matrix_snapshot(i + 1, target);
        }

        file_log("Phase 8F offset value matrix finished");
    }).detach();
}
// END PHASE8F_OFFSET_VALUE_MATRIX



// BEGIN PHASE8G_GRID_AND_SECTION_CONFIRMATION
static int phase8g_extract_int_after(const std::string& text, const std::string& token)
{
    size_t pos = text.rfind(token);
    if (pos == std::string::npos) return -1;
    pos += token.size();

    std::string digits;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
    {
        digits.push_back(text[pos]);
        pos++;
    }

    if (digits.empty()) return -1;

    try { return std::stoi(digits); }
    catch (...) { return -1; }
}

static int32_t phase8g_i32(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase8g_ptr(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static bool phase8g_probably_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static std::string phase8g_join_set(const std::set<int32_t>& values, int maxn = 20)
{
    std::string out;
    int shown = 0;
    for (auto v : values)
    {
        out += std::to_string(v);
        shown++;
        if (shown >= maxn) break;
        out += "|";
    }
    return out;
}

static int phase8g_guess_spacing(const std::set<int32_t>& values)
{
    if (values.size() < 2) return 0;

    std::vector<int32_t> v(values.begin(), values.end());
    std::map<int, int> diffs;

    for (size_t i = 1; i < v.size(); ++i)
    {
        int d = std::abs(v[i] - v[i - 1]);
        if (d > 0 && d < 10000) diffs[d]++;
    }

    int best_d = 0;
    int best_c = 0;

    for (const auto& kv : diffs)
    {
        if (kv.second > best_c)
        {
            best_d = kv.first;
            best_c = kv.second;
        }
    }

    return best_d;
}

static void write_phase8g_grid_and_section_confirmation_snapshot(int run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream log(out / "phase8g_grid_and_section_summary.txt", std::ios::app);
    std::ofstream csv(out / "phase8g_component_candidate_pairs.csv", std::ios::app);
    std::ofstream grid(out / "phase8g_island_grid_candidates.csv", std::ios::app);
    std::ofstream ptrcsv(out / "phase8g_pointer_candidates.csv", std::ios::app);

    struct PairDef
    {
        const char* label;
        size_t xoff;
        size_t yoff;
    };

    const PairDef pairs[] = {
        {"p1284_1288", 1284, 1288},
        {"p1288_1292", 1288, 1292},
        {"p1292_1296", 1292, 1296},
        {"p1296_1300", 1296, 1300},
        {"p1300_1304", 1300, 1304},
        {"p1328_1332", 1328, 1332},
        {"p1344_1348", 1344, 1348},
        {"p1360_1364", 1360, 1364},
        {"p1880_1888", 1880, 1888},
        {"p2020_2040", 2020, 2040},
        {"p2060_2120", 2060, 2120}
    };

    const size_t pair_count = sizeof(pairs) / sizeof(pairs[0]);

    const size_t ptr_offsets[] = {
        0, 8, 16, 24, 44, 52, 60,
        140, 144, 156, 160, 180, 196, 200,
        260, 512, 580, 764,
        1176, 1180, 1284, 1288, 1292, 1296, 1300, 1304,
        1684, 1688, 1732, 1740, 1748, 1752, 1760,
        1880, 1888, 1944, 1952, 2000, 2020, 2040, 2060, 2080, 2120
    };

    const size_t ptr_count = sizeof(ptr_offsets) / sizeof(ptr_offsets[0]);

    struct Rec
    {
        std::string kind;
        std::string cls;
        std::string name;
        std::string path;
        uintptr_t addr;
        int landscape_id;
        int component_id;
        UObject* obj;
    };

    std::vector<Rec> rows;
    int scanned = 0;
    int collisions = 0;
    int components = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool runtime =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!runtime) return RC::LoopAction::Continue;
            if (path.find("Default__") != std::string::npos) return RC::LoopAction::Continue;

            bool is_collision = cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos;
            bool is_component = cls == "LandscapeComponent" || cls.find("LandscapeComponent") != std::string::npos;

            if (!is_collision && !is_component) return RC::LoopAction::Continue;

            Rec r{};
            r.kind = is_collision ? "collision" : "landscape_component";
            r.cls = cls;
            r.name = name;
            r.path = path;
            r.addr = reinterpret_cast<uintptr_t>(o);
            r.landscape_id = phase8g_extract_int_after(path, "Landscape_");
            r.component_id = is_collision
                ? phase8g_extract_int_after(path, "LandscapeHeightfieldCollisionComponent_")
                : phase8g_extract_int_after(path, "LandscapeComponent_");
            r.obj = o;

            rows.push_back(r);

            if (is_collision) collisions++;
            if (is_component) components++;

            return RC::LoopAction::Continue;
        }
    );

    std::sort(rows.begin(), rows.end(), [](const Rec& a, const Rec& b)
    {
        if (a.landscape_id != b.landscape_id) return a.landscape_id < b.landscape_id;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.component_id != b.component_id) return a.component_id < b.component_id;
        return a.path < b.path;
    });

    log << "\n===== PHASE 8G RUN " << run << " DELAY " << delay_seconds << "s =====\n";
    log << "scanned_objects=" << scanned << "\n";
    log << "rows=" << rows.size() << "\n";
    log << "collisions=" << collisions << "\n";
    log << "landscape_components=" << components << "\n";

    if (rows.empty() || collisions == 0 || components == 0)
    {
        log << "DECISION=skip_not_loaded\n";
        file_log("Phase 8G skip run=" + std::to_string(run));
        return;
    }

    log << "DECISION=write_grid_confirmation\n";

    if (run == 1)
    {
        csv << "run,delay_seconds,kind,landscape_id,component_id,name,path,addr,"
               "pair_label,xoff,yoff,x,y,x_abs_ok,y_abs_ok,both_nonzero,both_small,xy_distinct_hint\n";

        grid << "run,delay_seconds,kind,pair_label,landscape_id,count,"
                "unique_x,unique_y,min_x,max_x,min_y,max_y,spacing_x,spacing_y,"
                "grid_guess,score,values_x,values_y\n";

        ptrcsv << "run,delay_seconds,kind,landscape_id,component_id,name,path,addr,"
                  "ptr_offset,ptr_value,ptrlike,nearby_i32_0,nearby_i32_4,nearby_i32_8,nearby_i32_12\n";
    }

    struct GridAgg
    {
        int count = 0;
        std::set<int32_t> xs;
        std::set<int32_t> ys;
        int32_t minx = INT32_MAX;
        int32_t maxx = INT32_MIN;
        int32_t miny = INT32_MAX;
        int32_t maxy = INT32_MIN;
    };

    std::map<std::string, GridAgg> aggs;

    auto agg_key = [](const std::string& kind, const std::string& pair_label, int landscape_id) -> std::string
    {
        return kind + "|" + pair_label + "|" + std::to_string(landscape_id);
    };

    for (const auto& r : rows)
    {
        uint8_t* base = reinterpret_cast<uint8_t*>(r.obj);

        for (size_t i = 0; i < pair_count; ++i)
        {
            int32_t x = phase8g_i32(base, pairs[i].xoff);
            int32_t y = phase8g_i32(base, pairs[i].yoff);

            bool x_abs_ok = std::abs(x) <= 1000000;
            bool y_abs_ok = std::abs(y) <= 1000000;
            bool both_nonzero = (x != 0 && y != 0);
            bool both_small = std::abs(x) <= 10000 && std::abs(y) <= 10000;

            int xy_distinct_hint = 0;
            if (x != y) xy_distinct_hint++;
            if (x != 0) xy_distinct_hint++;
            if (y != 0) xy_distinct_hint++;

            csv << run << ","
                << delay_seconds << ","
                << "\"" << r.kind << "\","
                << r.landscape_id << ","
                << r.component_id << ","
                << "\"" << r.name << "\","
                << "\"" << r.path << "\","
                << "\"0x" << std::hex << r.addr << std::dec << "\","
                << "\"" << pairs[i].label << "\","
                << pairs[i].xoff << ","
                << pairs[i].yoff << ","
                << x << ","
                << y << ","
                << (x_abs_ok ? "1" : "0") << ","
                << (y_abs_ok ? "1" : "0") << ","
                << (both_nonzero ? "1" : "0") << ","
                << (both_small ? "1" : "0") << ","
                << xy_distinct_hint
                << "\n";

            if (x_abs_ok && y_abs_ok)
            {
                auto& a = aggs[agg_key(r.kind, pairs[i].label, r.landscape_id)];
                a.count++;
                a.xs.insert(x);
                a.ys.insert(y);
                a.minx = std::min(a.minx, x);
                a.maxx = std::max(a.maxx, x);
                a.miny = std::min(a.miny, y);
                a.maxy = std::max(a.maxy, y);
            }
        }

        for (size_t i = 0; i < ptr_count; ++i)
        {
            size_t off = ptr_offsets[i];
            uintptr_t ptr = phase8g_ptr(base, off);
            bool ptrlike = phase8g_probably_ptr(ptr);

            if (!ptrlike) continue;

            int32_t n0 = phase8g_i32(base, off);
            int32_t n4 = phase8g_i32(base, off + 4);
            int32_t n8 = phase8g_i32(base, off + 8);
            int32_t n12 = phase8g_i32(base, off + 12);

            ptrcsv << run << ","
                   << delay_seconds << ","
                   << "\"" << r.kind << "\","
                   << r.landscape_id << ","
                   << r.component_id << ","
                   << "\"" << r.name << "\","
                   << "\"" << r.path << "\","
                   << "\"0x" << std::hex << r.addr << std::dec << "\","
                   << off << ","
                   << "\"0x" << std::hex << ptr << std::dec << "\","
                   << "1,"
                   << n0 << ","
                   << n4 << ","
                   << n8 << ","
                   << n12
                   << "\n";
        }
    }

    std::vector<std::tuple<int, std::string, std::string, int, GridAgg>> scored;

    for (const auto& kv : aggs)
    {
        const std::string& key = kv.first;
        const GridAgg& a = kv.second;

        size_t p1 = key.find("|");
        size_t p2 = key.find("|", p1 + 1);
        std::string kind = key.substr(0, p1);
        std::string pair_label = key.substr(p1 + 1, p2 - p1 - 1);
        int landscape_id = std::stoi(key.substr(p2 + 1));

        int ux = static_cast<int>(a.xs.size());
        int uy = static_cast<int>(a.ys.size());
        int spacing_x = phase8g_guess_spacing(a.xs);
        int spacing_y = phase8g_guess_spacing(a.ys);

        int score = 0;

        if (a.count >= 4) score += 10;
        if (ux >= 2 && uy >= 2) score += 20;
        if (ux * uy >= a.count) score += 15;
        if (spacing_x > 0 && spacing_y > 0) score += 20;
        if (spacing_x == spacing_y && spacing_x > 0) score += 20;
        if (spacing_x == 127 || spacing_x == 159 || spacing_x == 255) score += 15;
        if (spacing_y == 127 || spacing_y == 159 || spacing_y == 255) score += 15;
        if (ux == 2 || ux == 3 || ux == 5 || ux == 7 || ux == 8) score += 10;
        if (uy == 2 || uy == 3 || uy == 5 || uy == 7 || uy == 8) score += 10;

        scored.push_back({score, kind, pair_label, landscape_id, a});
    }

    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b)
    {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
        if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
        if (std::get<2>(a) != std::get<2>(b)) return std::get<2>(a) < std::get<2>(b);
        return std::get<3>(a) < std::get<3>(b);
    });

    int written = 0;

    log << "\nTOP_GRID_CANDIDATES\n";

    for (const auto& row : scored)
    {
        int score = std::get<0>(row);
        const std::string& kind = std::get<1>(row);
        const std::string& pair_label = std::get<2>(row);
        int landscape_id = std::get<3>(row);
        const GridAgg& a = std::get<4>(row);

        int ux = static_cast<int>(a.xs.size());
        int uy = static_cast<int>(a.ys.size());
        int spacing_x = phase8g_guess_spacing(a.xs);
        int spacing_y = phase8g_guess_spacing(a.ys);

        std::string grid_guess = std::to_string(ux) + "x" + std::to_string(uy);

        grid << run << ","
             << delay_seconds << ","
             << "\"" << kind << "\","
             << "\"" << pair_label << "\","
             << landscape_id << ","
             << a.count << ","
             << ux << ","
             << uy << ","
             << a.minx << ","
             << a.maxx << ","
             << a.miny << ","
             << a.maxy << ","
             << spacing_x << ","
             << spacing_y << ","
             << "\"" << grid_guess << "\","
             << score << ","
             << "\"" << phase8g_join_set(a.xs, 30) << "\","
             << "\"" << phase8g_join_set(a.ys, 30) << "\""
             << "\n";

        if (written < 80)
        {
            log << "score=" << score
                << " kind=" << kind
                << " pair=" << pair_label
                << " landscape=" << landscape_id
                << " count=" << a.count
                << " grid=" << grid_guess
                << " x=[" << a.minx << "," << a.maxx << "]"
                << " y=[" << a.miny << "," << a.maxy << "]"
                << " spacing=" << spacing_x << "/" << spacing_y
                << " xs=" << phase8g_join_set(a.xs, 12)
                << " ys=" << phase8g_join_set(a.ys, 12)
                << "\n";
        }

        written++;
    }

    log << "\nSUMMARY\n";
    log << "grid_candidate_rows=" << written << "\n";
    log << "csv_rows_written=" << rows.size() * pair_count << "\n";

    file_log("Phase 8G done run=" + std::to_string(run) + " rows=" + std::to_string(rows.size()) + " grid_candidates=" + std::to_string(written));
}

static void start_phase8g_grid_and_section_confirmation()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("Phase 8G grid and section confirmation started");

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
            {
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            }

            write_phase8g_grid_and_section_confirmation_snapshot(i + 1, target);
        }

        file_log("Phase 8G grid and section confirmation finished");
    }).detach();
}
// END PHASE8G_GRID_AND_SECTION_CONFIRMATION



// BEGIN PHASE8H_WORLD_RECONSTRUCTION

static int32_t phase8h_i32(uint8_t* base, size_t off)
{
    int32_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static float phase8h_f32(uint8_t* base, size_t off)
{
    float v = 0.f;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static uintptr_t phase8h_ptr(uint8_t* base, size_t off)
{
    uintptr_t v = 0;
    std::memcpy(&v, base + off, sizeof(v));
    return v;
}

static bool phase8h_probably_ptr(uintptr_t v)
{
    if (v < 0x10000ULL) return false;
    if (v == 0xccccccccccccccccULL) return false;
    if (v == 0xcdcdcdcdcdcdcdcdULL) return false;
    if (v == 0xddddddddddddddddULL) return false;
    if ((v & 0xffff000000000000ULL) == 0xffff000000000000ULL) return false;
    return true;
}

static int phase8h_extract_int_after(const std::string& text, const std::string& token)
{
    size_t pos = text.rfind(token);
    if (pos == std::string::npos) return -1;

    pos += token.size();

    std::string digits;

    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
    {
        digits.push_back(text[pos]);
        pos++;
    }

    if (digits.empty()) return -1;

    try
    {
        return std::stoi(digits);
    }
    catch (...)
    {
        return -1;
    }
}

static void write_phase8h_world_reconstruction_snapshot(int run, int delay_seconds)
{
    auto out = out_dir();

    std::ofstream summary(out / "phase8h_summary.txt", std::ios::app);
    std::ofstream compcsv(out / "phase8h_confirmed_components.csv", std::ios::app);
    std::ofstream actorcsv(out / "phase8h_landscape_actor_candidates.csv", std::ios::app);
    std::ofstream boundscsv(out / "phase8h_island_global_bounds.csv", std::ios::app);
    std::ofstream ptrcsv(out / "phase8h_height_pointer_hints.csv", std::ios::app);

    if (run == 1)
    {
        compcsv <<
        "run,delay_seconds,kind,landscape_id,component_id,"
        "section_x,section_y,"
        "obj_addr,path\n";

        actorcsv <<
        "run,delay_seconds,landscape_id,class,name,"
        "actor_x,actor_y,actor_z,"
        "rot_x,rot_y,rot_z,"
        "scale_x,scale_y,scale_z,"
        "path,obj_addr\n";

        boundscsv <<
        "run,delay_seconds,landscape_id,"
        "count,min_x,max_x,min_y,max_y,"
        "grid_x,grid_y,"
        "spacing_x,spacing_y,"
        "world_guess_x,world_guess_y\n";

        ptrcsv <<
        "run,delay_seconds,kind,landscape_id,component_id,"
        "ptr_offset,ptr_value,"
        "near0,near4,near8,near12,"
        "path\n";
    }

    struct Comp
    {
        std::string kind;
        int landscape_id = -1;
        int component_id = -1;
        int32_t section_x = 0;
        int32_t section_y = 0;
        std::string path;
        UObject* obj = nullptr;
    };

    struct Actor
    {
        int landscape_id = -1;
        std::string cls;
        std::string name;
        std::string path;
        UObject* obj = nullptr;

        float x = 0;
        float y = 0;
        float z = 0;

        float rx = 0;
        float ry = 0;
        float rz = 0;

        float sx = 0;
        float sy = 0;
        float sz = 0;
    };

    std::vector<Comp> comps;
    std::vector<Actor> actors;

    int scanned = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o) return RC::LoopAction::Continue;

            scanned++;

            std::string cls = class_name(o);
            std::string name = obj_name(o);
            std::string path = obj_path(o);

            bool runtime =
                path.find("/Game/Maps/GYM/Genlandia/GenlandiaMulty") != std::string::npos &&
                path.find("PersistentLevel") != std::string::npos;

            if (!runtime) return RC::LoopAction::Continue;

            if (path.find("Default__") != std::string::npos)
                return RC::LoopAction::Continue;

            bool is_collision =
                cls.find("LandscapeHeightfieldCollisionComponent") != std::string::npos;

            bool is_component =
                cls == "LandscapeComponent" ||
                cls.find("LandscapeComponent") != std::string::npos;

            bool is_actor =
                cls == "Landscape" ||
                cls == "LandscapeProxy" ||
                cls == "LandscapeStreamingProxy";

            int landscape_id =
                phase8h_extract_int_after(path, "Landscape_");

            if (is_collision || is_component)
            {
                uint8_t* base = reinterpret_cast<uint8_t*>(o);

                Comp c{};

                c.kind = is_collision ? "collision" : "component";
                c.landscape_id = landscape_id;

                c.component_id =
                    is_collision
                    ? phase8h_extract_int_after(path, "LandscapeHeightfieldCollisionComponent_")
                    : phase8h_extract_int_after(path, "LandscapeComponent_");

                // confirmed Phase 8G offsets
                c.section_x = phase8h_i32(base, 1296);
                c.section_y = phase8h_i32(base, 1300);

                c.path = path;
                c.obj = o;

                comps.push_back(c);

                compcsv
                    << run << ","
                    << delay_seconds << ","
                    << c.kind << ","
                    << c.landscape_id << ","
                    << c.component_id << ","
                    << c.section_x << ","
                    << c.section_y << ","
                    << "\"0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\","
                    << "\"" << c.path << "\"\n";

                // targeted pointer hints
                const size_t ptr_offsets[] = {
                    1684,1688,1732,1740,1748,1752,1760,
                    1880,1888,1944,1952,
                    2000,2020,2040,2060,2080,2120
                };

                for (size_t poff : ptr_offsets)
                {
                    uintptr_t ptr = phase8h_ptr(base, poff);

                    if (!phase8h_probably_ptr(ptr))
                        continue;

                    ptrcsv
                        << run << ","
                        << delay_seconds << ","
                        << c.kind << ","
                        << c.landscape_id << ","
                        << c.component_id << ","
                        << poff << ","
                        << "\"0x" << std::hex << ptr << std::dec << "\","
                        << phase8h_i32(base, poff + 0) << ","
                        << phase8h_i32(base, poff + 4) << ","
                        << phase8h_i32(base, poff + 8) << ","
                        << phase8h_i32(base, poff + 12) << ","
                        << "\"" << c.path << "\"\n";
                }
            }

            if (is_actor)
            {
                uint8_t* base = reinterpret_cast<uint8_t*>(o);

                Actor a{};

                a.landscape_id = landscape_id;
                a.cls = cls;
                a.name = name;
                a.path = path;
                a.obj = o;

                // broad transform probing
                a.x = phase8h_f32(base, 656);
                a.y = phase8h_f32(base, 660);
                a.z = phase8h_f32(base, 664);

                a.rx = phase8h_f32(base, 668);
                a.ry = phase8h_f32(base, 672);
                a.rz = phase8h_f32(base, 676);

                a.sx = phase8h_f32(base, 680);
                a.sy = phase8h_f32(base, 684);
                a.sz = phase8h_f32(base, 688);

                actors.push_back(a);

                actorcsv
                    << run << ","
                    << delay_seconds << ","
                    << a.landscape_id << ","
                    << "\"" << a.cls << "\","
                    << "\"" << a.name << "\","
                    << a.x << ","
                    << a.y << ","
                    << a.z << ","
                    << a.rx << ","
                    << a.ry << ","
                    << a.rz << ","
                    << a.sx << ","
                    << a.sy << ","
                    << a.sz << ","
                    << "\"" << a.path << "\","
                    << "\"0x" << std::hex << reinterpret_cast<uintptr_t>(o) << std::dec << "\"\n";
            }

            return RC::LoopAction::Continue;
        }
    );

    summary << "\n===== PHASE 8H RUN " << run << " DELAY " << delay_seconds << "s =====\n";
    summary << "scanned_objects=" << scanned << "\n";
    summary << "components=" << comps.size() << "\n";
    summary << "actors=" << actors.size() << "\n";

    struct Bounds
    {
        int count = 0;

        int minx = INT32_MAX;
        int maxx = INT32_MIN;

        int miny = INT32_MAX;
        int maxy = INT32_MIN;

        std::set<int> xs;
        std::set<int> ys;
    };

    std::map<int, Bounds> island_bounds;

    for (const auto& c : comps)
    {
        auto& b = island_bounds[c.landscape_id];

        b.count++;

        b.minx = std::min(b.minx, c.section_x);
        b.maxx = std::max(b.maxx, c.section_x);

        b.miny = std::min(b.miny, c.section_y);
        b.maxy = std::max(b.maxy, c.section_y);

        b.xs.insert(c.section_x);
        b.ys.insert(c.section_y);
    }

    for (const auto& kv : island_bounds)
    {
        int lid = kv.first;
        const auto& b = kv.second;

        int spacing_x = 0;
        int spacing_y = 0;

        if (b.xs.size() >= 2)
        {
            auto it = b.xs.begin();
            int a = *it++;
            int bb = *it;
            spacing_x = bb - a;
        }

        if (b.ys.size() >= 2)
        {
            auto it = b.ys.begin();
            int a = *it++;
            int bb = *it;
            spacing_y = bb - a;
        }

        int grid_x = static_cast<int>(b.xs.size());
        int grid_y = static_cast<int>(b.ys.size());

        // provisional world placement guess
        int world_guess_x = b.minx * spacing_x;
        int world_guess_y = b.miny * spacing_y;

        boundscsv
            << run << ","
            << delay_seconds << ","
            << lid << ","
            << b.count << ","
            << b.minx << ","
            << b.maxx << ","
            << b.miny << ","
            << b.maxy << ","
            << grid_x << ","
            << grid_y << ","
            << spacing_x << ","
            << spacing_y << ","
            << world_guess_x << ","
            << world_guess_y
            << "\n";

        summary
            << "Landscape_" << lid
            << " count=" << b.count
            << " grid=" << grid_x << "x" << grid_y
            << " x=[" << b.minx << "," << b.maxx << "]"
            << " y=[" << b.miny << "," << b.maxy << "]"
            << " spacing=" << spacing_x << "/" << spacing_y
            << "\n";
    }

    summary << "DECISION=world_reconstruction_active\n";

    file_log(
        "Phase 8H done run=" +
        std::to_string(run) +
        " comps=" +
        std::to_string(comps.size()) +
        " actors=" +
        std::to_string(actors.size())
    );
}

static void start_phase8h_world_reconstruction()
{
    static bool started = false;
    if (started) return;
    started = true;

    file_log("Phase 8H world reconstruction started");

    std::thread([]()
    {
        const int delays[] = {180, 360, 600};

        int previous = 0;

        for (int i = 0; i < 3; ++i)
        {
            int target = delays[i];
            int delta = target - previous;
            previous = target;

            if (delta > 0)
            {
                std::this_thread::sleep_for(std::chrono::seconds(delta));
            }

            write_phase8h_world_reconstruction_snapshot(i + 1, target);
        }

        file_log("Phase 8H world reconstruction finished");
    }).detach();
}

// END PHASE8H_WORLD_RECONSTRUCTION



static void scan_render_targets()
{
    static int attempts = 0;

    if (attempts >= 90)
    {
        file_log("Phase 8C4 scan_render_targets stopping at attempt=" + std::to_string(attempts));
        return;
    }

    attempts++;
    file_log("Phase 8C4 scan_render_targets attempt=" + std::to_string(attempts));

    file_log("Phase 7 clean scan_render_targets started attempt " + std::to_string(attempts));

    int scanned = 0;
    int found = 0;

    RC::Unreal::UObjectGlobals::ForEachUObject(
        [&](UObject* o, [[maybe_unused]] int32_t chunk_index, [[maybe_unused]] int32_t object_index)
        {
            if (!o)
            {
                return RC::LoopAction::Continue;
            }

            scanned++;

            std::string cls = class_name(o);

            if (
                cls != "TextureRenderTarget2D" &&
                cls != "TextureRenderTarget2DArray"
            )
            {
                return RC::LoopAction::Continue;
            }

            std::string full = obj_path(o);

            if (
                full.find("RT_MapCapture") == std::string::npos &&
                full.find("RT_MapFog") == std::string::npos &&
                full.find("RT_LandscapeTable") == std::string::npos &&
                full.find("RT_LandscapeHeights") == std::string::npos &&
                full.find("RT_Biomes") == std::string::npos &&
                full.find("RT_SubBiomes") == std::string::npos &&
                full.find("RT_BiomeDistanceFields") == std::string::npos
            )
            {
                return RC::LoopAction::Continue;
            }

            found++;

            write_phase7a_ue_runtime_readback_discovery(o);
            write_phase7b_ufunction_signature_dump(o);
            write_phase7c1_context_object_discovery(o);
            // Phase 7D disabled in v1.9.9. Phase 7E supersedes it.
            // write_phase7d_export_render_target_attempt(o);

            // Delay Phase 7F until later scan attempts so player can log in,
            // world can load, and fullscreen map can initialize RT_MapCapture.
            if (attempts >= 90)
            {
                // disabled v1.10.1: write_phase7f_pixel_grid_sample(o);
                // disabled v1.11.0: write_phase7g_long_sampling_campaign(o, attempts);
                // disabled v1.12.0: write_phase8a_landscape_heightmap_extraction(o, attempts);
                // disabled v1.13.0: write_phase8b_targeted_landscape_runtime_index(o, attempts);
                write_phase8c_component_layout_and_tiny_raw_probes(o, attempts);
            }

            return RC::LoopAction::Continue;
        }
    );

    file_log("Phase 7 clean scan_render_targets done. found=" + std::to_string(found) + " scanned=" + std::to_string(scanned));
}


class RTNativeExporter : public CppUserModBase
{
public:
    RTNativeExporter() : CppUserModBase()
    {
        ModName = STR("RTNativeExporter");
        ModVersion = STR("1.18.0");
    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter v1.18.0.2.2 on_unreal_init\n"));
        file_log("RTNativeExporter v1.18.0.2.2 on_unreal_init");
        // disabled v1.15.0: start_phase8d_independent_landscape_watchdog();
        // disabled v1.16.0: start_phase8e_timed_layout_memory_probes();
        // disabled v1.17.0: start_phase8f_offset_value_matrix();
        // disabled v1.18.0: start_phase8g_grid_and_section_confirmation();
        start_phase8h_world_reconstruction();
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
