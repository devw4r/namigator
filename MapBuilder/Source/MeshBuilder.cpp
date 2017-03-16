#include "MeshBuilder.hpp"
#include "RecastContext.hpp"

#include "parser/Include/parser.hpp"
#include "parser/Include/Adt/Adt.hpp"
#include "parser/Include/Adt/AdtChunk.hpp"
#include "parser/Include/DBC.hpp"

#include "utility/Include/MathHelper.hpp"
#include "utility/Include/LinearAlgebra.hpp"
#include "utility/Include/AABBTree.hpp"
#include "utility/Include/Exception.hpp"

#include "RecastDetourBuild/Include/Common.hpp"
#include "RecastDetourBuild/Include/TileCacheCompressor.hpp"

#include "recastnavigation/Recast/Include/Recast.h"
#include "recastnavigation/Detour/Include/DetourNavMeshBuilder.h"
#include "recastnavigation/Detour/Include/DetourAlloc.h"
#include "recastnavigation/DetourTileCache/Include/DetourTileCacheBuilder.h"

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <iomanip>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <iostream>
#include <limits>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <unordered_map>
#include <experimental/filesystem>

#define ZERO(x) memset(&x, 0, sizeof(x))

static_assert(sizeof(int) == sizeof(std::int32_t), "Recast requires 32 bit int type");
static_assert(sizeof(float) == 4, "float must be a 32 bit type");
static_assert(sizeof(unsigned char) == 1, "unsigned char must be size 1");

//#define DISABLE_SELECTIVE_FILTERING

