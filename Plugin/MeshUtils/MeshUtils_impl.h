#pragma once

namespace mu {
namespace impl {

// "welded" indices
struct IndicesW
{
    IArray<int> m_indices;
    IArray<int> m_weld_map;

    size_t size() const { return m_indices.size(); }
    int operator[](size_t i) const { return m_weld_map[m_indices[i]]; }
};

struct CountsC
{
    int m_ngon;
    size_t m_size;

    size_t size() const { return m_size; }
    int operator[](size_t /*i*/) const { return m_ngon; }
};

struct OffsetsC
{
    int m_ngon;
    size_t m_size;

    size_t size() const { return m_size; }
    int operator[](size_t i) const { return m_ngon * (int)i; }
};


template<class Indices, class Counts>
inline void BuildConnection(
    ConnectionData& connection, const Indices& indices, const Counts& counts, const IArray<float3>& vertices)
{
    size_t num_points = vertices.size();
    size_t num_faces = counts.size();
    size_t num_indices = indices.size();

    connection.v2f_offsets.resize(num_points);
    connection.v2f_faces.resize(num_indices);
    connection.v2f_indices.resize(num_indices);

    connection.v2f_counts.resize(num_points);
    connection.v2f_counts.zeroclear();
    {
        int ii = 0;
        for (size_t fi = 0; fi < num_faces; ++fi) {
            int c = counts[fi];
            for (int ci = 0; ci < c; ++ci) {
                connection.v2f_counts[indices[ii + ci]]++;
            }
            ii += c;
        }

        int offset = 0;
        for (size_t i = 0; i < num_points; ++i) {
            connection.v2f_offsets[i] = offset;
            offset += connection.v2f_counts[i];
        }
    }

    connection.v2f_counts.zeroclear();
    {
        int i = 0;
        for (int fi = 0; fi < (int)num_faces; ++fi) {
            int c = counts[fi];
            for (int ci = 0; ci < c; ++ci) {
                int vi = indices[i + ci];
                int ti = connection.v2f_offsets[vi] + connection.v2f_counts[vi]++;
                connection.v2f_faces[ti] = fi;
                connection.v2f_indices[ti] = i + ci;
            }
            i += c;
        }
    }
}

inline void BuildWeldMap(
    ConnectionData& connection, const IArray<float3>& vertices)
{
    auto& weld_map = connection.weld_map;
    auto& weld_counts = connection.weld_counts;
    auto& weld_offsets = connection.weld_offsets;
    auto& weld_indices = connection.weld_indices;

    int n = (int)vertices.size();
    weld_map.resize(n);
    weld_counts.resize(n);
    weld_offsets.resize(n);
    weld_indices.resize(n);

    parallel_for(0, n, [&](int vi) {
        int r = vi;
        float3 p = vertices[vi];
        for (int i = 0; i < vi; ++i) {
            if (near_equal(length_sq(vertices[i] - p), 0.0f)) {
                r = i;
                break;
            }
        }
        weld_map[vi] = r;
    });

    weld_counts.zeroclear();
    for (int vi : weld_map) {
        weld_counts[vi]++;
    }

    int offset = 0;
    for (int vi = 0; vi < n; ++vi) {
        weld_offsets[vi] = offset;
        offset += weld_counts[vi];
    }

    weld_counts.zeroclear();
    for (int vi = 0; vi < n; ++vi) {
        int mvi = weld_map[vi];
        int i = weld_offsets[mvi] + weld_counts[mvi]++;
        weld_indices[i] = vi;
    }
}

template<class Indices, class Counts, class Offsets>
inline bool OnEdgeImpl(const Indices& indices, const Counts& counts, const Offsets& offsets, const IArray<float3>& vertices, const ConnectionData& connection, int vertex_index)
{
    int num_shared = connection.v2f_counts[vertex_index];
    int offset = connection.v2f_offsets[vertex_index];

    float angle = 0.0f;
    for (int si = 0; si < num_shared; ++si) {
        int fi = connection.v2f_faces[offset + si];
        int fo = offsets[fi];
        int c = counts[fi];
        if (c < 3) { continue; }
        int nth = connection.v2f_indices[offset + si] - fo;

        int f0 = nth;
        int f1 = f0 - 1; if (f1 < 0) { f1 = c - 1; }
        int f2 = f0 + 1; if (f2 == c) { f2 = 0; }
        float3 v0 = vertices[indices[c * fi + f0]];
        float3 v1 = vertices[indices[c * fi + f1]];
        float3 v2 = vertices[indices[c * fi + f2]];
        angle += angle_between(v1, v2, v0);
    }
    // angle_between's precision seems very low on Mac.. it can be like 357.9f on closed edge
    return angle < 357.0f * Deg2Rad;
}

template<class Indices, class Counts, class Offsets>
inline bool IsEdgeOpenedImpl(
    const Indices& indices, const Counts& counts, const Offsets& offsets, const ConnectionData& connection, int i0, int i1)
{
    if (i1 < i0) { std::swap(i0, i1); }
    int edge[2]{ i0, i1 };

    int num_connection = 0;
    for (int e : edge) {
        int num_shared = connection.v2f_counts[e];
        int offset = connection.v2f_offsets[e];
        for (int si = 0; si < num_shared; ++si) {
            int fi = connection.v2f_faces[offset + si];
            int fo = offsets[fi];
            int c = counts[fi];
            if (c < 3) { continue; }
            for (int ci = 0; ci < c; ++ci) {
                int j0 = indices[fo + ci];
                int j1 = indices[fo + (ci + 1) % c];
                if (j1 < j0) { std::swap(j0, j1); }

                if (i0 == j0 && i1 == j1) { ++num_connection; }
            }
        }
    }
    return num_connection == 2;
}

template<class Indices, class Counts, class Offsets>
struct SelectEdgeImpl
{
    const Indices& indices;
    const Counts& counts;
    const Offsets& offsets;
    const ConnectionData& connection;

