// Isolated causal-input probe. Deliberately invalid zero input is NEVER treated
// as validated native data. This module refuses every executable but fixture.exe.
#define WIN32_LEAN_AND_MEAN
#include "temporal_sensitivity.h"
#include <windows.h>
#include <dxgi.h>
#include <d3dcommon.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

namespace TemporalSensitivity {
namespace {
const GUID debugNameWideId={0x4cca5fd8,0x921f,0x42c8,{0x85,0x66,0x70,0xca,0xf2,0xa9,0xb7,0x41}};
Log logLine;
bool enabled,alwaysReset=true;
bool nativeGapConfigured;
uint64_t nativeGapStart,nativeGapEnd;
enum class Mode {Native,ZeroDepth,ZeroMotion};
Mode mode;
char directory[MAX_PATH],modeName[32],resetPolicy[16];
uint64_t frozenSceneFrame;
uint64_t pendingSerial,lastOverrideSerial,zeroOverridesApplied,zeroOverrideFailures;
bool lastOverrideApplied;
thread_local bool internal;
template<class T>void release(T*& p){if(p){p->Release();p=nullptr;}}

struct Zero {IUnknown* owner;ID3D12Resource *texture,*upload;UINT width,height;bool depth;};
Zero zeros[8]{};UINT zeroCount;
ID3D12Resource* createZero(ID3D12Device* d,ID3D12GraphicsCommandList* list,bool depth,UINT width,UINT height){
    IUnknown* owner=nullptr;HRESULT hr=d->QueryInterface(__uuidof(IUnknown),(void**)&owner);
    if(FAILED(hr)||!owner)return nullptr;
    for(UINT i=0;i<zeroCount;++i){auto& z=zeros[i];if(z.owner==owner&&z.width==width&&z.height==height&&z.depth==depth){owner->Release();return z.texture;}}
    if(zeroCount==8){owner->Release();logLine("SENS zero resource capacity reached; native fallback");return nullptr;}
    Zero z{};z.owner=owner;z.width=width;z.height=height;z.depth=depth;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=depth?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16_FLOAT;
    hr=d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,__uuidof(ID3D12Resource),(void**)&z.texture);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,totalBytes=0;
    if(SUCCEEDED(hr))d->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&totalBytes);
    if(!totalBytes||totalBytes>SIZE_MAX)hr=E_INVALIDARG;
    if(SUCCEEDED(hr)){
        heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=totalBytes;buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr=d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,__uuidof(ID3D12Resource),(void**)&z.upload);
    }
    if(SUCCEEDED(hr)){void* bytes=nullptr;D3D12_RANGE none{0,0};hr=z.upload->Map(0,&none,&bytes);
        if(SUCCEEDED(hr)){memset(bytes,0,(SIZE_T)totalBytes);D3D12_RANGE written{0,(SIZE_T)totalBytes};z.upload->Unmap(0,&written);}}
    if(FAILED(hr)){logLine("SENS zero %s creation failed hr=%08lx; native fallback",depth?"depth":"motion",hr);release(z.texture);release(z.upload);release(z.owner);return nullptr;}
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=z.upload;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;
    D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=z.texture;dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=z.texture;
    barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COMMON;
    list->ResourceBarrier(1,&barrier);
    // Both texture AND upload remain alive until process termination. No GPU
    // retirement is inferred merely from returning from EvaluateFeature.
    zeros[zeroCount++]=z;logLine("SENS recorded deliberate zero-%s upload %ux%u format=%u bytes=%llu",depth?"depth":"motion",width,height,desc.Format,totalBytes);
    return z.texture;
}

