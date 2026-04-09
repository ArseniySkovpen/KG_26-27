#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DirectXMath.h>
#include <cmath>
using namespace DirectX;

// ============================================================
// AABB — ограничивающий параллелепипед
// ============================================================
struct AABB
{
    XMFLOAT3 Min = { 0,0,0 };
    XMFLOAT3 Max = { 0,0,0 };

    XMFLOAT3 Center()  const { return { (Min.x + Max.x) * .5f, (Min.y + Max.y) * .5f, (Min.z + Max.z) * .5f }; }
    XMFLOAT3 Extents() const { return { (Max.x - Min.x) * .5f, (Max.y - Min.y) * .5f, (Max.z - Min.z) * .5f }; }
};

// ============================================================
// Frustum — 6 плоскостей (нормаль направлена ВНУТРЬ)
// ============================================================
struct Frustum
{
    XMFLOAT4 Planes[6]; // left, right, bottom, top, near, far
};

// Извлекаем плоскости из объединённой матрицы ViewProj (Gribb-Hartmann).
// Без транспозиции: m._ij = ViewProj[row=i-1][col=j-1].
inline Frustum ExtractFrustum(XMMATRIX viewProj)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);   // НЕ транспонируем

    auto Norm = [](XMFLOAT4& p)
        {
            float l = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
            if (l > 1e-7f) { p.x /= l; p.y /= l; p.z /= l; p.w /= l; }
        };

    Frustum f;
    // Left:   col3 + col0
    f.Planes[0] = { m._11 + m._14, m._21 + m._24, m._31 + m._34, m._41 + m._44 };
    // Right:  col3 - col0
    f.Planes[1] = { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 };
    // Bottom: col3 + col1
    f.Planes[2] = { m._12 + m._14, m._22 + m._24, m._32 + m._34, m._42 + m._44 };
    // Top:    col3 - col1
    f.Planes[3] = { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 };
    // Near:   col2  (DX LH: z_clip >= 0)
    f.Planes[4] = { m._13,       m._23,       m._33,       m._43 };
    // Far:    col3 - col2
    f.Planes[5] = { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 };

    for (auto& p : f.Planes) Norm(p);
    return f;
}

// Возвращает true, если AABB не полностью за пределами фрустума.
inline bool TestAABBFrustum(const Frustum& f, const AABB& box)
{
    XMFLOAT3 c = box.Center();
    XMFLOAT3 e = box.Extents();

    for (const auto& p : f.Planes)
    {
        // Проекция радиуса AABB на нормаль плоскости
        float r = fabsf(p.x) * e.x + fabsf(p.y) * e.y + fabsf(p.z) * e.z;
        // Расстояние центра до плоскости
        float d = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
        if (d < -r) return false; // вся коробка позади плоскости
    }
    return true;
}