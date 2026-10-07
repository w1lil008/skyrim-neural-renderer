#pragma once
#include <d3d12.h>
#include <stdint.h>

namespace TemporalGpuWatch {
struct Stats {
    uint64_t depthSrvCreated = 0;
    uint64_t motionSrvCreated = 0;
    uint64_t depthDispatchBindings = 0;
    uint64_t motionDispatchBindings = 0;
    uint64_t depthCopySourceCalls = 0;
    uint64_t motionCopySourceCalls = 0;
    uint64_t depthCopiedSrvCreated = 0;
    uint64_t motionCopiedSrvCreated = 0;
    uint64_t depthCopiedDispatchBindings = 0;
    uint64_t motionCopiedDispatchBindings = 0;
    uint64_t dispatchCalls = 0;
    uint64_t executeIndirectCalls = 0;
    uint64_t rootTableSetCalls = 0;
    uint64_t descriptorHeapSetCalls = 0;
    uint64_t rootSignatureSetCalls = 0;
    uint64_t metaInitializeCalls = 0;
    uint64_t metaExecuteCalls = 0;
    uint64_t metaHookAvailable = 0;
    uint64_t metaSchemaKnownCalls = 0;
    uint64_t depthMetaInputBindings = 0;
    uint64_t motionMetaInputBindings = 0;
    uint64_t metaIdentityLast = 0;
    uint64_t metaParameterBytesLast = 0;
    uint64_t metaParameterHashLast = 0;
    uint64_t metaBytesHashedLast = 0;
    uint64_t metaGuidKnownLast = 0;
    uint64_t metaGuidLowLast = 0;
    uint64_t metaGuidHighLast = 0;
};

// MinHook must already be initialized. The scope observes only this command
// list on the calling thread. Pair every BeginWatch with EndWatch. False means
// the observer is unavailable; EndWatch then returns zero counters.
// Null depth/motion are allowed for baseline command-event observation.
bool BeginWatch(ID3D12Device*, ID3D12GraphicsCommandList*,
                ID3D12Resource* depth, ID3D12Resource* motion, uint64_t serial);
Stats EndWatch();

// Dispatch counters mean an observed compute root-table START resolved exactly
// to an SRV for the supplied native input at Dispatch recording time. They do
// not imply shader access or completed GPU execution. Zero is unknown/a lower
// bound: interior descriptors, root SRVs, bundles, indirect dispatches, prior
// root state, unobserved threads/lists, and descriptor-map eviction are omitted.
// Copy-source counters observe command recording, not GPU completion. Copied
// binding counters track full-copy provenance within this call; they do not
// prove the texture remains unchanged after other unobserved GPU writes.
}
