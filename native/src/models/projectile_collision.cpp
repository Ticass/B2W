// Missile-only prop collision (see projectile_collision.h).
#include "projectile_collision.h"

#include "core/asset_paths.h"
#include "core/files.h"
#include "core/hash.h"
#include "materials/techsets.h"
#include "world/collision_world.h"
#include "world/render_world.h"

#include <tomcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace w2b::models
{
    namespace fs = std::filesystem;
    using util::Json;
    using V = std::array<double, 3>;
    using Tri = std::array<V, 3>;

    namespace
    {
        constexpr int MISSILECLIP = 0x80, MOVEMENT_CLIP = 0x30000;
        // WaW SurfaceTypeBits stores enum value - 1: bark, brick, concrete, metal,
        // plaster, rock, wood, ceramic, plastic, rubber and painted metal
        constexpr unsigned HARD_SURFACES = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 12) | (1u << 15) | (1u << 16) | (1u << 20) | (1u << 22) | (1u << 23) |
                                           (1u << 24) | (1u << 27);

        struct Error : std::runtime_error
        {
            using std::runtime_error::runtime_error;
        };

        double Dot(const V& a, const V& b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }
        V Sub(const V& a, const V& b)
        {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }
        V Cross(const V& a, const V& b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }
        V Unit(const V& a)
        {
            const double length = std::sqrt(Dot(a, a));
            return {a[0] / length, a[1] / length, a[2] / length};
        }

        std::optional<V> Solve3(const std::array<V, 3>& rows, const V& rhs)
        {
            const double det = Dot(rows[0], Cross(rows[1], rows[2]));
            if (std::abs(det) < 1e-12)
                return std::nullopt;
            const auto c12 = Cross(rows[1], rows[2]), c20 = Cross(rows[2], rows[0]), c01 = Cross(rows[0], rows[1]);
            V out{};
            for (int i = 0; i < 3; i++)
                out[i] = (rhs[0] * c12[i] + rhs[1] * c20[i] + rhs[2] * c01[i]) / det;
            return out;
        }

        // hulls.collision_triangle: model-space corners of one XModelCollTri_s
        std::optional<Tri> CollisionTriangle(const std::array<double, 12>& tri)
        {
            const V n{tri[0], tri[1], tri[2]}, sv{tri[4], tri[5], tri[6]}, tv{tri[8], tri[9], tri[10]};
            const double d = tri[3], sw = tri[7], tw = tri[11];
            Tri corners{};
            const double st[3][2] = {{0, 0}, {1, 0}, {0, 1}};
            for (int i = 0; i < 3; i++)
            {
                const auto p = Solve3({n, sv, tv}, {d, st[i][0] + sw, st[i][1] + tw});
                if (!p)
                    return std::nullopt;
                corners[i] = *p;
            }
            return corners;
        }

        // hulls.collision_tri_record
        Json CollisionTriRecord(const V& a, const V& b, const V& c)
        {
            const auto e1 = Sub(b, a), e2 = Sub(c, a);
            const auto n = Unit(Cross(e2, e1));
            const auto svec = Solve3({e1, e2, n}, {1.0, 0.0, 0.0}), tvec = Solve3({e1, e2, n}, {0.0, 1.0, 0.0});
            if (!svec || !tvec)
                throw Error("degenerate collision triangle");
            const auto plane = [&](const V& v) { return Json::array({v[0], v[1], v[2], Dot(v, a)}); };
            return Json{{"plane", plane(n)}, {"svec", plane(*svec)}, {"tvec", plane(*tvec)}};
        }

        // hulls.Placement: world transform of a static model from its clipmap entry
        struct Placement
        {
            std::array<V, 3> inv{};
            V origin{};

            Placement(const V& o, const std::array<double, 9>& a) : origin(o)
            {
                const std::array<V, 3> m{V{a[0], a[3], a[6]}, V{a[1], a[4], a[7]}, V{a[2], a[5], a[8]}};
                const double det = Dot(m[0], Cross(m[1], m[2]));
                if (std::abs(det) < 1e-12)
                    throw Error("degenerate static model transform");
                const auto c0 = Cross(m[1], m[2]), c1 = Cross(m[2], m[0]), c2 = Cross(m[0], m[1]);
                for (int i = 0; i < 3; i++)
                    inv[i] = {c0[i] / det, c1[i] / det, c2[i] / det};
            }

            V Apply(const V& p) const
            {
                return {origin[0] + Dot(inv[0], p), origin[1] + Dot(inv[1], p), origin[2] + Dot(inv[2], p)};
            }
        };

        std::string Str(fs::path p)
        {
            return files::Utf8(p.make_preferred());
        }

        bool IsFile(const fs::path& p)
        {
            std::error_code e;
            return fs::is_regular_file(p, e);
        }

        std::string Bytes(const fs::path& p)
        {
            std::string data;
            if (!files::ReadBytes(p, data))
                throw Error("[Errno 2] No such file or directory: '" + Str(p) + "'");
            return data;
        }

        Json ReadJson(const fs::path& p)
        {
            return Json::parse(files::ReadText(p));
        }

        std::string Base64Decode(std::string_view text)
        {
            unsigned long size = static_cast<unsigned long>(text.size() / 4 * 3 + 3);
            std::string out(size, '\0');
            if (base64_decode(text.data(), static_cast<unsigned long>(text.size()), reinterpret_cast<unsigned char*>(out.data()), &size) != CRYPT_OK)
                throw Error("Incorrect padding");
            out.resize(size);
            return out;
        }

        std::string Base64Encode(const std::string& data)
        {
            unsigned long size = static_cast<unsigned long>(4 * ((data.size() + 2) / 3) + 1);
            std::string out(size, '\0');
            if (base64_encode(reinterpret_cast<const unsigned char*>(data.data()), static_cast<unsigned long>(data.size()), out.data(), &size) != CRYPT_OK)
                throw Error("could not encode collision buffer");
            out.resize(size);
            return out;
        }

        // accessor(): one GLTF accessor's elements as doubles
        std::vector<std::vector<double>> Accessor(const Json& data, const std::vector<std::string>& buffers, const size_t index)
        {
            const auto& a = data["accessors"][index];
            if (a.contains("sparse") && !a["sparse"].is_null() && !a["sparse"].empty())
                throw Error("sparse GLTF accessor unsupported for prop collision");
            const auto& view = data["bufferViews"][a["bufferView"].get<size_t>()];
            const int type = a["componentType"].get<int>();
            const size_t component = type == 5121 ? 1 : type == 5123 ? 2 : 4;
            const auto kind = a["type"].get<std::string>();
            const size_t width = kind == "SCALAR" ? 1 : kind == "VEC2" ? 2 : kind == "VEC3" ? 3 : 4;
            const size_t stride = view.contains("byteStride") ? view["byteStride"].get<size_t>() : component * width;
            const size_t offset = view.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
            const auto& buffer = buffers[view["buffer"].get<size_t>()];
            const size_t count = a["count"].get<size_t>();
            std::vector<std::vector<double>> out(count, std::vector<double>(width));
            for (size_t i = 0; i < count; i++)
                for (size_t c = 0; c < width; c++)
                {
                    const size_t at = offset + i * stride + c * component;
                    if (at + component > buffer.size())
                        throw Error("unpack_from requires a buffer of at least " + std::to_string(at + component) + " bytes");
                    if (type == 5121)
                        out[i][c] = static_cast<unsigned char>(buffer[at]);
                    else if (type == 5123)
                    {
                        uint16_t v;
                        std::memcpy(&v, buffer.data() + at, 2);
                        out[i][c] = v;
                    }
                    else if (type == 5125)
                    {
                        uint32_t v;
                        std::memcpy(&v, buffer.data() + at, 4);
                        out[i][c] = v;
                    }
                    else
                    {
                        float v;
                        std::memcpy(&v, buffer.data() + at, 4);
                        out[i][c] = v;
                    }
                }
            return out;
        }

        std::vector<Tri> HardTriangles(const fs::path& path, const std::vector<fs::path>& roots)
        {
            const auto data = ReadJson(path);
            std::vector<std::string> buffers;
            for (const auto& buffer : data["buffers"])
            {
                const auto uri = buffer["uri"].get<std::string>();
                buffers.push_back(uri.rfind("data:", 0) == 0 ? Base64Decode(std::string_view(uri).substr(uri.find(',') + 1)) : Bytes(path.parent_path() / files::FromUtf8(uri)));
            }
            // exported mesh vertices are in bind pose; reject mesh-node transforms
            for (const auto& node : data.value("nodes", Json::array()))
                if (node.contains("mesh") && (node.contains("matrix") || node.contains("translation") || node.contains("rotation") || node.contains("scale")))
                    throw Error("transformed GLTF mesh node unsupported for prop collision");
            std::vector<Tri> triangles;
            for (const auto& mesh : data["meshes"])
                for (const auto& primitive : mesh["primitives"])
                {
                    if (primitive.value("mode", 4) != 4 || !primitive.contains("material"))
                        continue;
                    const auto name = data["materials"][primitive["material"].get<size_t>()]["name"].get<std::string>();
                    std::optional<fs::path> materialPath;
                    for (const auto& r : roots)
                        if (IsFile(r / assets::MaterialPath(name)))
                        {
                            materialPath = r / assets::MaterialPath(name);
                            break;
                        }
                    if (!materialPath)
                        continue;
                    const auto material = ReadJson(*materialPath);
                    const auto surface = material.value("surfaceTypeBits", 0u);
                    const auto shader = materials::ParseTechnique(material.value("techniqueSet", std::string()));
                    // an imported prop with WaW's default surface type: only an
                    // opaque lit model replace pass qualifies
                    const bool opaqueDefault = surface == 0 && (shader.family == materials::Family::model || shader.family == materials::Family::modelVertex) &&
                                               shader.lit && !shader.layers.empty() && shader.layers[0].blend == 'r' && material.contains("sortKey") &&
                                               material["sortKey"].is_number() && material["sortKey"].get<double>() == 4;
                    if (!((surface & HARD_SURFACES) || opaqueDefault))
                        continue;
                    std::vector<V> vertices;
                    for (const auto& v : Accessor(data, buffers, primitive["attributes"]["POSITION"].get<size_t>()))
                        vertices.push_back({v[0], -v[2], v[1]});
                    std::vector<size_t> indices;
                    if (primitive.contains("indices"))
                        for (const auto& v : Accessor(data, buffers, primitive["indices"].get<size_t>()))
                            indices.push_back(static_cast<size_t>(v[0]));
                    else
                        for (size_t i = 0; i < vertices.size(); i++)
                            indices.push_back(i);
                    for (size_t i = 0; i < indices.size(); i += 3)
                    {
                        if (i + 3 > indices.size())
                            throw Error("not enough values to unpack (expected 3, got " + std::to_string(indices.size() - i) + ")");
                        // GLTF indices are opposite the game's collision winding
                        const auto &a = vertices.at(indices[i]), &b = vertices.at(indices[i + 1]), &c = vertices.at(indices[i + 2]);
                        const auto cr = Cross(Sub(b, a), Sub(c, a));
                        if (Dot(cr, cr) > 1e-10)
                            triangles.push_back({a, c, b});
                    }
                }
            return triangles;
        }

        // clip_polygon: the surface inside the convex brush halfspaces
        std::vector<V> ClipPolygon(std::vector<V> polygon, const std::vector<world::BrushPlane>& planes)
        {
            for (const auto& plane : planes)
            {
                if (polygon.empty())
                    break;
                const V normal{plane.normal[0], plane.normal[1], plane.normal[2]};
                const double distance = plane.distance;
                std::vector<V> result;
                V previous = polygon.back();
                double pd = Dot(normal, previous) - distance;
                for (const auto& current : polygon)
                {
                    const double cd = Dot(normal, current) - distance;
                    if ((pd <= 0) != (cd <= 0))
                    {
                        const double t = pd / (pd - cd);
                        result.push_back({previous[0] + t * (current[0] - previous[0]), previous[1] + t * (current[1] - previous[1]), previous[2] + t * (current[2] - previous[2])});
                    }
                    if (cd <= 0)
                        result.push_back(current);
                    previous = current, pd = cd;
                }
                polygon = std::move(result);
            }
            return polygon;
        }

        // collision_lod: the one face a collision-only model needs as its LOD
        void CollisionLod(const fs::path& path, const Tri& triangle, const std::string& material)
        {
            const auto normal = Unit(Cross(Sub(triangle[2], triangle[0]), Sub(triangle[1], triangle[0])));
            const auto rotate = [](const V& p) { return V{p[0], p[2], -p[1]}; };
            std::array<V, 3> positions{rotate(triangle[0]), rotate(triangle[1]), rotate(triangle[2])};
            std::string blob;
            const auto put = [&](const double v)
            {
                const float f = static_cast<float>(v);
                blob.append(reinterpret_cast<const char*>(&f), 4);
            };
            for (const auto& p : positions)
                for (const double v : p)
                    put(v);
            const auto rn = rotate(normal);
            for (int i = 0; i < 3; i++)
                for (const double v : rn)
                    put(v);
            for (const double v : {0., 0., 0., 1., 1., 0.})
                put(v);
            for (const uint16_t v : std::initializer_list<uint16_t>{0, 2, 1})
                blob.append(reinterpret_cast<const char*>(&v), 2);
            Json mins = Json::array(), maxs = Json::array();
            for (int k = 0; k < 3; k++)
            {
                mins.push_back(std::min({positions[0][k], positions[1][k], positions[2][k]}));
                maxs.push_back(std::max({positions[0][k], positions[1][k], positions[2][k]}));
            }
            Json views = Json::array();
            for (const auto& [o, n] : {std::pair{0, 36}, {36, 36}, {72, 24}, {96, 6}})
                views.push_back(Json{{"buffer", 0}, {"byteOffset", o}, {"byteLength", n}});
            const Json data{{"asset", Json{{"version", "2.0"}}},
                            {"scene", 0},
                            {"scenes", Json::array({Json{{"nodes", Json::array({0})}}})},
                            {"nodes", Json::array({Json{{"mesh", 0}}})},
                            {"materials", Json::array({Json{{"name", material}}})},
                            {"buffers", Json::array({Json{{"byteLength", blob.size()}, {"uri", "data:application/octet-stream;base64," + Base64Encode(blob)}}})},
                            {"bufferViews", views},
                            {"accessors", Json::array({Json{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", mins}, {"max", maxs}},
                                                       Json{{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
                                                       Json{{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
                                                       Json{{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}}})},
                            {"meshes", Json::array({Json{{"primitives", Json::array({Json{{"attributes", Json{{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                                                                          {"indices", 3},
                                                                                          {"material", 0}}})}}})}};
            fs::create_directories(path.parent_path());
            files::WriteText(path, util::JsonDumpsDefault(data) + "\n");
        }

        // round(v, 5): the double nearest the correctly rounded decimal
        double Round5(const double v)
        {
            char text[64];
            std::snprintf(text, sizeof text, "%.5f", v);
            return std::strtod(text, nullptr);
        }

        std::array<double, 12> TriArray(const Json& t)
        {
            std::array<double, 12> out{};
            size_t i = 0;
            for (const char* key : {"plane", "svec", "tvec"})
                for (const auto& v : t[key])
                    out[i++] = v.get<double>();
            return out;
        }

        long long Floor(const double v)
        {
            return static_cast<long long>(std::floor(v));
        }

        // batch_proxies: nearby collision surfaces packed into shared models
        std::pair<Json, std::set<std::string>> BatchProxies(const Json& records, Json& entries, const fs::path& projectRoot)
        {
            std::map<std::array<long long, 3>, std::vector<size_t>> groups;
            for (size_t i = 0; i < records.size(); i++)
            {
                const auto& r = records[i];
                std::array<long long, 3> cell{};
                for (int k = 0; k < 3; k++)
                    cell[k] = Floor((r["absmin"][k].get<double>() + r["absmax"][k].get<double>()) / 512);
                groups[cell].push_back(i);
            }
            Json output = Json::array();
            std::set<std::string> names;
            for (const auto& [cell, group] : groups)
                for (size_t start = 0; start < group.size(); start += 64)
                {
                    const std::vector<size_t> chunk(group.begin() + static_cast<long long>(start), group.begin() + static_cast<long long>(std::min(group.size(), start + 64)));
                    for (const auto i : chunk)
                        entries[i]["source_collision_asset"] = records[i]["name"];
                    if (chunk.size() == 1)
                    {
                        output.push_back(records[chunk[0]]);
                        names.insert(records[chunk[0]]["name"].get<std::string>());
                        entries[chunk[0]]["surface"] = 0;
                        continue;
                    }
                    const V origin{(cell[0] + 0.5) * 256, (cell[1] + 0.5) * 256, (cell[2] + 0.5) * 256};
                    Json surfaces = Json::array();
                    Json model;
                    for (const auto i : chunk)
                    {
                        const auto& record = records[i];
                        model = ReadJson(projectRoot / "xmodel" / files::FromUtf8(record["name"].get<std::string>() + ".json"));
                        std::array<double, 9> axis{};
                        for (int k = 0; k < 9; k++)
                            axis[k] = record["invScaledAxis"][k].get<double>();
                        const Placement placement({record["origin"][0].get<double>(), record["origin"][1].get<double>(), record["origin"][2].get<double>()}, axis);
                        const auto transform = [&](const V& p)
                        {
                            const auto w = placement.Apply(p);
                            return V{w[0] - origin[0], w[1] - origin[1], w[2] - origin[2]};
                        };
                        for (const auto& surf : model["collSurfs"])
                        {
                            std::vector<Tri> tris;
                            for (const auto& t : surf["tris"])
                            {
                                const auto corners = CollisionTriangle(TriArray(t));
                                if (!corners)
                                    throw Error("'NoneType' object is not iterable");
                                tris.push_back({transform((*corners)[0]), transform((*corners)[1]), transform((*corners)[2])});
                            }
                            Json mins = Json::array(), maxs = Json::array(), triRecords = Json::array();
                            for (int k = 0; k < 3; k++)
                            {
                                double lo = tris[0][0][k], hi = tris[0][0][k];
                                for (const auto& tri : tris)
                                    for (const auto& p : tri)
                                        lo = std::min(lo, p[k]), hi = std::max(hi, p[k]);
                                mins.push_back(lo), maxs.push_back(hi);
                            }
                            for (const auto& tri : tris)
                                triRecords.push_back(CollisionTriRecord(tri[0], tri[1], tri[2]));
                            surfaces.push_back(Json{{"bone", "tag_origin"}, {"contents", MISSILECLIP}, {"surfFlags", surf["surfFlags"]}, {"mins", mins}, {"maxs", maxs}, {"tris", triRecords}});
                        }
                        entries[i]["surface"] = surfaces.size() - 1;
                    }
                    // include placement transforms and bounds: equal model names
                    // do not imply equal world-space batches
                    Json chunkRecords = Json::array();
                    for (const auto i : chunk)
                        chunkRecords.push_back(records[i]);
                    const auto digest = hash::Sha256Hex(util::JsonDumpsSorted(chunkRecords)).substr(0, 16);
                    const auto name = "waw_projectile/cell_" + std::to_string(cell[0]) + "_" + std::to_string(cell[1]) + "_" + std::to_string(cell[2]) + "_" + digest;
                    const auto lod = "model_export/" + name + ".gltf";
                    const auto carrier = ReadJson(projectRoot / files::FromUtf8(model["lods"][0]["file"].get<std::string>()));
                    const auto first = TriArray(surfaces[0]["tris"][0]);
                    const auto corners = CollisionTriangle(first);
                    if (!corners)
                        throw Error("'NoneType' object is not subscriptable");
                    CollisionLod(projectRoot / files::FromUtf8(lod), *corners, carrier["materials"][0]["name"].get<std::string>());
                    model["lods"] = Json::array({Json{{"file", lod}, {"distance", 100000}}});
                    model["collSurfs"] = surfaces;
                    files::WriteText(projectRoot / "xmodel" / files::FromUtf8(name + ".json"), util::JsonDumps(model, 1) + "\n");
                    Json absmin = Json::array(), absmax = Json::array();
                    for (int k = 0; k < 3; k++)
                    {
                        double lo = records[chunk[0]]["absmin"][k].get<double>(), hi = records[chunk[0]]["absmax"][k].get<double>();
                        for (const auto i : chunk)
                            lo = std::min(lo, records[i]["absmin"][k].get<double>()), hi = std::max(hi, records[i]["absmax"][k].get<double>());
                        absmin.push_back(lo), absmax.push_back(hi);
                    }
                    output.push_back(Json{{"name", name},
                                          {"contents", MISSILECLIP},
                                          {"origin", Json::array({origin[0], origin[1], origin[2]})},
                                          {"invScaledAxis", Json::array({1, 0, 0, 0, 1, 0, 0, 0, 1})},
                                          {"absmin", absmin},
                                          {"absmax", absmax}});
                    names.insert(name);
                    for (const auto i : chunk)
                        entries[i]["name"] = name;
                }
            return {output, names};
        }
    } // namespace

    ProjectileCollision RecoverProjectileCollision(const ProjectileCollisionRequest& request)
    {
        const auto& projectRoot = request.projectRoot;
        const auto& roots = request.roots;
        const auto clip = world::ReadCollisionWorld(request.clip);
        std::vector<std::string> warnings;
        if (!clip.brushOwnershipComplete || clip.models.empty())
        {
            warnings.push_back("PROJECTILE_PROP_COLLISION_UNSUPPORTED: complete source brush ownership required");
            return {{}, std::nullopt, std::move(warnings)};
        }
        const auto worldModel = world::ReadRenderWorld(request.gfx, true);
        const std::set<std::uint16_t> worldIds(clip.models[0].brushes.begin(), clip.models[0].brushes.end());
        std::set<std::uint16_t> entityIds;
        for (size_t i = 1; i < clip.models.size(); i++)
            entityIds.insert(clip.models[i].brushes.begin(), clip.models[i].brushes.end());
        std::vector<const world::ClipBrush*> brushes;
        for (const auto id : worldIds)
            if (!entityIds.count(id))
            {
                const auto& b = clip.brushes.at(id);
                if ((b.contents & MOVEMENT_CLIP) && !(b.contents & (1 | MISSILECLIP)))
                    brushes.push_back(&b);
            }
        const auto recordsPath = projectRoot / "BSP/staticmodels.json";
        auto records = ReadJson(recordsPath);
        {
            // repeated selective staging is idempotent
            Json kept = Json::array();
            for (const auto& m : records["staticModels"])
                if (m["name"].get<std::string>().rfind("waw_projectile/", 0) != 0)
                    kept.push_back(m);
            records["staticModels"] = kept;
        }
        const size_t originalRecords = records["staticModels"].size();
        std::map<std::string, std::vector<Tri>> cache;
        Json entries = Json::array();
        for (size_t index = 0; index < worldModel.staticModels.size(); index++)
        {
            const auto& placement = worldModel.staticModels[index];
            const auto& name = placement.name;
            const auto modelPath = projectRoot / "xmodel" / files::FromUtf8(name + ".json");
            if (!IsFile(modelPath))
                continue;
            const auto model = ReadJson(modelPath);
            // existing collision (including map-placed entity boxes) owns this model
            const bool collides = model.contains("collSurfs") && !model["collSurfs"].is_null() && !model["collSurfs"].empty();
            if (collides || (model.value("contents", 0) & (1 | MISSILECLIP)))
                continue;
            if (!cache.count(name))
            {
                try
                {
                    if (!model.contains("lods"))
                        throw Error("'lods'");
                    cache[name] = HardTriangles(projectRoot / files::FromUtf8(model["lods"][0]["file"].get<std::string>()), roots);
                }
                catch (const std::exception& exc)
                {
                    warnings.push_back("PROJECTILE_PROP_COLLISION_UNSUPPORTED " + name + ": " + exc.what());
                    cache[name] = {};
                }
            }
            const auto& triangles = cache[name];
            if (triangles.empty() || placement.scale <= 0)
                continue;
            const V origin{placement.origin[0], placement.origin[1], placement.origin[2]};
            const double scale = placement.scale;
            std::array<double, 9> axis{};
            for (int k = 0; k < 9; k++)
                axis[k] = placement.axis[k];
            const auto transform = [&](const V& p)
            {
                V out{};
                for (int k = 0; k < 3; k++)
                {
                    double sum = 0;
                    for (int j = 0; j < 3; j++)
                        sum = sum + axis[3 * j + k] * p[j];
                    out[k] = origin[k] + scale * sum;
                }
                return out;
            };
            V mins{}, maxs{};
            bool firstPoint = true;
            for (const auto& tri : triangles)
                for (const auto& p : tri)
                {
                    const auto v = transform(p);
                    for (int k = 0; k < 3; k++)
                    {
                        mins[k] = firstPoint ? v[k] : std::min(mins[k], v[k]);
                        maxs[k] = firstPoint ? v[k] : std::max(maxs[k], v[k]);
                    }
                    firstPoint = false;
                }
            std::vector<const world::ClipBrush*> candidates;
            for (const auto* b : brushes)
                if (b->mins[0] <= maxs[0] && b->maxs[0] >= mins[0] && b->mins[1] <= maxs[1] && b->maxs[1] >= mins[1] && b->mins[2] <= maxs[2] && b->maxs[2] >= mins[2])
                    candidates.push_back(b);
            if (candidates.empty())
                continue;
            bool overlaps = false;
            for (const auto& triangle : triangles)
            {
                std::vector<V> points{transform(triangle[0]), transform(triangle[1]), transform(triangle[2])};
                V lo{}, hi{};
                for (int k = 0; k < 3; k++)
                    lo[k] = std::min({points[0][k], points[1][k], points[2][k]}), hi[k] = std::max({points[0][k], points[1][k], points[2][k]});
                for (const auto* brush : candidates)
                {
                    if (!(brush->mins[0] <= hi[0] && brush->maxs[0] >= lo[0] && brush->mins[1] <= hi[1] && brush->maxs[1] >= lo[1] && brush->mins[2] <= hi[2] && brush->maxs[2] >= lo[2]))
                        continue;
                    const auto polygon = ClipPolygon(points, brush->planes);
                    for (size_t j = 1; j + 1 < polygon.size(); j++)
                    {
                        const auto cross = Cross(Sub(polygon[j], polygon[0]), Sub(polygon[j + 1], polygon[0]));
                        if (Dot(cross, cross) < 1e-10)
                            continue;
                        overlaps = true;
                        break;
                    }
                    if (overlaps)
                        break;
                }
                if (overlaps)
                    break;
            }
            if (!overlaps)
                continue;
            // missile-only recovery is two-sided: render meshes can hold open
            // sheets and inconsistent back faces
            std::vector<std::pair<std::array<double, 9>, Tri>> recovered; // dict: first position, last value
            for (const auto& [a, b, c] : triangles)
                for (const Tri& local : {Tri{a, b, c}, Tri{a, c, b}})
                {
                    std::array<double, 9> key{};
                    for (int p = 0; p < 3; p++)
                        for (int k = 0; k < 3; k++)
                            key[3 * p + k] = Round5(local[p][k]);
                    const auto it = std::find_if(recovered.begin(), recovered.end(), [&](const auto& kv) { return kv.first == key; });
                    if (it != recovered.end())
                        it->second = local;
                    else
                        recovered.emplace_back(key, local);
                }
            std::vector<std::array<double, 9>> keys;
            for (const auto& [k, _] : recovered)
                keys.push_back(k);
            std::sort(keys.begin(), keys.end());
            Json keyJson = Json::array();
            for (const auto& k : keys)
                keyJson.push_back(Json(std::vector<double>(k.begin(), k.end())));
            const auto digest = hash::Sha256Hex(util::JsonDumpsDefault(keyJson)).substr(0, 16);
            const auto output = "waw_projectile/" + name + "_" + digest;
            std::vector<V> points;
            for (const auto& [_, tri] : recovered)
                points.insert(points.end(), tri.begin(), tri.end());
            Json lo = Json::array(), hi = Json::array();
            for (int k = 0; k < 3; k++)
            {
                double l = points[0][k], h = points[0][k];
                for (const auto& p : points)
                    l = std::min(l, p[k]), h = std::max(h, p[k]);
                lo.push_back(l), hi.push_back(h);
            }
            auto proxy = model;
            const auto sourceMesh = ReadJson(projectRoot / files::FromUtf8(model["lods"][0]["file"].get<std::string>()));
            const auto lodFile = "model_export/" + output + ".gltf";
            CollisionLod(projectRoot / files::FromUtf8(lodFile), recovered.front().second, sourceMesh["materials"][0]["name"].get<std::string>());
            proxy["type"] = "rigid";
            proxy["rootBoneName"] = "tag_origin";
            proxy["lods"] = Json::array({Json{{"file", lodFile}, {"distance", 100000}}});
            proxy["contents"] = MISSILECLIP;
            proxy["collLod"] = 0;
            Json tris = Json::array();
            for (const auto& [_, tri] : recovered)
                tris.push_back(CollisionTriRecord(tri[0], tri[1], tri[2]));
            proxy["collSurfs"] = Json::array({Json{{"bone", "tag_origin"}, {"contents", MISSILECLIP}, {"surfFlags", 0}, {"mins", lo}, {"maxs", hi}, {"tris", tris}}});
            const auto target = projectRoot / "xmodel" / files::FromUtf8(output + ".json");
            fs::create_directories(target.parent_path());
            files::WriteText(target, util::JsonDumps(proxy, 1) + "\n");
            Json absmin = Json::array(), absmax = Json::array();
            for (int k = 0; k < 3; k++)
            {
                double l = 0, h = 0;
                bool first = true;
                for (const auto& p : points)
                {
                    const double v = transform(p)[k];
                    l = first ? v : std::min(l, v), h = first ? v : std::max(h, v);
                    first = false;
                }
                absmin.push_back(l), absmax.push_back(h);
            }
            Json inverse = Json::array();
            for (const double v : axis)
                inverse.push_back(v / scale);
            records["staticModels"].push_back(Json{{"name", output},
                                                   {"contents", MISSILECLIP},
                                                   {"origin", Json::array({origin[0], origin[1], origin[2]})},
                                                   {"invScaledAxis", inverse},
                                                   {"absmin", absmin},
                                                   {"absmax", absmax}});
            entries.push_back(Json{{"source_model", name}, {"placement", index}, {"name", output}, {"triangles", recovered.size()}, {"source_movement_brushes", candidates.size()}});
        }
        Json added = Json::array();
        for (size_t i = originalRecords; i < records["staticModels"].size(); i++)
            added.push_back(records["staticModels"][i]);
        auto [batched, names] = BatchProxies(added, entries, projectRoot);
        Json all = Json::array();
        for (size_t i = 0; i < originalRecords; i++)
            all.push_back(records["staticModels"][i]);
        for (const auto& r : batched)
            all.push_back(r);
        records["staticModels"] = all;
        files::WriteText(recordsPath, util::JsonDumps(records, 1) + "\n");
        warnings.push_back("PROJECTILE_PROP_COLLISION_RECOVERED " + std::to_string(entries.size()) +
                           " placements: hard render surfaces complete two-sided hard meshes qualified by source movement brushes; missile only, source masks unchanged");
        return {{names.begin(), names.end()}, std::move(entries), std::move(warnings)};
    }
} // namespace w2b::models