    RawVector<bool> checked;
    RawVector<std::pair<int, int>> next_edges;
    RawVector<int> next_points;

    SelectEdgeImpl(const Indices& indices_, const Counts& counts_, const Offsets& offsets_, const IArray<float3>& vertices_,
        const ConnectionData& connection_)
        : indices(indices_)
        , counts(counts_)
        , offsets(offsets_)
        , connection(connection_)
    {
        checked.resize(vertices_.size());
        checked.zeroclear();
    }

    template<class Handler>
    void selectEdge(int vertex_index, const Handler& handler)
    {
        if (checked[vertex_index]) { return; }

        auto check_vertex = [&](int vi) {
            connection.eachConnectedFaces(vi, [&](int fi, int ii) {
                int fo = offsets[fi];
                int c = counts[fi];
                int nth = ii - fo;

                int f0 = nth;
                int f1 = f0 - 1; if (f1 < 0) { f1 = c - 1; }
                int f2 = f0 + 1; if (f2 == c) { f2 = 0; }

                next_edges.push_back({ indices[fo + f0], indices[fo + f1] });
                next_edges.push_back({ indices[fo + f0], indices[fo + f2] });
            });
        };

        check_vertex(vertex_index);

        while (!next_edges.empty()) {
            int i0 = next_edges.back().first;
            int i1 = next_edges.back().second;
            next_edges.pop_back();

            if (checked[i0] && checked[i1]) { continue; }

            if (IsEdgeOpenedImpl(indices, counts, offsets, connection, i0, i1)) {
                if (!checked[i0]) { checked[i0] = true; handler(i0); }
                if (!checked[i1]) { checked[i1] = true; handler(i1); }
                check_vertex(i1);
            }
        }
    }

