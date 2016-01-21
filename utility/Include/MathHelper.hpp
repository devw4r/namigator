#pragma once

#include "utility/Include/LinearAlgebra.hpp"

#include <memory>
#include <vector>

namespace utility
{
class MathHelper
{
    public:
        static constexpr float AdtSize = 533.f + (1.f / 3.f);
        static constexpr float AdtChunkSize = AdtSize / 16.f;
        static constexpr float Pi = 3.14159264f;

        static bool FaceTooSteep(const utility::Vertex &a, const utility::Vertex &b, const utility::Vertex &c, float degrees);
        static float InterpolateHeight(const utility::Vertex &a, const utility::Vertex &b, const utility::Vertex &c, float x, float y);

        static Vector3 CalculateTriangleNormal(const Vector3 a, const Vector3 b, const Vector3 c)
        {
            const Vector3 ab(b.X - a.X, b.Y - a.Y, b.Z - a.Z), ac(c.X - a.X, c.Y - a.Y, c.Z - a.Z);
            const Vector3 n = Vector3::CrossProduct(ab, ac);

            return Vector3::Normalize(n);
        }
};

class Convert
{
    public:
        static float ToRadians(float degrees);

        static void WorldToAdt(const utility::Vertex &vertex, int &adtX, int &adtY);

        static void VerticesToRecast(const std::vector<utility::Vertex> &input, std::vector<float> &output);
        static void VerticesToWow(const float *input, int vertexCount, std::vector<utility::Vertex> &output);

        // XXX FIXME we maybe never need to change the indices from ushort to int in the first place?
        static void ToShort(const std::vector<int> &input, std::vector<unsigned short> &output);
};
}