namespace
{
std::experimental::filesystem::path BuildAbsoluteFilename(const std::experimental::filesystem::path &outputPath, const std::string &in)
{
    const std::experimental::filesystem::path path(in);
    auto const filename = path.filename().replace_extension("bvh").string();

    auto const extension = path.extension().string();

    std::string kind;
    if (extension[1] == 'm' || extension[1] == 'M')
        kind = "Doodad";
    else if (extension[1] == 'w' || extension[1] == 'W')
        kind = "WMO";
    else
        THROW("Unrecognized extension");

    return outputPath / "BVH" / (kind + "_" + filename);
}

void ComputeRequiredChunks(const parser::Map *map, int tileX, int tileY, std::vector<std::pair<int, int>> &chunks)
{
    chunks.clear();
    
    constexpr float ChunksPerTile = MeshSettings::TileSize / MeshSettings::AdtChunkSize;

    // NOTE: these are global chunk coordinates, not ADT or tile
    auto const startX = static_cast<int>((tileX - 1) * ChunksPerTile);
    auto const startY = static_cast<int>((tileY - 1) * ChunksPerTile);
    auto const stopX = static_cast<int>((tileX + 1) * ChunksPerTile);
    auto const stopY = static_cast<int>((tileY + 1) * ChunksPerTile);

    for (auto y = startY; y <= stopY; ++y)
        for (auto x = startX; x <= stopX; ++x)
        {
            auto const adtX = x / MeshSettings::ChunksPerAdt;
            auto const adtY = y / MeshSettings::ChunksPerAdt;

            if (adtX < 0 || adtY < 0 || adtX >= MeshSettings::Adts || adtY >= MeshSettings::Adts)
                continue;

            if (map->HasAdt(adtX, adtY))
                chunks.push_back({ x, y });
        }
}

bool TransformAndRasterize(rcContext &ctx, rcHeightfield &heightField, bool filterWalkable, float slope,
               const std::vector<utility::Vertex> &vertices, const std::vector<int> &indices, unsigned char areaFlags)
{
    if (!vertices.size() || !indices.size())
        return true;

    std::vector<float> rastVert;
    utility::Convert::VerticesToRecast(vertices, rastVert);

    std::vector<unsigned char> areas(indices.size() / 3, areaFlags);

    if (filterWalkable)
        rcClearUnwalkableTriangles(&ctx, slope, &rastVert[0], static_cast<int>(vertices.size()), &indices[0], static_cast<int>(indices.size() / 3), &areas[0]);

    return rcRasterizeTriangles(&ctx, &rastVert[0], static_cast<int>(vertices.size()), &indices[0], &areas[0], static_cast<int>(indices.size() / 3), heightField);
}

void FilterGroundBeneathLiquid(rcHeightfield &solid)
{
    for (int i = 0; i < solid.width*solid.height; ++i)
    {
        // as we go, we build a list of spans which will be removed in the event we find liquid
        std::vector<rcSpan *> spans;

        for (rcSpan *s = solid.spans[i]; s; s = s->next)
        {
            // if we found a non-wmo liquid span, remove everything beneath it
            if (!!(s->area & AreaFlags::Liquid) && !(s->area & AreaFlags::WMO))
            {
                for (auto ns : spans)
                    ns->area = RC_NULL_AREA;

                spans.clear();
            }
            // if we found a wmo liquid span, remove every wmo span beneath it
            else if (!!(s->area & (AreaFlags::Liquid | AreaFlags::WMO)))
            {
                for (auto ns : spans)
                    if (!!(ns->area & AreaFlags::WMO))
                        ns->area = RC_NULL_AREA;

                spans.clear();
            }
            else
                spans.push_back(s);
        }
    }
}

void RestoreAdtSpans(const std::vector<rcSpan *> &spans)
{
    for (auto s : spans)
        s->area |= AreaFlags::ADT;
}

// Recast does not support multiple walkable climb values.  However, when being used for NPCs, who can walk up ADT terrain of any slope, this is what we need.
// As a workaround for this situation, we will have Recast build the compact height field with an 'infinite' value for walkable climb, and then run our own
// custom filter on the compact heightfield to enforce the walkable climb for WMOs and doodads
void SelectivelyEnforceWalkableClimb(rcCompactHeightfield &chf, int walkableClimb)
{
    for (int y = 0; y < chf.height; ++y)
        for (int x = 0; x < chf.width; ++x)
        {
            auto const &cell = chf.cells[y*chf.width + x];

            // for each span in this cell of the compact heightfield...
            for (int i = static_cast<int>(cell.index), ni = static_cast<int>(cell.index + cell.count); i < ni; ++i)
            {
                auto &span = chf.spans[i];
                const AreaFlags spanArea = static_cast<AreaFlags>(chf.areas[i]);

                // check all four directions for this span
                for (int dir = 0; dir < 4; ++dir)
                {
                    // there will be at most one connection for this span in this direction
                    auto const k = rcGetCon(span, dir);

                    // if there is no connection, do nothing
                    if (k == RC_NOT_CONNECTED)
                        continue;

                    auto const nx = x + rcGetDirOffsetX(dir);
                    auto const ny = y + rcGetDirOffsetY(dir);

                    // this should never happen since we already know there is a connection in this direction
                    assert(nx >= 0 && ny >= 0 && nx < chf.width && ny < chf.height);

                    auto const &neighborCell = chf.cells[ny*chf.width + nx];
                    auto const &neighborSpan = chf.spans[k + neighborCell.index];

                    // if the span height difference is <= the walkable climb, nothing else matters.  skip it
                    if (rcAbs(static_cast<int>(neighborSpan.y) - static_cast<int>(span.y)) <= walkableClimb)
                        continue;

                    const AreaFlags neighborSpanArea = static_cast<AreaFlags>(chf.areas[k + neighborCell.index]);

                    // if both the current span and the neighbor span are ADTs, do nothing
                    if (spanArea == AreaFlags::ADT && neighborSpanArea == AreaFlags::ADT)
                        continue;

                    rcSetCon(span, dir, RC_NOT_CONNECTED);
                }
            }
        }
}

// NOTE: this does not set bmin/bmax
void InitializeRecastConfig(rcConfig &config)
{
    ZERO(config);

    config.cs = MeshSettings::CellSize;
    config.ch = MeshSettings::CellHeight;
    config.walkableSlopeAngle = MeshSettings::WalkableSlope;
    config.walkableClimb = MeshSettings::VoxelWalkableClimb;
    config.walkableHeight = MeshSettings::VoxelWalkableHeight;
    config.walkableRadius = MeshSettings::VoxelWalkableRadius;
    config.maxEdgeLen = config.walkableRadius * 4;
    config.maxSimplificationError = MeshSettings::MaxSimplificationError;
    config.minRegionArea = MeshSettings::MinRegionSize;
    config.mergeRegionArea = MeshSettings::MergeRegionSize;
    config.maxVertsPerPoly = MeshSettings::VerticesPerPolygon;
    config.tileSize = MeshSettings::TileVoxelSize;
    config.borderSize = config.walkableRadius + 3;
    config.width = config.tileSize + config.borderSize * 2;
    config.height = config.tileSize + config.borderSize * 2;
    config.detailSampleDist = MeshSettings::DetailSampleDistance;
    config.detailSampleMaxError = MeshSettings::DetailSampleMaxError;
}

void SerializeWMOAndDoodadIDs(const std::unordered_set<std::uint32_t> &wmos, const std::unordered_set<std::uint32_t> &doodads, utility::BinaryStream &out)
{
    utility::BinaryStream result(sizeof(std::uint32_t)*(2 + wmos.size() + doodads.size()));

    result << static_cast<std::uint32_t>(wmos.size());

    for (auto const &wmo : wmos)
        result << wmo;

    result << static_cast<std::uint32_t>(doodads.size());

    for (auto const &doodad : doodads)
        result << doodad;

    out = std::move(result);
}

void SerializeHeightField(const rcHeightfield &solid, utility::BinaryStream &out)
{
    out << static_cast<std::int32_t>(solid.width) << static_cast<std::int32_t>(solid.height);

    out.Write(&solid.bmin, sizeof(solid.bmin));
    out.Write(&solid.bmax, sizeof(solid.bmax));

    out << solid.cs << solid.ch;

    // TODO this might be storable in less space, since rcSpan is a bitfield struct
    for (auto i = 0; i < solid.width*solid.height; ++i)
    {
        // place holder for the height of this column in the height field
        auto const columnSize = out.wpos();
        out << static_cast<std::uint32_t>(0);

        auto count = 0;
        for (const rcSpan *s = solid.spans[i]; !!s; s = s->next)
        {
            out << static_cast<std::uint32_t>(s->smin)
                << static_cast<std::uint32_t>(s->smax)
                << static_cast<std::uint32_t>(s->area);
            ++count;
        }

        auto const height = (out.wpos() - columnSize + sizeof(std::uint32_t)) / (3 * sizeof(std::uint32_t));

        out.Write(columnSize, static_cast<std::uint32_t>(height));
    }
}

using SmartHeightFieldPtr = std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)>;
using SmartHeightFieldLayerSetPtr = std::unique_ptr<rcHeightfieldLayerSet, decltype(&rcFreeHeightfieldLayerSet)>;
using SmartCompactHeightFieldPtr = std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)>;
using SmartContourSetPtr = std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)>;
using SmartPolyMeshPtr = std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)>;
using SmartPolyMeshDetailPtr = std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>;

