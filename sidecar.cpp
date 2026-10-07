#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <wct.h>
#include "sidecar.h"

static volatile LONG bootState;
static ID3D12Device* bootDevice;
static void(*bootLog)(const char*,...);
static volatile LONG creating12;
static DWORD createThread;
static DWORD WINAPI WaitChainThread(void*) {
    Sleep(3000);if(!creating12)return 0;
    HWCT session=OpenThreadWaitChainSession(0,nullptr);if(!session){bootLog("SIDECAR WCT open error=%lu",GetLastError());return 0;}
    WAITCHAIN_NODE_INFO nodes[16]{};DWORD count=16;BOOL cycle=FALSE;
    BOOL ok=GetThreadWaitChain(session,0,1|2|4,createThread,&count,nodes,&cycle);
    bootLog("SIDECAR WCT thread=%lu ok=%d error=%lu nodes=%lu cycle=%d",createThread,ok,ok?0:GetLastError(),count,cycle);
    if(ok)for(DWORD i=0;i<count;++i){auto& n=nodes[i];if(n.ObjectType==WctThreadType)bootLog("SIDECAR WCT node=%lu type=%u status=%u pid=%lu tid=%lu wait_ms=%lu",i,n.ObjectType,n.ObjectStatus,n.ThreadObject.ProcessId,n.ThreadObject.ThreadId,n.ThreadObject.WaitTime);else{char name[512];WideCharToMultiByte(CP_UTF8,0,n.LockObject.ObjectName,-1,name,sizeof(name),nullptr,nullptr);bootLog("SIDECAR WCT node=%lu type=%u status=%u name=%s",i,n.ObjectType,n.ObjectStatus,name);}}
    HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME,FALSE,createThread);
    CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
    if(thread && SuspendThread(thread)!=(DWORD)-1){BOOL sampled=GetThreadContext(thread,&context);ResumeThread(thread);if(sampled){for(UINT frame=0;frame<16 && context.Rip;++frame){HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)context.Rip,&module);wchar_t path[MAX_PATH];GetModuleFileNameW(module,path,MAX_PATH);bootLog("SIDECAR stack frame=%u RIP=%p module=%ls offset=%llx RSP=%p",frame,(void*)context.Rip,path,context.Rip-(UINT_PTR)module,(void*)context.Rsp);DWORD64 imageBase=0;auto function=RtlLookupFunctionEntry(context.Rip,&imageBase,nullptr);if(function){void* handler=nullptr;DWORD64 establisher=0;RtlVirtualUnwind(0,imageBase,context.Rip,function,&context,&handler,&establisher,nullptr);}else{DWORD64 next=0;SIZE_T read=0;if(!ReadProcessMemory(GetCurrentProcess(),(void*)context.Rsp,&next,sizeof(next),&read))break;context.Rip=next;context.Rsp+=8;}}}}
    if(thread)CloseHandle(thread);
    CloseThreadWaitChainSession(session);return 0;
}
static void BeginCreation(){creating12=1;createThread=GetCurrentThreadId();HANDLE watcher=CreateThread(nullptr,0,WaitChainThread,nullptr,0,nullptr);if(watcher)CloseHandle(watcher);}
void BootstrapSidecarBefore11(IDXGIAdapter* adapter,void(*log)(const char*,...)) {
    if(InterlockedCompareExchange(&bootState,1,0))return;
    bootLog=log;log("SIDECAR pre-create D3D12 device before native D3D11 initialization adapter=%p",adapter);
    BeginCreation();HRESULT hr=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),(void**)&bootDevice);creating12=0;
    log("SIDECAR pre-create D3D12CreateDevice hr=%08lx device=%p",hr,bootDevice);
    InterlockedExchange(&bootState,SUCCEEDED(hr)?2:3);
}
static DWORD WINAPI BootstrapThread(void* parameter) {
    auto adapter=(IDXGIAdapter*)parameter;
    ID3D12Device* device=nullptr;
    bootLog("SIDECAR worker creating D3D12 device on same adapter=%p",adapter);
    BeginCreation();HRESULT hr=D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),(void**)&device);creating12=0;
    adapter->Release();bootDevice=device;
    bootLog("SIDECAR worker D3D12CreateDevice hr=%08lx device=%p",hr,device);
    InterlockedExchange(&bootState,SUCCEEDED(hr)?2:3);return 0;
}
void BootstrapSidecar(ID3D11Device* device,void(*log)(const char*,...)) {
    if(InterlockedCompareExchange(&bootState,1,0))return;
    bootLog=log;IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;
    if(FAILED(device->QueryInterface(__uuidof(IDXGIDevice),(void**)&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))){if(dxgi)dxgi->Release();InterlockedExchange(&bootState,3);return;}
    dxgi->Release();HANDLE worker=CreateThread(nullptr,0,BootstrapThread,adapter,0,nullptr);
    if(worker)CloseHandle(worker);else{adapter->Release();InterlockedExchange(&bootState,3);}
}