    template<class Handler>
    void selectHole(int vertex_index, const Handler& handler)
    {
        if (checked[vertex_index]) { return; }

        auto check_vertex = [&](int vi) {
            connection.eachConnectedFaces(vi, [&](int fi, int ii) {
                int fo = offsets[fi];
                int c = counts[fi];
                int nth = ii - fo;

                int f0 = nth;
                int f1 = f0 - 1; if (f1 < 0) { f1 = c - 1; }
                int f2 = f0 + 1; if (f2 == c) { f2 = 0; }

                next_edges.push_back({ indices[fo + f0], indices[fo + f1] });
                next_edges.push_back({ indices[fo + f0], indices[fo + f2] });
            });
        };

        auto handle_welded_vertices = [&](int vi) {
            connection.eachWeldedVertices(vi, [&](int i) {
                handler(i);
            });
        };

        check_vertex(vertex_index);

        while (!next_edges.empty()) {
            int i0 = next_edges.back().first;
            int i1 = next_edges.back().second;
            next_edges.pop_back();

            if (checked[i0] && checked[i1]) { continue; }

            if (IsEdgeOpenedImpl(indices, counts, offsets, connection, i0, i1)) {
                if (!checked[i0]) { checked[i0] = true; handle_welded_vertices(i0); }
                if (!checked[i1]) { checked[i1] = true; handle_welded_vertices(i1); }
                check_vertex(i1);
            }
        }
    }

    template<class Handler>
    void selectConnected(int vertex_index, const Handler& handler)
    {
        next_points.push_back(vertex_index);

        while (!next_points.empty()) {
            int vi = next_points.back();
            next_points.pop_back();

            if (checked[vi]) { continue; }

            checked[vi] = true;
            handler(vi);

            connection.eachConnectedFaces(vi, [&](int fi, int ii) {
                int fo = offsets[fi];
                int c = counts[fi];
                int nth = ii - fo;

                int f0 = nth;
                int f1 = f0 - 1; if (f1 < 0) { f1 = c - 1; }
                int f2 = f0 + 1; if (f2 == c) { f2 = 0; }

                next_points.push_back(indices[fo + f1]);
                next_points.push_back(indices[fo + f2]);
            });
        }
    }
};

} // namespace impl


template<class Handler>
inline void SelectEdge(const IArray<int>& indices, int ngon, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    impl::CountsC counts{ ngon, indices.size() / ngon };
    impl::OffsetsC offsets{ ngon, indices.size() / ngon };

    ConnectionData connection;
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectEdge(i, handler);
    }
}

template<class Handler>
inline void SelectEdge(const IArray<int>& indices, const IArray<int>& counts, const IArray<int>& offsets, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    ConnectionData connection;
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectEdge(i, handler);
    }
}


template<class Handler>
inline void SelectHole(const IArray<int>& indices_, int ngon, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    impl::CountsC counts{ ngon, indices_.size() / ngon };
    impl::OffsetsC offsets{ ngon, indices_.size() / ngon };

    ConnectionData connection;
    impl::BuildWeldMap(connection, vertices);

    impl::IndicesW indices{ indices_, connection.weld_map };
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectHole(i, handler);
    }
}

template<class Handler>
inline void SelectHole(const IArray<int>& indices_, const IArray<int>& counts, const IArray<int>& offsets, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    ConnectionData connection;
    impl::BuildWeldMap(connection, vertices);

    impl::IndicesW indices{ indices_, connection.weld_map };
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectHole(i, handler);
    }
}


template<class Handler>
inline void SelectConnected(const IArray<int>& indices, int ngon, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    impl::CountsC counts{ ngon, indices.size() / ngon };
    impl::OffsetsC offsets{ ngon, indices.size() / ngon };

    ConnectionData connection;
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectConnected(i, handler);
    }
}

template<class Handler>
inline void SelectConnected(const IArray<int>& indices, const IArray<int>& counts, const IArray<int>& offsets, const IArray<float3>& vertices,
    const IArray<int>& vertex_indices, const Handler& handler)
{
    ConnectionData connection;
    impl::BuildConnection(connection, indices, counts, vertices);

    impl::SelectEdgeImpl<decltype(indices), decltype(counts), decltype(offsets)>
        impl(indices, counts, offsets, vertices, connection);

    for (int i : vertex_indices) {
        impl.selectConnected(i, handler);
    }
}


} // namespace mu