bool SerializeMeshTile(rcContext &ctx, const rcConfig &config, int tileX, int tileY, rcHeightfield &solid, utility::BinaryStream &out)
{
    // initialize compact height field
    SmartCompactHeightFieldPtr chf(rcAllocCompactHeightfield(), rcFreeCompactHeightfield);

    if (!rcBuildCompactHeightfield(&ctx, config.walkableHeight, (std::numeric_limits<int>::max)(), solid, *chf))
        return false;

    SelectivelyEnforceWalkableClimb(*chf, config.walkableClimb);

    if (!rcBuildDistanceField(&ctx, *chf))
        return false;

    if (!rcBuildRegions(&ctx, *chf, config.borderSize, config.minRegionArea, config.mergeRegionArea))
        return false;

    SmartContourSetPtr cset(rcAllocContourSet(), rcFreeContourSet);

    if (!rcBuildContours(&ctx, *chf, config.maxSimplificationError, config.maxEdgeLen, *cset))
        return false;

    // it is possible that this tile has no navigable geometry.  in this case, we 'succeed' by doing nothing further
    if (!cset->nconts)
        return true;

    SmartPolyMeshPtr polyMesh(rcAllocPolyMesh(), rcFreePolyMesh);

    if (!rcBuildPolyMesh(&ctx, *cset, config.maxVertsPerPoly, *polyMesh))
        return false;

    SmartPolyMeshDetailPtr polyMeshDetail(rcAllocPolyMeshDetail(), rcFreePolyMeshDetail);

    if (!rcBuildPolyMeshDetail(&ctx, *polyMesh, *chf, config.detailSampleDist, config.detailSampleMaxError, *polyMeshDetail))
        return false;

    chf.reset();
    cset.reset();

    // too many vertices?
    if (polyMesh->nverts >= 0xFFFF)
    {
        std::stringstream str;
        str << "Too many mesh vertices produces for tile (" << tileX << ", " << tileY << ")" << std::endl;
        std::cerr << str.str();
        return false;
    }

    for (int i = 0; i < polyMesh->npolys; ++i)
    {
        if (!polyMesh->areas[i])
            continue;

        polyMesh->flags[i] = static_cast<unsigned short>(PolyFlags::Walkable | polyMesh->areas[i]);
    }

    dtNavMeshCreateParams params;
    ZERO(params);

    params.verts = polyMesh->verts;
    params.vertCount = polyMesh->nverts;
    params.polys = polyMesh->polys;
    params.polyAreas = polyMesh->areas;
    params.polyFlags = polyMesh->flags;
    params.polyCount = polyMesh->npolys;
    params.nvp = polyMesh->nvp;
    params.detailMeshes = polyMeshDetail->meshes;
    params.detailVerts = polyMeshDetail->verts;
    params.detailVertsCount = polyMeshDetail->nverts;
    params.detailTris = polyMeshDetail->tris;
    params.detailTriCount = polyMeshDetail->ntris;
    params.walkableHeight = MeshSettings::WalkableHeight;
    params.walkableRadius = MeshSettings::WalkableRadius;
    params.walkableClimb = MeshSettings::WalkableClimb;
    params.tileX = tileX;
    params.tileY = tileY;
    params.tileLayer = 0;
    memcpy(params.bmin, polyMesh->bmin, sizeof(polyMesh->bmin));
    memcpy(params.bmax, polyMesh->bmax, sizeof(polyMesh->bmax));
    params.cs = config.cs;
    params.ch = config.ch;
    params.buildBvTree = true;

    unsigned char *outData;
    int outDataSize;
    if (!dtCreateNavMeshData(&params, &outData, &outDataSize))
        return false;

    utility::BinaryStream result(outDataSize);
    result.Write(outData, outDataSize);

    out = std::move(result);

    dtFree(outData);

    return true;
}

bool IsHeightFieldEmpty(rcHeightfield const& solid)
{
    for (auto i = 0; i < solid.width*solid.height; ++i)
        if (!!solid.spans[i])
            return false;

    return true;
}
}

MeshBuilder::MeshBuilder(const std::string &outputPath, const std::string &mapName, int logLevel)
    : m_outputPath(outputPath), m_chunkReferences(MeshSettings::ChunkCount*MeshSettings::ChunkCount), m_completedTiles(0), m_logLevel(logLevel)
{
    // this must follow the parser initialization
    m_map = std::make_unique<parser::Map>(mapName);

    if (auto const wmo = m_map->GetGlobalWmoInstance())
    {
        auto const tileWidth = static_cast<int>(std::ceilf((wmo->Bounds.MaxCorner.X - wmo->Bounds.MinCorner.X) / MeshSettings::TileSize));
        auto const tileHeight = static_cast<int>(std::ceilf((wmo->Bounds.MaxCorner.Y - wmo->Bounds.MinCorner.Y) / MeshSettings::TileSize));

        m_globalWMO = std::make_unique<meshfiles::GlobalWMO>();

        for (auto tileY = 0; tileY < tileHeight; ++tileY)
            for (auto tileX = 0; tileX < tileWidth; ++tileX)
                m_pendingTiles.push_back({ tileX, tileY });

        wmo->BuildTriangles(m_globalWMOVertices, m_globalWMOIndices);
        wmo->BuildLiquidTriangles(m_globalWMOLiquidVertices, m_globalWMOLiquidIndices);
        wmo->BuildDoodadTriangles(m_globalWMODoodadVertices, m_globalWMODoodadIndices);

        std::vector<float> vertices;
        utility::Convert::VerticesToRecast(m_globalWMOVertices, vertices);

        m_minX = m_maxX = vertices[0];
        m_minY = m_maxY = vertices[1];
        m_minZ = m_maxZ = vertices[2];

        for (size_t i = 3; i < vertices.size(); i+=3)
        {
            auto const x = vertices[i + 0];
            auto const y = vertices[i + 1];
            auto const z = vertices[i + 2];

            m_minX = (std::min)(m_minX, x);
            m_maxX = (std::max)(m_maxX, x);

            m_minY = (std::min)(m_minY, y);
            m_maxY = (std::max)(m_maxY, y);

            m_minZ = (std::min)(m_minZ, z);
            m_maxZ = (std::max)(m_maxZ, z);
        }
    }
    else
    {
        // this build order should ensure that the tiles for one ADT finish before the next ADT begins (more or less)
        for (auto y = MeshSettings::Adts - 1; !!y; --y)
            for (auto x = MeshSettings::Adts - 1; !!x; --x)
            {
                if (!m_map->HasAdt(x, y))
                    continue;

                for (auto tileY = 0; tileY < MeshSettings::TilesPerADT; ++tileY)
                    for (auto tileX = 0; tileX < MeshSettings::TilesPerADT; ++tileX)
                    {
                        auto const globalTileX = x * MeshSettings::TilesPerADT + tileX;
                        auto const globalTileY = y * MeshSettings::TilesPerADT + tileY;

                        std::vector<std::pair<int, int>> chunks;
                        ComputeRequiredChunks(m_map.get(), globalTileX, globalTileY, chunks);

                        for (auto chunk : chunks)
                            AddChunkReference(chunk.first, chunk.second);

                        m_pendingTiles.push_back({ globalTileX, globalTileY });
                    }
            }
    }

    m_totalTiles = m_pendingTiles.size();

    if (!std::experimental::filesystem::is_directory(m_outputPath / "Nav" / mapName))
        std::experimental::filesystem::create_directory(m_outputPath / "Nav" / mapName);
}

