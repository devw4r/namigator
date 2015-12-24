#include "Input/WMO/Group File/Chunks/MOPY.hpp"

namespace parser_input
{
    MOPY::MOPY(long position, utility::BinaryStream *groupFileStream) : WmoGroupChunk(position, groupFileStream), TriangleCount(Size/2)
    {
        groupFileStream->SetPosition(position + 8);

        Type = WmoGroupChunkType::MOPY;

        Flags.reserve(TriangleCount);
        MaterialId.reserve(TriangleCount);

        for (int i = 0; i < TriangleCount; ++i)
        {
            Flags.push_back(groupFileStream->Read<unsigned char>());
            MaterialId.push_back(groupFileStream->Read<unsigned char>());
        }
    }
}