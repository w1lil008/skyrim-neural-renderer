// Optional, bounded CPU observation; no resource references or GPU waits.
// C interface declarations expose the SDK's exact COM method slot layout and
// its aggregate-return wrappers, avoiding hard-coded interface assumptions.
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include "temporal_gpu_watch.h"
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <MinHook.h>

namespace TemporalGpuWatch {
namespace {
constexpr UINT MAP_CAPACITY=256, ROOT_CAPACITY=64;
static_assert(offsetof(ID3D12DeviceVtbl,CreateShaderResourceView)/sizeof(void*)==18,"SDK SRV slot");
static_assert(offsetof(ID3D12DeviceVtbl,CopyDescriptors)/sizeof(void*)==23,"SDK descriptor copy slot");
static_assert(offsetof(ID3D12DeviceVtbl,CopyDescriptorsSimple)/sizeof(void*)==24,"SDK simple copy slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,Dispatch)/sizeof(void*)==14,"SDK dispatch slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,SetDescriptorHeaps)/sizeof(void*)==28,"SDK heap slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,SetComputeRootSignature)/sizeof(void*)==29,"SDK root signature slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,SetComputeRootDescriptorTable)/sizeof(void*)==31,"SDK root table slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,CopyResource)/sizeof(void*)==17,"SDK resource copy slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,CopyTextureRegion)/sizeof(void*)==16,"SDK texture copy slot");
static_assert(offsetof(ID3D12GraphicsCommandListVtbl,ExecuteIndirect)/sizeof(void*)==59,"SDK indirect slot");
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,InitializeMetaCommand)/sizeof(void*)==70,"SDK meta initialize slot");
static_assert(offsetof(ID3D12GraphicsCommandList4Vtbl,ExecuteMetaCommand)/sizeof(void*)==71,"SDK meta execute slot");

struct Descriptor { void* device; SIZE_T cpu; ID3D12Resource* resource; bool srv; };
Descriptor descriptors[MAP_CAPACITY]{};
UINT nextDescriptor;
SRWLOCK descriptorLock=SRWLOCK_INIT, installLock=SRWLOCK_INIT;
struct Watch {
    bool active;
    ID3D12Device* device;
    void* deviceIdentity;
    ID3D12GraphicsCommandList* list;
    void* listIdentity;
    ID3D12Resource *depth,*motion;
    ID3D12RootSignature* rootSignature;
    uint64_t serial;
    UINT increment, heapCount;
    SIZE_T heapCpu;
    UINT64 heapGpu;
    bool rootsValid[ROOT_CAPACITY];
    UINT64 roots[ROOT_CAPACITY];
    struct Provenance {ID3D12Resource* resource;UINT mask;} copies[128];
    UINT nextCopy;
    Stats stats;
};
thread_local Watch watch{};

using SrvFn=decltype(ID3D12DeviceVtbl::CreateShaderResourceView);
using CbvFn=decltype(ID3D12DeviceVtbl::CreateConstantBufferView);
using UavFn=decltype(ID3D12DeviceVtbl::CreateUnorderedAccessView);
using CopyFn=decltype(ID3D12DeviceVtbl::CopyDescriptors);
using CopySimpleFn=decltype(ID3D12DeviceVtbl::CopyDescriptorsSimple);
using HeapsFn=decltype(ID3D12GraphicsCommandListVtbl::SetDescriptorHeaps);
using SignatureFn=decltype(ID3D12GraphicsCommandListVtbl::SetComputeRootSignature);
using TableFn=decltype(ID3D12GraphicsCommandListVtbl::SetComputeRootDescriptorTable);
using DispatchFn=decltype(ID3D12GraphicsCommandListVtbl::Dispatch);
using ResetFn=decltype(ID3D12GraphicsCommandListVtbl::Reset);
using ClearFn=decltype(ID3D12GraphicsCommandListVtbl::ClearState);
using BundleFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteBundle);
using ResourceCopyFn=decltype(ID3D12GraphicsCommandListVtbl::CopyResource);
using TextureCopyFn=decltype(ID3D12GraphicsCommandListVtbl::CopyTextureRegion);
using IndirectFn=decltype(ID3D12GraphicsCommandListVtbl::ExecuteIndirect);
SrvFn originalSrv;
CbvFn originalCbv;
UavFn originalUav;
CopyFn originalCopy;
CopySimpleFn originalCopySimple;
HeapsFn originalHeaps;
SignatureFn originalSignature;
TableFn originalTable;
DispatchFn originalDispatch;
ResetFn originalReset;
ClearFn originalClear;
BundleFn originalBundle;
ResourceCopyFn originalResourceCopy;
TextureCopyFn originalTextureCopy;
IndirectFn originalIndirect;
using MetaInitFn=decltype(ID3D12GraphicsCommandList4Vtbl::InitializeMetaCommand);
using MetaExecuteFn=decltype(ID3D12GraphicsCommandList4Vtbl::ExecuteMetaCommand);
using MetaCreateFn=decltype(ID3D12Device5Vtbl::CreateMetaCommand);
MetaInitFn originalMetaInit;
MetaExecuteFn originalMetaExecute;
MetaCreateFn originalMetaCreate;
void* metaTargets[3]{};
bool metaReady;
struct MetaSchema {
    void *identity,*device;
    GUID id;
    UINT count;
    bool known;
    struct Field {D3D12_META_COMMAND_PARAMETER_TYPE type;D3D12_META_COMMAND_PARAMETER_FLAGS flags;UINT offset;} fields[64];
};
MetaSchema schemas[32]{};UINT nextSchema;
bool attempted,ready;
constexpr UINT HOOK_COUNT=15;
void* installedTargets[HOOK_COUNT]{};