MeshBuilder::MeshBuilder(const std::string &outputPath, const std::string &mapName, int logLevel, int adtX, int adtY)
    : m_outputPath(outputPath), m_chunkReferences(MeshSettings::ChunkCount*MeshSettings::ChunkCount), m_completedTiles(0), m_logLevel(logLevel)
{
    // this must follow the parser initialization
    m_map = std::make_unique<parser::Map>(mapName);

    for (auto y = adtY * MeshSettings::TilesPerADT; y < (adtY + 1)*MeshSettings::TilesPerADT; ++y)
        for (auto x = adtX * MeshSettings::TilesPerADT; x < (adtX + 1)*MeshSettings::TilesPerADT; ++x)
            m_pendingTiles.push_back({ x, y });

    for (auto const &tile : m_pendingTiles)
    {
        std::vector<std::pair<int, int>> chunks;
        ComputeRequiredChunks(m_map.get(), tile.first, tile.second, chunks);

        for (auto chunk : chunks)
            AddChunkReference(chunk.first, chunk.second);
    }

    m_totalTiles = m_pendingTiles.size();

    if (!std::experimental::filesystem::is_directory(m_outputPath / "Nav" / mapName))
        std::experimental::filesystem::create_directory(m_outputPath / "Nav" / mapName);
}

void MeshBuilder::LoadGameObjects(const std::string &path)
{
    std::cout << "Reading game object..." << std::endl;

    std::ifstream in(path);

    if (in.fail())
        THROW("Failed to open gameobject file").ErrorCode();

    m_gameObjectInstances.clear();

    std::string line;
    std::vector<std::string> cells;
    while (std::getline(in, line))
    {
        std::istringstream str(line);
        std::string field;

        while (std::getline(str, field, ','))
            cells.emplace_back(field);
    }

    in.close();

    if (!!(cells.size() % 10))
        THROW("Bad format of gameobject file");

    m_gameObjectInstances.reserve(cells.size() / 10);

    for (auto i = 0u; i < cells.size(); i += 10)
    {
        GameObjectInstance instance
        {
            std::stoull(cells[i]),      // guid
            std::stoul(cells[i + 1]),   // display id
            std::stoul(cells[i + 2]),   // map
            std::stof(cells[i + 3]),    // position x
            std::stof(cells[i + 4]),    // position y
            std::stof(cells[i + 5]),    // position z
            std::stof(cells[i + 6]),    // quaternion
            std::stof(cells[i + 7]),
            std::stof(cells[i + 8]),
            std::stof(cells[i + 9]),
        };

        // only concern ourselves with gameobjects on the current map
        if (instance.map == m_map->Id)
            m_gameObjectInstances.emplace_back(instance);
    }

    std::cout << "Loaded " << m_gameObjectInstances.size() << " game object instances.  Loading model information..." << std::endl;

    auto const displayInfo = parser::DBC("DBFilesClient\\GameObjectDisplayInfo.dbc");

    std::unordered_map<std::uint32_t, utility::AABBTree> models;

    for (auto i = 0u; i < displayInfo.RecordCount(); ++i)
    {
        auto const id = displayInfo.GetField(i, 0);
        auto const modelPath = displayInfo.GetStringField(i, 1);

        // not sure why this happens.  might we be missing some needed information?
        if (modelPath.length() == 0)
            continue;

        auto const extension = std::experimental::filesystem::path(modelPath).extension().string();

        // doodad
        if (extension[1] == 'm' || extension[1] == 'M')
        {
            auto const doodad = m_map->GetDoodad(modelPath);

            int asdf = 1234;
        }
        // wmo
        else if (extension[1] == 'w' || extension[1] == 'W')
        {
            auto const wmo = m_map->GetWmo(modelPath);

            int asdf = 1234;
        }
        else
            THROW("Unrecognized model extension");

        auto const modelFullPath = BuildAbsoluteFilename(m_outputPath, modelPath);

        utility::AABBTree model;

        std::ifstream modelIn(modelFullPath, std::ifstream::binary);

        // if the file fails to open, continue to the next one silently.  this happens when the model has no collision geometry
        if (modelIn.fail())
            continue;

        if (!model.Deserialize(modelIn))
        {
            std::cerr << "Failed to deserialize model " << modelPath << std::endl;
            continue;
        }

        models[id] = std::move(model);
    }

    std::cout << "Done.  Loaded " << models.size() << " models.  Instantiating game objects..." << std::endl;

    for (auto const &go : m_gameObjectInstances)
    {
        if (models.find(go.displayId) == models.end())
            THROW("Game object references non-existent model id");
    }
}

