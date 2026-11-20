// UCOpenBridge: the engine bridge (bridge.h) for DeusEdix, backed by OpenDE instead of the Deus Ex SDK's
// Windows engine (Editor.dll).
//
// The host (UCMapKitHost) reads the commands, keeps the session and writes the files; this library is the only
// code that talks to an engine. It runs OpenDE (https://github.com/.../OpenDE, a portable C++ reimplementation
// of the Deus Ex engine) and implements the same UCBridgeApi as UCEngineBridge.cpp does for the SDK engine:
// the same answers ("UCMapKit|..." lines), the same file formats (host/PROTOCOL.md), the same UnrealEd command
// lines through exec() (MAP LOAD, BRUSH ADD, BSP REBUILD, LIGHT APPLY, PATHS DEFINE, MAP SAVE...).
//
// Differences from the SDK bridge, and why:
//   - No window, no renderer: OpenDE's editor (deused) draws with ImGui; the bridge uses the engine's own
//     packages, BSP/CSG builder, lightmap builder, path builder and map writer directly, and never starts a
//     window or a GL context. LIGHT APPLY is dx::lightbuild::rebuild (the same build deused runs).
//   - The engine is a set of static libraries (libdx-engine.a, libdx-compiler.a): no DLL patching, no SEH.
//   - Text is UTF-8 everywhere (OpenDE keeps std::string), so no TCHAR conversion.
//   - Properties are read and written through OpenDE's own reflection (UObject::propertyAddress and
//     UProperty::toText/fromText), which is OpenDE's ExportText/ImportText.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"

#include "bridge.h"

#include "core/config.h"
#include "core/objects.h"
#include "engine/actor.h"
#include "engine/actor_search.h"
#include "engine/bsp.h"
#include "engine/brush_edit.h"
#include "engine/csg.h"
#include "engine/editor_ops.h"
#include "engine/lightmap_builder.h"
#include "engine/map_check.h"
#include "engine/map_writer.h"
#include "engine/mesh.h"
#include "engine/path_builder.h"
#include "engine/props_engine.h"
#include "engine/resources.h"
#include "engine/t3d.h"
#include "engine/world.h"

#include "compiler/assets.h"
#include "compiler/script_compiler.h"
#include "compiler/script_package.h"

#include "extension/consys.h"

// PolyFlags the bridge needs and OpenDE's bsp.h does not name
static constexpr uint32_t PF_Translucent = 0x00000004;
static constexpr uint32_t PF_Modulated = 0x00000040;

// A JSON row keeps its keys in the order they are written.
typedef nlohmann::ordered_json Json;

// The host (bridge.h): the bridge's lines go to its stdout, its rows to the files it opens.
static const UCHostApi* Host = nullptr;

// A line on the host's stdout, formatted like printf: the answers of the commands ("UCMapKit|...", README.md).
static void say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int size = vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    std::vector<char> line(size > 0 ? static_cast<size_t>(size) + 1 : 1, 0);
    vsnprintf(line.data(), line.size(), format, args);
    va_end(args);
    Host->print(line.data());
}

// A file of one JSON value a line, the format of every file the host writes (README.md), opened by the host.
class JsonLines {
public:
    explicit JsonLines(const char* path, bool append = false)
        : file(Host->open(path, append ? 1 : 0))
    {
    }
    ~JsonLines()
    {
        if (file) {
            Host->close(file);
        }
    }
    bool ok() const { return file != nullptr; }
    void write(const Json& row) { Host->row(file, row.dump(-1, ' ', false, Json::error_handler_t::replace).c_str()); }
    void bytes(const void* data, size_t size)
    {
        if (size) {
            Host->bytes(file, data, size);
        }
    }

private:
    UCFile* file;
    JsonLines(const JsonLines&);
    JsonLines& operator=(const JsonLines&);
};

// Numbers kept to the precision the dumps have always had: %g's 6 significant digits, or a number of decimals.
static double digits(double v)
{
    char text[32];
    snprintf(text, sizeof(text), "%g", v);
    return strtod(text, nullptr);
}

static double decimals(double v, int places)
{
    char text[64];
    snprintf(text, sizeof(text), "%.*f", places, v);
    return strtod(text, nullptr);
}

static Json xyz(const dx::Vector& v)
{
    return Json::array({ digits(v.x), digits(v.y), digits(v.z) });
}

static Json xyz(const dx::Vector& v, int places)
{
    return Json::array({ decimals(v.x, places), decimals(v.y, places), decimals(v.z, places) });
}

// A dump of bulk numbers (BSP polygons, mesh triangles, lightmaps): a JSON header line, with the dump's strings
// and the list of its arrays (name, type, count), then those arrays, little endian, back to back (PROTOCOL.md).
class Arrays {
public:
    Json header;
    explicit Arrays(const char* format) { header["format"] = format; }
    void add(const char* name, const std::vector<int32_t>& v)
    {
        push(name, "i32", v.empty() ? nullptr : v.data(), v.size() * sizeof(int32_t), v.size());
    }
    void add(const char* name, const std::vector<uint32_t>& v)
    {
        push(name, "u32", v.empty() ? nullptr : v.data(), v.size() * sizeof(uint32_t), v.size());
    }
    void add(const char* name, const std::vector<double>& v)
    {
        push(name, "f64", v.empty() ? nullptr : v.data(), v.size() * sizeof(double), v.size());
    }
    void add(const char* name, const std::vector<uint8_t>& v)
    {
        push(name, "u8", v.empty() ? nullptr : v.data(), v.size(), v.size());
    }
    int write(const char* path)
    {
        JsonLines out(path);
        if (!out.ok()) {
            return 0;
        }
        header["arrays"] = list;
        out.write(header);
        for (size_t i = 0; i < parts.size(); i++) {
            out.bytes(parts[i].first, parts[i].second);
        }
        return 1;
    }

private:
    Json list = Json::array();
    std::vector<std::pair<const void*, size_t>> parts;
    void push(const char* name, const char* type, const void* data, size_t bytes, size_t count)
    {
        Json entry;
        entry["name"] = name;
        entry["type"] = type;
        entry["count"] = count;
        list.push_back(entry);
        parts.emplace_back(data, bytes);
    }
};

static void push3(std::vector<double>& out, double x, double y, double z)
{
    out.push_back(x);
    out.push_back(y);
    out.push_back(z);
}

// The index of a texture in a dump's texture list, added the first time ("none" for no texture)
class TextureList {
public:
    Json names = Json::array();
    int32_t operator()(dx::UObject* texture)
    {
        auto it = index.find(texture);
        if (it != index.end()) {
            return it->second;
        }
        int32_t at = static_cast<int32_t>(names.size());
        names.push_back(texture ? texture->pathName() : std::string("none"));
        index[texture] = at;
        return at;
    }

private:
    std::map<dx::UObject*, int32_t> index;
};

// ------------------------------------------------------------------------------------- the session's state
static std::filesystem::path GameRoot;              // the game's folders (System, Textures...)
static dx::ULevel* Level = nullptr;                  // the level the commands work on
static dx::World World;                              // traces and visibility on the level
static std::string MapName;

static void attachLevel(dx::ULevel* level, const std::string& name)
{
    Level = level;
    MapName = name;
    World.level = level;
    World.levelInfo = level && !level->actors.empty() ? level->actors[0] : nullptr;
    World.actors = level ? level->actors : std::vector<dx::UObject*> {};
    World.collisionHash.clear();
    if (level) {
        World.collisionHash.sync(World.actors);
    }
}

// The properties OpenDE gives Object itself (name, class, outer, flags). The SDK's engine has them as plain C++
// members, not as UProperties, so UnrealEd's FindField finds none of them: a property named at run time is never
// one of these (writing one would rename an object behind the level's back).
static bool intrinsicProperty(const std::string& name)
{
    static const char* const names[] = { "name", "class", "outer", "objectflags", "objectinternal" };
    std::string lower = name;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char* known : names) {
        if (lower == known) {
            return true;
        }
    }
    return false;
}

// A property of an object by name (OpenDE's reflection): nullptr when the class has none.
static dx::UProperty* propertyOf(dx::UObject* object, const char* name)
{
    return object && object->cls ? object->cls->findProperty(dx::Name(name)) : nullptr;
}

static uint8_t* propertyAddress(dx::UObject* object, dx::UProperty* property, int index = 0)
{
    if (!object || !object->data || !property || index < 0 || index >= property->arrayDim) {
        return nullptr;
    }
    return object->data + property->offset + static_cast<size_t>(index) * property->elementSize();
}

// The ExportText of a property ("" when the object does not have it)
static std::string propertyText(dx::UObject* object, const char* name)
{
    dx::UProperty* property = propertyOf(object, name);
    uint8_t* data = propertyAddress(object, property);
    return data && property ? property->toText(data) : std::string();
}

// The ImportText of a property (false when the object does not have it, or the text is not for it)
static bool setPropertyText(dx::UObject* object, dx::UProperty* property, int index, const char* text)
{
    uint8_t* data = propertyAddress(object, property, index);
    return data && property && property->fromText(data, text);
}

// The name of a class as the editor sends it, and the @<Class> / @@<Class> forms
static dx::UClass* classNamed(const char* name)
{
    return dx::objects::findClass(name);
}

static bool sameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// by name, or "@<Class>": the first actor of that class (or a subclass); "@LevelInfo" is the map's own
static dx::UObject* findActor(const char* name)
{
    if (!Level || !name || !*name) {
        return nullptr;
    }
    if (name[0] == '@') {
        dx::UClass* cls = classNamed(name + 1);
        if (!cls) {
            return nullptr;
        }
        if (dx::engineClasses().levelInfo && cls == dx::engineClasses().levelInfo) {
            return Level->actors.empty() ? nullptr : Level->actors[0];
        }
        for (dx::UObject* actor : Level->actors) {
            if (actor && !actor->destroyed && actor->isA(cls)) {
                return actor;
            }
        }
        return nullptr;
    }
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed && sameName(actor->name.str(), name)) {
            return actor;
        }
    }
    return nullptr;
}

// The builder brush: the Brush actor with CsgOper CSG_Active (as deused finds it)
static dx::UObject* builderBrush()
{
    if (!Level) {
        return nullptr;
    }
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed && actor->cls && actor->cls->name == dx::Name("Brush") && dx::brush::CsgOper(actor) == dx::csg::CsgActive) {
            return actor;
        }
    }
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed && actor->cls && actor->cls->name == dx::Name("Brush")) {
            return actor;
        }
    }
    return nullptr;
}

static bool isStaticBrush(dx::UObject* actor)
{
    return actor && !actor->destroyed && dx::isBrush(actor) && dx::actor::Brush(actor) && dx::actor::bStatic(actor) && actor != builderBrush();
}

static dx::UModel* brushModel(dx::UObject* actor)
{
    return dx::cast<dx::UModel>(dx::actor::Brush(actor));
}

// The static brushes of the level in the order they are in the level (the build order of an imported map)
static std::vector<dx::UObject*> staticBrushes()
{
    std::vector<dx::UObject*> brushes;
    if (!Level) {
        return brushes;
    }
    dx::UObject* builder = builderBrush();
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed && actor != builder && dx::isBrush(actor) && dx::actor::Brush(actor) && dx::actor::bStatic(actor)) {
            brushes.push_back(actor);
        }
    }
    return brushes;
}

// ------------------------------------------------------------------------------------- level geometry
// UC_POINT x y z: which BSP leaf and zone a point falls in (leaf -1 = inside solid).
static int probePoint(float x, float y, float z)
{
    if (!Level || !Level->model || Level->model->nodes.empty()) {
        return 0;
    }
    dx::UModel& model = *Level->model;
    dx::Vector point { x, y, z };
    int32_t leaf = -1;
    int32_t zone = dx::bsp::pointLeafZone(model, point, &leaf);
    if (dx::csg::pointSolid(model, point)) {
        leaf = -1;
    }
    const char* zoneName = "none";
    std::string name;
    if (zone >= 0 && static_cast<size_t>(zone) < model.zones.size()) {
        dx::UObject* actor = model.zones[static_cast<size_t>(zone)].zoneActor;
        if (actor) {
            name = actor->name.str();
            zoneName = name.c_str();
        }
    }
    say("UCMapKit|point|%.0f %.0f %.0f|leaf %d|zone %d|%s", static_cast<double>(x), static_cast<double>(y),
        static_cast<double>(z), leaf, zone, zoneName);
    return 1;
}

// UC_TRACE x0 y0 z0 x1 y1 z1: the first level-geometry hit on the segment, and its normal.
static int traceLine(const float from[3], const float to[3])
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::CheckResult hit;
    dx::Vector start { from[0], from[1], from[2] };
    dx::Vector end { to[0], to[1], to[2] };
    if (!World.bspLineCheck(hit, start, end, dx::Vector { 0, 0, 0 })) {
        say("UCMapKit|trace|none");
    } else {
        say("UCMapKit|trace|hit|%.2f %.2f %.2f|%.3f %.3f %.3f", static_cast<double>(hit.location.x),
            static_cast<double>(hit.location.y), static_cast<double>(hit.location.z), static_cast<double>(hit.normal.x),
            static_cast<double>(hit.normal.y), static_cast<double>(hit.normal.z));
    }
    return 1;
}

// The world size of a texture in world units, as the engine draws it: USize * Scale, VSize * Scale
static float textureWorldWidth(dx::UObject* texture)
{
    if (!texture) {
        return 128.f;
    }
    dx::UProperty* size = propertyOf(texture, "USize");
    dx::UProperty* scale = propertyOf(texture, "DrawScale");
    if (!scale) {
        scale = propertyOf(texture, "Scale");
    }
    float us = size ? static_cast<float>(*reinterpret_cast<int32_t*>(propertyAddress(texture, size))) : 128.f;
    float sc = scale ? *reinterpret_cast<float*>(propertyAddress(texture, scale)) : 1.f;
    if (!(sc > 0)) {
        sc = 1.f;
    }
    return us * sc;
}

static float textureWorldHeight(dx::UObject* texture)
{
    if (!texture) {
        return 128.f;
    }
    dx::UProperty* size = propertyOf(texture, "VSize");
    dx::UProperty* scale = propertyOf(texture, "DrawScale");
    if (!scale) {
        scale = propertyOf(texture, "Scale");
    }
    float vs = size ? static_cast<float>(*reinterpret_cast<int32_t*>(propertyAddress(texture, size))) : 128.f;
    float sc = scale ? *reinterpret_cast<float*>(propertyAddress(texture, scale)) : 1.f;
    if (!(sc > 0)) {
        sc = 1.f;
    }
    return vs * sc;
}

// The PolyFlags a texture adds to the surfaces it is on (what deused's renderer adds: scene_renderer.cc,
// textureFlags). Deus Ex textures keep their style in b* booleans, not in a PolyFlags field.
static uint32_t textureFlags(dx::UObject* texture)
{
    if (!texture || !texture->data) {
        return 0;
    }
    uint32_t flags = 0;
    dx::UProperty* masked = propertyOf(texture, "bMasked");
    dx::UProperty* transparent = propertyOf(texture, "bTransparent");
    dx::UProperty* modulate = propertyOf(texture, "bModulate");
    dx::UProperty* unlit = propertyOf(texture, "bUnlit");
    dx::UProperty* twoSided = propertyOf(texture, "bTwoSided");
    if (masked && *reinterpret_cast<uint32_t*>(propertyAddress(texture, masked))) {
        flags |= dx::bsp::PF_Masked;
    }
    if (transparent && *reinterpret_cast<uint32_t*>(propertyAddress(texture, transparent))) {
        flags |= PF_Translucent;
    }
    if (modulate && *reinterpret_cast<uint32_t*>(propertyAddress(texture, modulate))) {
        flags |= PF_Modulated;
    }
    if (unlit && *reinterpret_cast<uint32_t*>(propertyAddress(texture, unlit))) {
        flags |= dx::bsp::PF_Unlit;
    }
    if (twoSided && *reinterpret_cast<uint32_t*>(propertyAddress(texture, twoSided))) {
        flags |= dx::bsp::PF_TwoSided;
    }
    return flags;
}

// UC_BSPDUMP <file>: the final world geometry (every visible BSP polygon) with its texture and mapping, for the
// 3D view of the map editor. The arrays and their order are PROTOCOL.md's (see UCEngineBridge.cpp dumpBsp).
static int dumpBsp(const char* path)
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UModel& m = *Level->model;
    std::vector<dx::UObject*> brushes = staticBrushes();
    std::unordered_map<dx::UObject*, int32_t> brushIndex;
    for (size_t i = 0; i < brushes.size(); i++) {
        brushIndex[brushes[i]] = static_cast<int32_t>(i);
    }
    TextureList textures;
    std::vector<int32_t> texture, pan, vertexCount, brush, surface, brushPoly;
    std::vector<uint32_t> flags;
    std::vector<uint8_t> zone;
    std::vector<double> mapping, vertices;
    for (size_t n = 0; n < m.nodes.size(); n++) {
        const dx::BspNode& node = m.nodes[n];
        if (node.numVertices < 3 || node.surf < 0 || static_cast<size_t>(node.surf) >= m.surfs.size()) {
            continue;
        }
        const dx::BspSurf& surf = m.surfs[static_cast<size_t>(node.surf)];
        if (surf.polyFlags & (dx::bsp::PF_Invisible | dx::bsp::PF_Portal)) {
            continue;
        }
        float tw = 128, th = 128;
        if (surf.texture) {
            tw = textureWorldWidth(surf.texture);
            th = textureWorldHeight(surf.texture);
        }
        dx::Vector base = m.points[static_cast<size_t>(surf.pBase)];
        dx::Vector u = m.vectors[static_cast<size_t>(surf.vTextureU)];
        dx::Vector v = m.vectors[static_cast<size_t>(surf.vTextureV)];
        texture.push_back(textures(surf.texture));
        flags.push_back(surf.polyFlags);
        pan.push_back(surf.panU);
        pan.push_back(surf.panV);
        vertexCount.push_back(node.numVertices);
        auto found = brushIndex.find(surf.actor);
        brush.push_back(found == brushIndex.end() ? -1 : found->second);
        surface.push_back(node.surf);
        brushPoly.push_back(surf.brushPoly);
        zone.push_back(node.zone[1]);        push3(mapping, digits(base.x), digits(base.y), digits(base.z));
        push3(mapping, digits(u.x), digits(u.y), digits(u.z));
        push3(mapping, digits(v.x), digits(v.y), digits(v.z));
        mapping.push_back(digits(tw));
        mapping.push_back(digits(th));
        for (int k = 0; k < node.numVertices; k++) {
            const dx::Vector& p = m.points[static_cast<size_t>(m.verts[static_cast<size_t>(node.vertPool + k)].vertex)];
            push3(vertices, digits(p.x), digits(p.y), digits(p.z));
        }
    }
    std::vector<std::string> zones;
    for (const dx::ZoneProperties& z : m.zones) {
        zones.push_back(z.zoneActor ? z.zoneActor->name.str() : std::string());
    }
    // Zone 0 is the level's own (no ZoneInfo); trailing zones without one are left out, and a polygon in one
    // of them names zone 0, as the SDK engine writes an empty name for it.
    while (zones.size() > 1 && zones.back().empty()) {
        zones.pop_back();
    }
    for (uint8_t& number : zone) {
        if (number >= zones.size()) {
            number = 0;
        }
    }
    Arrays out("bsp");
    out.header["textures"] = textures.names;
    out.header["zones"] = zones;
    out.header["polygons"] = static_cast<int>(texture.size());
    out.add("texture", texture);
    out.add("flags", flags);
    out.add("pan", pan);
    out.add("vertexCount", vertexCount);
    out.add("brush", brush);
    out.add("surface", surface);
    out.add("brushPoly", brushPoly);
    out.add("zone", zone);
    out.add("mapping", mapping);
    out.add("vertices", vertices);
    if (!out.write(path)) {
        return 0;
    }
    say("UCMapKit|bsp|%d polygons", static_cast<int>(texture.size()));
    return 1;
}