// A one-frame, read-only sidecar proof. No Skyrim renderer translation, no NGX.
struct Sidecar {
    ID3D11Texture2D* shared11=nullptr;ID3D11Device5* device5=nullptr;ID3D11DeviceContext4* context4=nullptr;ID3D11Fence* fence11=nullptr;
    IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIResource1* resource1=nullptr;
    ID3D12Device* device12=nullptr;ID3D12CommandQueue* queue=nullptr;ID3D12Resource *shared12=nullptr,*readback=nullptr;
    ID3D12Fence* fence=nullptr;ID3D12CommandAllocator* allocator=nullptr;ID3D12GraphicsCommandList* list=nullptr;
    HANDLE textureHandle=nullptr,fenceHandle=nullptr,done=nullptr;bool pending=false;
    ~Sidecar(){
        if(pending)return; // Keep timed-out GPU resources alive until process exit.
        if(list)list->Release();if(allocator)allocator->Release();if(readback)readback->Release();if(shared12)shared12->Release();if(fence)fence->Release();if(queue)queue->Release();if(device12)device12->Release();
        if(fence11)fence11->Release();if(context4)context4->Release();if(device5)device5->Release();if(resource1)resource1->Release();if(shared11)shared11->Release();if(adapter)adapter->Release();if(dxgi)dxgi->Release();
        if(textureHandle)CloseHandle(textureHandle);if(fenceHandle)CloseHandle(fenceHandle);if(done)CloseHandle(done);
    }
};