bool MeshBuilder::GetNextTile(int &tileX, int &tileY)
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if (m_pendingTiles.empty())
        return false;

    auto const back = m_pendingTiles.back();
    m_pendingTiles.pop_back();

    tileX = back.first;
    tileY = back.second;

    return true;
}

bool MeshBuilder::IsGlobalWMO() const
{
    return !!m_map->GetGlobalWmoInstance();
}

void MeshBuilder::AddChunkReference(int chunkX, int chunkY)
{
    assert(chunkX >= 0 && chunkY >= 0 && chunkX < MeshSettings::ChunkCount && chunkY < MeshSettings::ChunkCount);
    ++m_chunkReferences[chunkY * MeshSettings::ChunkCount + chunkX];
}

void MeshBuilder::RemoveChunkReference(int chunkX, int chunkY)
{
    auto const adtX = chunkX / MeshSettings::ChunksPerAdt;
    auto const adtY = chunkY / MeshSettings::ChunksPerAdt;

    std::lock_guard<std::mutex> guard(m_mutex);
    --m_chunkReferences[chunkY * MeshSettings::ChunkCount + chunkX];

    // check to see if all chunks on this ADT are without references.  if so, unload the ADT
    bool unload = true;
    for (int x = 0; x < MeshSettings::ChunksPerAdt; ++x)
        for (int y = 0; y < MeshSettings::ChunksPerAdt; ++y)
        {
            auto const globalChunkX = adtX * MeshSettings::ChunksPerAdt + x;
            auto const globalChunkY = adtY * MeshSettings::ChunksPerAdt + y;

            if (m_chunkReferences[globalChunkY * MeshSettings::ChunkCount + globalChunkX] > 0)
            {
                unload = false;
                break;
            }
        }

    if (unload)
    {
#ifdef _DEBUG
        std::stringstream str;
        str << "No threads need ADT (" << std::setfill(' ') << std::setw(2) << adtX << ", "
            << std::setfill(' ') << std::setw(2) << adtY << ").  Unloading.\n";
        std::cout << str.str();
#endif

        m_map->UnloadAdt(adtX, adtY);
    }
}

void MeshBuilder::SerializeWmo(const parser::Wmo *wmo)
{
    std::lock_guard<std::mutex> guard(m_mutex);

    if (m_bvhWmos.find(wmo->FileName) != m_bvhWmos.end())
        return;

    utility::AABBTree aabbTree(wmo->Vertices, wmo->Indices);

    std::ofstream o(m_outputPath / "BVH" / ("WMO_" + wmo->FileName + ".bvh"), std::ofstream::binary|std::ofstream::trunc);
    aabbTree.Serialize(o);

    m_bvhWmos.insert(wmo->FileName);

    const std::uint32_t doodadSetCount = static_cast<std::uint32_t>(wmo->DoodadSets.size());
    o.write(reinterpret_cast<const char *>(&doodadSetCount), sizeof(doodadSetCount));

    // Also write BVH for any WMO-spawned doodads
    for (auto const &doodadSet : wmo->DoodadSets)
    {
        const std::uint32_t doodadSetSize = static_cast<std::uint32_t>(doodadSet.size());
        o.write(reinterpret_cast<const char *>(&doodadSetSize), sizeof(doodadSetSize));

        for (auto const &wmoDoodad : doodadSet)
        {
            auto const doodad = wmoDoodad->Parent;

            o << wmoDoodad->TransformMatrix;
            o << wmoDoodad->Bounds;
            o << std::left << std::setw(64) << std::setfill('\000') << doodad->FileName;

            SerializeDoodad(doodad);
        }
    }
}

void MeshBuilder::SerializeDoodad(const parser::Doodad *doodad)
{
    if (m_bvhDoodads.find(doodad->FileName) != m_bvhDoodads.end())
        return;

    utility::AABBTree doodadTree(doodad->Vertices, doodad->Indices);

    std::ofstream doodadOut(m_outputPath / "BVH" / ("Doodad_" + doodad->FileName + ".bvh"), std::ofstream::binary|std::ofstream::trunc);
    doodadTree.Serialize(doodadOut);

    m_bvhDoodads.insert(doodad->FileName);
}

