// Optional Skyrim D3D11 input acquisition. The tested DXL binary is unchanged.
// Copy-in identifies the same frame that DXL will evaluate. Its existing two
// shared fences order our uploads and retire NR reads before the next overwrite.
// No additional Present hook, D3D12 device, game offsets, or CPU GPU waits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <MinHook.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "temporal.h"
#include "temporal_gpu_watch.h"
#include "temporal_sensitivity.h"
extern "C" void* TemporalSwapStateTarget(ID3D11DeviceContext1*);

namespace {
TemporalLog logLine;
char directory[MAX_PATH];
bool enabled, feedDepth, feedMotion;
bool captureBlocked;
uint64_t validatedShader;
UINT validatedWidth,validatedHeight,validatedDepthFormat,validatedMotionFormat;
bool spatialDepthValidated,spatialMotionValidated;
ID3D11Device* device;
ID3D11DeviceContext* context;
ID3D11DeviceContext1* context1;
ID3DDeviceContextState* privateState;
IDXGISwapChain* swapChain;
UINT width,height;
uint64_t frame=1, calls, failures, depthReads, motionReads, rejected;
uint64_t sensitivityNativeEpoch;bool sensitivityEffectiveReset;
TemporalGpuWatch::Stats gpuWatchLast{},gpuWatchTotal{};
bool gpuWatchRequestedLast,gpuWatchReadyLast,sharedDeviceMatchLast,nativeDepthOfferedLast,nativeMotionOfferedLast;
bool routeResetRequestedLast,routeResetReadLast;
uint64_t gpuWatchSerialLast,gpuWatchRequests,gpuWatchUnavailable,sharedDeviceRejections;
void writeGpuTelemetry(FILE*,const char* event);
uint64_t taaSerial, lastTaaUsed, taaShader;
thread_local bool internal;
DWORD renderThread;
const GUID shaderCodeId={0x72819273,0xa771,0x49cc,{0xbb,0x55,0x51,0x4b,0x2d,0x7e,0x01,0x06}};
const GUID debugNameWideId={0x4cca5fd8,0x921f,0x42c8,{0x85,0x66,0x70,0xca,0xf2,0xa9,0xb7,0x41}};
ID3D11PixelShader* currentPixel;
UINT currentOutputMask;
const GUID shaderOutputsId={0xb3dd8fc4,0x46ad,0x4f87,{0x81,0x60,0xef,0xba,0x5b,0x49,0xa1,0x87}};
UINT outputMask(ID3D11PixelShader* ps){UINT mask=0,bytes=sizeof(mask);if(ps)ps->GetPrivateData(shaderOutputsId,&bytes,&mask);return mask;}
struct Candidate {
    ID3D11Texture2D* texture;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D* snapshot;
    ID3D11ShaderResourceView* srv;
    UINT id, draws, segment, best, pairedDepth;
    UINT snapshotPairedDepth, copiedFrom;
    uint64_t snapshotFrame, lastWritten, readFrame, checkedSerial;
    bool depth, checked, valid, nonzero;
    float clearValue=1;
    uint64_t shaderHash;
};
Candidate candidates[96]; UINT candidateCount;
Candidate* boundDepth; Candidate* boundMotion[8];
Candidate *taaDepth,*taaMotion;
ID3D11Texture2D* taaColorSnapshot;
ID3D11Buffer *taaGlobalsSnapshot,*taaParamsSnapshot;
uint64_t taaColorFrame,taaGlobalsFrame,taaParamsFrame;
UINT dumps, dumpBurst;
void writeGpuTelemetry(FILE*,const char* event);
struct GpuCost {ID3D11Query *begin,*end,*disjoint;bool pending;};
GpuCost snapshotCosts[3],uploadCosts[3];UINT nextSnapshotCost,nextUploadCost;
double snapshotGpuMs,uploadGpuMs;
GpuCost* beginCost(GpuCost (&slots)[3],UINT& next){
    auto& s=slots[next++%3];if(s.pending)return nullptr;
    if(!s.begin){D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP,0};device->CreateQuery(&q,&s.begin);device->CreateQuery(&q,&s.end);q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;device->CreateQuery(&q,&s.disjoint);}
    if(!s.begin||!s.end||!s.disjoint)return nullptr;context->Begin(s.disjoint);context->End(s.begin);return &s;
}
void endCost(GpuCost* s){if(s){context->End(s->end);context->End(s->disjoint);s->pending=true;}}
void pollCosts(GpuCost (&slots)[3],double& ms){for(auto& s:slots)if(s.pending){D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};UINT64 b=0,e=0;if(context->GetData(s.disjoint,&d,sizeof(d),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||context->GetData(s.begin,&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||context->GetData(s.end,&e,sizeof(e),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;if(!d.Disjoint&&d.Frequency)ms=1000.0*double(e-b)/double(d.Frequency);s.pending=false;}}
bool writesDepth=true; bool screenViewport=true;
void* originalTexture; void* originalOM; void* originalOMUav;
void* originalCopy; void* originalClearDepth; void* originalClearRT;
void* originalDepthState; void* originalViewport; void* originalPSResources;
void* originalPixelShader; void* originalPSSet;
void* originalDraw[7];
void* originalStateSwap;
bool patch(void** slot,void* hook,void** orig);
bool nativeHook(void* target,void* callback,void** original){auto made=MH_CreateHook(target,callback,original);return made==MH_OK&&MH_QueueEnableHook(target)==MH_OK;}
using ModuleNameFn=DWORD(WINAPI*)(HMODULE,LPWSTR,DWORD);
ModuleNameFn previousModuleName;
HMODULE bridgeModule;
LONG CALLBACK exceptionTrace(EXCEPTION_POINTERS* p){
    if(p->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)p->ContextRecord->Rip,&m);wchar_t path[MAX_PATH]{};GetModuleFileNameW(m,path,MAX_PATH);logLine("TEMP exception code=%08lx rip=%llx module=%ls rva=%llx fault=%llx rsp=%llx",p->ExceptionRecord->ExceptionCode,p->ContextRecord->Rip,path,p->ContextRecord->Rip-(uintptr_t)m,p->ExceptionRecord->NumberParameters>1?p->ExceptionRecord->ExceptionInformation[1]:0,p->ContextRecord->Rsp);return EXCEPTION_CONTINUE_SEARCH;
}
DWORD WINAPI moduleName(HMODULE m,LPWSTR out,DWORD size){
    if(m==bridgeModule){constexpr wchar_t name[]=L"nvngx.dll";if(!out||!size){SetLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}UINT count=size>9?9:size-1;memcpy(out,name,count*sizeof(wchar_t));out[count]=0;if(size<=9){SetLastError(ERROR_INSUFFICIENT_BUFFER);return size;}return 9;}
    return previousModuleName(m,out,size);
}
bool extendModuleIdentity(HMODULE dll){
    auto base=(BYTE*)dll;auto dos=(IMAGE_DOS_HEADER*)base;auto nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    for(auto d=(IMAGE_IMPORT_DESCRIPTOR*)(base+rva);rva&&d->Name;++d){if(!d->OriginalFirstThunk)continue;auto names=(IMAGE_THUNK_DATA64*)(base+d->OriginalFirstThunk),slots=(IMAGE_THUNK_DATA64*)(base+d->FirstThunk);for(;names->u1.AddressOfData;++names,++slots){if(IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))continue;auto n=(IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);if(!strcmp((char*)n->Name,"GetModuleFileNameW")){GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&moduleName,&bridgeModule);return patch((void**)&slots->u1.Function,(void*)moduleName,(void**)&previousModuleName);}}}return false;
}