struct Pixels {bool ready,reset,overrideApplied;DXGI_FORMAT format;uint64_t taa,hash,rgbSum,nrCall,nativeEpoch;UINT min,max;};
struct Pair {uint64_t serial;UINT width,height;Pixels input,output;};
Pair pairs[8]{};
struct Readback {ID3D11Texture2D* staging;ID3D11Query* event;bool pending,output;UINT pair;};
Readback ring[3]{};UINT nextRing;
uint64_t dropped;
bool tagged(ID3D11Resource* resource,const wchar_t* expected){
    if(!resource)return false;wchar_t name[128]{};UINT bytes=sizeof(name)-sizeof(wchar_t);
    return SUCCEEDED(resource->GetPrivateData(debugNameWideId,&bytes,name))&&!wcscmp(name,expected);
}
void metadata(Pair& pair){
    if(!pair.input.ready||!pair.output.ready)return;
    char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\sensitivity-%06llu.json",directory,pair.serial);FILE* f=fopen(path,"w");if(!f)return;
    const char* override=mode==Mode::ZeroDepth?"depth_zero":mode==Mode::ZeroMotion?"motion_zero":"none";
    fprintf(f,"{\"fixture_only\":true,\"complete\":true,\"mode\":\"%s\",\"reset_policy\":\"%s\",\"reset_is_configured_request_not_runtime_proof\":true,\"fixture_scene_frame\":%llu,\"bridge_copy_serial\":%llu,\"taa_serial_input\":%llu,\"taa_serial_output\":%llu,\"width\":%u,\"height\":%u,\"storage\":\"tightly_packed_RGBA8\",\"input_source_format\":%u,\"output_source_format\":%u,\"input_hash_fnv64\":\"%016llx\",\"output_hash_fnv64\":\"%016llx\",\"input_rgb_sum\":%llu,\"output_rgb_sum\":%llu,\"input_rgb_min\":%u,\"input_rgb_max\":%u,\"output_rgb_min\":%u,\"output_rgb_max\":%u,\"resources_override\":\"%s\",\"deliberate_corrupt_input_probe\":%s,\"gpu_copy_recording_drops\":%llu,",
        modeName,resetPolicy,frozenSceneFrame,pair.serial,pair.input.taa,pair.output.taa,pair.width,pair.height,pair.input.format,pair.output.format,
        pair.input.hash,pair.output.hash,pair.input.rgbSum,pair.output.rgbSum,pair.input.min,pair.input.max,pair.output.min,pair.output.max,override,mode==Mode::Native?"false":"true",dropped);
    const bool history=pair.output.nativeEpoch&&pair.output.nrCall>=pair.output.nativeEpoch;
    fprintf(f,"\"nr_call_index\":%llu,\"native_route_epoch_first_call\":%llu,\"nr_calls_since_native_epoch\":%llu,\"effective_reset\":%s,\"nr_history_not_observed\":%s,\"zero_override_applied_this_frame\":%s,\"zero_override_applied_calls_total\":%llu,\"zero_override_failures_total\":%llu}",
        pair.output.nrCall,pair.output.nativeEpoch,history?pair.output.nrCall-pair.output.nativeEpoch:0,pair.output.reset?"true":"false",history?"false":"true",
        pair.output.overrideApplied?"true":"false",zeroOverridesApplied,zeroOverrideFailures);
    fclose(f);logLine("SENS paired capture serial=%llu input=%016llx output=%016llx mode=%s reset=%s",pair.serial,pair.input.hash,pair.output.hash,modeName,resetPolicy);
}
void poll(ID3D11DeviceContext* context){
    for(auto& r:ring){
        if(!r.pending||context->GetData(r.event,nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr=context->Map(r.staging,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);if(FAILED(hr))continue;
        auto& pair=pairs[r.pair];auto& pixels=r.output?pair.output:pair.input;
        const bool bgra=pixels.format==DXGI_FORMAT_B8G8R8A8_UNORM||pixels.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\sensitivity-%06llu-%s.rgba",directory,pair.serial,r.output?"output":"input");FILE* f=fopen(path,"wb");
        auto row=(BYTE*)HeapAlloc(GetProcessHeap(),0,SIZE_T(pair.width)*4);
        uint64_t hash=14695981039346656037ULL,sum=0;UINT minimum=255,maximum=0;
        bool okay=f&&row;
        if(okay)for(UINT y=0;y<pair.height;++y){
            auto input=(BYTE*)mapped.pData+SIZE_T(y)*mapped.RowPitch;
            for(UINT x=0;x<pair.width;++x){row[x*4]=input[x*4+(bgra?2:0)];row[x*4+1]=input[x*4+1];row[x*4+2]=input[x*4+(bgra?0:2)];row[x*4+3]=input[x*4+3];}
            for(UINT x=0;x<pair.width*4;++x){hash^=row[x];hash*=1099511628211ULL;if(x%4!=3){UINT v=row[x];sum+=v;if(v<minimum)minimum=v;if(v>maximum)maximum=v;}}
            if(fwrite(row,4,pair.width,f)!=pair.width){okay=false;break;}
        }
        if(row)HeapFree(GetProcessHeap(),0,row);if(f&&fclose(f))okay=false;context->Unmap(r.staging,0);
        r.pending=false;pixels.ready=okay;pixels.hash=hash;pixels.rgbSum=sum;pixels.min=minimum;pixels.max=maximum;
        if(okay)metadata(pair);else logLine("SENS raw capture write failed serial=%llu kind=%s",pair.serial,r.output?"output":"input");
    }
}
void schedule(ID3D11DeviceContext* context,ID3D11Resource* resource,bool output,UINT index,uint64_t serial,uint64_t taa,uint64_t nrCall,uint64_t epoch,bool reset){
    auto& pair=pairs[index];auto& pixels=output?pair.output:pair.input;
    if(pixels.ready)return;
    for(auto& r:ring)if(r.pending&&r.pair==index&&r.output==output)return;
    Readback* slot=nullptr;for(UINT i=0;i<3;++i){auto& r=ring[(nextRing+i)%3];if(!r.pending){slot=&r;nextRing=(nextRing+i+1)%3;break;}}
    if(!slot){++dropped;return;}
    ID3D11Texture2D* texture=nullptr;HRESULT hr=resource->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&texture);if(FAILED(hr))return;
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    const bool supported=desc.SampleDesc.Count==1&&desc.ArraySize==1&&desc.MipLevels==1&&
        (desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM||desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM||desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if(!supported){texture->Release();logLine("SENS unsupported color format=%u; capture skipped",desc.Format);return;}
    if(pair.serial&&((pair.width&&pair.width!=desc.Width)||(pair.height&&pair.height!=desc.Height))){texture->Release();logLine("SENS input/output dimensions differ; capture skipped");return;}
    ID3D11Device* device=nullptr;context->GetDevice(&device);
    if(slot->staging){D3D11_TEXTURE2D_DESC prior{};slot->staging->GetDesc(&prior);if(prior.Width!=desc.Width||prior.Height!=desc.Height||prior.Format!=desc.Format)release(slot->staging);}
    if(!slot->staging){auto staging=desc;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=staging.MiscFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr=device->CreateTexture2D(&staging,nullptr,&slot->staging);}
    if(SUCCEEDED(hr)&&!slot->event){D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};hr=device->CreateQuery(&query,&slot->event);}
    if(SUCCEEDED(hr)&&slot->staging&&slot->event){
        pair.serial=serial;pair.width=desc.Width;pair.height=desc.Height;pixels.format=desc.Format;pixels.taa=taa;
        pixels.nrCall=nrCall;pixels.nativeEpoch=epoch;pixels.reset=reset;pixels.overrideApplied=output&&lastOverrideApplied&&lastOverrideSerial==serial;
        context->CopyResource(slot->staging,texture);context->End(slot->event);
        slot->pending=true;slot->output=output;slot->pair=index;
    }else logLine("SENS staging/query creation failed hr=%08lx",hr);
    release(device);release(texture);
}
}