bool MeshBuilder::BuildAndSerializeWMOTile(int tileX, int tileY)
{
    auto const wmoInstance = m_map->GetGlobalWmoInstance();

    assert(!!wmoInstance);

    SerializeWmo(wmoInstance->Model);

    rcConfig config;
    InitializeRecastConfig(config);

    // tile coordinates are calculated offset from the northwest corner
    auto const westStart = wmoInstance->Bounds.MaxCorner.Y - (tileX * MeshSettings::TileSize);  // wow coordinate system
    auto const northStart = wmoInstance->Bounds.MaxCorner.X - (tileY * MeshSettings::TileSize); // wow coordinate system

    config.bmin[0] = -westStart;
    config.bmin[1] = wmoInstance->Bounds.MinCorner.Z;
    config.bmin[2] = -northStart;

    config.bmax[0] = config.bmin[0] + MeshSettings::TileSize;
    config.bmax[1] = wmoInstance->Bounds.MaxCorner.Z;
    config.bmax[2] = config.bmin[2] + MeshSettings::TileSize;

    // bounding box of tile in wow coordinates for culling wmos and doodads
    const utility::BoundingBox tileBounds({ config.bmax[2], config.bmax[0], config.bmin[1] }, { config.bmin[2], config.bmin[0], config.bmax[1] });

    // erode mesh tile boundaries to force recast to examine obstacles on or near the tile boundary
    config.bmin[0] -= config.borderSize * config.cs;
    config.bmin[2] -= config.borderSize * config.cs;
    config.bmax[0] += config.borderSize * config.cs;
    config.bmax[2] += config.borderSize * config.cs;

    RecastContext ctx(m_logLevel);

    SmartHeightFieldPtr solid(rcAllocHeightfield(), rcFreeHeightField);

    if (!rcCreateHeightfield(&ctx, *solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        return false;

    // wmo terrain
    if (!TransformAndRasterize(ctx, *solid, true, config.walkableSlopeAngle, m_globalWMOVertices, m_globalWMOIndices, AreaFlags::WMO))
        return false;

    // wmo liquid
    if (!TransformAndRasterize(ctx, *solid, false, config.walkableSlopeAngle, m_globalWMOLiquidVertices, m_globalWMOLiquidIndices, AreaFlags::WMO | AreaFlags::Liquid))
        return false;

    // wmo doodads
    if (!TransformAndRasterize(ctx, *solid, true, config.walkableSlopeAngle, m_globalWMODoodadVertices, m_globalWMODoodadIndices, AreaFlags::WMO | AreaFlags::Doodad))
        return false;

    auto const solidEmpty = IsHeightFieldEmpty(*solid);

    if (!solidEmpty)
    {
        FilterGroundBeneathLiquid(*solid);

        // note that no area id preservation is necessary here because we have no ADT terrain
        rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *solid);
        rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *solid);
        rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *solid);
    }

    // serialize heightfield for this tile
    utility::BinaryStream heightFieldData(sizeof(std::uint32_t)*(10 + 3 * (solid->width*solid->height)));

    if (!solidEmpty)
        SerializeHeightField(*solid, heightFieldData);

    // serialize final navmesh tile
    utility::BinaryStream meshData(0);
    if (!solidEmpty)
    {
        auto const result = SerializeMeshTile(ctx, config, tileX, tileY, *solid, meshData);
        assert(result);
    }

    std::lock_guard<std::mutex> guard(m_mutex);

    if (!solidEmpty)
        m_globalWMO->AddTile(tileX, tileY, heightFieldData, meshData);

    if (++m_completedTiles == m_totalTiles)
    {
        m_globalWMO->Serialize(m_outputPath / "Nav" / m_map->Name / "Map.nav");

#ifdef _DEBUG
        std::stringstream log;
        log << "Finished " << m_map->Name << ".";
        std::cout << log.str() << std::endl;
#endif
    }

    return true;
}