// UC_BRUSHES <file>: the static brushes in build order, a row each.
static int listBrushes(const char* path)
{
    if (!Level) {
        return 0;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int k = 0;
    for (dx::UObject* actor : staticBrushes()) {
        Json row;
        row["ord"] = k++;
        row["name"] = actor->name.str();
        row["csg"] = static_cast<int>(dx::brush::CsgOper(actor));
        row["flags"] = dx::brush::PolyFlags(actor);
        row["group"] = dx::actor::Group(actor).none() ? std::string() : dx::actor::Group(actor).str();
        row["location"] = xyz(dx::actor::Location(actor));
        dx::Rotator rotation = dx::actor::Rotation(actor);
        row["rotation"] = Json::array({ rotation.pitch, rotation.yaw, rotation.roll });
        row["prePivot"] = xyz(dx::actor::PrePivot(actor));
        out.write(row);
    }
    say("UCMapKit|brushes|%d", k);
    return 1;
}

// UC_UNLINK <model name prefix>: every polygon of the brushes whose model's name starts with the prefix gets a
// surface of its own (iLink = its index), as BRUSH IMPORT + BRUSH ADD left them.
static int unlinkBrushes(const char* prefix)
{
    if (!Level || !prefix) {
        return 0;
    }
    size_t length = strlen(prefix);
    int brushes = 0, polys = 0;
    for (dx::UObject* actor : Level->actors) {
        dx::UModel* model = actor ? brushModel(actor) : nullptr;
        if (!model || !model->polys) {
            continue;
        }
        std::string name = model->name.str();
        if (name.size() < length || strncasecmp(name.c_str(), prefix, length) != 0) {
            continue;
        }
        for (size_t i = 0; i < model->polys->polys.size(); i++) {
            model->polys->polys[i].link = static_cast<int32_t>(i);
        }
        model->linked = 1;
        brushes++;
        polys += static_cast<int>(model->polys->polys.size());
    }
    say("UCMapKit|unlink|%d brushes|%d polygons", brushes, polys);
    return 1;
}

// Zones, visibility and collision bounds for the whole level, after CSG.
static int finishWorld()
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UModel& model = *Level->model;
    dx::bsp::refresh(model, true);
    dx::bsp::testVisibility(Level, model);
    dx::bsp::buildBounds(model);
    // the zones the geometry uses: zone 0 is the level's own, the others have a ZoneInfo actor
    int zones = 1;
    for (const dx::BspNode& node : model.nodes) {
        zones = std::max(zones, std::max(node.zone[0], node.zone[1]) + 1);
    }
    say("UCMapKit|world|%d nodes|%d surfs|%d zones", static_cast<int>(model.nodes.size()),
        static_cast<int>(model.surfs.size()), zones);
    return 1;
}

// UC_SIZES <file>: the collision cylinder of every non-brush actor, a row each.
static int listSizes(const char* path)
{
    if (!Level) {
        return 0;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed || dx::isBrush(actor)) {
            continue;
        }
        if (dx::engineClasses().levelInfo && actor->isA(dx::engineClasses().levelInfo)) {
            continue;
        }
        Json row;
        row["class"] = actor->cls->name.str();
        row["radius"] = decimals(dx::actor::CollisionRadius(actor), 2);
        row["height"] = decimals(dx::actor::CollisionHeight(actor), 2);
        out.write(row);
    }
    return 1;
}

// UC_ACTORS check: every non-brush actor whose origin lies inside solid geometry.
static int checkActors()
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UModel& model = *Level->model;
    int checked = 0, buried = 0;
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed || dx::isBrush(actor)) {
            continue;
        }
        if (dx::engineClasses().levelInfo && actor->isA(dx::engineClasses().levelInfo)) {
            continue;
        }
        if (dx::engineClasses().camera && actor->isA(dx::engineClasses().camera)) {
            continue;
        }
        checked++;
        if (!dx::csg::pointSolid(model, dx::actor::Location(actor))) {
            continue;
        }
        buried++;
        dx::Vector location = dx::actor::Location(actor);
        say("UCMapKit|in solid|%s|%s|%.0f %.0f %.0f", actor->cls->name.str().c_str(), actor->name.str().c_str(),
            static_cast<double>(location.x), static_cast<double>(location.y), static_cast<double>(location.z));
    }
    say("UCMapKit|actors|%d checked|%d in solid", checked, buried);
    return 1;
}

// UC_LIGHTSTATS: lightmap samples and storage, before/after LIGHT APPLY.
static int lightStats()
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UModel& m = *Level->model;
    long long samples = 0;
    for (const dx::LightMapIndex& lightMap : m.lightMaps) {
        samples += static_cast<long long>(lightMap.uClamp) * lightMap.vClamp;
    }
    say("UCMapKit|lightstats|%d|%lld|%d", static_cast<int>(m.lightMaps.size()), samples,
        static_cast<int>(m.lightBits.size()));
    return 1;
}

// The lighting build spawns a view actor (Camera) for its viewport in UnrealEd; OpenDE lights without one, so
// this only removes any the map carries.
static int clearCameras()
{
    if (!Level) {
        return 0;
    }
    dx::UClass* camera = dx::engineClasses().camera;
    int removed = 0;
    if (camera) {
        std::vector<dx::UObject*> kept;
        for (dx::UObject* actor : Level->actors) {
            if (actor && !actor->destroyed && actor->isA(camera)) {
                actor->destroyed = true;
                removed++;
                continue;
            }
            kept.push_back(actor);
        }
        Level->actors = kept;
        World.actors = kept;
    }
    say("UCMapKit|cameras removed|%d", removed);
    return 1;
}

// ------------------------------------------------------------------------------------- lighting dump
static dx::Vector lightColour(uint8_t H, uint8_t S, uint8_t V)
{
    dx::Vector hue = H < 86 ? dx::Vector { (85.f - H) / 85.f, H / 85.f, 0 }
                    : H < 171 ? dx::Vector { 0, (170.f - H) / 85.f, (H - 85.f) / 85.f }
                              : dx::Vector { (H - 170.f) / 85.f, 0, (255.f - H) / 85.f };
    // the engine's brightness curve (FGetHSV): dim lights and ambients count for much more than V/255
    float b = V * 1.4f / 255.f;
    b *= 0.70f / (0.01f + std::sqrt(b));
    b = std::clamp(b, 0.f, 1.f);
    return (hue + (dx::Vector { 1, 1, 1 } - hue) * (S / 255.f)) * b;
}

// A light of the level, read once (the dump asks about every texel and every object)
struct LightInfo {
    dx::Vector location;
    dx::Vector colour;
    dx::Vector direction;
    float radius = 0;
    int effect = 0;
    int cone = 0;
};

// The light a light actor gives a point (with a surface normal, or without for an object's centre)
static dx::Vector lightAt(const LightInfo& light, const dx::Vector& p, const dx::Vector* normal)
{
    dx::Vector d = light.location - p;
    float dist = d.size();
    if (dist >= light.radius) {
        return dx::Vector { 0, 0, 0 };
    }
    float f = 1.f - dist / light.radius;
    if (normal && light.effect != 13 /* LE_NonIncidence */ && dist > 0.01f) {
        float incidence = d.dot(*normal) / dist;
        if (incidence <= 0) {
            return dx::Vector { 0, 0, 0 };
        }
        f *= incidence;
    }
    if ((light.effect == 12 /* LE_Spotlight */ || light.effect == 8 /* LE_StaticSpot */) && dist > 0.01f) {
        float c = -d.dot(light.direction) / dist;
        float edge = std::cos(light.cone * dx::Pi / 512.f);
        if (c <= edge) {
            return dx::Vector { 0, 0, 0 };
        }
        f *= std::min(1.f, (c - edge) / std::max(0.05f, 1.f - edge) * 2.f);
    }
    return light.colour * f;
}

static dx::Vector ambientOf(dx::UObject* zone)
{
    if (!zone) {
        zone = Level && !Level->actors.empty() ? Level->actors[0] : nullptr;
    }
    if (!zone || !zone->data) {
        return dx::Vector { 0, 0, 0 };
    }
    dx::UProperty* hue = propertyOf(zone, "AmbientHue");
    dx::UProperty* saturation = propertyOf(zone, "AmbientSaturation");
    dx::UProperty* brightness = propertyOf(zone, "AmbientBrightness");
    if (!hue || !saturation || !brightness) {
        return dx::Vector { 0, 0, 0 };
    }
    return lightColour(*propertyAddress(zone, hue), *propertyAddress(zone, saturation), *propertyAddress(zone, brightness));
}

// a point in the XY projection of a (convex) BSP polygon
static bool insideXy(const dx::UModel& m, const dx::BspNode& node, float x, float y)
{
    int pos = 0, neg = 0;
    for (int k = 0; k < node.numVertices; k++) {
        const dx::Vector& a = m.points[static_cast<size_t>(m.verts[static_cast<size_t>(node.vertPool + k)].vertex)];
        const dx::Vector& b = m.points[static_cast<size_t>(m.verts[static_cast<size_t>(node.vertPool + (k + 1) % node.numVertices)].vertex)];
        float c = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
        if (c > 0.01f) {
            pos++;
        } else if (c < -0.01f) {
            neg++;
        }
    }
    return node.numVertices >= 3 && !(pos && neg);
}

struct SurfaceLight {
    std::vector<int32_t> texelAt;               // per surface: where its texels start, -1 without a lightmap
    std::vector<dx::Vector> texels, ambients;
};

struct LightDump {
    std::vector<int32_t> surface, size;
    std::vector<double> mapping;
    std::vector<uint8_t> rgb;
    std::vector<double> lights;
    Json actors = Json::array();
    std::vector<double> actorLight, actorAmbient;
    std::vector<int32_t> actorLightCount, actorLights;
};

// the surfaces' lightmaps as the game draws them; returns the surfaces written
static int lightSurfaces(dx::UModel& m, LightDump& out, SurfaceLight& lit, int& fit, int& checked)
{
    std::vector<int32_t> firstNode(m.surfs.size(), -1);
    for (size_t i = 0; i < m.nodes.size(); i++) {
        const dx::BspNode& node = m.nodes[i];
        if (node.surf >= 0 && static_cast<size_t>(node.surf) < firstNode.size() && firstNode[static_cast<size_t>(node.surf)] < 0) {
            firstNode[static_cast<size_t>(node.surf)] = static_cast<int32_t>(i);
        }
    }
    lit.texelAt.assign(m.surfs.size(), -1);
    lit.ambients.assign(m.surfs.size(), dx::Vector {});
    int surfs = 0;
    for (size_t s = 0; s < m.surfs.size(); s++) {
        dx::BspSurf& surf = m.surfs[s];
        if (surf.lightMap < 0 || static_cast<size_t>(surf.lightMap) >= m.lightMaps.size()) {
            continue;
        }
        const dx::LightMapIndex& lightMap = m.lightMaps[static_cast<size_t>(surf.lightMap)];
        int w = lightMap.uClamp, h = lightMap.vClamp;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
            continue;
        }
        dx::Vector normal = m.vectors[static_cast<size_t>(surf.vNormal)];
        dx::Vector base = m.points[static_cast<size_t>(surf.pBase)];
        dx::Vector tu = m.vectors[static_cast<size_t>(surf.vTextureU)];
        dx::Vector tv = m.vectors[static_cast<size_t>(surf.vTextureV)];
        dx::Vector bxc = tv.cross(normal), cxa = normal.cross(tu);
        float det = tu.dot(bxc);
        if (std::fabs(det) < 1e-6f) {
            continue;
        }
        if (firstNode[s] >= 0) {
            const dx::BspNode& node = m.nodes[static_cast<size_t>(firstNode[s])];
            int inside = 1;
            for (int k = 0; k < node.numVertices; k++) {
                dx::Vector d = m.points[static_cast<size_t>(m.verts[static_cast<size_t>(node.vertPool + k)].vertex)] - base;
                float u = (d.dot(tu) - lightMap.pan.x) / lightMap.uScale;
                float v = (d.dot(tv) - lightMap.pan.y) / lightMap.vScale;
                if (u < -1.5f || v < -1.5f || u > w + 1.5f || v > h + 1.5f) {
                    inside = 0;
                }
            }
            fit += inside;
            checked++;
        }
        dx::UObject* zone = nullptr;
        if (firstNode[s] >= 0) {
            const dx::BspNode& node = m.nodes[static_cast<size_t>(firstNode[s])];
            if (static_cast<size_t>(node.zone[1]) < m.zones.size()) {
                zone = m.zones[static_cast<size_t>(node.zone[1])].zoneActor;
            }
        }
        dx::Vector ambient = ambientOf(zone);
        // the lights of this surface: lights(lightActors...) up to a nullptr, one shadow mask each
        std::vector<const LightInfo*> lights;
        std::vector<LightInfo> lightStorage;
        if (lightMap.lightActors >= 0) {
            for (size_t k = static_cast<size_t>(lightMap.lightActors); k < m.lights.size() && m.lights[k]; k++) {
                dx::UObject* actor = m.lights[k];
                LightInfo info;
                info.location = dx::actor::Location(actor);
                info.colour = lightColour(dx::actor::LightHue(actor), dx::actor::LightSaturation(actor), dx::actor::LightBrightness(actor));
                info.direction = dx::rotationVector(dx::actor::Rotation(actor));
                info.radius = 25.f * (dx::actor::LightRadius(actor) + 1.f);
                info.effect = dx::actor::LightEffect(actor);
                info.cone = dx::actor::LightCone(actor);
                lightStorage.push_back(info);
            }
        }
        for (const LightInfo& info : lightStorage) {
            lights.push_back(&info);
        }
        int stride = (w + 7) / 8;
        lit.texelAt[s] = static_cast<int32_t>(lit.texels.size());
        lit.ambients[s] = ambient;
        lit.texels.resize(lit.texels.size() + static_cast<size_t>(w) * static_cast<size_t>(h), dx::Vector {});
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                float r1 = lightMap.pan.x + i * lightMap.uScale;
                float r2 = lightMap.pan.y + j * lightMap.vScale;
                dx::Vector p = base + (bxc * r1 + cxa * r2) / det + normal * 4.f;
                dx::Vector light {};
                for (size_t k = 0; k < lights.size(); k++) {
                    long long byte = static_cast<long long>(lightMap.dataOffset) + static_cast<long long>(k) * stride * h + static_cast<long long>(j) * stride + (i >> 3);
                    if (byte < 0 || byte >= static_cast<long long>(m.lightBits.size())) {
                        continue;
                    }
                    if (!(m.lightBits[static_cast<size_t>(byte)] & (1 << (i & 7)))) {
                        continue;                       // in shadow
                    }
                    light += lightAt(*lights[k], p, &normal);
                }
                lit.texels[static_cast<size_t>(lit.texelAt[s]) + static_cast<size_t>(j) * static_cast<size_t>(w) + static_cast<size_t>(i)] = light;
                dx::Vector colour = ambient + light * 2.f;      // the lights are overbright on the surfaces
                out.rgb.push_back(static_cast<uint8_t>(std::clamp(std::floor(colour.x * 127.5f), 0.f, 255.f)));
                out.rgb.push_back(static_cast<uint8_t>(std::clamp(std::floor(colour.y * 127.5f), 0.f, 255.f)));
                out.rgb.push_back(static_cast<uint8_t>(std::clamp(std::floor(colour.z * 127.5f), 0.f, 255.f)));
            }
        }
        out.surface.push_back(static_cast<int32_t>(s));
        out.size.push_back(w);
        out.size.push_back(h);
        out.mapping.push_back(digits(lightMap.pan.x));
        out.mapping.push_back(digits(lightMap.pan.y));
        out.mapping.push_back(digits(lightMap.uScale));
        out.mapping.push_back(digits(lightMap.vScale));
        surfs++;
    }
    return surfs;
}

// every light, in order (the actors' light numbers count from 0); returns the actors in the same order
static std::vector<dx::UObject*> lightList(LightDump& out)
{
    std::vector<dx::UObject*> lights;
    if (!Level) {
        return lights;
    }
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed || !dx::actor::LightType(actor) || !dx::actor::LightBrightness(actor)) {
            continue;
        }
        dx::Vector colour = lightColour(dx::actor::LightHue(actor), dx::actor::LightSaturation(actor), dx::actor::LightBrightness(actor));
        dx::Vector direction = dx::rotationVector(dx::actor::Rotation(actor));
        dx::Vector location = dx::actor::Location(actor);
        push3(out.lights, decimals(location.x, 1), decimals(location.y, 1), decimals(location.z, 1));
        push3(out.lights, decimals(colour.x, 4), decimals(colour.y, 4), decimals(colour.z, 4));
        out.lights.push_back(decimals(25.f * (dx::actor::LightRadius(actor) + 1.f), 1));
        out.lights.push_back(static_cast<double>(dx::actor::LightEffect(actor)));
        push3(out.lights, decimals(direction.x, 4), decimals(direction.y, 4), decimals(direction.z, 4));
        out.lights.push_back(static_cast<double>(dx::actor::LightCone(actor)));
        lights.push_back(actor);
    }
    return lights;
}

