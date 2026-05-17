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

static void scan_render_targets()
{
    static int attempts = 0;

    if (attempts >= 36)
    {
        return;
    }

    attempts++;

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
            if (attempts >= 8)
            {
                // disabled v1.10.1: write_phase7f_pixel_grid_sample(o);
                // disabled v1.11.0: write_phase7g_long_sampling_campaign(o, attempts);
                write_phase8a_landscape_heightmap_extraction(o, attempts);
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
        ModVersion = STR("1.11.2");
    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter v1.11.2.2.2 on_unreal_init\n"));
        file_log("RTNativeExporter v1.11.2.2.2 on_unreal_init");
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