bool MeshBuilder::BuildAndSerializeMapTile(int tileX, int tileY)
{
    float minZ = (std::numeric_limits<float>::max)(), maxZ = std::numeric_limits<float>::lowest();

    // regardless of tile size, we only need the surrounding ADT chunks
    std::vector<const parser::AdtChunk *> chunks;
    std::vector<std::pair<int, int>> chunkPositions;

    ComputeRequiredChunks(m_map.get(), tileX, tileY, chunkPositions);

    chunks.reserve(chunkPositions.size());
    for (auto const chunkPosition : chunkPositions)
    {
        auto const adtX = chunkPosition.first / MeshSettings::ChunksPerAdt;
        auto const adtY = chunkPosition.second / MeshSettings::ChunksPerAdt;
        auto const chunkX = chunkPosition.first % MeshSettings::ChunksPerAdt;
        auto const chunkY = chunkPosition.second % MeshSettings::ChunksPerAdt;

        assert(adtX >= 0 && adtY >= 0 && adtX < MeshSettings::Adts && adtY < MeshSettings::Adts);

        auto const adt = m_map->GetAdt(adtX, adtY);
        auto const chunk = adt->GetChunk(chunkX, chunkY);

        minZ = (std::min)(minZ, chunk->m_minZ);
        maxZ = (std::max)(maxZ, chunk->m_maxZ);

        chunks.push_back(chunk);
    }

    rcConfig config;
    InitializeRecastConfig(config);
    
    config.bmin[0] = tileX * MeshSettings::TileSize - 32.f * MeshSettings::AdtSize;
    config.bmin[1] = minZ;
    config.bmin[2] = tileY * MeshSettings::TileSize - 32.f * MeshSettings::AdtSize;

    config.bmax[0] = (tileX + 1) * MeshSettings::TileSize - 32.f * MeshSettings::AdtSize;
    config.bmax[1] = maxZ;
    config.bmax[2] = (tileY + 1) * MeshSettings::TileSize - 32.f * MeshSettings::AdtSize;

    // bounding box of tile in wow coordinates for culling wmos and doodads
    const utility::BoundingBox tileBounds({ -config.bmax[2], -config.bmax[0], config.bmin[1] }, { -config.bmin[2], -config.bmin[0], config.bmax[1] });

    // erode mesh tile boundaries to force recast to examine obstacles on or near the tile boundary
    config.bmin[0] -= config.borderSize * config.cs;
    config.bmin[2] -= config.borderSize * config.cs;
    config.bmax[0] += config.borderSize * config.cs;
    config.bmax[2] += config.borderSize * config.cs;

    RecastContext ctx(m_logLevel);

    SmartHeightFieldPtr solid(rcAllocHeightfield(), rcFreeHeightField);

    if (!rcCreateHeightfield(&ctx, *solid, config.width, config.height, config.bmin, config.bmax, config.cs, config.ch))
        return false;

    std::unordered_set<std::uint32_t> rasterizedWmos;
    std::unordered_set<std::uint32_t> rasterizedDoodads;

    // incrementally rasterize mesh geometry into the height field, setting area flags as appropriate
    for (auto const &chunk : chunks)
    {
        // adt terrain
        if (!TransformAndRasterize(ctx, *solid, false, config.walkableSlopeAngle, chunk->m_terrainVertices, chunk->m_terrainIndices, AreaFlags::ADT))
            return false;

        // liquid
        if (!TransformAndRasterize(ctx, *solid, false, config.walkableSlopeAngle, chunk->m_liquidVertices, chunk->m_liquidIndices, AreaFlags::Liquid))
            return false;

        // wmos (and included doodads and liquid)
        for (auto const &wmoId : chunk->m_wmoInstances)
        {
            if (rasterizedWmos.find(wmoId) != rasterizedWmos.end())
                continue;

            auto const wmoInstance = m_map->GetWmoInstance(wmoId);

            if (!wmoInstance)
            {
                std::stringstream str;
                str << "Could not find required WMO ID = " << wmoId << " needed by tile (" << tileX << ", " << tileY << ")";

                THROW(str.str());
            }

            assert(!!wmoInstance);

            // FIXME: it is possible that the added geometry of the doodads will expand the bounding box of this wmo, thereby placing it on this tile
            if (!tileBounds.intersect2d(wmoInstance->Bounds))
                continue;

            std::vector<utility::Vertex> vertices;
            std::vector<int> indices;

            wmoInstance->BuildTriangles(vertices, indices);
            if (!TransformAndRasterize(ctx, *solid, true, config.walkableSlopeAngle, vertices, indices, AreaFlags::WMO))
                return false;

            wmoInstance->BuildLiquidTriangles(vertices, indices);
            if (!TransformAndRasterize(ctx, *solid, false, config.walkableSlopeAngle, vertices, indices, AreaFlags::WMO | AreaFlags::Liquid))
                return false;

            wmoInstance->BuildDoodadTriangles(vertices, indices);
            if (!TransformAndRasterize(ctx, *solid, true, config.walkableSlopeAngle, vertices, indices, AreaFlags::WMO | AreaFlags::Doodad))
                return false;

            rasterizedWmos.insert(wmoId);
        }

        // doodads
        for (auto const &doodadId : chunk->m_doodadInstances)
        {
            if (rasterizedDoodads.find(doodadId) != rasterizedDoodads.end())
                continue;

            auto const doodadInstance = m_map->GetDoodadInstance(doodadId);

            assert(!!doodadInstance);

            if (!tileBounds.intersect2d(doodadInstance->Bounds))
                continue;

            std::vector<utility::Vertex> vertices;
            std::vector<int> indices;

            doodadInstance->BuildTriangles(vertices, indices);
            if (!TransformAndRasterize(ctx, *solid, true, config.walkableSlopeAngle, vertices, indices, AreaFlags::Doodad))
                return false;

            rasterizedDoodads.insert(doodadId);
        }
    }

    FilterGroundBeneathLiquid(*solid);

#ifndef DISABLE_SELECTIVE_FILTERING
    // save all span area flags because we dont want the upcoming filtering to apply to ADT terrain
    {
        std::vector<rcSpan *> adtSpans;

        adtSpans.reserve(solid->width*solid->height);

        for (int i = 0; i < solid->width * solid->height; ++i)
            for (rcSpan *s = solid->spans[i]; s; s = s->next)
                if (!!(s->area & AreaFlags::ADT))
                    adtSpans.push_back(s);

#endif
        rcFilterLedgeSpans(&ctx, config.walkableHeight, config.walkableClimb, *solid);

#ifndef DISABLE_SELECTIVE_FILTERING
        RestoreAdtSpans(adtSpans);
    }
#endif

    rcFilterWalkableLowHeightSpans(&ctx, config.walkableHeight, *solid);
    rcFilterLowHangingWalkableObstacles(&ctx, config.walkableClimb, *solid);

    // Write the BVH for every new WMO
    for (auto const& wmoId : rasterizedWmos)
        SerializeWmo(m_map->GetWmoInstance(wmoId)->Model);

    {
        std::lock_guard<std::mutex> guard(m_mutex);

        // Write the BVH for every new doodad
        for (auto const& doodadId : rasterizedDoodads)
            SerializeDoodad(m_map->GetDoodadInstance(doodadId)->Model);
    }

    // serialize WMO and doodad IDs for this tile
    utility::BinaryStream wmosAndDoodads(0);
    SerializeWMOAndDoodadIDs(rasterizedWmos, rasterizedDoodads, wmosAndDoodads);

    // serialize heightfield for this tile
    utility::BinaryStream heightFieldData(sizeof(std::uint32_t)*(11 + 3 * (solid->width*solid->height)));
    SerializeHeightField(*solid, heightFieldData);

    // serialize final navmesh tile
    utility::BinaryStream meshData(0);
    auto const result = SerializeMeshTile(ctx, config, tileX, tileY, *solid, meshData);

    {
        std::lock_guard<std::mutex> guard(m_mutex);

        auto const adtX = tileX / MeshSettings::TilesPerADT;
        auto const adtY = tileY / MeshSettings::TilesPerADT;
        auto const localTileX = tileX % MeshSettings::TilesPerADT;
        auto const localTileY = tileY % MeshSettings::TilesPerADT;

        auto adt = GetInProgressADT(adtX, adtY);

        adt->AddTile(localTileX, localTileY, wmosAndDoodads, heightFieldData, meshData);

        if (adt->IsComplete())
        {
            std::stringstream str;
            str << std::setw(2) << std::setfill('0') << adtX << "_" << std::setw(2) << std::setfill('0') << adtY << ".nav";

            adt->Serialize(m_outputPath / "Nav" / m_map->Name / str.str());

#ifdef _DEBUG
            std::stringstream log;
            log << "Finished " << m_map->Name << " ADT (" << adtX << ", " << adtY << ")";
            std::cout << log.str() << std::endl;
#endif

            RemoveADT(adt);
        }
    }

    ++m_completedTiles;
    for (auto const &chunk : chunkPositions)
        RemoveChunkReference(chunk.first, chunk.second);

    return result;
}