// The light of the floor under an actor, read from the lightmap (the real shadows, lights not doubled).
static dx::Vector floorLight(dx::UModel& m, dx::UObject* a, const SurfaceLight& lit)
{
    dx::Vector location = dx::actor::Location(a);
    float x = location.x, y = location.y, top = location.z + 4.f;
    float low = location.z - std::max(dx::actor::CollisionHeight(a), 16.f) - 48.f, best = -1e9f;
    int bestNode = -1;
    for (size_t n = 0; n < m.nodes.size(); n++) {
        const dx::BspNode& node = m.nodes[n];
        if (node.surf < 0 || static_cast<size_t>(node.surf) >= lit.texelAt.size() || lit.texelAt[static_cast<size_t>(node.surf)] < 0) {
            continue;
        }
        if (std::fabs(node.plane.z) < 0.5f) {
            continue;
        }
        if (m.vectors[static_cast<size_t>(m.surfs[static_cast<size_t>(node.surf)].vNormal)].z < 0.5f) {
            continue;
        }
        float z = (node.plane.w - node.plane.x * x - node.plane.y * y) / node.plane.z;
        if (z > top || z < low || z <= best || !insideXy(m, node, x, y)) {
            continue;
        }
        best = z;
        bestNode = static_cast<int>(n);
    }
    if (bestNode < 0) {
        return dx::Vector { -1, -1, -1 };
    }
    int32_t s = m.nodes[static_cast<size_t>(bestNode)].surf;
    dx::BspSurf& surf = m.surfs[static_cast<size_t>(s)];
    const dx::LightMapIndex& lightMap = m.lightMaps[static_cast<size_t>(surf.lightMap)];
    int w = lightMap.uClamp, h = lightMap.vClamp;
    dx::Vector d = dx::Vector { x, y, best } - m.points[static_cast<size_t>(surf.pBase)];
    float u = std::clamp((d.dot(m.vectors[static_cast<size_t>(surf.vTextureU)]) - lightMap.pan.x) / lightMap.uScale, 0.f, static_cast<float>(w - 1));
    float v = std::clamp((d.dot(m.vectors[static_cast<size_t>(surf.vTextureV)]) - lightMap.pan.y) / lightMap.vScale, 0.f, static_cast<float>(h - 1));
    int i0 = std::min(static_cast<int>(std::floor(u)), w - 1), j0 = std::min(static_cast<int>(std::floor(v)), h - 1);
    int i1 = std::min(i0 + 1, w - 1), j1 = std::min(j0 + 1, h - 1);
    float fu = u - static_cast<float>(i0), fv = v - static_cast<float>(j0);
    const dx::Vector* t = &lit.texels[static_cast<size_t>(lit.texelAt[static_cast<size_t>(s)])];
    dx::Vector light = t[j0 * w + i0] * ((1 - fu) * (1 - fv)) + t[j0 * w + i1] * (fu * (1 - fv))
                     + t[j1 * w + i0] * ((1 - fu) * fv) + t[j1 * w + i1] * (fu * fv);
    return lit.ambients[static_cast<size_t>(s)] + light;
}

// Objects and doors: the lights that see their centre plus the ambient of their zone; the floor under them is
// the lower bound. Returns how many took their floor's light.
static int lightActors(dx::UModel& m, LightDump& out, const std::vector<dx::UObject*>& lights, const SurfaceLight& lit)
{
    int floors = 0;
    if (!Level) {
        return 0;
    }
    std::vector<dx::Vector> lightPositions;
    std::vector<LightInfo> lightInfos;
    for (dx::UObject* actor : lights) {
        LightInfo info;
        info.location = dx::actor::Location(actor);
        info.colour = lightColour(dx::actor::LightHue(actor), dx::actor::LightSaturation(actor), dx::actor::LightBrightness(actor));
        info.radius = 25.f * (dx::actor::LightRadius(actor) + 1.f);
        info.effect = dx::actor::LightEffect(actor);
        info.direction = dx::rotationVector(dx::actor::Rotation(actor));
        info.cone = dx::actor::LightCone(actor);
        lightInfos.push_back(info);
    }
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed) {
            continue;
        }
        dx::UMesh* mesh = dx::cast<dx::UMesh>(dx::actor::Mesh(actor));
        bool mover = dx::isMovingBrush(actor);
        if (!((dx::actor::DrawType(actor) == dx::DT_Mesh && mesh) || mover)) {
            continue;
        }
        dx::Vector location = dx::actor::Location(actor);
        int32_t zone = dx::csg::pointZone(m, location);
        dx::UObject* zoneActor = zone >= 0 && static_cast<size_t>(zone) < m.zones.size() ? m.zones[static_cast<size_t>(zone)].zoneActor : nullptr;
        dx::Vector zoneAmbient = ambientOf(zoneActor);
        dx::Vector colour = zoneAmbient;
        int seen = 0;
        for (size_t k = 0; k < lights.size(); k++) {
            const LightInfo& light = lightInfos[k];
            if (lights[k] != actor && (light.location - location).sizeSquared() < light.radius * light.radius
                && World.fastLineCheck(light.location, location)) {
                colour += lightAt(light, location, nullptr);
                out.actorLights.push_back(static_cast<int32_t>(k));
                seen++;
            }
        }
        dx::Vector occluded = colour, floor = floorLight(m, actor, lit);
        if (!(floor.x == -1 && floor.y == -1 && floor.z == -1) && floor.x + floor.y + floor.z > colour.x + colour.y + colour.z) {
            colour = floor;
            floors++;
        }
        dx::Vector extra = colour - occluded;
        dx::Vector ambient = zoneAmbient + dx::Vector { std::max(extra.x, 0.f), std::max(extra.y, 0.f), std::max(extra.z, 0.f) };
        out.actors.push_back(actor->name.str());
        push3(out.actorLight, decimals(colour.x, 3), decimals(colour.y, 3), decimals(colour.z, 3));
        push3(out.actorAmbient, decimals(ambient.x, 3), decimals(ambient.y, 3), decimals(ambient.z, 3));
        out.actorLightCount.push_back(seen);
    }
    return floors;
}

// UC_LIGHTDUMP <file>: the lighting of the map as the game draws it, for the map editor's "Lit" view.
static int dumpLights(const char* path)
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UModel& m = *Level->model;
    SurfaceLight lit;
    LightDump dump;
    int fit = 0, checked = 0;
    int surfs = lightSurfaces(m, dump, lit, fit, checked);
    std::vector<dx::UObject*> lights = lightList(dump);
    int floors = lightActors(m, dump, lights, lit);
    Arrays out("lights");
    out.header["actors"] = dump.actors;
    out.header["fit"] = fit;
    out.header["checked"] = checked;
    out.add("surface", dump.surface);
    out.add("size", dump.size);
    out.add("mapping", dump.mapping);
    out.add("rgb", dump.rgb);
    out.add("lights", dump.lights);
    out.add("actorLight", dump.actorLight);
    out.add("actorAmbient", dump.actorAmbient);
    out.add("actorLightCount", dump.actorLightCount);
    out.add("actorLights", dump.actorLights);
    if (!out.write(path)) {
        return 0;
    }
    say("UCMapKit|lights|%d surfaces|mapping %d/%d|%d objects lit by their floor", surfs, fit, checked, floors);
    return 1;
}

// ------------------------------------------------------------------------------------- meshes
// UC_MESHES <file>: the real look of every mesh actor and of every mover, for the 3D view.
struct MeshDump {
    TextureList textures;
    Json actors = Json::array();
    std::vector<int32_t> texture, slot;
    std::vector<uint32_t> flags;
    std::vector<double> corners;
    void triangle(dx::UObject* tex, uint32_t polyFlags, int32_t skin)
    {
        texture.push_back(textures(tex));
        flags.push_back(polyFlags);
        slot.push_back(skin);
    }
    void corner(const dx::Vector& p, float u, float v)
    {
        push3(corners, decimals(p.x, 2), decimals(p.y, 2), decimals(p.z, 2));
        corners.push_back(decimals(u, 4));
        corners.push_back(decimals(v, 4));
    }
};

// the texture a mesh face uses, as the engine chooses it (MultiSkins, then the mesh's own, then the actor's Skin)
static dx::UTexture* meshSkin(dx::UObject* actor, dx::UMesh* mesh, size_t index)
{
    dx::UObject* skin = index < 8 ? dx::actor::MultiSkins(actor, static_cast<int>(index)) : nullptr;
    if (skin) {
        return dx::cast<dx::UTexture>(skin);
    }
    dx::UObject* meshTexture = index < mesh->textures.size() ? mesh->textures[index] : nullptr;
    if (index != 0 && meshTexture) {
        return dx::cast<dx::UTexture>(meshTexture);
    }
    dx::UObject* actorSkin = dx::actor::Skin(actor);
    if (actorSkin) {
        return dx::cast<dx::UTexture>(actorSkin);
    }
    return dx::cast<dx::UTexture>(meshTexture);
}

// a mesh actor's triangles in its current frame, at the origin; -1 when the engine cannot pose it
static int meshTriangles(dx::UObject* actor, MeshDump& out)
{
    dx::UMesh* mesh = dx::cast<dx::UMesh>(dx::actor::Mesh(actor));
    if (!mesh) {
        return -1;
    }
    dx::objects::ensureLoaded(mesh);
    dx::objects::flushLoads();
    dx::MeshAnimState state;
    dx::UClass* cls = actor->cls;
    dx::UProperty* sequence = cls ? cls->findProperty(dx::Name("AnimSequence")) : nullptr;
    dx::UProperty* frame = cls ? cls->findProperty(dx::Name("AnimFrame")) : nullptr;
    if (sequence && propertyAddress(actor, sequence)) {
        state.sequence = *reinterpret_cast<dx::Name*>(propertyAddress(actor, sequence));
    }
    if (frame && propertyAddress(actor, frame)) {
        state.frame = *reinterpret_cast<float*>(propertyAddress(actor, frame));
    }
    dx::MeshPoseCaches caches;
    std::vector<dx::Vector> positions;
    int32_t count = 0;
    try {
        count = mesh->animatedPose(state, caches, positions, mesh->frameVerts);
    } catch (...) {
        return -1;
    }
    if (positions.empty()) {
        return -1;
    }
    std::vector<dx::MeshTriangle> triangles;
    try {
        triangles = mesh->triangles();
    } catch (...) {
        return -1;
    }
    for (const dx::MeshTriangle& triangle : triangles) {
        dx::UTexture* texture = meshSkin(actor, mesh, static_cast<size_t>(std::max(triangle.textureIndex, 0)));
        out.triangle(texture, triangle.polyFlags, triangle.textureIndex);
        for (int j = 0; j < 3; j++) {
            int32_t index = triangle.vertex[j];
            dx::Vector p = index >= 0 && index < static_cast<int32_t>(positions.size()) ? positions[static_cast<size_t>(index)] : dx::Vector {};
            out.corner(p, triangle.u[j] / 256.f, triangle.v[j] / 256.f);
        }
    }
    (void)count;
    return static_cast<int>(triangles.size());
}

// a mover's brush as triangles (fans of its polygons), relative to its pivot
static int moverTriangles(dx::UObject* actor, MeshDump& out)
{
    dx::UModel* model = brushModel(actor);
    if (!model || !model->polys) {
        return 0;
    }
    dx::Vector prePivot = dx::actor::PrePivot(actor);
    int n = 0;
    for (const dx::Poly& poly : model->polys->polys) {
        float tw = 128, th = 128;
        if (poly.texture) {
            tw = textureWorldWidth(poly.texture);
            th = textureWorldHeight(poly.texture);
        }
        for (size_t j = 1; j + 1 < poly.vertices.size(); j++) {
            size_t indices[3] = { 0, j, j + 1 };
            out.triangle(poly.texture, poly.polyFlags, -1);
            for (size_t q = 0; q < 3; q++) {
                dx::Vector v = poly.vertices[indices[q]];
                dx::Vector d = v - poly.base;
                out.corner(v - prePivot, (d.dot(poly.textureU) + poly.panU) / tw, (d.dot(poly.textureV) + poly.panV) / th);
            }
            n++;
        }
    }
    return n;
}

static int dumpMeshes(const char* path)
{
    if (!Level) {
        return 0;
    }
    MeshDump dump;
    int actors = 0, tris = 0;
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed) {
            continue;
        }
        int n;
        const char* kind;
        if (dx::actor::DrawType(actor) == dx::DT_Mesh && dx::actor::Mesh(actor)) {
            kind = "mesh";
            n = meshTriangles(actor, dump);
            if (n < 0) {
                say("UCMapKit|mesh fault|%s", actor->name.str().c_str());
                continue;
            }
        } else if (dx::isMovingBrush(actor) && brushModel(actor) && brushModel(actor)->polys) {
            kind = "mover";
            n = moverTriangles(actor, dump);
        } else {
            continue;
        }
        Json row;
        row["actor"] = actor->name.str();
        row["class"] = actor->cls->name.str();
        row["kind"] = kind;
        row["style"] = static_cast<int>(dx::actor::Style(actor));
        row["triangles"] = n;
        dump.actors.push_back(row);
        tris += n;
        actors++;
    }
    Arrays out("meshes");
    out.header["textures"] = dump.textures.names;
    out.header["actors"] = dump.actors;
    out.add("texture", dump.texture);
    out.add("flags", dump.flags);
    out.add("slot", dump.slot);
    out.add("corners", dump.corners);
    if (!out.write(path)) {
        return 0;
    }
    say("UCMapKit|meshes|%d actors|%d triangles", actors, tris);
    return 1;
}

// ------------------------------------------------------------------------------------- actors and properties
// UC_SET <name> x y z pitch yaw roll: place an actor or a brush (doors keep their base with them).
static int setActor(const char* name, float x, float y, float z, int pitch, int yaw, int roll)
{
    if (!Level) {
        return 0;
    }
    dx::UObject* actor = findActor(name);
    if (!actor) {
        say("UCMapKit|set|missing|%s", name);
        return 1;
    }
    dx::Rotator rotation;
    rotation.pitch = pitch;
    rotation.yaw = yaw;
    rotation.roll = roll;
    dx::actor::Location(actor) = dx::Vector { x, y, z };
    dx::actor::Rotation(actor) = rotation;
    if (dx::isMover(actor)) {
        dx::mover::BasePos(actor) = dx::Vector { x, y, z };
        dx::mover::BaseRot(actor) = rotation;
        dx::mover::OldPos(actor) = dx::Vector { x, y, z };
        dx::mover::SavedPos(actor) = dx::Vector { x, y, z };
    }
    say("UCMapKit|set|ok|%s", name);
    return 1;
}

// UC_DEL <name>: remove an actor or a brush.
static int deleteActor(const char* name)
{
    if (!Level) {
        return 0;
    }
    dx::UObject* actor = findActor(name);
    if (!actor) {
        say("UCMapKit|del|missing|%s", name);
        return 1;
    }
    Level->actors.erase(std::remove(Level->actors.begin(), Level->actors.end(), actor), Level->actors.end());
    World.actors = Level->actors;
    actor->destroyed = true;
    say("UCMapKit|del|ok|%s", name);
    return 1;
}

// UC_PROP <name or @Class or @@Class> <property>[(index)] <value...>: one property of an actor as text.
static int setPropOn(dx::UObject* actor, const char* name, const std::string& prop, const char* value)
{
    if (!actor) {
        say("UCMapKit|prop|missing|%s", name);
        return 1;
    }
    size_t paren = prop.find('(');
    std::string field = prop.substr(0, paren);
    int index = paren == std::string::npos ? 0 : atoi(prop.c_str() + paren + 1);
    dx::UProperty* property = intrinsicProperty(field) ? nullptr : propertyOf(actor, field.c_str());
    if (!property || index < 0 || index >= property->arrayDim) {
        say("UCMapKit|prop|noprop|%s|%s", name, field.c_str());
        return 1;
    }
    if (!setPropertyText(actor, property, index, value)) {
        say("UCMapKit|prop|failed|%s|%s", name, field.c_str());
        return 1;
    }
    say("UCMapKit|prop|ok|%s|%s", name, field.c_str());
    return 1;
}

static int setProp(const char* name, const char* prop, const char* value)
{
    if (!Level) {
        return 0;
    }
    if (name[0] == '@' && name[1] == '@') {             // "@@<Class>": every actor of that class
        dx::UClass* cls = classNamed(name + 2);
        int n = 0;
        if (cls) {
            for (dx::UObject* actor : Level->actors) {
                if (actor && !actor->destroyed && actor->isA(cls)) {
                    setPropOn(actor, name, prop, value);
                    n++;
                }
            }
        }
        say("UCMapKit|prop|all|%s|%d", name + 2, n);
        return 1;
    }
    return setPropOn(findActor(name), name, prop, value);
}

// UC_OBJPROP <object> <property> <value...>: a property of any object found by name.
static int setObjProp(const char* name, const char* prop, const char* value)
{
    dx::UObject* object = dx::objects::findObject(name, nullptr);
    if (!object) {
        say("UCMapKit|objprop|missing|%s", name);
        return 1;
    }
    dx::UProperty* property = intrinsicProperty(prop) ? nullptr : propertyOf(object, prop);
    if (!property) {
        say("UCMapKit|objprop|noprop|%s|%s", name, prop);
        return 1;
    }
    if (!setPropertyText(object, property, 0, value)) {
        say("UCMapKit|objprop|failed|%s|%s", name, prop);
        return 1;
    }
    say("UCMapKit|objprop|ok|%s|%s", name, prop);
    return 1;
}

// UC_SCALE <name> <factor>: an object made bigger or smaller: DrawScale, collision cylinder and pivot.
static int scaleActor(const char* name, float factor)
{
    if (!(factor > 0.01f && factor < 100.0f)) {
        return 0;
    }
    if (!Level) {
        return 0;
    }
    dx::UObject* actor = findActor(name);
    if (!actor) {
        say("UCMapKit|scale|missing|%s", name);
        return 1;
    }
    dx::actor::DrawScale(actor) *= factor;
    dx::actor::CollisionRadius(actor) *= factor;
    dx::actor::CollisionHeight(actor) *= factor;
    dx::actor::PrePivot(actor) = dx::actor::PrePivot(actor) * factor;
    say("UCMapKit|scale|ok|%s|%.3f|%.2f|%.2f", name, static_cast<double>(dx::actor::DrawScale(actor)),
        static_cast<double>(dx::actor::CollisionRadius(actor)), static_cast<double>(dx::actor::CollisionHeight(actor)));
    return 1;
}

// UC_MOVER <bounds|prep> <name> x y z
static int editMover(const char* operation, const char* name, float x, float y, float z)
{
    (void)x;
    (void)y;
    (void)z;
    if (!Level) {
        return 0;
    }
    dx::UObject* actor = findActor(name);
    dx::UModel* model = actor ? brushModel(actor) : nullptr;
    if (!model || !model->polys) {
        return 0;
    }
    if (!strcmp(operation, "bounds")) {
        dx::bsp::buildBounds(*model);
        dx::bsp::buildBound(*model);
    } else if (!strcmp(operation, "prep")) {
        dx::bsp::prepMovingBrush(actor);
        dx::bsp::buildBound(*model);
    } else {
        return 0;
    }
    say("UCMapKit|mover|%s|%s|%d", name, operation, static_cast<int>(model->polys->polys.size()));
    return 1;
}

// ------------------------------------------------------------------------------------- surfaces
// retexture the faces of one brush (its polys and the BSP surfaces it made) without a rebuild
static int retextureBrush(dx::UObject* actor, const char* oldTexture, dx::UObject* texture)
{
    bool all = !strcmp(oldTexture, "*");
    dx::UModel* model = brushModel(actor);
    int n = 0;
    if (model && model->polys) {
        for (dx::Poly& poly : model->polys->polys) {
            if (poly.texture && (all || sameName(poly.texture->name.str(), oldTexture))) {
                poly.texture = texture;
                n++;
            }
        }
    }
    if (Level && Level->model) {
        for (dx::BspSurf& surf : Level->model->surfs) {
            if (surf.actor != actor || !surf.texture) {
                continue;
            }
            if (all || sameName(surf.texture->name.str(), oldTexture)) {
                surf.texture = texture;
                n++;
            }
        }
    }
    return n;
}

