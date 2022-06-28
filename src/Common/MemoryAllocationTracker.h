#pragma once
#include <Common/PODArray_fwd.h>
#include <vector>

namespace MemoryAllocationTracker
{

struct Trace
{
    using Frames = std::vector<uintptr_t>;

    Frames frames;

    /// The total number of bytes allocated for traces with the same prefix.
    size_t allocated_total = 0;
    /// This counter is relevant in case we want to filter some traces with small amount of bytes.
    /// It shows the total number of bytes for *filtered* traces with the same prefix.
    /// This is the value whis is used in flamegraph.
    size_t allocated_self = 0;
};

using Traces = std::vector<Trace>;

Traces dump_allocations(size_t max_depth, size_t max_bytes);

struct DumpTree
{
    struct Node
    {
        uintptr_t id{};
        const void * ptr{};
        size_t allocated{};
    };

    struct Edge
    {
        uintptr_t from{};
        uintptr_t to{};
    };

    using Nodes = std::vector<Node>;
    using Edges = std::vector<Edge>;

    Nodes nodes;
    Edges edges;
};

DumpTree dump_allocations_tree(size_t max_depth, size_t max_bytes);

void dump_allocations_tree(DB::PaddedPODArray<UInt8> & chars, DB::PaddedPODArray<UInt64> & offsets, size_t max_depth, size_t max_bytes);
void dump_allocations_flamegraph(DB::PaddedPODArray<UInt8> & chars, DB::PaddedPODArray<UInt64> & offsets, size_t max_depth, size_t max_bytes);

}