// All access to the fixed map is protected; stored pointers are identities
// only, never dereferenced or retained. Overwritten non-SRVs erase old entries.
void* objectKey(void* object){
    if(!object)return nullptr;
    IUnknown* identity=nullptr;
    if(FAILED(IUnknown_QueryInterface((IUnknown*)object,IID_IUnknown,(void**)&identity))||!identity)return object;
    void* key=identity;IUnknown_Release(identity);return key;
}
void* deviceKey(ID3D12Device* d){return objectKey(d);}
ID3D12Resource* lookupLocked(void* d,SIZE_T cpu) {
    for(auto& entry:descriptors)if(entry.device==d&&entry.cpu==cpu)return entry.srv?entry.resource:nullptr;
    return nullptr;
}
void storeLocked(void* d,SIZE_T cpu,ID3D12Resource* resource,bool isSrv=true) {
    if(!cpu)return;
    Descriptor* slot=nullptr;
    for(auto& entry:descriptors)if(entry.device==d&&entry.cpu==cpu){slot=&entry;break;}
    if(!resource){if(slot)*slot={};return;}
    if(!slot)for(auto& entry:descriptors)if(!entry.cpu){slot=&entry;break;}
    if(!slot)slot=&descriptors[nextDescriptor++%MAP_CAPACITY];
    *slot={d,cpu,resource,isSrv};
}
void eraseDeviceLocked(void* d) {for(auto& entry:descriptors)if(entry.device==d)entry={};}
void remember(ID3D12Device* d,SIZE_T cpu,ID3D12Resource* resource,bool isSrv=true) {
    auto key=deviceKey(d);AcquireSRWLockExclusive(&descriptorLock);storeLocked(key,cpu,resource,isSrv);ReleaseSRWLockExclusive(&descriptorLock);
}
void clearRoots() {for(auto& valid:watch.rootsValid)valid=false;}
bool matching(ID3D12GraphicsCommandList* list) {return watch.active&&(watch.list==list||watch.listIdentity==objectKey(list));}
UINT provenance(ID3D12Resource* resource){
    if(!resource)return 0;
    UINT mask=(resource==watch.depth?1:0)|(resource==watch.motion?2:0);
    for(auto& copy:watch.copies)if(copy.resource==resource)return mask|copy.mask;
    return mask;
}
void copiedTo(ID3D12Resource* dest,UINT mask){
    if(!dest)return;
    Watch::Provenance* slot=nullptr;
    for(auto& copy:watch.copies)if(copy.resource==dest){slot=&copy;break;}
    if(!mask){if(slot)*slot={};return;}
    if(!slot)for(auto& copy:watch.copies)if(!copy.resource){slot=&copy;break;}
    if(!slot)slot=&watch.copies[watch.nextCopy++%128];
    *slot={dest,mask};
}