template<class T> void release(T*& p){if(p){p->Release();p=nullptr;}}
bool patch(void** slot,void* hook,void** orig){DWORD protect;if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protect))return false;*orig=*slot;MemoryBarrier();auto previous=InterlockedCompareExchangePointer((void* volatile*)slot,hook,*orig);DWORD ignored;VirtualProtect(slot,sizeof(void*),protect,&ignored);return *orig&&previous==*orig;}
bool tracking(ID3D11DeviceContext* c){return c==context && !internal && !captureBlocked && GetCurrentThreadId()==renderThread;}
uint64_t hashBytes(const void* p,size_t size){auto b=(const unsigned char*)p;uint64_t h=14695981039346656037ULL;for(size_t i=0;i<size;++i){h^=b[i];h*=1099511628211ULL;}return h;}
Candidate* find(ID3D11Resource* r){for(UINT i=0;i<candidateCount;++i)if(candidates[i].texture==r)return &candidates[i];return nullptr;}
Candidate* fromView(ID3D11View* v){if(!v)return nullptr;ID3D11Resource* r=nullptr;v->GetResource(&r);auto result=find(r);release(r);return result;}
DXGI_FORMAT srvFormat(DXGI_FORMAT f){switch(f){case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_FLOAT;case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_UNORM;default:return f;}}
DXGI_FORMAT copyFormat(DXGI_FORMAT f){switch(f){case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24G8_TYPELESS;case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_TYPELESS;case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_TYPELESS;default:return f;}}
bool makeSnapshot(Candidate& c){
    if(c.snapshot)return true;
    auto d=c.desc;d.Format=copyFormat(d.Format);d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=0;d.MiscFlags=0;
    bool savedInternal=internal;internal=true;HRESULT hr=device->CreateTexture2D(&d,nullptr,&c.snapshot);internal=savedInternal;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};s.Format=srvFormat(d.Format);s.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;s.Texture2D.MipLevels=1;
    if(SUCCEEDED(hr))hr=device->CreateShaderResourceView(c.snapshot,&s,&c.srv);
    if(FAILED(hr)){release(c.snapshot);release(c.srv);logLine("TEMP snapshot unavailable source=%u hr=%08lx",c.id,hr);return false;}return true;
}
void snapshot(Candidate* c){
    if(!c || !c->segment || c->segment<c->best || !makeSnapshot(*c))return;
    ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*))originalCopy)(context,c->snapshot,c->texture);
    c->best=c->segment;c->snapshotFrame=frame;
    c->snapshotPairedDepth=c->pairedDepth;
}
void finishBindings(){}
uint64_t saveShader(ID3D11PixelShader* ps,const char* label){
    if(!ps)return 0;UINT bytes=0;ps->GetPrivateData(shaderCodeId,&bytes,nullptr);if(!bytes||bytes>=524288)return 0;
    auto code=HeapAlloc(GetProcessHeap(),0,bytes);uint64_t hash=0;
    if(code&&SUCCEEDED(ps->GetPrivateData(shaderCodeId,&bytes,code))){hash=hashBytes(code,bytes);char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\%s-%016llx.dxbc",directory,label,hash);if(GetFileAttributesA(path)==INVALID_FILE_ATTRIBUTES){FILE* f=fopen(path,"wb");if(f){fwrite(code,1,bytes,f);fclose(f);}ID3DBlob* text=nullptr;if(SUCCEEDED(D3DDisassemble(code,bytes,0,nullptr,&text))){snprintf(path,MAX_PATH,"%s\\%s-%016llx.txt",directory,label,hash);f=fopen(path,"wb");if(f){fwrite(text->GetBufferPointer(),1,text->GetBufferSize(),f);fclose(f);}text->Release();}}}
    if(code)HeapFree(GetProcessHeap(),0,code);return hash;
}
void observeTaa(){
    // Read the bindings actually consumed by this draw, including state restored
    // through SwapDeviceContextState. Slot identity is verified from captured DXBC.
    if(boundDepth||!screenViewport)return;
    ID3D11ShaderResourceView* views[4]{};context->PSGetShaderResources(0,4,views);
    auto motion=fromView(views[2]),depth=fromView(views[3]);
    bool match=views[0]&&views[1]&&motion&&depth&&!motion->depth&&depth->depth&&motion->draws&&depth->draws&&motion->lastWritten==frame&&depth->lastWritten==frame&&(motion->pairedDepth==depth->id||motion->pairedDepth==depth->copiedFrom);
    if(match){ID3D11PixelShader* ps=nullptr;context->PSGetShader(&ps,nullptr,nullptr);auto hash=saveShader(ps,"taa");release(ps);if(hash){
        // At the TAA boundary both resources are complete for the scene, before
        // later first-person/UI clears can destroy or replace the main depth.
        internal=true;if(makeSnapshot(*depth)&&makeSnapshot(*motion)){auto cost=(taaSerial%60==0)?beginCost(snapshotCosts,nextSnapshotCost):nullptr;auto cp=(void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*))originalCopy;cp(context,depth->snapshot,depth->texture);cp(context,motion->snapshot,motion->texture);
            // A bounded copy of TAA's current-color input separates scene
            // correspondence from later tonemapping, HUD and neural output.
            if(dumps<8&&depth->valid&&depth->nonzero&&motion->valid&&motion->nonzero){ID3D11Resource* color=nullptr;views[0]->GetResource(&color);ID3D11Texture2D* texture=nullptr;if(SUCCEEDED(color->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&texture))){D3D11_TEXTURE2D_DESC cd{};texture->GetDesc(&cd);bool supported=cd.Format==DXGI_FORMAT_R11G11B10_FLOAT||cd.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||cd.Format==DXGI_FORMAT_R8G8B8A8_UNORM||cd.Format==DXGI_FORMAT_B8G8R8A8_UNORM;if(supported&&cd.Width==width&&cd.Height==height&&cd.SampleDesc.Count==1){if(!taaColorSnapshot){cd.Usage=D3D11_USAGE_DEFAULT;cd.BindFlags=cd.CPUAccessFlags=cd.MiscFlags=0;device->CreateTexture2D(&cd,nullptr,&taaColorSnapshot);logLine("TEMP TAA current color %ux%u format=%u snapshot=%p",cd.Width,cd.Height,cd.Format,taaColorSnapshot);}if(taaColorSnapshot){D3D11_TEXTURE2D_DESC saved{};taaColorSnapshot->GetDesc(&saved);if(saved.Width==cd.Width&&saved.Height==cd.Height&&saved.Format==cd.Format&&saved.MipLevels==cd.MipLevels&&saved.ArraySize==cd.ArraySize){cp(context,taaColorSnapshot,texture);taaColorFrame=frame;}else{taaColorFrame=0;logLine("TEMP TAA color snapshot format/shape changed; image correspondence rejected until restart");}}}release(texture);}release(color);}
            if(taaColorFrame==frame){UINT slots[]={12,2};ID3D11Buffer** outputs[]={&taaGlobalsSnapshot,&taaParamsSnapshot};uint64_t* stamps[]={&taaGlobalsFrame,&taaParamsFrame};for(UINT i=0;i<2;++i){*stamps[i]=0;ID3D11Buffer* b=nullptr;context->PSGetConstantBuffers(slots[i],1,&b);if(b){D3D11_BUFFER_DESC bd{};b->GetDesc(&bd);if(!*outputs[i]){bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=bd.CPUAccessFlags=bd.MiscFlags=bd.StructureByteStride=0;device->CreateBuffer(&bd,nullptr,outputs[i]);}if(*outputs[i]){D3D11_BUFFER_DESC prior{};(*outputs[i])->GetDesc(&prior);if(prior.ByteWidth==bd.ByteWidth){cp(context,*outputs[i],b);*stamps[i]=frame;}else logLine("TEMP TAA constant buffer size changed slot=%u; stale constants excluded",slots[i]);}release(b);}}}
            endCost(cost);depth->snapshotFrame=motion->snapshotFrame=frame;motion->snapshotPairedDepth=motion->pairedDepth;taaDepth=depth;taaMotion=motion;taaShader=hash;++taaSerial;}internal=false;
    }}
    for(auto& v:views)release(v);
}
void recordBindings(UINT n,ID3D11RenderTargetView*const* views,ID3D11DepthStencilView* d){
    boundDepth=fromView(d);for(UINT i=0;i<8;++i)boundMotion[i]=i<n?fromView(views[i]):nullptr;
    for(auto& c:boundMotion)if(c && c->depth)c=nullptr;
}
void refreshState(){
    ID3D11RenderTargetView* targets[8]{};ID3D11DepthStencilView* depth=nullptr;context->OMGetRenderTargets(8,targets,&depth);
    boundDepth=fromView(depth);for(UINT i=0;i<8;++i){boundMotion[i]=fromView(targets[i]);if(boundMotion[i]&&boundMotion[i]->depth)boundMotion[i]=nullptr;release(targets[i]);}release(depth);
    ID3D11DepthStencilState* state=nullptr;UINT stencil;context->OMGetDepthStencilState(&state,&stencil);D3D11_DEPTH_STENCIL_DESC d{};if(state)state->GetDesc(&d);else{d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}writesDepth=d.DepthEnable&&d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;release(state);
    D3D11_VIEWPORT vp[16]{};UINT count=16;context->RSGetViewports(&count,vp);screenViewport=count==1&&vp[0].TopLeftX==0&&vp[0].TopLeftY==0&&vp[0].Width==width&&vp[0].Height==height;
    ID3D11PixelShader* ps=nullptr;context->PSGetShader(&ps,nullptr,nullptr);currentPixel=ps;currentOutputMask=outputMask(ps);release(ps);
}
void STDMETHODCALLTYPE stateSwap(ID3D11DeviceContext1* c,ID3DDeviceContextState* state,ID3DDeviceContextState** previous){
    ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*,ID3DDeviceContextState*,ID3DDeviceContextState**))originalStateSwap)(c,state,previous);
    if(c==context1&&!internal&&!captureBlocked&&GetCurrentThreadId()==renderThread)refreshState();
}
void draw(){
    if(!screenViewport)return;
    observeTaa();
    if(boundDepth && writesDepth){++boundDepth->draws;++boundDepth->segment;boundDepth->lastWritten=frame;}
    for(UINT target=0;target<8;++target){auto c=boundMotion[target];if(!c||!(currentOutputMask&(1u<<target)))continue;++c->draws;++c->segment;c->lastWritten=frame;c->pairedDepth=boundDepth?boundDepth->id:0;
        if(!c->shaderHash && currentPixel){UINT bytes=0;currentPixel->GetPrivateData(shaderCodeId,&bytes,nullptr);if(bytes && bytes<524288){auto code=HeapAlloc(GetProcessHeap(),0,bytes);if(code && SUCCEEDED(currentPixel->GetPrivateData(shaderCodeId,&bytes,code))){c->shaderHash=hashBytes(code,bytes);char name[MAX_PATH];snprintf(name,MAX_PATH,"%s\\velocity-%016llx.dxbc",directory,c->shaderHash);FILE* f=fopen(name,"wb");if(f){fwrite(code,1,bytes,f);fclose(f);}ID3DBlob* text=nullptr;if(SUCCEEDED(D3DDisassemble(code,bytes,0,nullptr,&text))){snprintf(name,MAX_PATH,"%s\\velocity-%016llx.txt",directory,c->shaderHash);f=fopen(name,"wb");if(f){fwrite(text->GetBufferPointer(),1,text->GetBufferSize(),f);fclose(f);}text->Release();}logLine("TEMP velocity writer source=%u depth=%u shader=%016llx",c->id,c->pairedDepth,c->shaderHash);}if(code)HeapFree(GetProcessHeap(),0,code);}}
    }
}