void MeshBuilder::SaveMap() const
{
    std::ofstream out(m_outputPath / (m_map->Name + ".map"), std::ofstream::binary|std::ofstream::trunc);

    m_map->Serialize(out);
}

float MeshBuilder::PercentComplete() const
{
    return 100.f * (float(m_completedTiles) / float(m_totalTiles));
}

meshfiles::ADT* MeshBuilder::GetInProgressADT(int x, int y)
{
    if (!m_adtsInProgress[{x, y}])
        m_adtsInProgress[{x, y}] = std::make_unique<meshfiles::ADT>(x, y);

    return m_adtsInProgress[{x, y}].get();
}

void MeshBuilder::RemoveADT(const meshfiles::ADT *adt)
{
    for (auto const &a : m_adtsInProgress)
        if (a.second.get() == adt)
        {
            m_adtsInProgress.erase(a.first);
            return;
        }
}

namespace meshfiles
{
void File::AddTile(int x, int y, utility::BinaryStream &heightfield, const utility::BinaryStream &mesh)
{
    m_tiles[{x, y}] = std::move(heightfield);
    m_tiles[{x, y}] << static_cast<std::uint32_t>(mesh.wpos());
    m_tiles[{x, y}].Append(mesh);
}

void ADT::AddTile(int x, int y, utility::BinaryStream &wmosAndDoodads, utility::BinaryStream &heightField, utility::BinaryStream &mesh)
{
    std::lock_guard<std::mutex> guard(m_mutex);

    File::AddTile(x, y, heightField, mesh);

    m_wmosAndDoodadIds[{x, y}] = std::move(wmosAndDoodads);
}

void ADT::Serialize(const std::experimental::filesystem::path &filename) const
{
    size_t bufferSize = 6 * sizeof(std::uint32_t);

    // first compute total size, just to reduce reallocations
    for (auto const &tile : m_tiles)
    {
        bufferSize += 2 * sizeof(std::uint32_t);
        
        // in this case, the wmo and doodad counts are included in the buffer
        if (m_wmosAndDoodadIds.find(tile.first) != m_wmosAndDoodadIds.end())
            bufferSize += m_wmosAndDoodadIds.at(tile.first).wpos();
        // if the buffer is empty, two zeros are written
        else
            bufferSize += 2 * sizeof(std::uint32_t);

        bufferSize += tile.second.wpos();
    }

    utility::BinaryStream outBuffer(bufferSize);

    // header
    outBuffer << MeshSettings::FileSignature << MeshSettings::FileVersion << MeshSettings::FileADT;

    // ADT x and y
    outBuffer << m_x << m_y;

    // tile count
    outBuffer << static_cast<std::uint32_t>(m_tiles.size());

    for (auto const &tile : m_tiles)
    {
        // tile x and y
        outBuffer << tile.first.first << tile.first.second;

        // append wmo and doodad id buffer (which already contains size information), or an empty one if there is none
        if (m_wmosAndDoodadIds.find(tile.first) == m_wmosAndDoodadIds.end())
            outBuffer << static_cast<std::uint32_t>(0) << static_cast<std::uint32_t>(0);
        else
            outBuffer.Append(m_wmosAndDoodadIds.at(tile.first));

        // height field and finalized tile buffer
        outBuffer.Append(tile.second);
    }

    // temporary just to make sure our calculation still works.  if it doesnt, we could see copious reallocations in the above code!
    assert(outBuffer.wpos() == bufferSize);

    outBuffer.Compress();

    std::ofstream out(filename, std::ofstream::binary | std::ofstream::trunc);

    if (out.fail())
        THROW("ADT serialization failed to open output file");

    out << outBuffer;
}

void GlobalWMO::AddTile(int x, int y, utility::BinaryStream &heightField, utility::BinaryStream &mesh)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    File::AddTile(x, y, heightField, mesh);
}

void GlobalWMO::Serialize(const std::experimental::filesystem::path &filename) const
{
    size_t bufferSize = 6 * sizeof(std::uint32_t);

    // first compute total size, just to reduce reallocations
    for (auto const &tile : m_tiles)
        bufferSize += 4 * sizeof(std::uint32_t) + tile.second.wpos();

    utility::BinaryStream outBuffer(bufferSize);

    // header
    outBuffer << MeshSettings::FileSignature << MeshSettings::FileVersion << MeshSettings::FileWMO;

    // ADT x and y
    outBuffer << MeshSettings::WMOcoordinate << MeshSettings::WMOcoordinate;

    // tile count
    outBuffer << static_cast<std::uint32_t>(m_tiles.size());

    for (auto const &tile : m_tiles)
    {
        // tile x and y
        outBuffer << tile.first.first << tile.first.second;

        // no per-tile doodad ids for global WMOs
        outBuffer << static_cast<std::uint32_t>(0) << static_cast<std::uint32_t>(0);

        // height field and finalized tile buffer
        outBuffer.Append(tile.second);
    }

    // temporary just to make sure our calculation still works.  if it doesnt, we could see copious reallocations in the above code!
    assert(outBuffer.wpos() == bufferSize);

    outBuffer.Compress();

    std::ofstream out(filename, std::ofstream::binary | std::ofstream::trunc);

    if (out.fail())
        THROW("WMO serialization failed to open output file").ErrorCode();

    out << outBuffer;
}
}