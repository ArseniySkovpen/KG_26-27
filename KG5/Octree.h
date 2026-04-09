#pragma once
#include <vector>
#include <array>
#include "FrustumCuller.h"

// ============================================================
// Octree — разбивает набор AABB по пространству.
// Используется для ускорения frustum culling.
// ============================================================
class Octree
{
public:
    static constexpr int MAX_DEPTH = 5;
    static constexpr int MAX_PER_NODE = 24;

    // Строит дерево по переданному набору AABB объектов.
    // sceneBounds — AABB всей сцены (корневой узел).
    void Build(const std::vector<AABB>& items, const AABB& sceneBounds)
    {
        m_items = &items;
        m_nodes.clear();
        m_nodes.reserve(4096);

        Node root{};
        root.bounds = sceneBounds;
        root.isLeaf = true;
        m_nodes.push_back(root);

        for (int i = 0; i < (int)items.size(); ++i)
            InsertInto(0, i, 0);
    }

    // Возвращает индексы объектов, чьи AABB пересекают фрустум.
    // Дубликаты автоматически исключаются.
    void Query(const Frustum& f, std::vector<int>& out) const
    {
        out.clear();
        if (m_nodes.empty() || !m_items) return;

        m_visited.assign(m_items->size(), false);
        QueryNode(0, f, out);
    }

    int NodeCount() const { return (int)m_nodes.size(); }

private:
    struct Node
    {
        AABB              bounds{};
        std::array<int, 8> children{};
        std::vector<int>  items;
        bool              isLeaf = true;

        Node() { children.fill(-1); }
    };

    const std::vector<AABB>* m_items = nullptr;
    std::vector<Node>        m_nodes;
    mutable std::vector<bool> m_visited;

    // ---- Overlap AABB ----
    static bool Overlaps(const AABB& a, const AABB& b)
    {
        return a.Min.x <= b.Max.x && a.Max.x >= b.Min.x &&
            a.Min.y <= b.Max.y && a.Max.y >= b.Min.y &&
            a.Min.z <= b.Max.z && a.Max.z >= b.Min.z;
    }

    // ---- Вставка объекта в узел ----
    void InsertInto(int nIdx, int itemIdx, int depth)
    {
        if (m_nodes[nIdx].isLeaf)
        {
            m_nodes[nIdx].items.push_back(itemIdx);
            if ((int)m_nodes[nIdx].items.size() > MAX_PER_NODE && depth < MAX_DEPTH)
                Split(nIdx, depth);
        }
        else
        {
            for (int i = 0; i < 8; ++i)
            {
                int ch = m_nodes[nIdx].children[i];
                if (ch != -1 && Overlaps(m_nodes[ch].bounds, (*m_items)[itemIdx]))
                    InsertInto(ch, itemIdx, depth + 1);
            }
        }
    }

    // ---- Разбиваем листовой узел на 8 дочерних ----
    void Split(int nIdx, int depth)
    {
        // Сохраняем элементы до изменения узла
        std::vector<int> saved = std::move(m_nodes[nIdx].items);
        m_nodes[nIdx].items.clear();
        m_nodes[nIdx].isLeaf = false;

        XMFLOAT3 mn = m_nodes[nIdx].bounds.Min;
        XMFLOAT3 mx = m_nodes[nIdx].bounds.Max;
        XMFLOAT3 c = m_nodes[nIdx].bounds.Center();

        // 8 дочерних AABB (на стеке — не инвалидируются при push_back)
        AABB cb[8] = {
            {{mn.x,mn.y,mn.z},{c.x, c.y, c.z }},
            {{c.x, mn.y,mn.z},{mx.x,c.y, c.z }},
            {{mn.x,c.y, mn.z},{c.x, mx.y,c.z }},
            {{c.x, c.y, mn.z},{mx.x,mx.y,c.z }},
            {{mn.x,mn.y,c.z },{c.x, c.y, mx.z}},
            {{c.x, mn.y,c.z },{mx.x,c.y, mx.z}},
            {{mn.x,c.y, c.z },{c.x, mx.y,mx.z}},
            {{c.x, c.y, c.z },{mx.x,mx.y,mx.z}},
        };

        // Создаём 8 дочерних узлов.
        // Доступ через m_nodes[nIdx] по индексу — безопасен даже при realloc.
        for (int i = 0; i < 8; ++i)
        {
            int chIdx = (int)m_nodes.size();
            m_nodes[nIdx].children[i] = chIdx;
            Node child{};
            child.bounds = cb[i];
            m_nodes.push_back(std::move(child));
        }

        // Перераспределяем сохранённые элементы по дочерним узлам
        for (int idx : saved)
        {
            for (int i = 0; i < 8; ++i)
            {
                int ch = m_nodes[nIdx].children[i];
                if (Overlaps(m_nodes[ch].bounds, (*m_items)[idx]))
                    InsertInto(ch, idx, depth + 1);
            }
        }
    }

    // ---- Рекурсивный обход при запросе ----
    void QueryNode(int nIdx, const Frustum& f, std::vector<int>& out) const
    {
        if (!TestAABBFrustum(f, m_nodes[nIdx].bounds)) return;

        const Node& n = m_nodes[nIdx];
        if (n.isLeaf)
        {
            for (int idx : n.items)
            {
                if (!m_visited[idx] && TestAABBFrustum(f, (*m_items)[idx]))
                {
                    m_visited[idx] = true;
                    out.push_back(idx);
                }
            }
        }
        else
        {
            for (int c = 0; c < 8; ++c)
                if (n.children[c] != -1)
                    QueryNode(n.children[c], f, out);
        }
    }
};