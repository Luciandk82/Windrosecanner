#define NOMINMAX

#include <Mod/CppUserModBase.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/FProperty.hpp>
#include <windows.h>
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

    file_log("Phase 6D resource probe candidate: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
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

    file_log("Phase 6D native memory probe: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
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

    file_log("Phase 6D deep pointer probe: " + path + " size=" + std::to_string(sx) + "x" + std::to_string(sy));
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
    if (path.find("RT_MapCapture") == std::string::npos) return;

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

    file_log("Phase 6D targeted chain probe RT_MapCapture size=" + std::to_string(sx) + "x" + std::to_string(sy));
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

    f << "Phase 6D engine method discovery\n";
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
    file_log("Phase 6D engine method discovery wrote phase6_engine_method_discovery.txt");
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

    f << "Phase 6D string context scanner\n";
    f << "Goal: locate actual string addresses inside WindroseServer-Win64-Shipping.exe and dump nearby memory.\n";
    f << "No function calls. No ReadPixels. No GPU access.\n\n";

    HMODULE hExe = GetModuleHandleW(nullptr);
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

    file_log("Phase 6D wrote phase6b_string_context.txt");
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

    f << "Phase 6D vtable diagnostic\n";
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
    file_log("Phase 6D wrote phase6c_vtable_diagnostic.txt");
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

    log << "Phase 6D raw candidate dump\n";
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

    file_log("Phase 6D raw candidate dump done count=" + std::to_string(dumped));
}
// END PHASE6D_RAW_CANDIDATE_DUMP

static void scan_render_targets()
{
    static bool phase6_done = false;
    if (!phase6_done) { phase6_done = true; phase6_engine_method_discovery(); phase6b_scan_context(); }
    file_log("Phase 6D scan_render_targets started");
    Output::send<LogLevel::Verbose>(STR("[RTN] Phase 6D scan_render_targets started\n"));

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
        write_native_memory_probe(o);
        write_deep_pointer_probe(o);
        write_targeted_chain_probe(o);
        write_phase6c_vtable_diagnostic(o);
        write_phase6d_raw_candidate_dump(o);

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

    file_log("Phase 6D scan_render_targets done. found=" + std::to_string(found) + " scanned=" + std::to_string(scanned));
    Output::send<LogLevel::Verbose>(STR("[RTN] Phase 6D done. found={} scanned={}\n"), found, scanned);
}

class RTNativeExporter : public CppUserModBase
{
public:
    RTNativeExporter() : CppUserModBase()
    {
        ModName = STR("RTNativeExporter");
        ModVersion = STR("1.2.0");
    }

    ~RTNativeExporter() override {}

    auto on_unreal_init() -> void override
    {
        Output::send<LogLevel::Verbose>(STR("[RTN] RTNativeExporter v1.2 on_unreal_init\n"));
        file_log("RTNativeExporter v1.2 on_unreal_init");
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