static int setSurfTexture(const char* name, const char* oldTexture, const char* newTexture)
{
    if (!Level || !Level->model) {
        return 0;
    }
    dx::UObject* texture = dx::objects::findObject(newTexture, dx::objects::findClass("Engine.Texture"));
    if (!texture) {
        say("UCMapKit|surftex|notexture|%s", newTexture);
        return 1;
    }
    int n = 0;
    if (!strcmp(name, "*")) {                           // every brush of the map (and every mover)
        for (dx::UObject* actor : Level->actors) {
            if (actor && !actor->destroyed && brushModel(actor) && actor != builderBrush()
                && (isStaticBrush(actor) || dx::isMover(actor))) {
                n += retextureBrush(actor, oldTexture, texture);
            }
        }
    } else {
        dx::UObject* actor = findActor(name);
        if (!actor || !brushModel(actor)) {
            say("UCMapKit|surftex|missing|%s", name);
            return 1;
        }
        n = retextureBrush(actor, oldTexture, texture);
    }
    say("UCMapKit|surftex|ok|%s|%d", name, n);
    return 1;
}

// A surface edit addresses the source brush polygon (BSP indices change at every rebuild): work in world
// space, then transform only the mapping back into the brush.
static int surfaceError(const char* reason)
{
    say("UCMapKit|failed|surface|%s", reason);
    return 1;
}

static void storeSurface(dx::UObject* brush, int face, dx::Poly poly, bool mapping, bool texture, uint32_t mask)
{
    dx::UModel* model = brushModel(brush);
    dx::bsp::ModelCoords uncoords;
    float orientation = dx::bsp::buildCoords(brush, nullptr, &uncoords);
    dx::Poly local = poly;
    dx::bsp::transformPoly(local, uncoords, dx::actor::Location(brush), dx::actor::PrePivot(brush), orientation);
    if (model && model->polys) {
        for (size_t i = 0; i < model->polys->polys.size(); i++) {
            dx::Poly& dst = model->polys->polys[i];
            if (static_cast<int>(i) != face && dst.link != face) {
                continue;
            }
            if (mapping) {
                dst.base = local.base;
                dst.textureU = local.textureU;
                dst.textureV = local.textureV;
                dst.panU = local.panU;
                dst.panV = local.panV;
            }
            if (texture) {
                dst.texture = poly.texture;
            }
            if (mask) {
                dst.polyFlags = (dst.polyFlags & ~mask) | (poly.polyFlags & mask);
            }
        }
    }
    if (!Level || !Level->model) {
        return;
    }
    dx::UModel& master = *Level->model;
    for (dx::BspSurf& surf : master.surfs) {
        if (surf.actor != brush || surf.brushPoly != face) {
            continue;
        }
        if (mapping) {
            // vectors and points may be shared by unrelated faces: allocate, do not overwrite them
            surf.pBase = static_cast<int32_t>(master.points.size());
            master.points.push_back(poly.base);
            surf.vTextureU = static_cast<int32_t>(master.vectors.size());
            master.vectors.push_back(poly.textureU);
            surf.vTextureV = static_cast<int32_t>(master.vectors.size());
            master.vectors.push_back(poly.textureV);
            surf.panU = poly.panU;
            surf.panV = poly.panV;
        }
        if (texture) {
            surf.texture = poly.texture;
        }
        if (mask) {
            surf.polyFlags = (surf.polyFlags & ~mask) | (poly.polyFlags & mask);
        }
    }
}

// Fit a face's texture to one tile over its extent (UnrealEd's "Fit")
static bool fitSurface(dx::Poly& poly, const std::string& mode)
{
    dx::Vector n = poly.normal.normal();
    dx::Vector u = (poly.textureU - n * poly.textureU.dot(n)).normal();
    if (u.sizeSquared() < .5f) {
        u = (poly.vertices[1] - poly.vertices[0]).normal();
    }
    dx::Vector v = (poly.textureV - n * poly.textureV.dot(n) - u * poly.textureV.dot(u)).normal();
    if (v.sizeSquared() < .5f) {
        v = u.cross(n);
    }
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    dx::Vector base = poly.vertices[0];
    for (const dx::Vector& vertex : poly.vertices) {
        dx::Vector d = vertex - base;
        float x = d.dot(u), y = d.dot(v);
        u0 = std::min(u0, x);
        u1 = std::max(u1, x);
        v0 = std::min(v0, y);
        v1 = std::max(v1, y);
    }
    if (u1 - u0 < .001f || v1 - v0 < .001f || !poly.texture) {
        return false;
    }
    float su = textureWorldWidth(poly.texture) / (u1 - u0), sv = textureWorldHeight(poly.texture) / (v1 - v0);
    if (mode == "tile-u") {
        sv = su;
    }
    if (mode == "tile-v") {
        su = sv;
    }
    poly.base = base + u * u0 + v * v0;
    poly.textureU = u * su;
    poly.textureV = v * sv;
    poly.panU = poly.panV = 0;
    return true;
}

// One continuous tile around a closed, vertical polygonal cylinder (as UnrealEd's cylinder mapping).
static int wrapSurfaceCylinder(dx::UObject* brush)
{
    std::vector<dx::Poly> polys = dx::brushedit::worldPolys(brush);
    std::vector<int> faces;
    std::vector<dx::Vector> starts, ends, axes;
    std::vector<float> widths;
    dx::UObject* texture = nullptr;
    float perimeter = 0, top = -1e30f;
    for (size_t i = 0; i < polys.size(); i++) {
        dx::Poly& poly = polys[i];
        if (poly.vertices.size() < 3 || std::fabs(poly.normal.z) > .001f) {
            continue;
        }
        if (!poly.texture || (texture && poly.texture != texture)) {
            return surfaceError("Cylinder sides must use the same texture");
        }
        texture = poly.texture;
        dx::Vector u = poly.normal.cross(dx::Vector { 0, 0, 1 }).normal();
        float lo = 1e30f, hi = -1e30f;
        dx::Vector start, end;
        for (const dx::Vector& vertex : poly.vertices) {
            float d = vertex.dot(u);
            if (d < lo) {
                lo = d;
                start = vertex;
            }
            if (d > hi) {
                hi = d;
                end = vertex;
            }
            top = std::max(top, vertex.z);
        }
        start.z = end.z = 0;
        if (hi - lo < .001f) {
            return surfaceError("Degenerate cylinder side");
        }
        faces.push_back(static_cast<int>(i));
        starts.push_back(start);
        ends.push_back(end);
        axes.push_back(u);
        widths.push_back(hi - lo);
        perimeter += hi - lo;
    }
    if (faces.size() < 3) {
        return surfaceError("Select a vertical cylinder with at least three side faces");
    }
    std::vector<int> order;
    int current = 0;
    for (size_t k = 0; k < faces.size(); k++) {
        for (int already : order) {
            if (already == current) {
                return surfaceError("Cylinder sides do not form one closed outline");
            }
        }
        order.push_back(current);
        int next = -1;
        for (size_t j = 0; j < faces.size(); j++) {
            if ((ends[static_cast<size_t>(current)] - starts[j]).sizeSquared() < .01f) {
                if (next != -1) {
                    return surfaceError("Cylinder outline branches");
                }
                next = static_cast<int>(j);
            }
        }
        if (next < 0) {
            return surfaceError("Cylinder sides do not meet");
        }
        current = next;
    }
    if (current != 0) {
        return surfaceError("Cylinder outline is not closed");
    }
    float scale = textureWorldWidth(texture) / perimeter, cumulative = 0;
    for (int j : order) {
        dx::Poly poly = polys[static_cast<size_t>(faces[static_cast<size_t>(j)])];
        poly.base = starts[static_cast<size_t>(j)] - axes[static_cast<size_t>(j)] * cumulative;
        poly.base.z = top;
        poly.textureU = axes[static_cast<size_t>(j)] * scale;
        poly.textureV = dx::Vector { 0, 0, -scale };
        poly.panU = poly.panV = 0;
        storeSurface(brush, faces[static_cast<size_t>(j)], poly, true, false, 0);
        cumulative += widths[static_cast<size_t>(j)];
    }
    say("UCMapKit|surface|ok|cylinder|%d", static_cast<int>(faces.size()));
    return 1;
}

// UC_SURFACE <brush> <source polygon or *> <mode> [args]
static int editSurface(const UCSurfaceEdit* edit)
{
    float x = 0, y = 0;
    if (!edit) {
        return 0;
    }
    std::string mode = edit->mode ? edit->mode : "";
    std::string argument = edit->text ? edit->text : "";
    dx::UObject* actor = findActor(edit->brush);
    dx::UModel* model = actor ? brushModel(actor) : nullptr;
    if (!model || !model->polys || !isStaticBrush(actor)) {
        return surfaceError("Source brush is missing; select its faces again");
    }
    bool all = !strcmp(edit->target, "*");
    int face = atoi(edit->target);
    int count = static_cast<int>(model->polys->polys.size());
    if (!all && (face < 0 || face >= count)) {
        return surfaceError("Source polygon is missing; select its faces again");
    }
    if (mode == "cylinder") {
        return wrapSurfaceCylinder(actor);
    }
    bool shadow = mode == "shadows", texture = mode == "texture", pan = mode == "pan", scale = mode == "scale";
    bool fit = mode == "tile" || mode == "tile-u" || mode == "tile-v";
    bool floor = mode == "floor", wall = mode == "wall", rotate = mode == "rotate";
    bool flipU = mode == "flip-u", flipV = mode == "flip-v", flags = mode == "flags";
    if (!(shadow || texture || pan || scale || fit || floor || wall || rotate || flipU || flipV || flags)) {
        return surfaceError("Unknown surface operation");
    }
    const uint32_t allowed = dx::bsp::PF_Invisible | dx::bsp::PF_Masked | PF_Translucent | PF_Modulated
        | dx::bsp::PF_FakeBackdrop | dx::bsp::PF_TwoSided | dx::bsp::PF_NoMerge | dx::bsp::PF_SpecialLit
        | dx::bsp::PF_Unlit | dx::bsp::PF_LowShadowDetail | dx::bsp::PF_HighShadowDetail | dx::bsp::PF_BrightCorners;
    uint32_t set = static_cast<uint32_t>(edit->set), clear = static_cast<uint32_t>(edit->clear);
    if (flags && (!edit->hasFlags || ((set | clear) & ~allowed) || (set & clear))) {
        return surfaceError("Invalid surface flags");
    }
    if (pan || scale) {
        if (!edit->hasXY) {
            return surfaceError("Invalid U/V values");
        }
        x = edit->x;
        y = edit->y;
        if (scale && (x < .001f || y < .001f || x > 1000 || y > 1000)) {
            return surfaceError("Texture scale must be 0.001 to 1000");
        }
    }
    if (shadow && argument != "ultra-low" && argument != "low" && argument != "normal" && argument != "high") {
        return surfaceError("Invalid shadow detail");
    }
    dx::UObject* newTexture = nullptr;
    if (texture) {
        newTexture = dx::objects::findObject(argument, dx::objects::findClass("Engine.Texture"));
        if (!newTexture) {
            return surfaceError("Texture is not loaded");
        }
    }
    std::vector<dx::Poly> polys = dx::brushedit::worldPolys(actor);
    int changed = 0;
    for (int i = 0; i < count; i++) {
        if (!all && i != face) {
            continue;
        }
        dx::Poly p = polys[static_cast<size_t>(i)];
        if (p.vertices.size() < 3) {
            return surfaceError("Invalid source polygon");
        }
        if (texture) {
            p.texture = newTexture;
        } else if (shadow) {
            p.polyFlags = (p.polyFlags & ~(dx::bsp::PF_LowShadowDetail | dx::bsp::PF_HighShadowDetail))
                | (argument == "ultra-low" ? (dx::bsp::PF_LowShadowDetail | dx::bsp::PF_HighShadowDetail)
                       : argument == "low"    ? dx::bsp::PF_LowShadowDetail
                       : argument == "high"   ? dx::bsp::PF_HighShadowDetail
                                              : 0u);
        } else if (fit) {
            if (!fitSurface(p, mode)) {
                return surfaceError("Cannot fit this face: degenerate UVs or no texture");
            }
        } else if (floor || wall) {
            dx::Vector n = p.normal.normal(), u;
            if (wall && std::fabs(n.z) < .999f) {
                u = n.cross(dx::Vector { 0, 0, 1 }).normal();
            } else {
                u = (dx::Vector { 1, 0, 0 } - n * n.x).normal();
            }
            if (u.sizeSquared() < .5f) {
                u = (dx::Vector { 0, 1, 0 } - n * n.y).normal();
            }
            p.textureU = u;
            p.textureV = wall ? n.cross(u) : u.cross(n);
            p.base = n * p.vertices[0].dot(n);
            p.panU = p.panV = 0;
        } else if (pan) {
            if (std::fabs(p.panU + x) > 32767 || std::fabs(p.panV + y) > 32767) {
                return surfaceError("Pan exceeds the engine's signed 16-bit range");
            }
            p.panU = static_cast<int16_t>(p.panU + static_cast<int>(x));
            p.panV = static_cast<int16_t>(p.panV + static_cast<int>(y));
        } else if (scale) {
            p.textureU = p.textureU / x;
            p.textureV = p.textureV / y;
        } else if (rotate) {
            dx::Vector u = p.textureU;
            int16_t panU = p.panU;
            p.textureU = p.textureV;
            p.textureV = -u;
            p.panU = p.panV;
            p.panV = static_cast<int16_t>(-panU);
        } else if (flipU) {
            p.textureU = -p.textureU;
            p.panU = static_cast<int16_t>(-p.panU);
        } else if (flipV) {
            p.textureV = -p.textureV;
            p.panV = static_cast<int16_t>(-p.panV);
        } else if (flags) {
            p.polyFlags = (p.polyFlags | set) & ~clear;
        }
        storeSurface(actor, i, p, !shadow && !texture && !flags, texture,
            shadow ? (dx::bsp::PF_LowShadowDetail | dx::bsp::PF_HighShadowDetail) : flags ? (set | clear) : 0);
        changed++;
    }
    say("UCMapKit|surface|ok|%s|%d", mode.c_str(), changed);
    return 1;
}

// ------------------------------------------------------------------------------------- AI paths
// UC_PATHS <file>: the AI's path network as PATHS DEFINE made it.
static int dumpPaths(const char* path)
{
    if (!Level) {
        return 0;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int nodes = 0, specs = 0;
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed) {
            continue;
        }
        if (!dx::engineClasses().navigationPoint || !actor->isA(dx::engineClasses().navigationPoint)) {
            continue;
        }
        Json row;
        row["kind"] = "point";
        row["name"] = actor->name.str();
        row["class"] = actor->cls->name.str();
        row["location"] = xyz(dx::actor::Location(actor), 1);
        out.write(row);
        nodes++;
    }
    for (const dx::ReachSpec& spec : Level->reachSpecs) {
        if (spec.pruned || !spec.start || !spec.end) {
            continue;
        }
        Json row;
        row["kind"] = "link";
        row["start"] = spec.start->name.str();
        row["end"] = spec.end->name.str();
        row["distance"] = spec.distance;
        row["flags"] = spec.reachFlags;
        row["radius"] = spec.collisionRadius;
        row["height"] = spec.collisionHeight;
        out.write(row);
        specs++;
    }
    say("UCMapKit|paths|%d points|%d links", nodes, specs);
    return 1;
}

// The bridge's navigation settings ([Navigation] of the SDK bridge's ini, built in here)
struct NavigationSettings {
    float floorSearch = 512.f;
    float floorTolerance = 2.f;
    float maxHeightOverFloor = 64.f;
    float movedTolerance = 1.f;
    std::vector<std::string> leftAloneClasses = { "Engine.PlayerStart", "Engine.Teleporter", "Engine.LiftCenter",
        "Engine.LiftExit", "Engine.InventorySpot", "Engine.WarpZoneMarker", "Engine.ButtonMarker", "Engine.TriggerMarker" };
};
static NavigationSettings Navigation;

static bool isLeftAlone(dx::UObject* actor)
{
    for (const std::string& name : Navigation.leftAloneClasses) {
        dx::UClass* cls = classNamed(name.c_str());
        if (cls && actor->isA(cls)) {
            return true;
        }
    }
    return false;
}

// UC_NAVDROP <* | name name ...>: navigation points rested on the floor below them.
static int dropNavPoints(const char* const* list, int count)
{
    if (count < 1 || !Level) {
        return 0;
    }
    bool all = !strcmp(list[0], "*");
    std::unordered_set<std::string> names;
    for (int k = 0; k < count; k++) {
        names.insert(list[k]);
    }
    int moved = 0, checked = 0;
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed) {
            continue;
        }
        if (!dx::engineClasses().navigationPoint || !actor->isA(dx::engineClasses().navigationPoint)) {
            continue;
        }
        if (isLeftAlone(actor)) {
            continue;
        }
        if (!all) {
            bool wanted = false;
            for (const std::string& name : names) {
                if (sameName(name, actor->name.str())) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) {
                continue;
            }
        }
        checked++;
        dx::Vector location = dx::actor::Location(actor);
        dx::CheckResult hit;
        if (!World.bspLineCheck(hit, location, location - dx::Vector { 0, 0, Navigation.floorSearch }, dx::Vector { 0, 0, 0 })) {
            continue;
        }
        float z = hit.location.z + dx::actor::CollisionHeight(actor);
        if (std::fabs(location.z - z) < Navigation.floorTolerance) {
            continue;
        }
        say("UCMapKit|navdrop|%s|%.1f -> %.1f", actor->name.str().c_str(), static_cast<double>(location.z), static_cast<double>(z));
        dx::actor::Location(actor).z = z;
        moved++;
    }
    say("UCMapKit|navdrop|%d checked|%d moved", checked, moved);
    return 1;
}

struct NavHeight {
    std::string name;
    float z;
};
static std::vector<NavHeight> navTemp;

// UC_NAVDROP temp: every walking point rested on the floor for PATHS DEFINE only.
static int dropNavTemp()
{
    if (!Level || !Level->model) {
        return 0;
    }
    navTemp.clear();
    int checked = 0;
    for (dx::UObject* actor : Level->actors) {
        if (!actor || actor->destroyed) {
            continue;
        }
        if (!dx::engineClasses().navigationPoint || !actor->isA(dx::engineClasses().navigationPoint)) {
            continue;
        }
        if (isLeftAlone(actor)) {
            continue;
        }
        checked++;
        dx::Vector location = dx::actor::Location(actor);
        int32_t zone = dx::csg::pointZone(*Level->model, location);
        dx::UObject* zoneActor = zone >= 0 && static_cast<size_t>(zone) < Level->model->zones.size()
            ? Level->model->zones[static_cast<size_t>(zone)].zoneActor : nullptr;
        if (zoneActor) {
            dx::UProperty* water = propertyOf(zoneActor, "bWaterZone");
            if (water && *reinterpret_cast<uint32_t*>(propertyAddress(zoneActor, water))) {
                say("UCMapKit|navdrop temp|%s|water", actor->name.str().c_str());
                continue;
            }
        }
        dx::CheckResult hit;
        if (!World.bspLineCheck(hit, location, location - dx::Vector { 0, 0, Navigation.floorSearch }, dx::Vector { 0, 0, 0 })) {
            say("UCMapKit|navdrop temp|%s|no floor", actor->name.str().c_str());
            continue;
        }
        float z = hit.location.z + dx::actor::CollisionHeight(actor);
        if (std::fabs(location.z - z) < Navigation.floorTolerance || z > location.z) {
            continue;
        }
        if (location.z - z > Navigation.maxHeightOverFloor) {
            say("UCMapKit|navdrop temp|%s|%.1f over the floor: kept", actor->name.str().c_str(), static_cast<double>(location.z - z));
            continue;
        }
        say("UCMapKit|navdrop temp|%s|%.1f -> %.1f", actor->name.str().c_str(), static_cast<double>(location.z), static_cast<double>(z));
        navTemp.push_back(NavHeight { actor->name.str(), location.z });
        dx::actor::Location(actor).z = z;
    }
    say("UCMapKit|navdrop temp|%d checked|%d moved", checked, static_cast<int>(navTemp.size()));
    return 1;
}