bool ProbeSidecar(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* back,const char* output,void(*log)(const char*,...)) {
    Sidecar s;HRESULT hr;
#define STEP(label, call) do {hr=(call);log("SIDECAR %s hr=%08lx",label,hr);if(FAILED(hr))return false;}while(0)
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1 || (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)){log("SIDECAR unsupported resolved colour format=%u samples=%u",desc.Format,desc.SampleDesc.Count);return false;}
    STEP("DXGI device",device->QueryInterface(__uuidof(IDXGIDevice),(void**)&s.dxgi));
    STEP("same adapter",s.dxgi->GetAdapter(&s.adapter));
    if(InterlockedCompareExchange(&bootState,0,0)!=2){log("SIDECAR worker not ready state=%ld; no wait on render thread",bootState);return false;}
    s.device12=bootDevice;s.device12->AddRef();
    D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    STEP("CreateCommandQueue",s.device12->CreateCommandQueue(&queueDesc,__uuidof(ID3D12CommandQueue),(void**)&s.queue));
    desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
    STEP("CreateSharedTexture11",device->CreateTexture2D(&desc,nullptr,&s.shared11));
    STEP("IDXGIResource1",s.shared11->QueryInterface(__uuidof(IDXGIResource1),(void**)&s.resource1));
    STEP("CreateSharedHandle(texture)",s.resource1->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&s.textureHandle));
    STEP("OpenSharedHandle12",s.device12->OpenSharedHandle(s.textureHandle,__uuidof(ID3D12Resource),(void**)&s.shared12));
    STEP("CreateFence12",s.device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,__uuidof(ID3D12Fence),(void**)&s.fence));
    STEP("CreateSharedHandle(fence)",s.device12->CreateSharedHandle(s.fence,nullptr,GENERIC_ALL,nullptr,&s.fenceHandle));
    STEP("ID3D11Device5",device->QueryInterface(__uuidof(ID3D11Device5),(void**)&s.device5));
    STEP("OpenSharedFence11",s.device5->OpenSharedFence(s.fenceHandle,__uuidof(ID3D11Fence),(void**)&s.fence11));
    STEP("ID3D11DeviceContext4",context->QueryInterface(__uuidof(ID3D11DeviceContext4),(void**)&s.context4));
    auto resourceDesc=s.shared12->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,totalBytes;
    s.device12->GetCopyableFootprints(&resourceDesc,0,1,0,&footprint,&rows,&rowBytes,&totalBytes);
    D3D12_HEAP_PROPERTIES props{};props.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=totalBytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    STEP("CreateReadback12",s.device12->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),(void**)&s.readback));
    STEP("CreateCommandAllocator",s.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),(void**)&s.allocator));
    STEP("CreateCommandList",s.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator,nullptr,__uuidof(ID3D12GraphicsCommandList),(void**)&s.list));
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=s.shared12;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;s.list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION source{},dest{};source.pResource=s.shared12;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dest.pResource=s.readback;dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dest.PlacedFootprint=footprint;s.list->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;s.list->ResourceBarrier(1,&barrier);STEP("Close",s.list->Close());
    context->CopyResource(s.shared11,back);s.pending=true;
    STEP("Signal11(copy complete)",s.context4->Signal(s.fence11,1));context->Flush();
    STEP("QueueWait12",s.queue->Wait(s.fence,1));ID3D12CommandList* commands[]={s.list};s.queue->ExecuteCommandLists(1,commands);STEP("QueueSignal12(readback complete)",s.queue->Signal(s.fence,2));
    s.done=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!s.done){log("SIDECAR CreateEvent failed=%lu",GetLastError());return false;}
    STEP("SetEventOnCompletion",s.fence->SetEventOnCompletion(2,s.done));
    DWORD waited=WaitForSingleObject(s.done,5000);if(waited!=WAIT_OBJECT_0){log("SIDECAR GPU timeout wait=%lu completed=%llu removed11=%08lx removed12=%08lx; retain resources until process exit",waited,s.fence->GetCompletedValue(),device->GetDeviceRemovedReason(),s.device12->GetDeviceRemovedReason());return false;}
    s.pending=false;
    BYTE* pixels=nullptr;D3D12_RANGE range{0,(SIZE_T)totalBytes};STEP("Map12",s.readback->Map(0,&range,(void**)&pixels));
    char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\sidecar-frame.ppm",output);FILE* file=fopen(path,"wb");
    if(!file){D3D12_RANGE noWrites{0,0};s.readback->Unmap(0,&noWrites);log("SIDECAR fopen failed");return false;}
    fprintf(file,"P6\n%u %u\n255\n",desc.Width,desc.Height);bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM;unsigned long long sum=0;
    for(UINT y=0;y<desc.Height;++y){auto row=pixels+footprint.Offset+y*footprint.Footprint.RowPitch;for(UINT x=0;x<desc.Width;++x){unsigned char rgb[]={row[x*4+(bgra?2:0)],row[x*4+1],row[x*4+(bgra?0:2)]};fwrite(rgb,1,3,file);sum+=rgb[0]+rgb[1]+rgb[2];}}
    fclose(file);D3D12_RANGE noWrites{0,0};s.readback->Unmap(0,&noWrites);
    log("SIDECAR VERIFIED D3D11 -> shared NT texture -> same-adapter D3D12 -> readback %s rgb_sum=%llu fence=%llu",path,sum,s.fence->GetCompletedValue());return true;
#undef STEP
}