HRESULT STDMETHODCALLTYPE texture(ID3D11Device* d,const D3D11_TEXTURE2D_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** out){
    HRESULT hr=((HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**))originalTexture)(d,desc,data,out);
    if(SUCCEEDED(hr)&&d==device&&!internal&&out&&*out&&candidateCount<96&&desc->Usage==D3D11_USAGE_DEFAULT&&desc->Width==width&&desc->Height==height&&desc->SampleDesc.Count==1&&desc->ArraySize==1&&desc->MipLevels==1&&!(desc->MiscFlags&(D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE))){
        bool depth=(desc->BindFlags&D3D11_BIND_DEPTH_STENCIL)!=0||desc->Format==DXGI_FORMAT_R24G8_TYPELESS;
        bool motion=(desc->BindFlags&D3D11_BIND_RENDER_TARGET)&&(desc->Format==DXGI_FORMAT_R16G16_FLOAT||desc->Format==DXGI_FORMAT_R32G32_FLOAT);
        if(depth||motion){auto& c=candidates[candidateCount++];c.texture=*out;c.texture->AddRef();c.desc=*desc;c.depth=depth;c.id=candidateCount;logLine("TEMP candidate id=%u depth=%d %ux%u format=%u bind=%x",c.id,depth,desc->Width,desc->Height,desc->Format,desc->BindFlags);}
    }return hr;
}
HRESULT STDMETHODCALLTYPE pixelShader(ID3D11Device* d,const void* code,SIZE_T bytes,ID3D11ClassLinkage* link,ID3D11PixelShader** out){auto hr=((HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const void*,SIZE_T,ID3D11ClassLinkage*,ID3D11PixelShader**))originalPixelShader)(d,code,bytes,link,out);if(SUCCEEDED(hr)&&d==device&&!internal&&out&&*out&&bytes<524288){(*out)->SetPrivateData(shaderCodeId,(UINT)bytes,code);UINT mask=0;ID3D11ShaderReflection* reflect=nullptr;if(SUCCEEDED(D3DReflect(code,bytes,IID_ID3D11ShaderReflection,(void**)&reflect))){D3D11_SHADER_DESC desc{};reflect->GetDesc(&desc);for(UINT i=0;i<desc.OutputParameters;++i){D3D11_SIGNATURE_PARAMETER_DESC parameter{};if(SUCCEEDED(reflect->GetOutputParameterDesc(i,&parameter))&&parameter.SystemValueType==D3D_NAME_TARGET&&parameter.SemanticIndex<8)mask|=1u<<parameter.SemanticIndex;}reflect->Release();}(*out)->SetPrivateData(shaderOutputsId,sizeof(mask),&mask);}return hr;}
void STDMETHODCALLTYPE psSet(ID3D11DeviceContext* c,ID3D11PixelShader* ps,ID3D11ClassInstance*const* classes,UINT n){if(tracking(c)){currentPixel=ps;currentOutputMask=outputMask(ps);}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11PixelShader*,ID3D11ClassInstance*const*,UINT))originalPSSet)(c,ps,classes,n);}
void STDMETHODCALLTYPE om(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d){if(tracking(c)){finishBindings();recordBindings(n,rt,d);}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView*const*,ID3D11DepthStencilView*))originalOM)(c,n,rt,d);}
void STDMETHODCALLTYPE omUav(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d,UINT start,UINT count,ID3D11UnorderedAccessView*const* uav,const UINT* values){if(tracking(c)&&n!=D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL){finishBindings();recordBindings(n,rt,d);}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView*const*,ID3D11DepthStencilView*,UINT,UINT,ID3D11UnorderedAccessView*const*,const UINT*))originalOMUav)(c,n,rt,d,start,count,uav,values);}
void STDMETHODCALLTYPE depthState(ID3D11DeviceContext* c,ID3D11DepthStencilState* s,UINT ref){if(tracking(c)){D3D11_DEPTH_STENCIL_DESC d{};if(s)s->GetDesc(&d);else{d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;}writesDepth=d.DepthEnable&&d.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ALL;}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11DepthStencilState*,UINT))originalDepthState)(c,s,ref);}
void STDMETHODCALLTYPE viewport(ID3D11DeviceContext* c,UINT n,const D3D11_VIEWPORT* v){if(tracking(c))screenViewport=n==1&&v&&v[0].TopLeftX==0&&v[0].TopLeftY==0&&v[0].Width==width&&v[0].Height==height;((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,const D3D11_VIEWPORT*))originalViewport)(c,n,v);}
void STDMETHODCALLTYPE clearDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* v,UINT flags,FLOAT depth,UINT8 stencil){if(tracking(c)&&(flags&D3D11_CLEAR_DEPTH)){auto p=fromView(v);if(p){p->draws=p->segment=0;p->lastWritten=0;p->clearValue=depth;}}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8))originalClearDepth)(c,v,flags,depth,stencil);}
void STDMETHODCALLTYPE clearRT(ID3D11DeviceContext* c,ID3D11RenderTargetView* v,const FLOAT color[4]){if(tracking(c)){auto p=fromView(v);if(p){p->draws=p->segment=0;p->lastWritten=0;}}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11RenderTargetView*,const FLOAT*))originalClearRT)(c,v,color);}
void STDMETHODCALLTYPE psResources(ID3D11DeviceContext* c,UINT start,UINT n,ID3D11ShaderResourceView*const* views){if(tracking(c))for(UINT i=0;i<n;++i){auto p=fromView(views[i]);if(p)p->readFrame=frame;}((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,ID3D11ShaderResourceView*const*))originalPSResources)(c,start,n,views);}
void STDMETHODCALLTYPE drawIndexed(ID3D11DeviceContext* c,UINT n,UINT start,INT base){if(tracking(c)&&n)draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT))originalDraw[0])(c,n,start,base);}
void STDMETHODCALLTYPE drawSimple(ID3D11DeviceContext* c,UINT n,UINT start){if(tracking(c)&&n)draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT))originalDraw[1])(c,n,start);}
void STDMETHODCALLTYPE drawIndexedInst(ID3D11DeviceContext* c,UINT n,UINT instances,UINT start,INT base,UINT first){if(tracking(c)&&n&&instances)draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT))originalDraw[2])(c,n,instances,start,base,first);}
void STDMETHODCALLTYPE drawInst(ID3D11DeviceContext* c,UINT n,UINT instances,UINT start,UINT first){if(tracking(c)&&n&&instances)draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT))originalDraw[3])(c,n,instances,start,first);}
void STDMETHODCALLTYPE drawAuto(ID3D11DeviceContext* c){if(tracking(c))draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*))originalDraw[4])(c);}
void STDMETHODCALLTYPE drawIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT offset){if(tracking(c))draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT))originalDraw[5])(c,b,offset);}
void STDMETHODCALLTYPE drawIndirect(ID3D11DeviceContext* c,ID3D11Buffer* b,UINT offset){if(tracking(c))draw();((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Buffer*,UINT))originalDraw[6])(c,b,offset);}