void Configure(const char* output,Log log){
    logLine=log;enabled=false;nativeGapConfigured=false;
    char requested[32]{};GetEnvironmentVariableA("SKYRIM_TEMPORAL_SENSITIVITY",requested,sizeof(requested));if(!requested[0])return;
    wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
    auto basename=wcsrchr(executable,L'\\');basename=basename?basename+1:executable;
    if(_wcsicmp(basename,L"fixture.exe")){if(logLine)logLine("SENS refused: actual executable must be fixture.exe");return;}
    if(!strcmp(requested,"native"))mode=Mode::Native;else if(!strcmp(requested,"zero-depth"))mode=Mode::ZeroDepth;else if(!strcmp(requested,"zero-motion"))mode=Mode::ZeroMotion;
    else {if(logLine)logLine("SENS refused unknown mode=%s",requested);return;}
    strncpy(directory,output?output:"",MAX_PATH-1);strncpy(modeName,requested,sizeof(modeName)-1);
    char policy[16]{},frozen[32]{};GetEnvironmentVariableA("SKYRIM_TEMPORAL_SENSITIVITY_RESET",policy,sizeof(policy));
    alwaysReset=strcmp(policy,"initial")!=0;strcpy(resetPolicy,alwaysReset?"always":"initial");
    GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_FREEZE_FRAME",frozen,sizeof(frozen));frozenSceneFrame=strtoull(frozen,nullptr,10);
    enabled=logLine&&directory[0];if(enabled)logLine("SENS fixture-only mode=%s reset=%s frozen_scene=%llu; zero variants deliberately corrupt input; capture pairs at360/361,420/421,480/481,540/541",modeName,resetPolicy,frozenSceneFrame);
    if(enabled){
        char gap[96]{};DWORD length=GetEnvironmentVariableA("SKYRIM_TEMPORAL_SENSITIVITY_NATIVE_GAP",gap,sizeof(gap));
        if(length){
            bool valid=length<sizeof(gap);UINT colon=0;
            for(UINT i=0;valid&&gap[i];++i){if(gap[i]==':'&&!colon&&i)colon=i;else if(gap[i]<'0'||gap[i]>'9')valid=false;}
            valid=valid&&colon&&gap[colon+1];
            if(valid){
                char* end=nullptr;errno=0;nativeGapStart=strtoull(gap,&end,10);valid=errno!=ERANGE&&end==gap+colon;
                errno=0;nativeGapEnd=strtoull(gap+colon+1,&end,10);valid=valid&&errno!=ERANGE&&end&&!*end&&nativeGapStart<nativeGapEnd;
            }
            nativeGapConfigured=valid;
            if(valid)logLine("SENS fixture-only native input gap configured [%llu,%llu) bridge copies",nativeGapStart,nativeGapEnd);
            else logLine("SENS native input gap refused: expected unsigned start:end with start<end");
        }
    }
}
bool Enabled(){return enabled;}
bool ForceReset(){return enabled&&alwaysReset;}
bool AllowNativeInputs(uint64_t copySerial){return !enabled||!nativeGapConfigured||copySerial<nativeGapStart||copySerial>=nativeGapEnd;}
ID3D12Resource* Override(ID3D12Device* device,ID3D12GraphicsCommandList* list,bool depth,UINT width,UINT height,ID3D12Resource* nativeResource){
    if(!enabled||mode==Mode::Native||(depth?mode!=Mode::ZeroDepth:mode!=Mode::ZeroMotion)||!device||!list||!nativeResource||!width||!height)return nativeResource;
    auto zero=createZero(device,list,depth,width,height);lastOverrideSerial=pendingSerial;lastOverrideApplied=zero!=nullptr;
    if(zero)++zeroOverridesApplied;else ++zeroOverrideFailures;
    return zero?zero:nativeResource;
}
void AfterCopy(ID3D11DeviceContext* context,ID3D11Resource* dest,ID3D11Resource* source,uint64_t serial,uint64_t taa,uint64_t nrCall,uint64_t epoch,bool reset){
    if(!enabled||internal||!context||!dest||!source)return;internal=true;poll(context);
    const bool input=tagged(dest,L"D5Q.Bridge.Color11"),output=!input&&tagged(source,L"D5Q.Bridge.Out11");
    if(input){pendingSerial=serial;lastOverrideApplied=false;}
    if(serial>=360&&serial<=541){
        uint64_t relative=serial-360;UINT within=(UINT)(relative%60),group=(UINT)(relative/60);
        if(group<4&&within<2){UINT index=group*2+within;
            if(input)schedule(context,source,false,index,serial,taa,nrCall,epoch,reset);
            else if(output)schedule(context,dest,true,index,serial,taa,nrCall,epoch,reset);
        }
    }
    internal=false;
}
}