void STDMETHODCALLTYPE srv(ID3D12Device* d,ID3D12Resource* resource,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    originalSrv(d,resource,desc,cpu);
    remember(d,cpu.ptr,resource);
    if(watch.active&&watch.deviceIdentity==deviceKey(d)&&resource){
        if(resource==watch.depth)++watch.stats.depthSrvCreated;
        if(resource==watch.motion)++watch.stats.motionSrvCreated;
        UINT mask=provenance(resource);
        if(resource!=watch.depth&&(mask&1))++watch.stats.depthCopiedSrvCreated;
        if(resource!=watch.motion&&(mask&2))++watch.stats.motionCopiedSrvCreated;
    }
}
void STDMETHODCALLTYPE cbv(ID3D12Device* d,const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    originalCbv(d,desc,cpu);remember(d,cpu.ptr,nullptr);
}
void STDMETHODCALLTYPE uav(ID3D12Device* d,ID3D12Resource* resource,ID3D12Resource* counter,
                           const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    originalUav(d,resource,counter,desc,cpu);remember(d,cpu.ptr,resource,false);
}
void STDMETHODCALLTYPE copySimple(ID3D12Device* d,UINT count,D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                  D3D12_CPU_DESCRIPTOR_HANDLE source,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    // Snapshot before calling through: implementations can delegate internally.
    ID3D12Resource* copied[MAP_CAPACITY]{};
    UINT increment=0;
    bool track=type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if(track){
        auto key=deviceKey(d);
        increment=ID3D12Device_GetDescriptorHandleIncrementSize(d,type);
        AcquireSRWLockExclusive(&descriptorLock);
        if(count<=MAP_CAPACITY&&increment)for(UINT i=0;i<count;++i)copied[i]=lookupLocked(key,source.ptr+SIZE_T(i)*increment);
        else eraseDeviceLocked(key); // conservative: never retain overwritten stale descriptors
        ReleaseSRWLockExclusive(&descriptorLock);
    }
    originalCopySimple(d,count,dest,source,type);
    if(track&&count<=MAP_CAPACITY&&increment){
        auto key=deviceKey(d);
        AcquireSRWLockExclusive(&descriptorLock);
        for(UINT i=0;i<count;++i)storeLocked(key,dest.ptr+SIZE_T(i)*increment,copied[i]);
        ReleaseSRWLockExclusive(&descriptorLock);
    }
}
void STDMETHODCALLTYPE copyGeneral(ID3D12Device* d,UINT destRanges,const D3D12_CPU_DESCRIPTOR_HANDLE* dest,
                                   const UINT* destSizes,UINT sourceRanges,const D3D12_CPU_DESCRIPTOR_HANDLE* source,
                                   const UINT* sourceSizes,D3D12_DESCRIPTOR_HEAP_TYPE type) {
    SIZE_T destination[MAP_CAPACITY]{};
    ID3D12Resource* copied[MAP_CAPACITY]{};
    UINT total=0;
    bool track=type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    if(track){
        auto key=deviceKey(d);
        const UINT increment=ID3D12Device_GetDescriptorHandleIncrementSize(d,type);
        bool bounded=increment&&destRanges<=MAP_CAPACITY&&sourceRanges<=MAP_CAPACITY&&
                     (!destRanges||dest)&&(!sourceRanges||source);
        UINT sourceTotal=0;
        if(bounded)for(UINT i=0;i<destRanges;++i){UINT n=destSizes?destSizes[i]:1;if(n>MAP_CAPACITY-total){bounded=false;break;}total+=n;}
        if(bounded)for(UINT i=0;i<sourceRanges;++i){UINT n=sourceSizes?sourceSizes[i]:1;if(n>MAP_CAPACITY-sourceTotal){bounded=false;break;}sourceTotal+=n;}
        bounded=bounded&&sourceTotal==total;
        AcquireSRWLockExclusive(&descriptorLock);
        if(bounded){
            UINT index=0;
            for(UINT i=0;i<destRanges;++i)for(UINT j=0,n=destSizes?destSizes[i]:1;j<n;++j)destination[index++]=dest[i].ptr+SIZE_T(j)*increment;
            index=0;
            for(UINT i=0;i<sourceRanges;++i)for(UINT j=0,n=sourceSizes?sourceSizes[i]:1;j<n;++j)copied[index++]=lookupLocked(key,source[i].ptr+SIZE_T(j)*increment);
        }else {eraseDeviceLocked(key);total=0;}
        ReleaseSRWLockExclusive(&descriptorLock);
    }
    originalCopy(d,destRanges,dest,destSizes,sourceRanges,source,sourceSizes,type);
    if(track&&total){
        auto key=deviceKey(d);
        AcquireSRWLockExclusive(&descriptorLock);
        for(UINT i=0;i<total;++i)storeLocked(key,destination[i],copied[i]);
        ReleaseSRWLockExclusive(&descriptorLock);
    }
}