static int liftNavTemp()
{
    if (!Level) {
        return 0;
    }
    int back = 0;
    for (const NavHeight& kept : navTemp) {
        dx::UObject* actor = findActor(kept.name.c_str());
        if (!actor) {
            continue;
        }
        dx::actor::Location(actor).z = kept.z;
        back++;
    }
    say("UCMapKit|navlift|%d of %d back", back, static_cast<int>(navTemp.size()));
    navTemp.clear();
    return 1;
}

// UC_KEEPPATHS save / restore: the map's own reach specs that PATHS DEFINE does not make again are put back.
struct KeptSpec {
    std::string a, b;
    dx::Vector la, lb;
    int32_t dist = 0, rad = 0, hgt = 0, flags = 0;
};
static std::vector<KeptSpec> keptSpecs;

static int* navPaths(dx::UObject* actor, const char* property, int index)
{
    dx::UProperty* p = propertyOf(actor, property);
    uint8_t* data = propertyAddress(actor, p, index);
    return data ? reinterpret_cast<int*>(data) : nullptr;
}

static int keepPaths(int restore)
{
    if (!Level) {
        return 0;
    }
    if (!restore) {
        keptSpecs.clear();
        for (const dx::ReachSpec& spec : Level->reachSpecs) {
            if (spec.pruned || !spec.start || !spec.end) {
                continue;
            }
            if (!dx::engineClasses().navigationPoint || !spec.start->isA(dx::engineClasses().navigationPoint)
                || !spec.end->isA(dx::engineClasses().navigationPoint)) {
                continue;
            }
            KeptSpec kept;
            kept.a = spec.start->name.str();
            kept.b = spec.end->name.str();
            kept.la = dx::actor::Location(spec.start);
            kept.lb = dx::actor::Location(spec.end);
            kept.dist = spec.distance;
            kept.rad = spec.collisionRadius;
            kept.hgt = spec.collisionHeight;
            kept.flags = spec.reachFlags;
            keptSpecs.push_back(kept);
        }
        say("UCMapKit|keeppaths|%d saved", static_cast<int>(keptSpecs.size()));
        return 1;
    }
    int back = 0, there = 0, moved = 0, blocked = 0, full = 0;
    for (const KeptSpec& kept : keptSpecs) {
        dx::UObject* a = findActor(kept.a.c_str());
        dx::UObject* b = findActor(kept.b.c_str());
        if (!a || !b || (dx::actor::Location(a) - kept.la).size() > Navigation.movedTolerance
            || (dx::actor::Location(b) - kept.lb).size() > Navigation.movedTolerance) {
            moved++;
            continue;
        }
        int linked = 0;
        for (int j = 0; j < 16 && !linked; j++) {
            int* p = navPaths(a, "Paths", j);
            int* q = navPaths(a, "PrunedPaths", j);
            if (p && *p >= 0 && static_cast<size_t>(*p) < Level->reachSpecs.size() && Level->reachSpecs[static_cast<size_t>(*p)].end == b) {
                linked = 1;
            }
            if (q && *q >= 0 && static_cast<size_t>(*q) < Level->reachSpecs.size() && Level->reachSpecs[static_cast<size_t>(*q)].end == b) {
                linked = 1;
            }
        }
        if (linked) {
            there++;
            continue;
        }
        bool clear = false;
        for (float dz = 16.f; dz <= 48.f && !clear; dz += 16.f) {
            dx::CheckResult hit;
            if (!World.bspLineCheck(hit, dx::actor::Location(a) + dx::Vector { 0, 0, dz }, dx::actor::Location(b) + dx::Vector { 0, 0, dz }, dx::Vector { 0, 0, 0 })) {
                clear = true;
            }
        }
        if (!clear) {
            blocked++;
            continue;
        }
        int pa = -1, pb = -1;
        for (int j = 0; j < 16; j++) {
            int* p = navPaths(a, "Paths", j);
            int* q = navPaths(b, "upstreamPaths", j);
            if (pa < 0 && p && *p == -1) {
                pa = j;
            }
            if (pb < 0 && q && *q == -1) {
                pb = j;
            }
        }
        if (pa < 0 || pb < 0) {
            full++;
            continue;
        }
        dx::ReachSpec spec;
        spec.start = a;
        spec.end = b;
        spec.distance = kept.dist;
        spec.collisionRadius = kept.rad;
        spec.collisionHeight = kept.hgt;
        spec.reachFlags = kept.flags;
        int index = static_cast<int>(Level->reachSpecs.size());
        Level->reachSpecs.push_back(spec);
        *navPaths(a, "Paths", pa) = index;
        *navPaths(b, "upstreamPaths", pb) = index;
        back++;
        say("UCMapKit|keeppaths|back|%s|%s", kept.a.c_str(), kept.b.c_str());
    }
    say("UCMapKit|keeppaths|%d of %d put back|%d made again|%d moved or gone|%d blocked|%d no room", back,
        static_cast<int>(keptSpecs.size()), there, moved, blocked, full);
    return 1;
}

// ------------------------------------------------------------------------------------- game content
// The engine class name of a property, as the editor's exporters write it ("IntProperty"...)
static const char* propertyTypeName(dx::PropType type)
{
    switch (type) {
    case dx::PropType::Byte: return "ByteProperty";
    case dx::PropType::Int: return "IntProperty";
    case dx::PropType::Bool: return "BoolProperty";
    case dx::PropType::Float: return "FloatProperty";
    case dx::PropType::Object: return "ObjectProperty";
    case dx::PropType::Class: return "ClassProperty";
    case dx::PropType::Name: return "NameProperty";
    case dx::PropType::Str: return "StrProperty";
    case dx::PropType::String: return "StringProperty";
    case dx::PropType::Struct: return "StructProperty";
    case dx::PropType::Array: return "ArrayProperty";
    case dx::PropType::Map: return "MapProperty";
    case dx::PropType::FixedArray: return "ArrayProperty";
    case dx::PropType::Pointer: return "PointerProperty";
    }
    return "Property";
}

static Json enumRow(const char* property, dx::UEnum* e)
{
    Json row, values = Json::array();
    for (const dx::Name& name : e->names) {
        values.push_back(name.str());
    }
    row["kind"] = "enum";
    row["property"] = property;
    row["values"] = values;
    return row;
}

// a class's parents, nearest first, up to (not including) the class stop (nullptr: all of them)
static Json parentsOf(dx::UClass* cls, dx::UClass* stop)
{
    Json parents = Json::array();
    for (dx::UClass* s = cls->superClass(); s && s != stop; s = s->superClass()) {
        parents.push_back(s->name.str());
    }
    return parents;
}

// UC_CLASSES <file>: every actor class loaded (for the editor palette), a row each.
static int listClasses(const char* path)
{
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int n = 0;
    for (dx::UObject* object : dx::objects::all()) {
        dx::UClass* cls = dx::cast<dx::UClass>(object);
        if (!cls || object->destroyed || !actorClass || !cls->isChildOf(actorClass)) {
            continue;
        }
        dx::UObject* defaults = cls->defaultObject;
        if (!defaults || !defaults->data) {
            continue;
        }
        Json row;
        row["class"] = cls->name.str();
        row["package"] = cls->outer ? cls->outer->name.str() : std::string();
        row["flags"] = cls->classFlags;
        row["drawType"] = static_cast<int>(dx::actor::DrawType(defaults));
        dx::UObject* mesh = dx::actor::Mesh(defaults);
        row["mesh"] = mesh ? mesh->pathName() : std::string("None");
        row["radius"] = decimals(dx::actor::CollisionRadius(defaults), 2);
        row["height"] = decimals(dx::actor::CollisionHeight(defaults), 2);
        row["parents"] = parentsOf(cls, actorClass);
        row["itemName"] = propertyText(defaults, "ItemName");
        row["familiarName"] = propertyText(defaults, "FamiliarName");
        row["drawScale"] = decimals(dx::actor::DrawScale(defaults), 4);
        out.write(row);
        n++;
    }
    say("UCMapKit|classes|%d", n);
    return 1;
}

// UC_DEFAULTS <file> <prop,prop,...>: the class defaults of those properties for every actor class.
static int dumpDefaults(const char* path, const char* const* names, int count)
{
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    std::vector<std::string> props(names, names + count);
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    std::vector<char> enumsDone(props.size(), 0);
    int n = 0;
    for (dx::UObject* object : dx::objects::all()) {
        dx::UClass* cls = dx::cast<dx::UClass>(object);
        if (!cls || object->destroyed || !actorClass || !cls->isChildOf(actorClass)) {
            continue;
        }
        dx::UObject* defaults = cls->defaultObject;
        if (!defaults || !defaults->data) {
            continue;
        }
        Json values = Json::array();
        for (size_t k = 0; k < props.size(); k++) {
            if (intrinsicProperty(props[k])) {
                continue;
            }
            dx::UProperty* property = propertyOf(defaults, props[k].c_str());
            if (!property) {
                continue;
            }
            if (!enumsDone[k] && property->type == dx::PropType::Byte && property->enumType) {
                out.write(enumRow(props[k].c_str(), property->enumType));
                enumsDone[k] = 1;
            }
            int dim = property->arrayDim > 8 ? 8 : property->arrayDim;
            for (int i = 0; i < dim; i++) {
                uint8_t* data = propertyAddress(defaults, property, i);
                if (!data) {
                    continue;
                }
                Json one;
                one["name"] = props[k];
                one["index"] = i;
                one["type"] = propertyTypeName(property->type);
                one["value"] = property->toText(data);
                values.push_back(one);
            }
        }
        Json row;
        row["kind"] = "class";
        row["class"] = cls->name.str();
        row["parents"] = parentsOf(cls, nullptr);
        row["properties"] = values;
        out.write(row);
        n++;
    }
    say("UCMapKit|defaults|%d", n);
    return 1;
}