// Typed shared inputs avoid exposing D24S8's two planes to D3D12/NGX.
struct Shared {ID3D11Texture2D* texture;ID3D11UnorderedAccessView* uav;ID3D12Resource* resource;HANDLE handle;};
Shared sharedDepth{},sharedMotion{};
ID3D11ComputeShader* conversion;
Candidate *selectedDepth, *selectedMotion;
uint64_t preparedFrame;
bool prepared, usedLastDepth, usedLastMotion,nativeFeedFailed,nativeResetPending;
bool createShared(Shared& s,DXGI_FORMAT format){
    if(s.texture)return true;
    D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=height;d.MipLevels=d.ArraySize=1;d.Format=format;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    auto hr=device->CreateTexture2D(&d,nullptr,&s.texture);if(FAILED(hr))logLine("TEMP shared CreateTexture format=%u hr=%08lx",format,hr);if(SUCCEEDED(hr)){hr=device->CreateUnorderedAccessView(s.texture,nullptr,&s.uav);if(FAILED(hr))logLine("TEMP shared CreateUAV format=%u hr=%08lx",format,hr);}
    IDXGIResource1* dx=nullptr;if(SUCCEEDED(hr))hr=s.texture->QueryInterface(__uuidof(IDXGIResource1),(void**)&dx);
    if(SUCCEEDED(hr))hr=dx->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&s.handle);release(dx);
    if(FAILED(hr)){logLine("TEMP shared format=%u hr=%08lx",format,hr);release(s.uav);release(s.texture);if(s.handle){CloseHandle(s.handle);s.handle=nullptr;}return false;}return true;
}
struct Readback {
    ID3D11Texture2D *depth,*motion,*color,*taaColor;
    ID3D11Buffer *taaGlobals,*taaParams;
    ID3D11Query *done,*begin,*end,*disjoint;
    Candidate *sourceDepth,*sourceMotion;
    uint64_t frame, taaSerial, shaderHash,taaColorFrame,taaGlobalsFrame,taaParamsFrame;
    float depthClear;
    bool depthValid,depthNonzero,motionValid,motionNonzero;
    bool pending, dump;
};
Readback readbacks[3];UINT nextReadback;
uint64_t nextDumpAt;
double captureGpuMs;
float halfValue(uint16_t h){unsigned exponent=(h>>10)&31,fraction=h&1023;float value=exponent==31?(fraction?NAN:INFINITY):exponent?ldexpf(float(1024+fraction),int(exponent)-25):ldexpf(float(fraction),-24);return h&0x8000?-value:value;}
FILE* telemetry;
void writeTelemetry(const char* event,uint64_t stamp,bool validDepth,bool nonzeroDepth,bool validMotion,bool nonzeroMotion,bool consumedDepth,bool consumedMotion,uint32_t result=1,Candidate* dSource=nullptr,Candidate* mSource=nullptr,const Readback* captured=nullptr){
    auto d=dSource?dSource:selectedDepth,m=mSource?mSource:selectedMotion;
    if(!telemetry){char name[MAX_PATH];snprintf(name,MAX_PATH,"%s\\temporal.jsonl",directory);telemetry=fopen(name,"w");}
    if(!telemetry)return;
    fprintf(telemetry,"{\"event\":\"%s\",\"bridge_copy_serial\":%llu,\"taa_serial\":%llu,\"taa_shader\":\"%016llx\",\"depth_source\":%u,\"motion_source\":%u,\"depth_source_format\":%u,\"motion_source_format\":%u,\"depth_input_format\":41,\"motion_input_format\":34,\"width\":%u,\"height\":%u,\"depth_numeric_valid\":%s,\"depth_nonzero\":%s,\"motion_numeric_valid\":%s,\"motion_nonzero\":%s,\"depth_parameter_read\":%s,\"motion_parameter_read\":%s,\"calls\":%llu,\"failures\":%llu,\"input_rejected\":%llu,\"readback_copy_gpu_ms\":%.6f,\"snapshot_gpu_ms\":%.6f,\"conversion_gpu_ms\":%.6f,\"depth_numeric_age_copies\":%llu,\"motion_numeric_age_copies\":%llu,\"ngx_result\":%u,\"depth_spatial_validated\":%s,\"motion_spatial_validated\":%s",event,stamp,captured?captured->taaSerial:taaSerial,captured?captured->shaderHash:taaShader,d?d->id:0,m?m->id:0,d?(UINT)d->desc.Format:0,m?(UINT)m->desc.Format:0,width,height,validDepth?"true":"false",nonzeroDepth?"true":"false",validMotion?"true":"false",nonzeroMotion?"true":"false",consumedDepth?"true":"false",consumedMotion?"true":"false",calls,failures,rejected,captureGpuMs,snapshotGpuMs,uploadGpuMs,!captured&&d&&d->checkedSerial&&stamp>=d->checkedSerial?stamp-d->checkedSerial:0,!captured&&m&&m->checkedSerial&&stamp>=m->checkedSerial?stamp-m->checkedSerial:0,result,spatialDepthValidated?"true":"false",spatialMotionValidated?"true":"false");
    writeGpuTelemetry(telemetry,event);fputs("}\n",telemetry);fflush(telemetry);
}
void dumpTexture(ID3D11Texture2D* t,const char* type,uint64_t stamp,UINT bytes){D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(context->Map(t,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m)))return;char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\frame-%06llu-%s.raw",directory,stamp,type);FILE* f=fopen(path,"wb");if(f){if(!strcmp(type,"motion")){auto row=(float*)HeapAlloc(GetProcessHeap(),0,width*8);if(row){for(UINT y=0;y<height;++y){auto p=(uint16_t*)((BYTE*)m.pData+y*m.RowPitch);for(UINT x=0;x<width*2;++x)row[x]=halfValue(p[x]);fwrite(row,8,width,f);}HeapFree(GetProcessHeap(),0,row);}}else for(UINT y=0;y<height;++y)fwrite((BYTE*)m.pData+y*m.RowPitch,bytes,width,f);fclose(f);}context->Unmap(t,0);}
void dumpBuffer(ID3D11Buffer* b,const char* type,uint64_t stamp){if(!b)return;D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(context->Map(b,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m)))return;D3D11_BUFFER_DESC desc{};b->GetDesc(&desc);char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\frame-%06llu-%s.raw",directory,stamp,type);FILE* f=fopen(path,"wb");if(f){fwrite(m.pData,1,desc.ByteWidth,f);fclose(f);}context->Unmap(b,0);}
void pollReadbacks(){
    for(auto& r:readbacks){if(!r.pending||context->GetData(r.done,nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};UINT64 begin=0,end=0;
        if(context->GetData(r.disjoint,&dj,sizeof(dj),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&!dj.Disjoint&&dj.Frequency&&context->GetData(r.begin,&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&context->GetData(r.end,&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK)captureGpuMs=1000.0*double(end-begin)/double(dj.Frequency);
        for(UINT i=0;i<2;++i){auto c=i?r.sourceMotion:r.sourceDepth;auto t=i?r.motion:r.depth;if(!c||!t)continue;D3D11_MAPPED_SUBRESOURCE m{};if(FAILED(context->Map(t,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m)))continue;
            double min=1e30,max=-1e30,sum=0;UINT bad=0,nonzero=0,count=0;
            for(UINT y=0;y<height;y+=4)for(UINT x=0;x<width;x+=4){auto row=(BYTE*)m.pData+y*m.RowPitch;float a=i?halfValue(((uint16_t*)row)[x*2]):((float*)row)[x],b=i?halfValue(((uint16_t*)row)[x*2+1]):0;if(!isfinite(a)||!isfinite(b)||(!i&&(a<0||a>1))||(i&&(fabsf(a)>2||fabsf(b)>2))){++bad;continue;}double magnitude=i?fmax(fabs(a),fabs(b)):a;if(magnitude<min)min=magnitude;if(magnitude>max)max=magnitude;sum+=magnitude;nonzero+=i?(magnitude>1e-7):(fabs(a-r.depthClear)>1e-7);++count;}
            c->checked=true;c->checkedSerial=r.frame;c->valid=bad==0&&count>0&&(i||max-min>1e-6);c->nonzero=nonzero>0;context->Unmap(t,0);
            if(i){r.motionValid=c->valid;r.motionNonzero=c->nonzero;}else{r.depthValid=c->valid;r.depthNonzero=c->nonzero;}
            logLine("TEMP pixels frame=%llu source=%u %s valid=%d nonzero=%u/%u bad=%u min=%.9g max=%.9g mean=%.9g",r.frame,c->id,i?"motion":"depth",c->valid,nonzero,count,bad,min,max,count?sum/count:0);
        }
        if(r.dump){if(r.sourceDepth)dumpTexture(r.depth,"depth",r.frame,4);if(r.sourceMotion)dumpTexture(r.motion,"motion",r.frame,8);dumpTexture(r.color,"color",r.frame,4);D3D11_TEXTURE2D_DESC td{};if(r.taaColorFrame==r.frame&&r.taaColor){r.taaColor->GetDesc(&td);dumpTexture(r.taaColor,"taa-color",r.frame,td.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4);if(r.taaGlobalsFrame==r.frame)dumpBuffer(r.taaGlobals,"taa-b12",r.frame);if(r.taaParamsFrame==r.frame)dumpBuffer(r.taaParams,"taa-b2",r.frame);}
            char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\frame-%06llu.json",directory,r.frame);FILE* f=fopen(path,"w");if(f){D3D11_TEXTURE2D_DESC d{};r.color->GetDesc(&d);fprintf(f,"{\"frame\":%llu,\"taa_serial\":%llu,\"taa_shader\":\"%016llx\",\"width\":%u,\"height\":%u,\"color_format\":%u,\"taa_color_format\":%u,\"taa_color_frame\":%llu,\"depth_format\":41,\"motion_format\":16,\"motion_gpu_format\":34,\"depth_clear\":%.9g,\"depth_source\":%u,\"motion_source\":%u,\"motion_units\":\"UV current to previous, pending reprojection validation\",\"depth_inverted\":false}",r.frame,r.taaSerial,r.shaderHash,width,height,d.Format,td.Format,r.taaColorFrame,r.depthClear,r.sourceDepth?r.sourceDepth->id:0,r.sourceMotion?r.sourceMotion->id:0);fclose(f);}}
        writeTelemetry("readback",r.frame,r.depthValid,r.depthNonzero,r.motionValid,r.motionNonzero,false,false,1,r.sourceDepth,r.sourceMotion,&r);r.pending=false;
    }
}
bool staging(ID3D11Texture2D* source,ID3D11Texture2D** out){if(*out)return true;D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;return SUCCEEDED(device->CreateTexture2D(&d,nullptr,out));}
bool stagingBuffer(ID3D11Buffer* source,ID3D11Buffer** out){if(!source)return false;if(*out)return true;D3D11_BUFFER_DESC d{};source->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;return SUCCEEDED(device->CreateBuffer(&d,nullptr,out));}
void scheduleReadback(ID3D11Resource* color){
    // Menus can run for minutes before gameplay. Spend the bounded image budget
    // only after real moving geometry has produced checked depth AND motion.
    // A request file allows another burst without touching the process or UI.
    if(frame%60==0){char request[MAX_PATH];snprintf(request,MAX_PATH,"%s\\capture.request",directory);if(GetFileAttributesA(request)!=INVALID_FILE_ATTRIBUTES&&DeleteFileA(request)){dumps=0;nextDumpAt=0;logLine("TEMP paired capture request accepted");}}
    bool eligible=selectedDepth&&selectedDepth->checked&&frame-selectedDepth->checkedSerial<=120&&selectedDepth->valid&&selectedDepth->nonzero&&selectedMotion&&selectedMotion->checked&&frame-selectedMotion->checkedSerial<=120&&selectedMotion->valid&&selectedMotion->nonzero;
    if(!dumpBurst&&eligible&&dumps<8&&frame>=nextDumpAt){dumpBurst=2;nextDumpAt=frame+240;}
    bool dump=eligible&&dumpBurst&&dumps<8&&taaColorFrame==frame;
    if(frame%60&&!dump)return;auto& r=readbacks[nextReadback++%3];if(r.pending)return;
    auto original=(void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*))originalCopy;
    if(!r.done){D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};device->CreateQuery(&q,&r.done);q.Query=D3D11_QUERY_TIMESTAMP;device->CreateQuery(&q,&r.begin);device->CreateQuery(&q,&r.end);q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;device->CreateQuery(&q,&r.disjoint);}if(!r.done||!r.begin||!r.end||!r.disjoint)return;
    context->Begin(r.disjoint);context->End(r.begin);
    if(selectedDepth&&staging(sharedDepth.texture,&r.depth))original(context,r.depth,sharedDepth.texture);
    if(selectedMotion&&staging(sharedMotion.texture,&r.motion))original(context,r.motion,sharedMotion.texture);
    if(dump){ID3D11Texture2D* t=nullptr;dump=SUCCEEDED(color->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&t))&&staging(t,&r.color)&&r.depth&&r.motion;if(dump){original(context,r.color,t);if(taaColorSnapshot&&taaColorFrame==frame&&staging(taaColorSnapshot,&r.taaColor)){original(context,r.taaColor,taaColorSnapshot);if(taaGlobalsFrame==frame&&stagingBuffer(taaGlobalsSnapshot,&r.taaGlobals))original(context,r.taaGlobals,taaGlobalsSnapshot);if(taaParamsFrame==frame&&stagingBuffer(taaParamsSnapshot,&r.taaParams))original(context,r.taaParams,taaParamsSnapshot);}++dumps;--dumpBurst;}release(t);}
    context->End(r.end);context->End(r.disjoint);context->End(r.done);r.frame=frame;r.taaSerial=taaSerial;r.shaderHash=taaShader;r.taaColorFrame=taaColorFrame;r.taaGlobalsFrame=taaGlobalsFrame;r.taaParamsFrame=taaParamsFrame;r.depthClear=selectedDepth?selectedDepth->clearValue:1;r.sourceDepth=selectedDepth;r.sourceMotion=selectedMotion;r.depthValid=r.depthNonzero=r.motionValid=r.motionNonzero=false;r.dump=dump;r.pending=true;
}
bool prepare(ID3D11Resource* color){
    pollReadbacks();pollCosts(snapshotCosts,snapshotGpuMs);pollCosts(uploadCosts,uploadGpuMs);selectedDepth=selectedMotion=nullptr;
    if(!captureBlocked&&taaSerial!=lastTaaUsed&&taaDepth&&taaMotion&&taaDepth->snapshotFrame==frame&&taaMotion->snapshotFrame==frame){selectedDepth=taaDepth;selectedMotion=taaMotion;lastTaaUsed=taaSerial;}
    if(!selectedDepth || !context1 || !privateState)return false;
    internal=true;
    if(!conversion){const char* shader="Texture2D<float> depth:register(t0);Texture2D<float2> motion:register(t1);RWTexture2D<float> dout:register(u0);RWTexture2D<float2> mout:register(u1);[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;dout.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;dout[p.xy]=depth.Load(int3(p.xy,0));mout[p.xy]=motion.Load(int3(p.xy,0));}";ID3DBlob *code=nullptr,*errors=nullptr;HRESULT hr=D3DCompile(shader,strlen(shader),"SkyrimNativeInputs",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);if(SUCCEEDED(hr))hr=device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&conversion);if(FAILED(hr))logLine("TEMP conversion shader failed hr=%08lx",hr);release(code);release(errors);}
    bool okay=conversion&&createShared(sharedDepth,DXGI_FORMAT_R32_FLOAT)&&createShared(sharedMotion,DXGI_FORMAT_R16G16_FLOAT);
    if(okay){ID3DDeviceContextState* saved=nullptr;context1->SwapDeviceContextState(privateState,&saved);if(!saved){internal=false;logLine("TEMP context state unavailable; input fallback");return false;}ID3D11ShaderResourceView* inputs[]={selectedDepth->srv,selectedMotion?selectedMotion->srv:nullptr};ID3D11UnorderedAccessView* outputs[]={sharedDepth.uav,sharedMotion.uav};auto cost=(frame%60==0)?beginCost(uploadCosts,nextUploadCost):nullptr;context->CSSetShader(conversion,nullptr,0);context->CSSetShaderResources(0,2,inputs);context->CSSetUnorderedAccessViews(0,2,outputs,nullptr);context->Dispatch((width+7)/8,(height+7)/8,1);endCost(cost);ID3D11ShaderResourceView* noInputs[2]{};ID3D11UnorderedAccessView* noOutputs[2]{};context->CSSetShaderResources(0,2,noInputs);context->CSSetUnorderedAccessViews(0,2,noOutputs,nullptr);context1->SwapDeviceContextState(saved,nullptr);release(saved);scheduleReadback(color);}
    internal=false;return okay;
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c,ID3D11Resource* dest,ID3D11Resource* source){
    static UINT debugCopies;if(debugCopies<3){++debugCopies;logLine("TEMP copy hook c=%p expected=%p thread=%lu",c,context,GetCurrentThreadId());}
    bool copyIn=false;
    if(c==context&&!internal){ID3D11Texture2D* t=nullptr;if(SUCCEEDED(dest->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&t))){D3D11_TEXTURE2D_DESC d{};t->GetDesc(&d);wchar_t name[128]{};UINT bytes=sizeof(name)-2;bool dxlColor=SUCCEEDED(t->GetPrivateData(debugNameWideId,&bytes,name))&&!wcscmp(name,L"D5Q.Bridge.Color11");release(t);if(dxlColor&&(d.MiscFlags&D3D11_RESOURCE_MISC_SHARED_NTHANDLE)){ID3D11Texture2D* back=nullptr;swapChain->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back);copyIn=source==back;release(back);if(copyIn&&(d.Width!=width||d.Height!=height)){captureBlocked=true;prepared=false;logLine("TEMP dimensions changed %ux%u -> %ux%u; native capture disabled until restart; display fallback active",width,height,d.Width,d.Height);}}}}
    if(tracking(c)){auto dst=find(dest),src=find(source);if(dst&&src&&src->depth&&dst->depth){dst->lastWritten=src->lastWritten;dst->draws=src->draws;dst->clearValue=src->clearValue;dst->copiedFrom=src->id;}}
    if(copyIn){prepared=GetCurrentThreadId()==renderThread&&prepare(source);preparedFrame=frame;if(!prepared)++rejected;}
    ((void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*))originalCopy)(c,dest,source);
    if(c==context&&!internal&&GetCurrentThreadId()==renderThread&&TemporalSensitivity::Enabled())TemporalSensitivity::AfterCopy(c,dest,source,preparedFrame,taaSerial,calls,sensitivityNativeEpoch,sensitivityEffectiveReset);
    if(copyIn){if(frame%120==0){logLine("TEMP frame=%llu depth=%u draws=%u motion=%u draws=%u paired=%u prepared=%d calls=%llu fail=%llu depth_reads=%llu motion_reads=%llu capture_ms=%.4f",frame,selectedDepth?selectedDepth->id:0,selectedDepth?selectedDepth->draws:0,selectedMotion?selectedMotion->id:0,selectedMotion?selectedMotion->draws:0,selectedMotion?selectedMotion->pairedDepth:0,prepared,calls,failures,depthReads,motionReads,captureGpuMs);logLine("TEMP GPU native snapshot=%.6f ms conversion=%.6f ms readback=%.6f ms",snapshotGpuMs,uploadGpuMs,captureGpuMs);}++frame;for(UINT i=0;i<candidateCount;++i){candidates[i].draws=candidates[i].segment=candidates[i].best=0;}}
}