void STDMETHODCALLTYPE heaps(ID3D12GraphicsCommandList* list,UINT count,ID3D12DescriptorHeap*const* bound) {
    originalHeaps(list,count,bound);
    if(!matching(list))return;
    ++watch.stats.descriptorHeapSetCalls;
    // Repeated identical heap binds are conservatively invalidated too.
    clearRoots();watch.heapCount=0;watch.heapCpu=0;watch.heapGpu=0;
    for(UINT i=0;bound&&i<count;++i)if(bound[i]){
        auto desc=ID3D12DescriptorHeap_GetDesc(bound[i]);
        if(desc.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||!(desc.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))continue;
        watch.heapCount=desc.NumDescriptors;
        watch.heapCpu=ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(bound[i]).ptr;
        watch.heapGpu=ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(bound[i]).ptr;
        break;
    }
}
void STDMETHODCALLTYPE signature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* root) {
    originalSignature(list,root);
    if(matching(list)){++watch.stats.rootSignatureSetCalls;clearRoots();watch.rootSignature=root;}
}
void STDMETHODCALLTYPE table(ID3D12GraphicsCommandList* list,UINT index,D3D12_GPU_DESCRIPTOR_HANDLE start) {
    originalTable(list,index,start);
    if(matching(list)){++watch.stats.rootTableSetCalls;if(index<ROOT_CAPACITY){watch.roots[index]=start.ptr;watch.rootsValid[index]=true;}}
}
void STDMETHODCALLTYPE dispatch(ID3D12GraphicsCommandList* list,UINT x,UINT y,UINT z) {
    if(matching(list))++watch.stats.dispatchCalls;
    if(matching(list)&&x&&y&&z&&watch.heapCount&&watch.increment){
        bool depth=false,motion=false,copiedDepth=false,copiedMotion=false;
        AcquireSRWLockShared(&descriptorLock);
        for(UINT i=0;i<ROOT_CAPACITY;++i)if(watch.rootsValid[i]&&watch.roots[i]>=watch.heapGpu){
            const UINT64 offset=watch.roots[i]-watch.heapGpu;
            if(offset%watch.increment||offset/watch.increment>=watch.heapCount)continue;
            auto resource=lookupLocked(watch.deviceIdentity,watch.heapCpu+SIZE_T(offset));
            depth|=watch.depth&&resource==watch.depth;
            motion|=watch.motion&&resource==watch.motion;
            UINT mask=provenance(resource);
            copiedDepth|=resource!=watch.depth&&(mask&1);
            copiedMotion|=resource!=watch.motion&&(mask&2);
        }
        ReleaseSRWLockShared(&descriptorLock);
        if(depth)++watch.stats.depthDispatchBindings;
        if(motion)++watch.stats.motionDispatchBindings;
        if(copiedDepth)++watch.stats.depthCopiedDispatchBindings;
        if(copiedMotion)++watch.stats.motionCopiedDispatchBindings;
    }
    originalDispatch(list,x,y,z);
}
void STDMETHODCALLTYPE resourceCopy(ID3D12GraphicsCommandList* list,ID3D12Resource* dest,ID3D12Resource* source){
    if(matching(list)){
        if(source&&source==watch.depth)++watch.stats.depthCopySourceCalls;
        if(source&&source==watch.motion)++watch.stats.motionCopySourceCalls;
        copiedTo(dest,provenance(source));
    }
    originalResourceCopy(list,dest,source);
}
void STDMETHODCALLTYPE textureCopy(ID3D12GraphicsCommandList* list,const D3D12_TEXTURE_COPY_LOCATION* dest,
    UINT x,UINT y,UINT z,const D3D12_TEXTURE_COPY_LOCATION* source,const D3D12_BOX* box){
    if(matching(list)&&source&&dest){
        if(source->pResource&&source->pResource==watch.depth)++watch.stats.depthCopySourceCalls;
        if(source->pResource&&source->pResource==watch.motion)++watch.stats.motionCopySourceCalls;
        UINT mask=0;
        if(!box&&!x&&!y&&!z&&source->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&dest->Type==D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX&&
           source->SubresourceIndex==0&&dest->SubresourceIndex==0&&source->pResource&&dest->pResource){
            auto s=ID3D12Resource_GetDesc(source->pResource),d=ID3D12Resource_GetDesc(dest->pResource);
            if(s.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Dimension==s.Dimension&&s.Width==d.Width&&s.Height==d.Height&&
               s.DepthOrArraySize==1&&d.DepthOrArraySize==1&&s.MipLevels==1&&d.MipLevels==1&&s.Format==d.Format)mask=provenance(source->pResource);
        }
        copiedTo(dest->pResource,mask); // partial/unrecognized overwrite invalidates provenance
    }
    originalTextureCopy(list,dest,x,y,z,source,box);
}
void STDMETHODCALLTYPE indirect(ID3D12GraphicsCommandList* list,ID3D12CommandSignature* signature,UINT maxCount,
    ID3D12Resource* args,UINT64 argsOffset,ID3D12Resource* counts,UINT64 countOffset){
    if(matching(list))++watch.stats.executeIndirectCalls;
    originalIndirect(list,signature,maxCount,args,argsOffset,counts,countOffset);
}
HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* initial) {
    HRESULT result=originalReset(list,allocator,initial);
    if(SUCCEEDED(result)&&matching(list)){clearRoots();watch.rootSignature=nullptr;watch.heapCount=0;}
    return result;
}
void STDMETHODCALLTYPE clear(ID3D12GraphicsCommandList* list,ID3D12PipelineState* state) {
    originalClear(list,state);
    if(matching(list)){clearRoots();watch.rootSignature=nullptr;watch.heapCount=0;}
}
void STDMETHODCALLTYPE bundle(ID3D12GraphicsCommandList* list,ID3D12GraphicsCommandList* child) {
    originalBundle(list,child);
    // Bundle state can be inherited by its parent, but child calls are outside
    // our exact-list scope. Invalidate rather than claim stale table bindings.
    if(matching(list)){clearRoots();watch.rootSignature=nullptr;watch.heapCount=0;}
}