// UC_CLASSPROPS <file> <class>: every property UnrealEd's Properties window shows (CPF_Edit).
static int dumpClassProps(const char* path, const char* className)
{
    dx::UClass* cls = classNamed(className);
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    if (!cls || !actorClass || !cls->isChildOf(actorClass)) {
        say("UCMapKit|classprops|missing|%s", className);
        return 1;
    }
    dx::UObject* defaults = cls->defaultObject;
    if (!defaults || !defaults->data) {
        return 0;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int n = 0;
    for (dx::UProperty* property : cls->fieldOrder()) {
        if (!(property->propertyFlags & dx::CPF_Edit) || intrinsicProperty(property->name.str())) {
            continue;
        }
        std::string name = property->name.str();
        if (property->type == dx::PropType::Byte && property->enumType) {
            out.write(enumRow(name.c_str(), property->enumType));
        }
        int dim = property->arrayDim > 32 ? 32 : property->arrayDim;
        for (int i = 0; i < dim; i++) {
            uint8_t* data = propertyAddress(defaults, property, i);
            if (!data) {
                continue;
            }
            Json row;
            row["kind"] = "property";
            row["name"] = name;
            row["index"] = property->arrayDim > 1 ? i : -1;
            row["type"] = propertyTypeName(property->type);
            row["category"] = property->category.str();
            row["default"] = property->toText(data);
            out.write(row);
        }
        n++;
    }
    say("UCMapKit|classprops|%d", n);
    return 1;
}

// UC_TEXDUMP <texture path> <file> [largest side]: a texture as raw RGBA.
static int dumpTexture(const char* name, const char* path, int maxSize)
{
    dx::UObject* object = dx::objects::loadObject(name, dx::objects::findClass("Engine.Texture"));
    dx::UTexture* texture = dx::cast<dx::UTexture>(object);
    if (!texture) {
        say("UCMapKit|texdump|missing|%s", name);
        return 1;
    }
    dx::objects::ensureLoaded(texture);
    dx::objects::flushLoads();
    int us = 0, vs = 0;
    float scale = 1.f;
    if (dx::UProperty* p = propertyOf(texture, "USize")) {
        us = *reinterpret_cast<int32_t*>(propertyAddress(texture, p));
    }
    if (dx::UProperty* p = propertyOf(texture, "VSize")) {
        vs = *reinterpret_cast<int32_t*>(propertyAddress(texture, p));
    }
    if (dx::UProperty* p = propertyOf(texture, "DrawScale")) {
        scale = *reinterpret_cast<float*>(propertyAddress(texture, p));
    }
    say("UCMapKit|texdump|info|%s|%dx%d|scale %g|format %d", name, us, vs, static_cast<double>(scale), static_cast<int>(texture->format()));
    if (texture->mips.empty()) {
        say("UCMapKit|texdump|nodata|%s", name);
        return 1;
    }
    size_t at = 0;
    while (at + 1 < texture->mips.size()
           && (texture->mips[at].width > static_cast<uint32_t>(maxSize) || texture->mips[at].height > static_cast<uint32_t>(maxSize))) {
        at++;
    }
    const dx::TextureMip& mip = texture->mips[at];
    if (mip.width == 0 || mip.height == 0) {
        say("UCMapKit|texdump|nodata|%s", name);
        return 1;
    }
    std::vector<uint8_t> pixels;
    try {
        pixels = texture->rgba(at);
    } catch (...) {
        say("UCMapKit|texdump|format|%s|%d", name, static_cast<int>(texture->format()));
        return 1;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    Json header;
    header["width"] = static_cast<int>(mip.width);
    header["height"] = static_cast<int>(mip.height);
    out.write(header);
    out.bytes(pixels.data(), pixels.size());
    say("UCMapKit|texdump|ok|%s|%d %d|format %d", name, static_cast<int>(mip.width), static_cast<int>(mip.height),
        static_cast<int>(texture->format()));
    return 1;
}

// UC_TEXFLAGS <file>: the PolyFlags of every loaded texture that has some, a row each.
static int dumpTexFlags(const char* path)
{
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int n = 0, all = 0;
    for (dx::UObject* object : dx::objects::all()) {
        if (!object || object->destroyed || !object->cls || !object->isA(dx::Name("Texture"))) {
            continue;
        }
        all++;
        uint32_t flags = textureFlags(object);
        if (!flags) {
            continue;
        }
        Json row;
        row["texture"] = object->pathName();
        row["flags"] = flags;
        out.write(row);
        n++;
    }
    say("UCMapKit|texflags|%d of %d textures", n, all);
    return 1;
}

// UC_SOUNDS <file>: every sound loaded, a row each.
static int listSounds(const char* path)
{
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int n = 0;
    for (dx::UObject* object : dx::objects::all()) {
        dx::USound* sound = dx::cast<dx::USound>(object);
        if (!sound || object->destroyed || !sound->data) {
            continue;
        }
        Json row;
        row["sound"] = object->pathName();
        row["bytes"] = sound->bytes.size();
        out.write(row);
        n++;
    }
    say("UCMapKit|sounds|%d", n);
    return 1;
}

// UC_MESHLIST <file>: every 3D model loaded, a row each.
static int listMeshes(const char* path)
{
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int n = 0;
    for (dx::UObject* object : dx::objects::all()) {
        dx::UMesh* mesh = dx::cast<dx::UMesh>(object);
        if (!mesh || object->destroyed) {
            continue;
        }
        dx::objects::ensureLoaded(mesh);
        dx::objects::flushLoads();
        int triangles = -1;
        try {
            triangles = static_cast<int>(mesh->triangles().size());
        } catch (...) {
            triangles = -1;
        }
        Json row;
        row["package"] = object->outermost()->name.str();
        row["mesh"] = object->pathName();
        row["class"] = object->cls ? object->cls->name.str() : std::string();
        row["triangles"] = triangles;
        row["frames"] = mesh->animFrames;
        row["sequences"] = static_cast<int>(mesh->sequences.size());
        out.write(row);
        n++;
    }
    say("UCMapKit|meshlist|%d", n);
    return 1;
}

// UC_ANIMDUMP <mesh path> <file>: a model with every frame of its animations.
struct AnimFace {
    uint16_t wedge[3];
    uint16_t material;
};
struct AnimWedge {
    uint16_t vertex;
    uint8_t u, v;
};
struct AnimVert {
    int16_t x, y, z;
};

static int dumpAnims(const char* meshPath, const char* path)
{
    dx::UMesh* mesh = dx::cast<dx::UMesh>(dx::objects::loadObject(meshPath, dx::objects::findClass("Engine.Mesh")));
    if (!mesh) {
        mesh = dx::cast<dx::UMesh>(dx::objects::loadObject(meshPath, nullptr));
    }
    if (!mesh) {
        say("UCMapKit|anim|no mesh|%s", meshPath);
        return 0;
    }
    dx::objects::ensureLoaded(mesh);
    dx::objects::flushLoads();
    dx::ULodMesh* lod = dx::cast<dx::ULodMesh>(mesh);
    std::vector<AnimFace> faces, special;
    std::vector<AnimWedge> wedges;
    std::vector<AnimVert> verts;
    if (lod) {
        for (const dx::MeshFace& face : lod->faces) {
            AnimFace f { { face.wedge[0], face.wedge[1], face.wedge[2] }, face.material };
            faces.push_back(f);
        }
        for (const dx::MeshWedge& wedge : lod->wedges) {
            AnimWedge w { wedge.vertex, wedge.u, wedge.v };
            wedges.push_back(w);
        }
        for (const dx::MeshFace& face : lod->specialFaces) {
            AnimFace f { { face.wedge[0], face.wedge[1], face.wedge[2] }, face.material };
            special.push_back(f);
        }
    }
    for (const dx::Vector& vertex : mesh->verts) {
        AnimVert v { static_cast<int16_t>(vertex.x), static_cast<int16_t>(vertex.y), static_cast<int16_t>(vertex.z) };
        verts.push_back(v);
    }
    Json header, counts, sequences = Json::array(), textures = Json::array(), materials = Json::array();
    counts["frameVerts"] = mesh->frameVerts;
    counts["frames"] = mesh->animFrames;
    counts["specialVerts"] = lod ? lod->specialVerts : 0;
    counts["modelVerts"] = lod ? lod->modelVerts : 0;
    counts["faces"] = lod ? static_cast<int>(lod->faces.size()) : 0;
    counts["wedges"] = lod ? static_cast<int>(lod->wedges.size()) : 0;
    counts["materials"] = lod ? static_cast<int>(lod->materials.size()) : 0;
    counts["specialFaces"] = lod ? static_cast<int>(lod->specialFaces.size()) : 0;
    counts["remap"] = lod ? static_cast<int>(lod->remapAnimVerts.size()) : 0;
    counts["oldFrameVerts"] = lod ? lod->oldFrameVerts : 0;
    counts["verts"] = static_cast<int>(mesh->verts.size());
    for (const dx::AnimSequence& sequence : mesh->sequences) {
        Json seq, notifys = Json::array();
        for (const dx::AnimNotify& notify : sequence.notifys) {
            notifys.push_back(Json::array({ digits(notify.time), notify.function.str() }));
        }
        seq["name"] = sequence.name.str();
        seq["group"] = sequence.group.str();
        seq["start"] = sequence.startFrame;
        seq["frames"] = sequence.numFrames;
        seq["rate"] = digits(sequence.rate);
        seq["notifys"] = notifys;
        sequences.push_back(seq);
    }
    for (dx::UObject* texture : mesh->textures) {
        textures.push_back(texture ? texture->pathName() : std::string("None"));
    }
    if (lod) {
        for (const dx::MeshMaterial& material : lod->materials) {
            materials.push_back(Json::array({ material.textureIndex, material.polyFlags }));
        }
    }
    dx::Vector scale = mesh->scale, origin = mesh->origin;
    header["mesh"] = meshPath;
    header["lod"] = lod ? 1 : 0;
    header["vertSize"] = 12;
    header["counts"] = counts;
    header["scale"] = xyz(scale);
    header["origin"] = xyz(origin);
    header["rotation"] = Json::array({ mesh->rotOrigin.pitch, mesh->rotOrigin.yaw, mesh->rotOrigin.roll });
    header["sequences"] = sequences;
    header["textures"] = textures;
    header["materials"] = materials;
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    out.write(header);
    out.bytes("data\n", 5);
    if (!faces.empty()) {
        out.bytes(faces.data(), sizeof(AnimFace) * faces.size());
    }
    if (!wedges.empty()) {
        out.bytes(wedges.data(), sizeof(AnimWedge) * wedges.size());
    }
    if (!special.empty()) {
        out.bytes(special.data(), sizeof(AnimFace) * special.size());
    }
    if (!verts.empty()) {
        out.bytes(verts.data(), sizeof(AnimVert) * verts.size());
    }
    say("UCMapKit|anim|%s|%d sequences|%d frames|%d verts", meshPath, static_cast<int>(mesh->sequences.size()),
        mesh->animFrames, mesh->frameVerts);
    return 1;
}

// The objects of one package: {path, parent, text} rows.
static bool packageLoaded(const char* name)
{
    return dx::objects::findLinker(dx::Name(name)) != nullptr;
}

// Every object of a loaded package, as UnrealEd loads a package: its classes and their defaults, its textures,
// its text. OpenDE creates an export's object when it is first asked for, so an unasked one is not in
// objects::all() and would be missing from a dump that walks the loaded objects (as the SDK bridge's does).
static void loadPackageFully(dx::Linker* linker)
{
    if (!linker) {
        return;
    }
    const dx::Package& file = linker->file();
    for (size_t i = 0; i < file.exports().size(); i++) {
        dx::UObject* object = linker->object(static_cast<int32_t>(i + 1));
        if (object) {
            dx::objects::ensureLoaded(object);
        }
    }
    dx::objects::flushLoads();
}

static void loadPackageFully(const char* name)
{
    loadPackageFully(dx::objects::findLinker(dx::Name(name)));
}


static bool inPackage(dx::UObject* object, const char* package)
{
    dx::UObject* top = object->outermost();
    return top && sameName(top->name.str(), package);
}

static int dumpScripts(const char* package, const char* path)
{
    if (!packageLoaded(package)) {
        return 0;
    }
    loadPackageFully(package);
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int count = 0;
    for (dx::UObject* object : dx::objects::all()) {
        dx::UClass* cls = dx::cast<dx::UClass>(object);
        if (!cls || object->destroyed || !cls->scriptText || !inPackage(cls, package)) {
            continue;
        }
        dx::objects::ensureLoaded(cls->scriptText);
        dx::objects::flushLoads();
        dx::UClass* super = cls->superClass();
        Json row;
        row["path"] = cls->pathName();
        row["parent"] = super ? super->pathName() : std::string();
        row["text"] = cls->scriptText->text;
        out.write(row);
        count++;
    }
    say("UCMapKit|scripts|%d", count);
    return count > 0;
}

static int dumpTexts(const char* package, const char* path)
{
    dx::UClass* extString = dx::objects::findClass("Extension.ExtString");
    if (!extString || !packageLoaded(package)) {
        return 0;
    }
    loadPackageFully(package);
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int count = 0;
    for (dx::UObject* object : dx::objects::all()) {
        if (!object || object->destroyed || !object->cls || !object->isA(extString) || !inPackage(object, package)) {
            continue;
        }
        dx::objects::ensureLoaded(object);
        dx::objects::flushLoads();
        std::string text = propertyText(object, "Text");
        Json row;
        row["path"] = object->pathName();
        row["parent"] = std::string();
        row["text"] = text;
        out.write(row);
        count++;
    }
    say("UCMapKit|texts|%d", count);
    return count > 0;
}

// UC_CONDUMP <file>: every conversation object loaded, with all their reflected properties.
static int dumpConversations(const char* path)
{
    dx::UClass* base = dx::objects::findClass("ConSys.ConObject");
    if (!base) {
        return 0;
    }
    JsonLines out(path);
    if (!out.ok()) {
        return 0;
    }
    int count = 0, errors = 0;
    for (dx::UObject* object : dx::objects::all()) {
        if (!object || object->destroyed || !object->cls || !object->isA(base)) {
            continue;
        }
        Json row;
        row["class"] = object->cls->name.str();
        row["path"] = object->pathName();
        Json& properties = row["properties"] = Json::object();
        for (dx::UProperty* property : object->cls->fieldOrder()) {
            if (intrinsicProperty(property->name.str())) {
                continue;
            }
            for (int i = 0; i < property->arrayDim; i++) {
                std::string key = property->name.str();
                if (property->arrayDim > 1) {
                    key += "(" + std::to_string(i) + ")";
                }
                uint8_t* data = propertyAddress(object, property, i);
                if (!data) {
                    ++errors;
                    continue;
                }
                // Some original camera transitions carry an out-of-range enum byte: export the number then.
                if (property->type == dx::PropType::Byte && property->enumType && *data >= property->enumType->names.size()) {
                    properties[key] = std::to_string(static_cast<int>(*data));
                } else {
                    properties[key] = property->toText(data);
                }
            }
        }
        if (object->isA(dx::Name("ConEventRandomLabel"))) {
            Json& labels = row["labels"] = Json::array();
            for (const std::string& label : dx::cast<dx::extension::UConEventRandomLabel>(object)->labels) {
                labels.push_back(label);
            }
        }
        out.write(row);
        ++count;
    }
    say("UCMapKit|condump|%d|property errors %d", count, errors);
    return errors == 0 && count > 0;
}

// UC_CONSOUND <conversation> <sound id> <file>: the sound a conversation plays for a speech id.
static int conversationSound(const char* name, int id, const char* path)
{
    if (id < 0) {
        return 0;
    }
    dx::UObject* conversation = dx::objects::findObject(name, dx::objects::findClass("ConSys.Conversation"));
    if (!conversation) {
        conversation = dx::objects::findObject(name, nullptr);
    }
    if (!conversation || !conversation->isA(dx::Name("Conversation"))) {
        return 0;
    }
    dx::UObject* sound = nullptr;
    std::string audioPackage;
    dx::UProperty* audio = propertyOf(conversation, "audioPackageName");
    if (audio) {
        audioPackage = *reinterpret_cast<std::string*>(propertyAddress(conversation, audio));
    }
    if (!audioPackage.empty()) {
        std::string prefix = conversation->outermost()->name.str();
        if (prefix.size() > 4 && sameName(prefix.substr(prefix.size() - 4), "Text")) {
            prefix.resize(prefix.size() - 4);
        }
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(prefix + "Audio" + audioPackage));
        if (linker) {
            int32_t ref = linker->findExport(dx::Name("ConAudioList_" + audioPackage), dx::Name("ConAudioList"), "*");
            if (ref) {
                dx::extension::UConAudioList* list = dx::cast<dx::extension::UConAudioList>(linker->object(ref));
                if (list) {
                    dx::objects::ensureLoaded(list);
                    if (static_cast<size_t>(id) < list->sounds.size()) {
                        sound = list->sounds[static_cast<size_t>(id)];
                        dx::objects::ensureLoaded(sound);
                    }
                }
            }
        }
    }
    Json row;
    row["conversation"] = name;
    row["id"] = id;
    row["sound"] = sound ? Json(sound->pathName()) : Json();
    JsonLines out(path, true);                          // one row per command, added to the file
    if (!out.ok()) {
        return 0;
    }
    out.write(row);
    say("UCMapKit|consound|%s|%d|%s", name, id, sound ? "found" : "missing");
    return 1;
}

// ------------------------------------------------------------------------------------- packages and models
// UC_LOADPKG <path>: a package loaded from its path, however long.
static int loadPackage(const char* path)
{
    if (!path || !*path) {
        return 0;
    }
    std::filesystem::path file(path);
    std::string stem = file.stem().string();
    dx::Linker* linker = nullptr;
    if (std::filesystem::is_regular_file(file)) {
        dx::objects::setPackageFile(dx::Name(stem), file);
        linker = dx::objects::loadPackageFile(dx::Name(stem), file, false);
    } else {
        linker = dx::objects::loadPackage(dx::Name(stem));
    }
    say("UCMapKit|loadpkg|%s|%s", linker ? "ok" : "failed", path);
    return linker != nullptr;
}

// A package created in memory for the objects the editor imports (textures, models, sounds)
static dx::UObject* sessionPackage(const char* name)
{
    dx::UObject* package = dx::objects::packageObject(dx::Name(name));
    return package;
}

// The compiler's asset importers (TEXTURE IMPORT, MESH IMPORT, MESH SEQUENCE, MESHMAP, ...), which are the
// engine's own #exec handlers in OpenDE (src/compiler/asset_*.cc).
static bool importAssetLine(const char* line, dx::UObject* package, const std::filesystem::path& directory)
{
    dx::compiler::ExecContext context;
    context.package = package;
    context.directory = directory;
    context.log = [](const std::string& text) { Host->print(text.c_str()); };
    bool handled = dx::compiler::importAsset(context, line);
    if (handled) {
        dx::compiler::finishAssets(context);
    }
    return handled;
}

// UC_MESHIMPORT <package> <name> <aniv file> <data file> [old [lod]]: a model from its two Unreal 3D files.
static int importMesh(const char* package, const char* name, const char* anivFile, const char* dataFile, int old, int lod)
{
    (void)old;                                  // OpenDE's importer reads the 16-bit vertex format only
    (void)package;
    std::string line = "MESH IMPORT MESH=" + std::string(name) + " ANIVFILE=" + anivFile + " DATAFILE=" + dataFile
        + (lod ? " MLOD=1" : "");
    dx::UObject* outer = sessionPackage(package);
    if (!importAssetLine(line.c_str(), outer, std::filesystem::path("."))) {
        say("UCMapKit|meshimport|%s|0|0 verts|0 frames|0 faces", name);
        return 0;
    }
    dx::UObject* object = dx::objects::findObject(std::string(package) + "." + name, nullptr);
    dx::UMesh* mesh = dx::cast<dx::UMesh>(object);
    int triangles = 0;
    if (mesh) {
        try {
            triangles = static_cast<int>(mesh->triangles().size());
        } catch (...) {
            triangles = -1;
        }
    }
    say("UCMapKit|meshimport|%s|%d|%d verts|%d frames|%d faces", name, mesh ? 1 : 0, mesh ? mesh->frameVerts : 0,
        mesh ? mesh->animFrames : 0, triangles);
    return mesh != nullptr;
}

// UC_SAVEPKG <package> <file>: a package made or changed in this session (the map editor's models) saved.
static int savePackage(const char* package, const char* path)
{
    dx::UObject* outer = dx::objects::packageObject(dx::Name(package));
    std::string error;
    std::vector<dx::UObject*> contents;
    for (dx::UObject* object : dx::objects::all()) {
        if (object && !object->destroyed && object->outer && object->outermost() == outer) {
            contents.push_back(object);
        }
    }
    bool ok = dx::compiler::saveScriptPackage(outer, contents, path, &error);
    if (!ok && !error.empty()) {
        say("UCMapKit|failed|savepkg|%s", error.c_str());
    }
    say("UCMapKit|savepkg|%s|%d", path, ok ? 1 : 0);
    return ok ? 1 : 0;
}

// UC_ANIMNEW <source model> <package> <name> <file>: a copy of a model (every sequence and frame, its
// triangles, its LOD, its skins) with the new animations after its own frames, in a package made in memory
// (written by UC_ANIMSAVE). The Animation Studio of the map editor makes the file: a JSON line
// {sequences: [{name, group, frames, rate}]}, the line "data", then the frames (frameVerts x int16 xyz each, in
// raw model units, the special verts first, as in the model's own Verts).
static dx::UObject* AnimPackage = nullptr;

// a frame's box and sphere, added to the model's (as the engine's importer does)
static void addFrameBounds(dx::UMesh* mesh, const std::vector<dx::Vector>& points)
{
    if (points.empty()) {
        return;
    }
    dx::BoundingBox box;
    box.min = points.front();
    box.max = points.front();
    box.valid = true;
    for (const dx::Vector& point : points) {
        box.min.x = std::min(box.min.x, point.x);
        box.min.y = std::min(box.min.y, point.y);
        box.min.z = std::min(box.min.z, point.z);
        box.max.x = std::max(box.max.x, point.x);
        box.max.y = std::max(box.max.y, point.y);
        box.max.z = std::max(box.max.z, point.z);
    }
    mesh->frameBounds.push_back(box);
    dx::Vector centre = (box.min + box.max) * 0.5f;
    float radius = 0;
    for (const dx::Vector& point : points) {
        radius = std::max(radius, (point - centre).size());
    }
    dx::Plane sphere;
    sphere.x = centre.x;
    sphere.y = centre.y;
    sphere.z = centre.z;
    sphere.w = radius;
    mesh->frameSpheres.push_back(sphere);
    if (!mesh->meshBounds.valid) {
        mesh->meshBounds = box;
        mesh->meshSphere = sphere;
        return;
    }
    mesh->meshBounds.min.x = std::min(mesh->meshBounds.min.x, box.min.x);
    mesh->meshBounds.min.y = std::min(mesh->meshBounds.min.y, box.min.y);
    mesh->meshBounds.min.z = std::min(mesh->meshBounds.min.z, box.min.z);
    mesh->meshBounds.max.x = std::max(mesh->meshBounds.max.x, box.max.x);
    mesh->meshBounds.max.y = std::max(mesh->meshBounds.max.y, box.max.y);
    mesh->meshBounds.max.z = std::max(mesh->meshBounds.max.z, box.max.z);
    if (sphere.w > mesh->meshSphere.w) {
        mesh->meshSphere = sphere;
    }
}

static int newAnimMesh(const char* source, const char* package, const char* name, const UCSequence* sequences, int count,
    const void* frames, size_t size)
{
    dx::UObject* object = dx::objects::loadObject(source, dx::objects::findClass("Engine.Mesh"));
    dx::ULodMesh* from = dx::cast<dx::ULodMesh>(object);
    if (!from) {
        say("UCMapKit|animnew|not a LOD model|%s", source ? source : "");
        return 0;
    }
    dx::objects::ensureLoaded(from);
    dx::objects::flushLoads();
    struct NewSequence {
        std::string name, group;
        int frames;
        float rate;
    };
    std::vector<NewSequence> added;
    int total = 0;
    for (int i = 0; i < count; i++) {
        const UCSequence& sequence = sequences[i];
        if (!sequence.name || !*sequence.name || sequence.frames <= 0) {
            continue;
        }
        added.push_back(NewSequence { sequence.name, sequence.group && *sequence.group ? sequence.group : "UCAnim",
            sequence.frames, sequence.rate });
        total += sequence.frames;
    }
    // after the "data" line, the new frames' vertices as editor/anims.py writes them: x y z, 16-bit each
    struct FrameVert {
        int16_t x, y, z;
    };
    int frameVerts = from->frameVerts;
    size_t needed = sizeof(FrameVert) * static_cast<size_t>(total) * static_cast<size_t>(frameVerts);
    if (size < needed) {
        say("UCMapKit|animnew|frames missing|%d of %d", static_cast<int>(size / sizeof(FrameVert)), total * frameVerts);
        return 0;
    }
    const FrameVert* vertices = static_cast<const FrameVert*>(frames);
    if (!AnimPackage) {
        AnimPackage = sessionPackage(package);
    }
    dx::UObject* created = dx::objects::newObject(from->cls, AnimPackage, dx::Name(name), dx::RF_Public | dx::RF_Standalone);
    dx::ULodMesh* mesh = dx::cast<dx::ULodMesh>(created);
    if (!mesh) {
        return 0;
    }
    mesh->loaded = true;
    // the model as it is: the compiled class's properties (its skins) and its C++ state
    from->cls->copyValue(mesh->data, from->data);
    mesh->meshBounds = from->meshBounds;
    mesh->meshSphere = from->meshSphere;
    mesh->verts = from->verts;
    mesh->tris = from->tris;
    mesh->sequences = from->sequences;
    mesh->connects = from->connects;
    mesh->vertLinks = from->vertLinks;
    mesh->textures = from->textures;
    mesh->frameBounds = from->frameBounds;
    mesh->frameSpheres = from->frameSpheres;
    mesh->frameVerts = from->frameVerts;
    mesh->animFrames = from->animFrames;
    mesh->andFlags = from->andFlags;
    mesh->orFlags = from->orFlags;
    mesh->scale = from->scale;
    mesh->origin = from->origin;
    mesh->rotOrigin = from->rotOrigin;
    mesh->curPoly = from->curPoly;
    mesh->curVertex = from->curVertex;
    mesh->textureLod = from->textureLod;
    mesh->collapsePointThus = from->collapsePointThus;
    mesh->faceLevel = from->faceLevel;
    mesh->faces = from->faces;
    mesh->collapseWedgeThus = from->collapseWedgeThus;
    mesh->wedges = from->wedges;
    mesh->materials = from->materials;
    mesh->specialFaces = from->specialFaces;
    mesh->modelVerts = from->modelVerts;
    mesh->specialVerts = from->specialVerts;
    mesh->meshScaleMax = from->meshScaleMax;
    mesh->lodHysteresis = from->lodHysteresis;
    mesh->lodStrength = from->lodStrength;
    mesh->lodMinVerts = from->lodMinVerts;
    mesh->lodMorph = from->lodMorph;
    mesh->lodZDisplace = from->lodZDisplace;
    mesh->remapAnimVerts = from->remapAnimVerts;
    mesh->oldFrameVerts = from->oldFrameVerts;
    // the new frames after its own, a sequence for each animation
    int first = mesh->animFrames;
    std::vector<dx::Vector> pose(static_cast<size_t>(frameVerts));
    mesh->verts.reserve(mesh->verts.size() + static_cast<size_t>(total) * static_cast<size_t>(frameVerts));
    for (int frame = 0; frame < total; frame++) {
        for (int i = 0; i < frameVerts; i++) {
            const FrameVert& v = vertices[static_cast<size_t>(frame) * static_cast<size_t>(frameVerts) + static_cast<size_t>(i)];
            dx::Vector point { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) };
            mesh->verts.push_back(point);
            pose[static_cast<size_t>(i)] = point;
        }
        addFrameBounds(mesh, pose);
    }
    mesh->animFrames += total;
    int start = first;
    for (const NewSequence& sequence : added) {
        dx::AnimSequence anim;
        anim.name = dx::Name(sequence.name);
        anim.group = dx::Name(sequence.group);
        anim.startFrame = start;
        anim.numFrames = sequence.frames;
        anim.rate = sequence.rate;
        mesh->sequences.push_back(anim);
        start += sequence.frames;
    }
    say("UCMapKit|animnew|%s.%s|%d sequences|%d new frames|%d frames|box %d %d", package, name,
        static_cast<int>(added.size()), total, mesh->animFrames, static_cast<int>(mesh->frameBounds.size()),
        static_cast<int>(mesh->verts.size()) / (frameVerts > 0 ? frameVerts : 1));
    return 1;
}