// MSVC's overload groups are reversed in its vtable; a raw forwarding table
// avoids mixing the MSVC ABI of the prebuilt core with MinGW's C++ ABI. The
// official NVSDK_NGX_Parameter is 8 Set + 8 Get + Reset, no virtual destructor.
// The wrapper leaves the core's actual parameter bag completely untouched.
using GetFn=uint32_t(*)(const void*,const char*,void*);
using EvaluateFn=uint32_t(*)(ID3D12GraphicsCommandList*,const void*,const void*,void*);
EvaluateFn evaluateOriginal;
struct ParameterView {
    void** table;
    const void* original;
    ID3D12Resource *depth,*motion;
    UINT width,height;
    bool reset;
    mutable bool gotDepth=false,gotMotion=false,gotReset=false;
};
uint32_t getParameter(const ParameterView* p,const char* key,void* out,UINT slot){
    bool pointer=slot<=10;
    if(pointer&&((p->depth&&!strcmp(key,"DLSSNR.Depth"))||(p->motion&&!strcmp(key,"DLSSNR.MVec")))){bool depth=!strcmp(key,"DLSSNR.Depth");*(void**)out=depth?(void*)p->depth:(void*)p->motion;if(depth)p->gotDepth=true;else p->gotMotion=true;return 1;}
    double value=0;bool override=false;
    if(p->depth&&!strcmp(key,"DLSSNR.DepthInverted")){override=true;value=0;}
    if(p->motion&&!strcmp(key,"DLSSNR.MVecScaleX")){override=true;value=p->width;}
    if(p->motion&&!strcmp(key,"DLSSNR.MVecScaleY")){override=true;value=p->height;}
    if(p->reset&&!strcmp(key,"DLSSNR.Reset")){override=true;value=1;if(!pointer)p->gotReset=true;}
    const char* prefix=p->depth&&!strncmp(key,"DLSSNR.DepthSubrect",19)?key+19:p->motion&&!strncmp(key,"DLSSNR.MVecSubrect",18)?key+18:nullptr;
    if(prefix){if(!strcmp(prefix,"Width")){override=true;value=p->width;}else if(!strcmp(prefix,"Height")){override=true;value=p->height;}else if(!strcmp(prefix,"BaseX")||!strcmp(prefix,"BaseY")){override=true;value=0;}}
    if(override&&!pointer){switch(slot){case 11:*(int*)out=(int)value;break;case 12:*(unsigned*)out=(unsigned)value;break;case 13:*(double*)out=value;break;case 14:*(float*)out=(float)value;break;case 15:*(unsigned long long*)out=(unsigned long long)value;break;default:break;}return 1;}
    return ((GetFn)(*(void***)p->original)[slot])(p->original,key,out);
}
#define GET_SLOT(N) uint32_t get##N(const ParameterView* p,const char* k,void* o){return getParameter(p,k,o,N);}
GET_SLOT(8) GET_SLOT(9) GET_SLOT(10) GET_SLOT(11) GET_SLOT(12) GET_SLOT(13) GET_SLOT(14) GET_SLOT(15)
#define SET_SLOT(N,T) void set##N(ParameterView* p,const char* k,T value){((void(*)(const void*,const char*,T))(*(void***)p->original)[N])(p->original,k,value);}
SET_SLOT(0,void*) SET_SLOT(1,ID3D12Resource*) SET_SLOT(2,ID3D11Resource*) SET_SLOT(3,int) SET_SLOT(4,unsigned) SET_SLOT(5,double) SET_SLOT(6,float) SET_SLOT(7,unsigned long long)
void resetParameter(ParameterView* p){((void(*)(const void*))(*(void***)p->original)[16])(p->original);}
void* parameterTable[]={(void*)set0,(void*)set1,(void*)set2,(void*)set3,(void*)set4,(void*)set5,(void*)set6,(void*)set7,(void*)get8,(void*)get9,(void*)get10,(void*)get11,(void*)get12,(void*)get13,(void*)get14,(void*)get15,(void*)resetParameter};
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,bool begin){if(!resource)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=begin?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=begin?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COMMON;list->ResourceBarrier(1,&b);}
void writeGpuTelemetry(FILE* out,const char* event){
    if(!out)return;
    const bool scoped=event&&!strcmp(event,"evaluate");
    const auto stats=scoped?gpuWatchLast:TemporalGpuWatch::Stats{};
    fprintf(out,",\"native_depth_supplied\":%s,\"native_motion_supplied\":%s,\"native_feed_failed_latched\":%s",scoped&&nativeDepthOfferedLast?"true":"false",scoped&&nativeMotionOfferedLast?"true":"false",nativeFeedFailed?"true":"false");
    fprintf(out,",\"route_reset_requested\":%s,\"route_reset_parameter_read\":%s,\"route_reset_pending\":%s",scoped&&routeResetRequestedLast?"true":"false",scoped&&routeResetReadLast?"true":"false",nativeResetPending?"true":"false");
    fprintf(out,",\"gpu_watch_requested\":%s,\"gpu_watch_ready\":%s,\"gpu_watch_copy_serial\":%llu,\"depth_srv_create_calls\":%llu,\"motion_srv_create_calls\":%llu,\"depth_compute_dispatch_bindings\":%llu,\"motion_compute_dispatch_bindings\":%llu,\"depth_srv_create_calls_total\":%llu,\"motion_srv_create_calls_total\":%llu,\"depth_compute_dispatch_bindings_total\":%llu,\"motion_compute_dispatch_bindings_total\":%llu,\"gpu_watch_requests_total\":%llu,\"gpu_watch_unavailable_total\":%llu,\"shared_inputs_device_match\":%s,\"shared_input_device_rejections_total\":%llu,\"gpu_binding_evidence\":\"exact_compute_root_table_start_at_recorded_dispatch; shader_access_not_proven; zero_is_unknown_lower_bound\"",
        scoped&&gpuWatchRequestedLast?"true":"false",scoped&&gpuWatchReadyLast?"true":"false",scoped?gpuWatchSerialLast:0,
        stats.depthSrvCreated,stats.motionSrvCreated,stats.depthDispatchBindings,stats.motionDispatchBindings,
        gpuWatchTotal.depthSrvCreated,gpuWatchTotal.motionSrvCreated,gpuWatchTotal.depthDispatchBindings,gpuWatchTotal.motionDispatchBindings,
        gpuWatchRequests,gpuWatchUnavailable,scoped&&sharedDeviceMatchLast?"true":"false",sharedDeviceRejections);
    fprintf(out,",\"depth_gpu_copy_source_calls\":%llu,\"motion_gpu_copy_source_calls\":%llu,\"depth_copied_srv_create_calls\":%llu,\"motion_copied_srv_create_calls\":%llu,\"depth_copied_compute_dispatch_bindings\":%llu,\"motion_copied_compute_dispatch_bindings\":%llu,\"observed_dispatch_calls\":%llu,\"observed_execute_indirect_calls\":%llu,\"observed_root_table_set_calls\":%llu,\"observed_descriptor_heap_set_calls\":%llu,\"observed_root_signature_set_calls\":%llu,\"depth_gpu_copy_source_calls_total\":%llu,\"motion_gpu_copy_source_calls_total\":%llu,\"depth_copied_compute_dispatch_bindings_total\":%llu,\"motion_copied_compute_dispatch_bindings_total\":%llu,\"observed_dispatch_calls_total\":%llu,\"observed_execute_indirect_calls_total\":%llu,\"observed_root_table_set_calls_total\":%llu,\"gpu_copy_evidence\":\"commands_recorded_only; copied_binding_is_full_copy_provenance_this_evaluate; subsequent_unobserved_writes_not_excluded\"",
        stats.depthCopySourceCalls,stats.motionCopySourceCalls,stats.depthCopiedSrvCreated,stats.motionCopiedSrvCreated,
        stats.depthCopiedDispatchBindings,stats.motionCopiedDispatchBindings,stats.dispatchCalls,stats.executeIndirectCalls,
        stats.rootTableSetCalls,stats.descriptorHeapSetCalls,stats.rootSignatureSetCalls,
        gpuWatchTotal.depthCopySourceCalls,gpuWatchTotal.motionCopySourceCalls,gpuWatchTotal.depthCopiedDispatchBindings,gpuWatchTotal.motionCopiedDispatchBindings,
        gpuWatchTotal.dispatchCalls,gpuWatchTotal.executeIndirectCalls,gpuWatchTotal.rootTableSetCalls);
    fprintf(out,",\"depth_copied_srv_create_calls_total\":%llu,\"motion_copied_srv_create_calls_total\":%llu,\"observed_descriptor_heap_set_calls_total\":%llu,\"observed_root_signature_set_calls_total\":%llu",
        gpuWatchTotal.depthCopiedSrvCreated,gpuWatchTotal.motionCopiedSrvCreated,gpuWatchTotal.descriptorHeapSetCalls,gpuWatchTotal.rootSignatureSetCalls);
    fprintf(out,",\"meta_command_hook_available\":%s,\"meta_initialize_calls\":%llu,\"meta_execute_calls\":%llu,\"meta_schema_known_calls\":%llu,\"depth_meta_input_bindings\":%llu,\"motion_meta_input_bindings\":%llu,\"meta_identity_last\":\"%016llx\",\"meta_parameter_bytes_last\":%llu,\"meta_parameter_hash_last\":\"%016llx\",\"meta_bytes_hashed_last\":%llu,\"meta_guid_known_last\":%s,\"meta_guid_raw_low_last\":\"%016llx\",\"meta_guid_raw_high_last\":\"%016llx\",\"meta_initialize_calls_total\":%llu,\"meta_execute_calls_total\":%llu,\"meta_schema_known_calls_total\":%llu,\"depth_meta_input_bindings_total\":%llu,\"motion_meta_input_bindings_total\":%llu,\"meta_evidence\":\"recorded_MetaCommand; native_binding_requires_witnessed_GUID_and_official_INPUT_descriptor_parameter; opaque_u64_and_texture_VA_not_matched\"",
        stats.metaHookAvailable?"true":"false",stats.metaInitializeCalls,stats.metaExecuteCalls,stats.metaSchemaKnownCalls,
        stats.depthMetaInputBindings,stats.motionMetaInputBindings,stats.metaIdentityLast,stats.metaParameterBytesLast,
        stats.metaParameterHashLast,stats.metaBytesHashedLast,stats.metaGuidKnownLast?"true":"false",stats.metaGuidLowLast,stats.metaGuidHighLast,
        gpuWatchTotal.metaInitializeCalls,gpuWatchTotal.metaExecuteCalls,gpuWatchTotal.metaSchemaKnownCalls,
        gpuWatchTotal.depthMetaInputBindings,gpuWatchTotal.motionMetaInputBindings);
}
uint32_t evaluate(ID3D12GraphicsCommandList* list,const void* handle,const void* params,void* callback){
    if(!context || GetCurrentThreadId()!=renderThread)return evaluateOriginal(list,handle,params,callback);
    ++calls;ID3D12Device* d=nullptr;HRESULT hr=list->GetDevice(__uuidof(ID3D12Device),(void**)&d);
    bool current=!captureBlocked&&prepared&&preparedFrame+1==frame&&SUCCEEDED(hr)&&d;
    // Geometry association + finite/nonconstant pixels are necessary checks.
    // Spatial correspondence is separately assessed from the paired dumps.
    bool depth=current&&width==validatedWidth&&height==validatedHeight&&!nativeFeedFailed&&feedDepth&&TemporalSensitivity::AllowNativeInputs(preparedFrame)&&spatialDepthValidated&&taaShader==validatedShader&&selectedDepth&&(!validatedDepthFormat||selectedDepth->desc.Format==validatedDepthFormat)&&selectedDepth->checked&&preparedFrame-selectedDepth->checkedSerial<=120&&selectedDepth->valid&&selectedDepth->nonzero;
    bool motion=depth&&feedMotion&&spatialMotionValidated&&selectedMotion&&(!validatedMotionFormat||selectedMotion->desc.Format==validatedMotionFormat)&&selectedMotion->checked&&preparedFrame-selectedMotion->checkedSerial<=120&&selectedMotion->valid;
    gpuWatchLast={};gpuWatchRequestedLast=gpuWatchReadyLast=sharedDeviceMatchLast=false;gpuWatchSerialLast=preparedFrame;
    if(depth||motion){
        // Open on the device that owns THIS private NR command list, and compare
        // canonical IUnknown identities when reusing an already opened resource.
        // A private-device change refuses native feeding; old resources stay
        // retained because their prior GPU work cannot be retired here safely.
        IUnknown* expected=nullptr;
        hr=d->QueryInterface(__uuidof(IUnknown),(void**)&expected);
        auto open=[&](Shared& s){
            if(!expected||!s.handle)return false;
            HRESULT opened=S_OK;
            if(!s.resource)opened=d->OpenSharedHandle(s.handle,__uuidof(ID3D12Resource),(void**)&s.resource);
            IUnknown* owner=nullptr;
            if(SUCCEEDED(opened)&&s.resource)opened=s.resource->GetDevice(__uuidof(IUnknown),(void**)&owner);
            bool match=SUCCEEDED(opened)&&owner==expected;
            release(owner);
            if(!match&&(sharedDeviceRejections==0||sharedDeviceRejections%120==0))logLine("TEMP shared input private-device mismatch/open failure hr=%08lx resource=%p device=%p",opened,s.resource,d);
            return match;
        };
        sharedDeviceMatchLast=SUCCEEDED(hr)&&open(sharedDepth)&&(!motion||open(sharedMotion));
        release(expected);
        if(!sharedDeviceMatchLast){++sharedDeviceRejections;depth=motion=false;}
    }
    nativeDepthOfferedLast=depth;nativeMotionOfferedLast=motion;
    ParameterView view{parameterTable,params,depth?sharedDepth.resource:nullptr,motion?sharedMotion.resource:nullptr,width,height,nativeResetPending||depth!=usedLastDepth||motion!=usedLastMotion};
    if(TemporalSensitivity::Enabled()&&(depth||motion)){if(!sensitivityNativeEpoch)sensitivityNativeEpoch=calls;view.depth=TemporalSensitivity::Override(d,list,true,width,height,view.depth);view.motion=TemporalSensitivity::Override(d,list,false,width,height,view.motion);view.reset=view.reset||TemporalSensitivity::ForceReset();}
    if(TemporalSensitivity::Enabled()){int resetValue=0;sensitivityEffectiveReset=get11(&view,"DLSSNR.Reset",&resetValue)==1&&resetValue!=0;view.gotReset=false;}
    routeResetRequestedLast=view.reset;routeResetReadLast=false;
    if(d){
        gpuWatchRequestedLast=true;++gpuWatchRequests;
        gpuWatchReadyLast=TemporalGpuWatch::BeginWatch(d,list,view.depth,view.motion,preparedFrame);
        if(!gpuWatchReadyLast)++gpuWatchUnavailable;
        if(gpuWatchRequests==1)logLine("TEMP GPU descriptor observer ready=%d; records exact compute table-start binding; shader reads/completion unproven",gpuWatchReadyLast);
    }
    transition(list,view.depth,true);transition(list,view.motion,true);
    // A reset-only view forwards the original fallback resources while discarding
    // native history. Observe/no-change calls retain the exact original bag.
    uint32_t result=evaluateOriginal(list,handle,depth||motion||view.reset?&view:params,callback);
    routeResetReadLast=view.gotReset;
    transition(list,view.motion,false);transition(list,view.depth,false);
    if(gpuWatchRequestedLast){
        gpuWatchLast=TemporalGpuWatch::EndWatch();
        gpuWatchTotal.depthSrvCreated+=gpuWatchLast.depthSrvCreated;gpuWatchTotal.motionSrvCreated+=gpuWatchLast.motionSrvCreated;
        gpuWatchTotal.depthDispatchBindings+=gpuWatchLast.depthDispatchBindings;gpuWatchTotal.motionDispatchBindings+=gpuWatchLast.motionDispatchBindings;
        gpuWatchTotal.depthCopySourceCalls+=gpuWatchLast.depthCopySourceCalls;gpuWatchTotal.motionCopySourceCalls+=gpuWatchLast.motionCopySourceCalls;
        gpuWatchTotal.depthCopiedSrvCreated+=gpuWatchLast.depthCopiedSrvCreated;gpuWatchTotal.motionCopiedSrvCreated+=gpuWatchLast.motionCopiedSrvCreated;
        gpuWatchTotal.depthCopiedDispatchBindings+=gpuWatchLast.depthCopiedDispatchBindings;gpuWatchTotal.motionCopiedDispatchBindings+=gpuWatchLast.motionCopiedDispatchBindings;
        gpuWatchTotal.dispatchCalls+=gpuWatchLast.dispatchCalls;gpuWatchTotal.executeIndirectCalls+=gpuWatchLast.executeIndirectCalls;
        gpuWatchTotal.rootTableSetCalls+=gpuWatchLast.rootTableSetCalls;gpuWatchTotal.descriptorHeapSetCalls+=gpuWatchLast.descriptorHeapSetCalls;
        gpuWatchTotal.rootSignatureSetCalls+=gpuWatchLast.rootSignatureSetCalls;
        gpuWatchTotal.metaInitializeCalls+=gpuWatchLast.metaInitializeCalls;gpuWatchTotal.metaExecuteCalls+=gpuWatchLast.metaExecuteCalls;
        gpuWatchTotal.metaSchemaKnownCalls+=gpuWatchLast.metaSchemaKnownCalls;
        gpuWatchTotal.depthMetaInputBindings+=gpuWatchLast.depthMetaInputBindings;gpuWatchTotal.motionMetaInputBindings+=gpuWatchLast.motionMetaInputBindings;
    }
    release(d);
    if(result!=1){++failures;if(view.reset||depth||motion)nativeResetPending=true;if(depth||motion){nativeFeedFailed=true;logLine("TEMP native evaluate failed result=%08x; native route disabled until restart, original display inputs preserved; fallback reset pending",result);}}
    else{usedLastDepth=depth;usedLastMotion=motion;nativeResetPending=false;}
    if(view.gotDepth)++depthReads;if(view.gotMotion)++motionReads;
    if(calls==1||calls%120==0||result!=1||view.reset)writeTelemetry("evaluate",preparedFrame,selectedDepth&&selectedDepth->checked&&selectedDepth->valid,selectedDepth&&selectedDepth->nonzero,selectedMotion&&selectedMotion->checked&&selectedMotion->valid,selectedMotion&&selectedMotion->nonzero,view.gotDepth,view.gotMotion,result);
    return result;
}
}