HRESULT STDMETHODCALLTYPE metaCreate(ID3D12Device5* d,REFGUID id,UINT node,const void* data,SIZE_T size,REFIID iid,void** output){
    HRESULT result=originalMetaCreate(d,id,node,data,size,iid,output);
    if(SUCCEEDED(result)&&output&&*output){
        MetaSchema schema{};schema.identity=objectKey(*output);schema.device=objectKey(d);schema.id=id;
        UINT bytes=0,count=0;
        HRESULT enumerated=ID3D12Device5_EnumerateMetaCommandParameters(d,id,D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,&bytes,&count,nullptr);
        if(SUCCEEDED(enumerated)&&count<=64){
            D3D12_META_COMMAND_PARAMETER_DESC descriptions[64]{};UINT capacity=count;
            enumerated=ID3D12Device5_EnumerateMetaCommandParameters(d,id,D3D12_META_COMMAND_PARAMETER_STAGE_EXECUTION,&bytes,&capacity,descriptions);
            if(SUCCEEDED(enumerated)&&capacity<=64){schema.known=true;schema.count=capacity;
                for(UINT i=0;i<capacity;++i)schema.fields[i]={descriptions[i].Type,descriptions[i].Flags,descriptions[i].StructureOffset};}
        }
        AcquireSRWLockExclusive(&descriptorLock);
        MetaSchema* slot=nullptr;for(auto& known:schemas)if(known.identity==schema.identity){slot=&known;break;}
        if(!slot)slot=&schemas[nextSchema++%32];*slot=schema;
        ReleaseSRWLockExclusive(&descriptorLock);
    }
    return result;
}
void STDMETHODCALLTYPE metaInitialize(ID3D12GraphicsCommandList4* list,ID3D12MetaCommand* command,const void* data,SIZE_T size){
    if(matching((ID3D12GraphicsCommandList*)list))++watch.stats.metaInitializeCalls;
    originalMetaInit(list,command,data,size);
}
ID3D12Resource* metaDescriptorResourceLocked(SIZE_T cpu){
    // The SDK metacommand type explicitly accepts SRV/UAV/CBV descriptors.
    // Here a witnessed descriptor's resource identity is sufficient; the
    // parameter's official INPUT flag supplies its role, never heap membership.
    for(auto& entry:descriptors)if(entry.device==watch.deviceIdentity&&entry.cpu==cpu)return entry.resource;
    return nullptr;
}
void STDMETHODCALLTYPE metaExecute(ID3D12GraphicsCommandList4* list,ID3D12MetaCommand* command,const void* data,SIZE_T size){
    if(matching((ID3D12GraphicsCommandList*)list)){
        ++watch.stats.metaExecuteCalls;auto identity=objectKey(command);
        watch.stats.metaIdentityLast=(uint64_t)(uintptr_t)identity;watch.stats.metaParameterBytesLast=size;
        watch.stats.metaParameterHashLast=watch.stats.metaBytesHashedLast=0;
        if(data&&size){
            const auto* bytes=(const unsigned char*)data;SIZE_T n=size<256?size:256;
            uint64_t hash=14695981039346656037ULL;for(SIZE_T i=0;i<n;++i){hash^=bytes[i];hash*=1099511628211ULL;}
            watch.stats.metaParameterHashLast=hash;watch.stats.metaBytesHashedLast=n;
        }
        MetaSchema schema{};
        AcquireSRWLockShared(&descriptorLock);
        for(auto& known:schemas)if(known.identity==identity&&known.device==watch.deviceIdentity){schema=known;break;}
        watch.stats.metaGuidKnownLast=schema.identity!=nullptr;
        watch.stats.metaGuidLowLast=watch.stats.metaGuidHighLast=0;
        if(schema.identity){memcpy(&watch.stats.metaGuidLowLast,&schema.id,8);memcpy(&watch.stats.metaGuidHighLast,(const unsigned char*)&schema.id+8,8);}
        if(schema.known){
            ++watch.stats.metaSchemaKnownCalls;bool depth=false,motion=false;
            for(UINT i=0;data&&i<schema.count;++i){
                const auto& field=schema.fields[i];
                if(!(field.flags&D3D12_META_COMMAND_PARAMETER_FLAG_INPUT)||field.offset>size||size-field.offset<8)continue;
                SIZE_T cpu=0;UINT64 value=0;memcpy(&value,(const unsigned char*)data+field.offset,8);
                if(field.type==D3D12_META_COMMAND_PARAMETER_TYPE_CPU_DESCRIPTOR_HANDLE_HEAP_TYPE_CBV_SRV_UAV)cpu=(SIZE_T)value;
                else if(field.type==D3D12_META_COMMAND_PARAMETER_TYPE_GPU_DESCRIPTOR_HANDLE_HEAP_TYPE_CBV_SRV_UAV&&watch.increment&&value>=watch.heapGpu){
                    UINT64 offset=value-watch.heapGpu;
                    if(offset%watch.increment==0&&offset/watch.increment<watch.heapCount)cpu=watch.heapCpu+(SIZE_T)offset;
                }
                // Opaque UINT64 fields and GPU virtual addresses are NOT scanned
                // as handles. Native texture GPU VAs are not valid evidence.
                auto resource=cpu?metaDescriptorResourceLocked(cpu):nullptr;
                depth|=watch.depth&&resource==watch.depth;motion|=watch.motion&&resource==watch.motion;
            }
            if(depth)++watch.stats.depthMetaInputBindings;if(motion)++watch.stats.motionMetaInputBindings;
        }
        ReleaseSRWLockShared(&descriptorLock);
        // The meta command's documented dirty states are not available for a
        // pre-existing object with no witnessed GUID. Invalidate root state.
        clearRoots();
    }
    originalMetaExecute(list,command,data,size);
}
bool optionalHook(void* target,void* callback,void** original){
    if(!target||MH_CreateHook(target,callback,original)!=MH_OK)return false;
    if(MH_EnableHook(target)==MH_OK)return true;
    MH_RemoveHook(target);*original=nullptr;return false;
}
void installMeta(ID3D12Device* d,ID3D12GraphicsCommandList* list){
    ID3D12GraphicsCommandList4* list4=nullptr;
    if(SUCCEEDED(ID3D12GraphicsCommandList_QueryInterface(list,IID_ID3D12GraphicsCommandList4,(void**)&list4))&&list4){
        metaTargets[0]=(void*)list4->lpVtbl->InitializeMetaCommand;metaTargets[1]=(void*)list4->lpVtbl->ExecuteMetaCommand;
        optionalHook(metaTargets[0],(void*)metaInitialize,(void**)&originalMetaInit);
        metaReady=optionalHook(metaTargets[1],(void*)metaExecute,(void**)&originalMetaExecute);
        ID3D12GraphicsCommandList4_Release(list4);
    }
    ID3D12Device5* device5=nullptr;
    if(SUCCEEDED(ID3D12Device_QueryInterface(d,IID_ID3D12Device5,(void**)&device5))&&device5){
        metaTargets[2]=(void*)device5->lpVtbl->CreateMetaCommand;
        optionalHook(metaTargets[2],(void*)metaCreate,(void**)&originalMetaCreate);
        ID3D12Device5_Release(device5);
    }
}