// UC_ANIMSAVE <file>: writes the package of UC_ANIMNEW (then a new one can be made); without any UC_ANIMNEW, an
// empty package named after the file.
static int saveAnimPackage(const char* path)
{
    if (!AnimPackage) {
        std::string name = std::filesystem::path(path).stem().string();
        AnimPackage = sessionPackage(name.c_str());
    }
    std::vector<dx::UObject*> contents;
    for (dx::UObject* object : dx::objects::all()) {
        if (object && !object->destroyed && object->outer && object->outer->outermost() == AnimPackage) {
            contents.push_back(object);
        }
    }
    std::string error;
    bool ok = dx::compiler::saveScriptPackage(AnimPackage, contents, path, &error);
    if (!ok && !error.empty()) {
        say("UCMapKit|failed|animsave|%s", error.c_str());
    }
    say("UCMapKit|animsave|%s|%d", path, ok ? 1 : 0);
    AnimPackage = nullptr;
    return ok ? 1 : 0;
}

// ------------------------------------------------------------------------------------- exec: UnrealEd's commands
static void removeQuotes(std::string& text)
{
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
}

// The words of a command line, and KEY=VALUE pairs ("FILE=..\Input\X.dx", "BALANCE=15")
class Words {
public:
    explicit Words(const std::string& line)
    {
        std::istringstream input(line);
        std::string word;
        while (input >> word) {
            words.push_back(word);
        }
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"') {
                size_t end = line.find('"', i + 1);
                if (end == std::string::npos) {
                    break;
                }
                quoted.push_back(line.substr(i + 1, end - i - 1));
                i = end;
            }
        }
    }
    size_t count() const { return words.size(); }
    const std::string& operator[](size_t i) const
    {
        static const std::string empty;
        return i < words.size() ? words[i] : empty;
    }
    std::string upper(size_t i) const
    {
        std::string text = (*this)[i];
        for (char& c : text) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return text;
    }
    // the value of KEY= in any word ("FILE=..\Input\X.dx")
    std::string value(const char* key) const
    {
        std::string prefix = std::string(key) + "=";
        for (size_t i = 0; i < words.size(); i++) {
            if (words[i].compare(0, prefix.size(), prefix) == 0) {
                std::string value = words[i].substr(prefix.size());
                removeQuotes(value);
                return value;
            }
        }
        // a quoted path is one word more ("FILE=" ..\Input\X.dx")
        for (size_t i = 0; i + 1 < words.size(); i++) {
            if (words[i] == prefix) {
                return quoted.empty() ? std::string() : quoted.front();
            }
        }
        return std::string();
    }
    bool has(const char* key) const { return !value(key).empty(); }
    int number(const char* key, int fallback) const
    {
        std::string text = value(key);
        return text.empty() ? fallback : atoi(text.c_str());
    }
    // the whole line from the first word on (a mesh sequence's trailing words)
    std::string rest(size_t from) const
    {
        std::string text;
        for (size_t i = from; i < words.size(); i++) {
            if (i) {
                text += ' ';
            }
            text += words[i];
        }
        return text;
    }
    const std::vector<std::string>& all() const { return words; }

private:
    std::vector<std::string> words, quoted;
};

// Create a new, empty map ("MAP NEW"): LevelInfo, the builder brush, and the Deus Ex level info.
static bool newMap(const std::string& name)
{
    dx::UObject* package = dx::objects::packageObject(dx::Name(name));
    dx::UClass* infoClass = dx::objects::findClass("Engine.LevelInfo");
    dx::UClass* deusExInfoClass = dx::objects::findClass("DeusEx.DeusExLevelInfo");
    dx::UClass* brushClass = dx::objects::findClass("Engine.Brush");
    dx::UClass* startClass = dx::objects::findClass("Engine.PlayerStart");
    dx::objects::flushLoads();
    dx::UClass* levelClass = dx::csg::nativeClass("Level");
    dx::UClass* modelClass = dx::csg::nativeClass("Model");
    dx::UClass* polysClass = dx::csg::nativeClass("Polys");
    if (!infoClass || !brushClass || !startClass || !levelClass || !modelClass || !polysClass) {
        say("UCMapKit|failed|map new|missing engine classes");
        return false;
    }
    const uint32_t editorFlags = dx::RF_Transactional | 0x00070000;
    const uint32_t editorOnly = editorFlags | dx::RF_NotForClient | dx::RF_NotForServer;
    auto create = [&](dx::UClass* cls, const char* objectName, uint32_t flags) -> dx::UObject* {
        dx::UObject* object = dx::objects::newObject(cls, package, objectName ? dx::Name(objectName) : dx::Name(), flags);
        if (object) {
            object->loaded = true;
        }
        return object;
    };
    dx::ULevel* level = dx::cast<dx::ULevel>(create(levelClass, "MyLevel", editorFlags));
    if (!level) {
        return false;
    }
    level->model = dx::cast<dx::UModel>(create(modelClass, nullptr, editorFlags));
    dx::UObject* info = create(infoClass, nullptr, editorFlags);
    dx::UObject* builder = create(brushClass, nullptr, editorOnly);
    dx::UModel* builderModel = dx::cast<dx::UModel>(create(modelClass, nullptr, editorOnly));
    if (builderModel) {
        builderModel->polys = dx::cast<dx::UPolys>(create(polysClass, nullptr, editorFlags));
        if (builderModel->polys) {
            dx::csg::buildCube(*builderModel->polys, 256.0f, 256.0f, 256.0f, nullptr);
            dx::csg::updateBounds(*builderModel);
        }
        dx::actor::Brush(builder) = builderModel;
        dx::brush::CsgOper(builder) = dx::csg::CsgActive;
    }
    dx::UObject* start = create(startClass, nullptr, editorFlags);
    dx::UObject* deusExInfo = deusExInfoClass ? create(deusExInfoClass, nullptr, editorFlags) : nullptr;
    level->actors = { info, builder };
    if (deusExInfo) {
        level->actors.push_back(deusExInfo);
    }
    if (start) {
        dx::actor::Location(start) = dx::Vector { 0, 0, 0 };
        level->actors.push_back(start);
    }
    for (dx::UObject* actor : level->actors) {
        if (actor) {
            dx::actor::Level(actor) = info;
        }
    }
    attachLevel(level, name);
    return true;
}

// A file the command names. The editor's command lines are written in the engine's own Windows form
// ("..\\Input\\map.dx"), so the separators are the ones file names use on every platform, and a relative path is
// taken from where the host runs: the run's System folder (so "..\\Input" is the run's Input folder, as on
// Windows, where the host runs there too).
static std::filesystem::path resolveFile(const std::string& file)
{
    std::string text = file;
    std::replace(text.begin(), text.end(), '\\', '/');
    std::filesystem::path path(text);
    if (path.is_absolute()) {
        return path;
    }
    std::error_code error;
    std::filesystem::path cwd = std::filesystem::current_path(error);
    if (error) {
        return path;
    }
    return cwd / path;
}

// MAP LOAD FILE=...: a map from its path, or from the game's Maps folder by name.
static bool loadMap(const std::string& file)
{
    if (file.empty()) {
        return false;
    }
    std::filesystem::path path = resolveFile(file);
    std::string name = path.stem().string();
    dx::Linker* linker = nullptr;
    if (std::filesystem::is_regular_file(path)) {
        dx::objects::setPackageFile(dx::Name(name), path);
        linker = dx::objects::loadPackageFile(dx::Name(name), path, false);
    } else {
        linker = dx::objects::loadPackage(dx::Name(name));
    }
    if (!linker) {
        say("UCMapKit|failed|MAP LOAD|no map %s", file.c_str());
        return false;
    }
    int32_t ref = linker->findExport(dx::Name("MyLevel"), dx::Name("Level"), "*");
    dx::ULevel* level = dx::cast<dx::ULevel>(linker->object(ref));
    if (!level) {
        say("UCMapKit|failed|MAP LOAD|no level in %s", file.c_str());
        return false;
    }
    dx::objects::ensureLoaded(level);
    dx::objects::flushLoads();
    for (dx::UObject* actor : level->actors) {
        if (actor) {
            dx::objects::ensureLoaded(actor);
        }
    }
    dx::objects::flushLoads();
    attachLevel(level, name);
    return true;
}

// MAP SAVE FILE=...: the level written to a .dx
static bool saveMap(const std::string& file)
{
    if (!Level || file.empty()) {
        return false;
    }
    std::string error;
    bool ok = dx::mapwriter::saveMap(Level, resolveFile(file), &error);
    if (!ok) {
        say("UCMapKit|failed|MAP SAVE|%s", error.c_str());
    }
    return ok;
}

// MAP EXPORT FILE=...: every actor as T3D text
static bool exportMap(const std::string& file)
{
    if (!Level || file.empty()) {
        return false;
    }
    std::vector<dx::UObject*> actors;
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed) {
            actors.push_back(actor);
        }
    }
    // OpenDE models Object's own fields (Outer, ObjectFlags, ObjectInternal) as properties and writes them into the
    // T3D; the SDK's engine has them as C++ members and writes none of them, and an import would take them for a
    // class's properties. They are left out.
    std::string text = dx::t3d::exportActors(actors);
    std::string filtered;
    filtered.reserve(text.size());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at + 1);
        at = end == std::string::npos ? text.size() : end + 1;
        size_t first = line.find_first_not_of(" \t");
        std::string key = first == std::string::npos ? std::string() : line.substr(first);
        size_t equals = key.find('=');
        if (equals != std::string::npos) {
            key.resize(equals);
            size_t paren = key.find('(');
            if (paren != std::string::npos) {
                key.resize(paren);
            }
            if (intrinsicProperty(key)) {
                continue;
            }
        }
        filtered += line;
    }
    std::ofstream out(resolveFile(file), std::ios::binary);
    if (!out) {
        return false;
    }
    out << filtered;
    return static_cast<bool>(out);
}

// The names a T3D gives its actors and brushes. OpenDE names an imported actor after its class (Brush1732), but
// UnrealEd keeps the T3D's own name, and the editor addresses actors by it (UC_SET, UC_PROP, UC_DEL, a script's
// UC_DEL UCBoot0): the names are put back here, in the order the importer made the actors.
namespace {
    struct T3dName {
        std::string className, name, brushName;
    };

    std::string trimText(std::string_view text)
    {
        size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) {
            return std::string();
        }
        size_t end = text.find_last_not_of(" \t\r\n");
        return std::string(text.substr(begin, end - begin + 1));
    }

    // the value of KEY= in a line, without its quotes
    std::string fieldOf(const std::string& line, const char* key)
    {
        size_t at = line.find(key);
        if (at == std::string::npos) {
            return std::string();
        }
        at += strlen(key);
        size_t end = line.find_first_of(" \t\r\n", at);
        return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }

    std::vector<T3dName> t3dNames(std::string_view text)
    {
        std::vector<T3dName> names;
        bool inActor = false, inBrush = false;
        while (!text.empty()) {
            size_t end = text.find('\n');
            std::string line = trimText(text.substr(0, end));
            text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
            if (!inActor) {
                if (line.compare(0, 11, "Begin Actor") == 0) {
                    T3dName name;
                    name.className = fieldOf(line, "Class=");
                    name.name = fieldOf(line, "Name=");
                    names.push_back(name);
                    inActor = true;
                }
                continue;
            }
            if (inBrush) {
                if (line.compare(0, 17, "Begin PolyList") == 0) {
                    continue;
                }
                if (line.compare(0, 9, "End Brush") == 0) {
                    inBrush = false;
                }
                continue;
            }
            if (line.compare(0, 11, "Begin Brush") == 0) {
                inBrush = true;
                names.back().brushName = fieldOf(line, "Name=");
                continue;
            }
            if (line.compare(0, 9, "End Actor") == 0) {
                inActor = false;
            }
        }
        return names;
    }
}

static void nameImportedActors(const std::vector<dx::UObject*>& created, std::string_view text, bool replace)
{
    std::vector<T3dName> records = t3dNames(text);
    dx::UClass* actorClass = dx::objects::findClass("Engine.Actor");
    dx::UClass* infoClass = dx::objects::findClass("Engine.LevelInfo");
    size_t next = 0;
    for (const T3dName& record : records) {
        if (next >= created.size()) {
            break;
        }
        dx::UClass* cls = dx::objects::findClass(record.className);
        if (!cls || !actorClass || !cls->isChildOf(actorClass) || (cls->classFlags & dx::CLASS_Abstract)) {
            continue;
        }
        if (!replace && infoClass && cls->isChildOf(infoClass)) {
            continue;
        }
        dx::UObject* actor = created[next++];
        if (!actor) {
            continue;
        }
        if (!record.name.empty()) {
            actor->name = dx::Name(record.name);
        }
        if (!record.brushName.empty()) {
            dx::UModel* model = brushModel(actor);
            if (model) {
                model->name = dx::Name(record.brushName);
            }
        }
    }
}

// MAP IMPORT FILE=... (the map's own actors) and MAP IMPORTADD FILE=... (added to the map)
static bool importMap(const std::string& file, bool replace)
{
    std::ifstream in(resolveFile(file), std::ios::binary);
    if (!in) {
        say("UCMapKit|failed|MAP IMPORT|cannot read %s", file.c_str());
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (replace) {
        std::string name = std::filesystem::path(file).stem().string();
        if (sameName(name, MapName) || dx::objects::findLinker(dx::Name(name))) {
            name += "_Import";
        }
        if (!newMap(name)) {
            return false;
        }
    } else if (!Level) {
        return false;
    }
    std::vector<dx::UObject*> created = dx::t3d::importActors(Level, text.str(), replace);
    nameImportedActors(created, text.str(), replace);
    // the imported actors join the level's own list: importActors() appends or replaces it already
    World.level = Level;
    World.levelInfo = Level->actors.empty() ? nullptr : Level->actors[0];
    World.actors = Level->actors;
    World.collisionHash.clear();
    World.collisionHash.sync(World.actors);
    if (replace) {
        std::string error;
        dx::csg::RebuildOptions options;
        dx::bsp::setCurrentTexture(nullptr);
        dx::csg::rebuild(Level, options);
    }
    return !created.empty();
}

// BRUSH ADD / BRUSH SUBTRACT: the builder brush into the level
static bool csgBrush(bool add, const std::string& flagsText)
{
    if (!Level) {
        return false;
    }
    dx::UObject* builder = builderBrush();
    if (!builder) {
        return false;
    }
    uint32_t flags = 0;
    if (add && !flagsText.empty()) {
        flags = static_cast<uint32_t>(std::strtoul(flagsText.c_str(), nullptr, 0));
    }
    dx::bsp::setCurrentTexture(nullptr);
    dx::UObject* brush = dx::csg::addBrush(Level, builder, add ? dx::csg::CsgAdd : dx::csg::CsgSubtract, flags);
    if (!brush) {
        return false;
    }
    World.actors = Level->actors;
    return true;
}

// BRUSH IMPORT FILE=...: the builder brush from a T3D brush
static bool importBrushCommand(const std::string& file)
{
    dx::UObject* builder = builderBrush();
    dx::UModel* model = builder ? brushModel(builder) : nullptr;
    if (!model || !model->polys) {
        return false;
    }
    std::ifstream in(resolveFile(file), std::ios::binary);
    if (!in) {
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    std::string content = text.str();
    // the brush is the polygons between Begin PolyList and End PolyList
    size_t begin = content.find("Begin PolyList");
    size_t end = content.find("End PolyList");
    if (begin != std::string::npos) {
        begin = content.find('\n', begin);
        begin = begin == std::string::npos ? content.size() : begin + 1;
    } else {
        begin = 0;
    }
    if (end == std::string::npos || end < begin) {
        end = content.size();
    }
    model->linked = 0;
    dx::editorops::importPolys(*model->polys, std::string_view(content).substr(begin, end - begin));
    for (size_t i = 0; i < model->polys->polys.size(); i++) {
        model->polys->polys[i].link = static_cast<int32_t>(i);
    }
    dx::csg::updateBounds(*model);
    return true;
}

// BRUSH RESET: the builder brush is a new 256-cube
static bool resetBrush()
{
    dx::UObject* builder = builderBrush();
    dx::UModel* model = builder ? brushModel(builder) : nullptr;
    if (!model || !model->polys) {
        return false;
    }
    model->polys->polys.clear();
    dx::csg::buildCube(*model->polys, 256.0f, 256.0f, 256.0f, nullptr);
    dx::csg::updateBounds(*model);
    dx::actor::Location(builder) = dx::Vector { 0, 0, 0 };
    dx::actor::Rotation(builder) = dx::Rotator {};
    return true;
}

// BSP REBUILD [OPTIMAL|GOOD|LAME] [BALANCE=n] and MAP REBUILD
static bool rebuildBsp(bool geometry, const Words& words)
{
    if (!Level) {
        return false;
    }
    dx::csg::RebuildOptions options;
    options.geometry = geometry;
    options.bsp = true;
    for (size_t i = 1; i < words.count(); i++) {
        std::string word = words.upper(i);
        if (word == "OPTIMAL") {
            options.optimization = dx::bsp::Optimization::Optimal;
        } else if (word == "GOOD") {
            options.optimization = dx::bsp::Optimization::Good;
        } else if (word == "LAME") {
            options.optimization = dx::bsp::Optimization::Lame;
        }
    }
    options.balance = std::clamp(words.number("BALANCE", 15), 0, 100);
    dx::bsp::setCurrentTexture(nullptr);
    dx::csg::Stats stats = dx::csg::rebuild(Level, options);
    World.actors = Level->actors;
    say("UCMapKit|geometry|%d brushes|%d nodes|%d surfs|%d zones", static_cast<int>(stats.brushes),
        static_cast<int>(stats.nodes), static_cast<int>(stats.surfs), static_cast<int>(stats.zones));
    return true;
}

// OBJ LOAD FILE=...: a package by name (the engine's Paths find it), or by path (the editor uses UC_LOADPKG
// for a path; OBJ LOAD takes one too when the file is there)
static bool objLoad(const std::string& file)
{
    std::string name = std::filesystem::path(file).stem().string();
    dx::Linker* linker = nullptr;
    std::error_code error;
    std::filesystem::path path = resolveFile(file);
    if (std::filesystem::is_regular_file(path, error)) {
        dx::objects::setPackageFile(dx::Name(name), path);
        linker = dx::objects::loadPackageFile(dx::Name(name), path, false);
    } else {
        linker = dx::objects::loadPackage(dx::Name(name));
    }
    if (!linker) {
        say("UCMapKit|failed|OBJ LOAD|%s", file.c_str());
        return false;
    }
    // OBJ LOAD loads the package's objects too: a later UC_CLASSES or UC_MESHLIST sees them
    loadPackageFully(linker);
    return true;
}

// OBJ EXPORT TYPE=Texture|Sound NAME=... FILE=...
static bool objExport(const Words& words)
{
    std::string type = words.value("TYPE");
    std::string name = words.value("NAME");
    std::string file = words.value("FILE");
    if (name.empty() || file.empty()) {
        return false;
    }
    dx::UObject* object = dx::objects::findObject(name, nullptr);
    if (!object) {
        object = dx::objects::loadObject(name, nullptr);
    }
    if (!object) {
        say("UCMapKit|failed|obj export|no object %s", name.c_str());
        return false;
    }
    // the format written is the file's own (OBJ EXPORT TYPE=Texture FILE=x.bmp writes a BMP)
    std::string extension = std::filesystem::path(file).extension().string();
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(0, 1);
    }
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!dx::compiler::exportAssetObject(object, extension, std::filesystem::path(file))) {
        say("UCMapKit|failed|obj export|%s|format %s", name.c_str(), extension.c_str());
        return false;
    }
    (void)type;
    return true;
}

