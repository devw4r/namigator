#include "NavMesh.hpp"
#include "utility/Include/MathHelper.hpp"

#include "DetourNavMesh.h"

#include <string>
#include <sstream>
#include <fstream>
#include <cassert>

namespace pathfind
{
NavMesh::NavMesh(const std::string &dataPath, const std::string &continentName) : m_dataPath(dataPath), m_continentName(continentName)
{
    dtNavMeshParams params;

    const float tileWidth = 533.f + (1.f / 3.f);

    params.orig[0] = -32.f * tileWidth;
    params.orig[1] = 0.f;
    params.orig[2] = -32.f * tileWidth;
    params.tileHeight = params.tileWidth = tileWidth;
    params.maxTiles = 64 * 64;
    params.maxPolys = 1 << DT_POLY_BITS;

    assert(m_navMesh.init(&params) == DT_SUCCESS);
}

bool NavMesh::LoadTile(int x, int y)
{
    std::stringstream str;
    str << m_dataPath << "\\" << m_continentName << "_" << x << "_" << y << ".map";

    std::ifstream in(str.str(), std::ifstream::binary);

    in.seekg(0, in.end);
    auto const size = in.tellg();
    in.seekg(0, in.beg);

    auto const xx = static_cast<unsigned int>(size);

    // the dtNavMesh destructor will handle deallocation of this data
    auto const buff = new unsigned char[static_cast<unsigned int>(size)];
    in.read(reinterpret_cast<char *>(buff), size);

    return m_navMesh.addTile(buff, static_cast<int>(size), 0, 0, nullptr) == DT_SUCCESS;
}

void NavMesh::GetTileGeometry(int x, int y, std::vector<utility::Vertex> &vertices, std::vector<int> &indices)
{
    auto const tile = m_navMesh.getTileAt(x, y, 0);

    unsigned int triCount = 0;

    for (int i = 0; i < tile->header->polyCount; ++i)
    {
        if (tile->polys[i].areaAndtype == DT_POLYTYPE_OFFMESH_CONNECTION)
            continue;

        triCount += tile->detailMeshes[i].triCount;
    }

    vertices.reserve(3 * triCount);
    indices.reserve(triCount);

    for (int i = 0; i < tile->header->polyCount; ++i)
    {
        auto const p = &tile->polys[i];

        if (p->areaAndtype == DT_POLYTYPE_OFFMESH_CONNECTION)
            continue;

        auto const pd = &tile->detailMeshes[i];

        for (int j = 0; j < pd->triCount; ++j)
        {
            auto const t = &tile->detailTris[(pd->triBase + j) * 4];

            for (int k = 0; k < 3; ++k)
            {
                auto const vert = t[k] < p->vertCount ? &tile->verts[p->verts[t[k]] * 3] : &tile->detailVerts[(pd->vertBase + t[k] - p->vertCount) * 3];

                vertices.push_back({ -vert[2], -vert[0], vert[1] });
            }

            indices.push_back(vertices.size() - 3);
            indices.push_back(vertices.size() - 2);
            indices.push_back(vertices.size() - 1);
        }
    }
}
}