bool install(ID3D12Device* d,ID3D12GraphicsCommandList* list) {
    void* targets[]={
        (void*)d->lpVtbl->CreateShaderResourceView,(void*)d->lpVtbl->CopyDescriptors,
        (void*)d->lpVtbl->CopyDescriptorsSimple,(void*)list->lpVtbl->SetDescriptorHeaps,
        (void*)list->lpVtbl->SetComputeRootSignature,(void*)list->lpVtbl->SetComputeRootDescriptorTable,
        (void*)list->lpVtbl->Dispatch,(void*)d->lpVtbl->CreateConstantBufferView,(void*)d->lpVtbl->CreateUnorderedAccessView,
        (void*)list->lpVtbl->Reset,(void*)list->lpVtbl->ClearState,(void*)list->lpVtbl->ExecuteBundle,
        (void*)list->lpVtbl->CopyResource,(void*)list->lpVtbl->CopyTextureRegion,(void*)list->lpVtbl->ExecuteIndirect
    };
    void* hooks[]={(void*)srv,(void*)copyGeneral,(void*)copySimple,(void*)heaps,(void*)signature,(void*)table,(void*)dispatch,(void*)cbv,(void*)uav,
                  (void*)reset,(void*)clear,(void*)bundle,(void*)resourceCopy,(void*)textureCopy,(void*)indirect};
    void** originals[]={(void**)&originalSrv,(void**)&originalCopy,(void**)&originalCopySimple,(void**)&originalHeaps,
                      (void**)&originalSignature,(void**)&originalTable,(void**)&originalDispatch,(void**)&originalCbv,(void**)&originalUav,
                      (void**)&originalReset,(void**)&originalClear,(void**)&originalBundle,
                      (void**)&originalResourceCopy,(void**)&originalTextureCopy,(void**)&originalIndirect};
    AcquireSRWLockExclusive(&installLock);
    if(attempted){
        bool compatible=ready;
        for(UINT i=0;i<HOOK_COUNT;++i)compatible&=installedTargets[i]==targets[i];
        ReleaseSRWLockExclusive(&installLock);return compatible;
    }
    attempted=true;
    UINT created=0,enabled=0;
    for(;created<HOOK_COUNT;++created){
        installedTargets[created]=targets[created];
        if(!targets[created]||MH_CreateHook(targets[created],hooks[created],originals[created])!=MH_OK)break;
    }
    if(created==HOOK_COUNT)for(;enabled<HOOK_COUNT;++enabled)if(MH_EnableHook(targets[enabled])!=MH_OK)break;
    ready=created==HOOK_COUNT&&enabled==HOOK_COUNT;
    if(!ready){
        // Never use MH_ALL_HOOKS: the working DXL/NR interception stays intact.
        for(UINT i=0;i<enabled;++i)MH_DisableHook(targets[i]);
        // Keep formerly active trampolines allocated until process exit. An
        // in-flight unrelated device call may still be returning through one.
        for(UINT i=enabled;i<created;++i)MH_RemoveHook(targets[i]);
    }
    if(ready)installMeta(d,list);
    ReleaseSRWLockExclusive(&installLock);return ready;
}
}

bool BeginWatch(ID3D12Device* d,ID3D12GraphicsCommandList* list,
                ID3D12Resource* depth,ID3D12Resource* motion,uint64_t serial) {
    if(watch.active){watch={};return false;} // nested attribution is unknown
    watch={};
    if(!d||!list||!install(d,list))return false;
    watch.device=d;watch.list=list;watch.depth=depth;watch.motion=motion;watch.serial=serial;
    watch.deviceIdentity=deviceKey(d);
    watch.listIdentity=objectKey(list);watch.stats.metaHookAvailable=metaReady;
    watch.increment=ID3D12Device_GetDescriptorHandleIncrementSize(d,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    watch.active=watch.increment!=0;
    return watch.active;
}
Stats EndWatch(){Stats result=watch.active?watch.stats:Stats{};watch={};return result;}
}