// The command lines the editor sends (mapkit/editor, kit.py). Returns 1 when the command worked.
static int exec(const char* line)
{
    if (!line) {
        return 0;
    }
    // the host's resident build worker (resident_host.h): a batch starts on a fresh level. The SDK bridge patches
    // its import path and restores its class counters; OpenDE has nothing to prepare and nothing to rewind (a new
    // level is new objects, and the map writer leaves the old ones out), so both succeed at once.
    if (!strcmp(line, "UC_BUILD_INIT")) {
        return 1;
    }
    if (!strcmp(line, "UC_BUILD_RESET")) {
        attachLevel(nullptr, std::string());
        bool ok = newMap("Untitled");
        say("UCMapKit|build reset|%s", ok ? "new level" : "failed");
        return ok ? 1 : 0;
    }
    std::string text(line);
    Words words(text);
    if (!words.count()) {
        return 0;
    }
    std::string verb = words.upper(0);
    std::string second = words.upper(1);

    if (verb == "MAP") {
        if (second == "NEW") {
            return newMap("Untitled") ? 1 : 0;
        }
        if (second == "LOAD") {
            return loadMap(words.value("FILE")) ? 1 : 0;
        }
        if (second == "SAVE") {
            return saveMap(words.value("FILE")) ? 1 : 0;
        }
        if (second == "EXPORT") {
            return exportMap(words.value("FILE")) ? 1 : 0;
        }
        if (second == "IMPORT") {
            return importMap(words.value("FILE"), true) ? 1 : 0;
        }
        if (second == "IMPORTADD") {
            return importMap(words.value("FILE"), false) ? 1 : 0;
        }
        if (second == "REBUILD") {
            return rebuildBsp(true, words) ? 1 : 0;
        }
        say("UCMapKit|failed|unknown MAP command: %s", text.c_str());
        return 0;
    }
    if (verb == "BRUSH") {
        if (second == "ADD") {
            return csgBrush(true, words.value("FLAGS")) ? 1 : 0;
        }
        if (second == "SUBTRACT") {
            return csgBrush(false, std::string()) ? 1 : 0;
        }
        if (second == "RESET") {
            return resetBrush() ? 1 : 0;
        }
        if (second == "IMPORT") {
            return importBrushCommand(words.value("FILE")) ? 1 : 0;
        }
        say("UCMapKit|failed|unknown BRUSH command: %s", text.c_str());
        return 0;
    }
    if (verb == "BSP") {
        if (second == "REBUILD") {
            return rebuildBsp(false, words) ? 1 : 0;
        }
        say("UCMapKit|failed|unknown BSP command: %s", text.c_str());
        return 0;
    }
    if (verb == "LIGHT" && second == "APPLY") {
        if (!Level) {
            return 0;
        }
        dx::lightbuild::Stats stats = dx::lightbuild::rebuild(Level, nullptr);
        World.actors = Level->actors;
        say("UCMapKit|lighting|%d lights|%d surfaces", static_cast<int>(stats.lights), static_cast<int>(stats.surfaces));
        return 1;
    }
    if (verb == "PATHS") {
        if (!Level) {
            return 0;
        }
        if (second == "DEFINE") {
            dx::paths::undefinePaths(Level);
            dx::paths::definePaths(Level);
            say("UCMapKit|paths defined|%d", static_cast<int>(Level->reachSpecs.size()));
            return 1;
        }
        if (second == "UNDEFINE") {
            dx::paths::undefinePaths(Level);
            return 1;
        }
        if (second == "BUILD") {
            dx::paths::removePaths(Level);
            int count = dx::paths::buildPaths(Level, 1);
            say("UCMapKit|paths built|%d", count);
            return 1;
        }
        say("UCMapKit|failed|unknown PATHS command: %s", text.c_str());
        return 0;
    }
    if (verb == "OBJ") {
        if (second == "LOAD") {
            return objLoad(words.value("FILE")) ? 1 : 0;
        }
        if (second == "EXPORT") {
            return objExport(words) ? 1 : 0;
        }
        say("UCMapKit|failed|unknown OBJ command: %s", text.c_str());
        return 0;
    }
    if (verb == "TEXTURE" || verb == "MESH" || verb == "MESHMAP" || verb == "NEW" || verb == "FONT") {
        // the compiler's #exec importers: TEXTURE IMPORT, MESH IMPORT, MESH SEQUENCE, MESH ORIGIN, MESHMAP...
        std::string package = words.value("PACKAGE");
        dx::UObject* outer = package.empty() ? (Level ? Level->outermost() : sessionPackage("MyLevel")) : sessionPackage(package.c_str());
        std::filesystem::path directory = std::filesystem::current_path();
        // a file's directory first (the editor's paths are relative to the run's System, and are written in the
        // engine's own Windows form)
        std::string file = words.value("FILE");
        if (!file.empty()) {
            std::filesystem::path candidate = resolveFile(file);
            if (candidate.has_parent_path() && std::filesystem::is_directory(candidate.parent_path())) {
                directory = candidate.parent_path();
            }
        }
        return importAssetLine(text.c_str(), outer, directory) ? 1 : 0;
    }
    say("UCMapKit|failed|unknown command: %s", text.c_str());
    return 0;
}

// ------------------------------------------------------------------------------------- the session (bridge.h)
// The level's actors: the filled slots (a loaded map's array keeps the empty ones, and a deleted actor's slot is
// emptied, as in UnrealEd). The editor's own count ("deused", state actors=) is the same number.
static int actorCount()
{
    if (!Level) {
        return -1;
    }
    int count = 0;
    for (dx::UObject* actor : Level->actors) {
        if (actor && !actor->destroyed) {
            count++;
        }
    }
    return count;
}

// The game's folders: DEUSEX_GAME_DATA, else the run's own (the System folder the host runs in has the game's
// packages), else the build's game-data.
static std::filesystem::path determineGameRoot()
{
    if (const char* value = std::getenv("DEUSEX_GAME_DATA")) {
        std::filesystem::path candidate(value);
        if (std::filesystem::is_regular_file(candidate / "System" / "DeusEx.u")) {
            return candidate;
        }
    }
    std::error_code error;
    std::filesystem::path cwd = std::filesystem::current_path(error);
    if (!error) {
        // a run of the editor: <run>/System is the current folder, and the game is where game-data is
        std::filesystem::path parent = cwd.parent_path();
        if (std::filesystem::is_regular_file(parent / "Textures" / "CoreTexMisc.utx")) {
            return parent;
        }
    }
    // a run whose System folder holds the game's own packages: the game is the parent
    if (!error && std::filesystem::is_regular_file(cwd / "DeusEx.u")) {
        std::filesystem::path parent = cwd.parent_path();
        if (std::filesystem::is_regular_file(parent / "Textures" / "CoreTexMisc.utx")) {
            return parent;
        }
    }
#ifdef UCOPENBRIDGE_GAME_DATA
    {
        std::filesystem::path candidate(UCOPENBRIDGE_GAME_DATA);
        if (std::filesystem::is_regular_file(candidate / "System" / "DeusEx.u")) {
            return candidate;
        }
    }
#endif
    return cwd.parent_path();
}

// The editor's EditPackages list: the run's own DeusEx.ini first (OpenDE's config prefers the user's copy of the
// file in ~/.local/share/deusex, which is the game's, not the editor run's), then the engine's config.
static std::vector<std::string> editPackages(const std::filesystem::path& system)
{
    std::error_code error;
    std::filesystem::path file = system / "DeusEx.ini";
    if (std::filesystem::is_regular_file(file, error)) {
        dx::Ini ini;
        if (ini.load(file)) {
            std::vector<std::string> values = ini.getAll("Editor.EditorEngine", "EditPackages");
            if (!values.empty()) {
                return values;
            }
        }
    }
    return dx::config::system().getAll("Editor.EditorEngine", "EditPackages");
}

// The packages of the run's System folder (mods, the editor's own packages) join the game's own.
static void registerRunPackages(const std::filesystem::path& system)
{
    std::error_code error;
    if (!std::filesystem::is_directory(system, error)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(system, error)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string extension = entry.path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (extension == ".u" || extension == ".utx" || extension == ".uax" || extension == ".umx" || extension == ".dx") {
            dx::objects::setPackageFile(dx::Name(entry.path().stem().string()), entry.path());
        }
    }
}

static int start(const UCHostApi* host)
{
    Host = host;
    GameRoot = determineGameRoot();
    dx::objects::setEditorMode(true);
    dx::config::setApplyClassDefaults(false);
    dx::engine::initialize(GameRoot.string());
    // the run's own System (its ini, its packages) when the host runs inside one
    std::error_code error;
    std::filesystem::path cwd = std::filesystem::current_path(error);
    if (!error && sameName(cwd.filename().string(), "System") && cwd != GameRoot / "System") {
        dx::config::initialize(cwd);
        registerRunPackages(cwd);
    }
    // the editor engine loads its EditPackages at startup (UnrealEd does the same): the editor's palette and its
    // class defaults are all the game's classes, and a mod's package that the ini lists comes with them.
    for (const std::string& package : editPackages(dx::objects::gameRoot() / "System")) {
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(package));
        if (linker) {
            loadPackageFully(linker);
        }
    }
    // nowhere to talk to yet
    attachLevel(nullptr, std::string());
    say("UCMapKit|engine|OpenDE|%s", GameRoot.string().c_str());
    return 1;
}

static void stop()
{
}

// --make: the UnrealScript compiler (OpenDE's PackageCompiler), as ucc make runs it: every package of the ini's
// EditPackages whose .u is missing is built from its Classes. Returns the process's exit code.
namespace {
    std::string lowerCase(std::string text)
    {
        for (char& c : text) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    void loadFully(dx::Linker* linker)
    {
        for (size_t i = 0; i < linker->file().exports().size(); i++) {
            dx::objects::ensureLoaded(linker->object(static_cast<int32_t>(i + 1)));
        }
        dx::objects::flushLoads();
    }

    std::vector<std::filesystem::path> classFiles(const std::filesystem::path& directory)
    {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (const auto& item : std::filesystem::directory_iterator(directory, error)) {
            if (item.is_regular_file() && lowerCase(item.path().extension().string()) == ".uc") {
                files.push_back(item.path());
            }
        }
        std::sort(files.begin(), files.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return lowerCase(a.filename().string()) < lowerCase(b.filename().string());
        });
        return files;
    }

    std::string exportPath(const dx::Package& file, int32_t ref)
    {
        const dx::ExportEntry& entry = file.exportAt(ref);
        std::string name = file.nameAt(entry.name);
        return entry.outer > 0 ? exportPath(file, entry.outer) + "." + name : file.name() + "." + name;
    }

    int errors = 0;

    bool buildPackage(const std::filesystem::path& root, const std::string& name, const std::filesystem::path& sources,
        const std::filesystem::path& output)
    {
        Host->print(("Building " + name + " from " + sources.string()).c_str());
        std::vector<std::filesystem::path> files = classFiles(sources);
        if (files.empty()) {
            Host->print(("No files for package: " + sources.string()).c_str());
            return false;
        }
        dx::objects::setPackageFile(dx::Name(name), {});
        dx::UObject* package = dx::objects::packageObject(dx::Name(name));
        dx::compiler::PackageCompiler compiler(package, [](const std::string& text) { Host->print(text.c_str()); });
        std::filesystem::path shipped = root / "System" / (name + ".u");
        std::unordered_map<std::string, size_t> conform;
        if (std::filesystem::exists(shipped)) {
            dx::Package original(shipped);
            for (size_t i = 0; i < original.exports().size(); i++) {
                conform.emplace(lowerCase(exportPath(original, static_cast<int32_t>(i + 1))), i);
            }
            std::vector<dx::Name> natives;
            for (size_t i = 0; i < original.exports().size(); i++) {
                const dx::ExportEntry& entry = original.exports()[i];
                if (entry.classRef == 0 && (entry.flags & dx::RF_Native)) {
                    natives.emplace_back(original.nameAt(entry.name));
                }
            }
            compiler.setNativeClasses(std::move(natives));
            std::unordered_map<std::string, size_t> created;
            for (size_t i = 0; i < original.exports().size(); i++) {
                if (original.exports()[i].classRef == 0) {
                    created.emplace(lowerCase(original.nameAt(original.exports()[i].name)), i);
                }
            }
            std::stable_sort(files.begin(), files.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
                auto first = created.find(lowerCase(a.stem().string()));
                auto second = created.find(lowerCase(b.stem().string()));
                size_t left = first != created.end() ? first->second : created.size();
                size_t right = second != created.end() ? second->second : created.size();
                return left < right;
            });
        }
        for (const std::filesystem::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            compiler.importClass(file, text.str());
        }
        if (!compiler.compile()) {
            errors += compiler.errors();
            Host->print(("Failure - " + std::to_string(compiler.errors()) + " error(s)").c_str());
            return false;
        }
        std::vector<dx::UObject*> contents = dx::compiler::packageContents(package);
        std::stable_sort(contents.begin(), contents.end(), [&](dx::UObject* a, dx::UObject* b) {
            auto first = conform.find(lowerCase(a->pathName()));
            auto second = conform.find(lowerCase(b->pathName()));
            size_t left = first != conform.end() ? first->second : conform.size();
            size_t right = second != conform.end() ? second->second : conform.size();
            return left < right;
        });
        std::string error;
        if (!dx::compiler::saveScriptPackage(package, contents, output, &error)) {
            Host->print(("Failed to save " + output.string() + ": " + error).c_str());
            return false;
        }
        Host->print(("Saved " + output.string()).c_str());
        dx::objects::setPackageFile(dx::Name(name), output);
        for (dx::UObject* object : dx::compiler::packageContents(package)) {
            if (dx::UClass* cls = dx::cast<dx::UClass>(object)) {
                if (cls->defaultObject) {
                    cls->defaultObject->destroyed = true;
                }
            }
            object->destroyed = true;
        }
        dx::Linker* linker = dx::objects::loadPackageFile(dx::Name(name), output, false);
        if (!linker) {
            Host->print(("Failed to reload " + output.string()).c_str());
            return false;
        }
        loadFully(linker);
        return true;
    }
}

static int make(const UCHostApi* host, const char* ini)
{
    Host = host;
    (void)ini;
    // The engine's MakeCommandlet, as the editor's build runs it: the run the host is in holds the ini (its
    // EditPackages), the sources of a package in <run>/<Package>/Classes and its .u in <run>/System.
    GameRoot = determineGameRoot();
    dx::objects::setEditorMode(true);
    dx::config::setApplyClassDefaults(false);
    std::error_code error;
    std::filesystem::path cwd = std::filesystem::current_path(error);
    std::filesystem::path system = GameRoot / "System";
    std::filesystem::path run = GameRoot;
    if (!error && sameName(cwd.filename().string(), "System")) {
        system = cwd;
        run = cwd.parent_path();
    }
    // the engine without its packages: the compiler loads what it needs, and the game's paths find them
    dx::engine::initializeCore(GameRoot.string());
    if (system != GameRoot / "System") {
        dx::config::initialize(system);                 // the run's ini: its EditPackages
        registerRunPackages(system);
    }
    std::vector<std::string> packages = editPackages(system);
    // a package is built when its sources are there and its .u is not (as the MakeCommandlet decides)
    std::vector<std::string> toBuild;
    for (const std::string& name : packages) {
        if (std::filesystem::is_directory(run / name / "Classes") && !std::filesystem::exists(system / (name + ".u"))) {
            toBuild.push_back(name);
        }
    }
    // the rest of the ini's packages, in its order: a script may name a class of any of them
    for (const std::string& name : packages) {
        bool building = false;
        for (const std::string& build : toBuild) {
            if (lowerCase(build) == lowerCase(name)) {
                building = true;
                break;
            }
        }
        if (building) {
            continue;
        }
        dx::Linker* linker = dx::objects::loadPackage(dx::Name(name));
        if (linker) {
            loadFully(linker);
        }
    }
    int result = 0;
    for (const std::string& name : toBuild) {
        Host->print(("--------------------" + name + "--------------------").c_str());
        std::filesystem::path output = system / (name + ".u");
        std::filesystem::path sources = run / name / "Classes";
        if (!std::filesystem::is_directory(sources)) {
            sources = GameRoot / name / "Classes";
        }
        if (!buildPackage(GameRoot, name, sources, output)) {
            result = 1;
            break;
        }
    }
    say("%s - %d error(s), 0 warnings", result ? "Failure" : "Success", result ? std::max(errors, 1) : errors);
    return result ? 5 : 0;
}

static const UCBridgeApi Api = {
    UC_BRIDGE_VERSION,
    "OpenDE (dx-engine, dx-compiler)",
    start,
    stop,
    make,
    exec,
    actorCount,
    probePoint,
    traceLine,
    dumpBsp,
    listBrushes,
    unlinkBrushes,
    finishWorld,
    listSizes,
    checkActors,
    dumpLights,
    lightStats,
    clearCameras,
    setActor,
    deleteActor,
    setProp,
    scaleActor,
    setObjProp,
    editMover,
    dumpMeshes,
    setSurfTexture,
    editSurface,
    dumpPaths,
    dropNavPoints,
    dropNavTemp,
    liftNavTemp,
    keepPaths,
    listClasses,
    dumpDefaults,
    dumpClassProps,
    dumpTexture,
    dumpTexFlags,
    listSounds,
    listMeshes,
    dumpAnims,
    dumpScripts,
    dumpTexts,
    dumpConversations,
    conversationSound,
    loadPackage,
    importMesh,
    savePackage,
    newAnimMesh,
    saveAnimPackage,
};

extern "C" const UCBridgeApi* UCBridge_Api(int version)
{
    return version == UC_BRIDGE_VERSION ? &Api : nullptr;
}