void TemporalConfigure(const char* output,TemporalLog log){TemporalSensitivity::Configure(output,log);logLine=log;strncpy(directory,output,MAX_PATH-1);char mode[32]{};GetEnvironmentVariableA("SKYRIM_TEMPORAL_MODE",mode,sizeof(mode));enabled=mode[0]!=0;feedDepth=!strcmp(mode,"depth")||!strcmp(mode,"depth-motion");feedMotion=!strcmp(mode,"depth-motion");char hash[32]{},flags[8]{};GetEnvironmentVariableA("SKYRIM_TEMPORAL_VALIDATED_SHADER",hash,sizeof(hash));validatedShader=strtoull(hash,nullptr,16);GetEnvironmentVariableA("SKYRIM_TEMPORAL_VALIDATED_DEPTH",flags,sizeof(flags));spatialDepthValidated=flags[0]=='1';GetEnvironmentVariableA("SKYRIM_TEMPORAL_VALIDATED_MOTION",flags,sizeof(flags));spatialMotionValidated=flags[0]=='1';auto number=[](const char* key){char value[32]{};GetEnvironmentVariableA(key,value,sizeof(value));return (UINT)strtoul(value,nullptr,10);};validatedWidth=number("SKYRIM_TEMPORAL_VALIDATED_WIDTH");validatedHeight=number("SKYRIM_TEMPORAL_VALIDATED_HEIGHT");validatedDepthFormat=number("SKYRIM_TEMPORAL_VALIDATED_DEPTH_FORMAT");validatedMotionFormat=number("SKYRIM_TEMPORAL_VALIDATED_MOTION_FORMAT");if(enabled)logLine("TEMP configured mode=%s validated_shader=%016llx depth=%d motion=%d; requires exact TAA capture and per-source numeric checks",mode,validatedShader,spatialDepthValidated,spatialMotionValidated);}
bool TemporalEnabled(){return enabled;}
void TemporalAttach(ID3D11Device* d,ID3D11DeviceContext* c,IDXGISwapChain* s){
    if(!enabled||device||!d||!c||!s)return;device=d;context=c;swapChain=s;device->AddRef();context->AddRef();swapChain->AddRef();renderThread=GetCurrentThreadId();DXGI_SWAP_CHAIN_DESC desc{};s->GetDesc(&desc);width=desc.BufferDesc.Width;height=desc.BufferDesc.Height;
    d->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)&context1);if(!context1)c->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)&context1);
    ID3D11Device1* d1=nullptr;if(SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device1),(void**)&d1))){D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;D3D_FEATURE_LEVEL selected;auto hr=d1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&selected,&privateState);logLine("TEMP private state hr=%08lx",hr);d1->Release();}
    AddVectoredExceptionHandler(1,exceptionTrace);
    auto dv=*(void***)d,cv=*(void***)c;
    bool ok=true;ok&=nativeHook(dv[5],(void*)texture,&originalTexture);ok&=nativeHook(dv[15],(void*)pixelShader,&originalPixelShader);
    ok&=nativeHook(cv[8],(void*)psResources,&originalPSResources);ok&=nativeHook(cv[9],(void*)psSet,&originalPSSet);
    ok&=nativeHook(cv[33],(void*)om,&originalOM);ok&=nativeHook(cv[34],(void*)omUav,&originalOMUav);ok&=nativeHook(cv[36],(void*)depthState,&originalDepthState);ok&=nativeHook(cv[44],(void*)viewport,&originalViewport);auto copied=MH_CreateHook(cv[47],(void*)copy,&originalCopy);auto copyOn=copied==MH_OK?MH_EnableHook(cv[47]):copied;ok&=copyOn==MH_OK;logLine("TEMP native CopyResource hook create=%d enable=%d",copied,copyOn);ok&=nativeHook(cv[50],(void*)clearRT,&originalClearRT);ok&=nativeHook(cv[53],(void*)clearDepth,&originalClearDepth);
    UINT slots[]={12,13,20,21,38,39,40};void* hooks[]={(void*)drawIndexed,(void*)drawSimple,(void*)drawIndexedInst,(void*)drawInst,(void*)drawAuto,(void*)drawIndexedIndirect,(void*)drawIndirect};for(UINT i=0;i<7;++i)ok&=nativeHook(cv[slots[i]],hooks[i],&originalDraw[i]);
    ok&=context1&&nativeHook(TemporalSwapStateTarget(context1),(void*)stateSwap,&originalStateSwap);
    auto applied=MH_ApplyQueued();ok&=applied==MH_OK;
    if(!ok){captureBlocked=true;prepared=false;logLine("TEMP required capture hook unavailable; native capture/feeding disabled, original display fallback active");}
    logLine("TEMP D3D11 attached %ux%u hooks=%d apply=%d thread=%lu context=%p",width,height,ok,applied,renderThread,c);
}
void TemporalInstallNeuralHook(){if(!enabled)return;for(UINT i=0;i<600;++i){auto dll=GetModuleHandleW(L"nvngx_dlssnr.dll");auto address=dll?(void*)GetProcAddress(dll,"NVSDK_NGX_D3D12_EvaluateFeature"):nullptr;if(address){bool identity=extendModuleIdentity(dll);logLine("TEMP module identity extension=%d",identity);if(!identity)return;auto made=MH_CreateHook(address,(void*)evaluate,(void**)&evaluateOriginal);auto on=made==MH_OK?MH_EnableHook(address):made;logLine("TEMP NR evaluate hook create=%d enable=%d",made,on);return;}Sleep(100);}logLine("TEMP NR evaluate export unavailable; display fallback remains active");}
