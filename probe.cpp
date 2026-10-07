#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <MinHook.h>
#include <knownfolders.h>
#include <winternl.h>
#include "sidecar.h"
#include "temporal.h"

// Graphics diagnostics use IAT/vtable slots. Known-folder isolation uses MinHook.
// No Skyrim offsets, executable modifications or renderer translation.
static HANDLE logFile = INVALID_HANDLE_VALUE;
static SRWLOCK logLock = SRWLOCK_INIT;
static char profile[MAX_PATH], output[MAX_PATH];
static HMODULE selfModule;
static bool captureEnabled;
static bool coreEnabled;
static bool coreDiagnostics;
static bool excludeRtss;
static bool inputsEnabled;
static bool sidecarEnabled;
static volatile LONG captured;
static volatile LONG64 frames;
static ULONGLONG lastLog;
static UINT screenWidth,screenHeight;

static void Log(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    char line[4096];
    int n = snprintf(line, sizeof(line), "[%llu tid=%lu] ", GetTickCount64(), GetCurrentThreadId());
    va_list args; va_start(args, format);
    n += vsnprintf(line+n, sizeof(line)-n-3, format, args); va_end(args);
    if (n > (int)sizeof(line)-3) n = sizeof(line)-3;
    line[n++]='\r'; line[n++]='\n';
    AcquireSRWLockExclusive(&logLock);
    DWORD written; WriteFile(logFile, line, n, &written, nullptr);
    ReleaseSRWLockExclusive(&logLock);
}

static bool Replace(void** slot, void* hook, void** original) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = InterlockedExchangePointer((void* volatile*)slot, hook);
    DWORD ignored; VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

static bool PatchImport(HMODULE module, const char* name, void* hook, void** original) {
    auto base = (BYTE*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return false;
    for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base+rva); desc->Name; ++desc) {
        if (!desc->OriginalFirstThunk) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base+desc->OriginalFirstThunk);
        auto slots = (IMAGE_THUNK_DATA64*)(base+desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto entry = (IMAGE_IMPORT_BY_NAME*)(base+names->u1.AddressOfData);
            if (strcmp((char*)entry->Name, name)) continue;
            bool ok=Replace((void**)&slots->u1.Function, hook, original);
            Log("IAT %s module=%p previous=%p hook=%p ok=%d", name, module, *original, hook, ok);
            return ok;
        }
    }
    return false;
}

using FolderFn = HRESULT(WINAPI*)(HWND,int,HANDLE,DWORD,LPSTR);
static FolderFn folderOriginal;
static HRESULT WINAPI ProbeFolder(HWND hwnd,int csidl,HANDLE token,DWORD flags,LPSTR path) {
    int id=csidl & 0xff;
    const char* suffix = id == CSIDL_PERSONAL ? "Documents" : id == CSIDL_LOCAL_APPDATA ? "LocalAppData" : id == CSIDL_APPDATA ? "AppData" : nullptr;
    if (suffix) {
        snprintf(path, MAX_PATH, "%s\\%s", profile, suffix);
        Log("isolated SHGetFolderPathA csidl=%d => %s",csidl,path);
        return S_OK;
    }
    return folderOriginal(hwnd,csidl,token,flags,path);
}
using ShellFn=HINSTANCE(WINAPI*)(HWND,LPCSTR,LPCSTR,LPCSTR,LPCSTR,INT);
static ShellFn shellOriginal;
static HINSTANCE WINAPI ProbeShell(HWND,LPCSTR,LPCSTR file,LPCSTR args,LPCSTR,INT) {
    Log("blocked external ShellExecuteA file=%s args=%s",file?file:"",args?args:"");
    return (HINSTANCE)(UINT_PTR)SE_ERR_ACCESSDENIED;
}

using KnownFolderFn=HRESULT(WINAPI*)(REFKNOWNFOLDERID,DWORD,HANDLE,PWSTR*);
static KnownFolderFn knownFolderOriginal;
static HRESULT WINAPI ProbeKnownFolder(REFKNOWNFOLDERID id,DWORD flags,HANDLE token,PWSTR* out) {
    const char* suffix=IsEqualGUID(id,FOLDERID_Documents)?"Documents":IsEqualGUID(id,FOLDERID_LocalAppData)?"LocalAppData":IsEqualGUID(id,FOLDERID_RoamingAppData)?"AppData":nullptr;
    if(!suffix)return knownFolderOriginal(id,flags,token,out);
    char path[MAX_PATH];snprintf(path,MAX_PATH,"%s\\%s",profile,suffix);
    int n=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);
    auto result=(PWSTR)CoTaskMemAlloc(n*sizeof(wchar_t));if(!result)return E_OUTOFMEMORY;
    MultiByteToWideChar(CP_ACP,0,path,-1,result,n);*out=result;
    Log("isolated SHGetKnownFolderPath => %s",path);return S_OK;
}

static void Modules() {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(snap==INVALID_HANDLE_VALUE){Log("module snapshot failed error=%lu",GetLastError());return;}
    MODULEENTRY32W m{}; m.dwSize=sizeof(m);
    if(Module32FirstW(snap,&m)) do {
        char name[MAX_PATH],path[MAX_PATH];
        WideCharToMultiByte(CP_UTF8,0,m.szModule,-1,name,MAX_PATH,nullptr,nullptr);
        WideCharToMultiByte(CP_UTF8,0,m.szExePath,-1,path,MAX_PATH,nullptr,nullptr);
        Log("MODULE %s base=%p size=%lu path=%s",name,m.modBaseAddr,m.modBaseSize,path);
    } while(Module32NextW(snap,&m));
    CloseHandle(snap);
}

using TextureFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,const D3D11_TEXTURE2D_DESC*,const D3D11_SUBRESOURCE_DATA*,ID3D11Texture2D**);
using OMFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView*const*,ID3D11DepthStencilView*);
static TextureFn textureOriginal;static OMFn omOriginal;
struct InputTexture{ID3D11Texture2D* texture;D3D11_TEXTURE2D_DESC desc;UINT binds;bool depth;};
static InputTexture inputs[48];static UINT inputCount;
static HRESULT STDMETHODCALLTYPE Texture(ID3D11Device* d,const D3D11_TEXTURE2D_DESC* desc,const D3D11_SUBRESOURCE_DATA* data,ID3D11Texture2D** out) {
    HRESULT hr=textureOriginal(d,desc,data,out);
    if(SUCCEEDED(hr) && desc && out && *out && desc->Width==screenWidth && desc->Height==screenHeight && inputCount<48) {
        bool depth=(desc->BindFlags & D3D11_BIND_DEPTH_STENCIL)!=0;
        bool motion=desc->Format==DXGI_FORMAT_R16G16_FLOAT || desc->Format==DXGI_FORMAT_R32G32_FLOAT;
        if(depth || motion){auto& entry=inputs[inputCount++];entry.texture=*out;entry.texture->AddRef();entry.desc=*desc;entry.depth=depth;Log("INPUT candidate=%u texture=%p depth=%d motion_format_candidate=%d %ux%u fmt=%u bind=%x usage=%u misc=%x",inputCount-1,*out,depth,motion,desc->Width,desc->Height,desc->Format,desc->BindFlags,desc->Usage,desc->MiscFlags);}
    }
    return hr;
}
static void Bound(ID3D11View* view) {
    if(!view)return;ID3D11Resource* resource=nullptr;view->GetResource(&resource);
    for(UINT i=0;i<inputCount;++i)if(resource==inputs[i].texture){if(!inputs[i].binds)Log("INPUT active candidate=%u texture=%p",i,resource);++inputs[i].binds;}
    if(resource)resource->Release();
}
static void STDMETHODCALLTYPE OM(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* depth){Bound(depth);for(UINT i=0;i<n;++i)Bound(rt[i]);omOriginal(c,n,rt,depth);}
static void HookInputs(ID3D11Device* d,ID3D11DeviceContext* c) {
    if(!inputsEnabled || textureOriginal || !d || !c)return;
    Replace((*(void***)d)+5,(void*)Texture,(void**)&textureOriginal);
    Replace((*(void***)c)+33,(void*)OM,(void**)&omOriginal);
    Log("INPUT hooks CreateTexture2D=%p OMSetRenderTargets=%p",textureOriginal,omOriginal);
}
static void CaptureInputs(ID3D11Device* d,ID3D11DeviceContext* c) {
    static bool attempted;if(!inputsEnabled || attempted)return;attempted=true;
    for(UINT i=0;i<inputCount;++i){auto& entry=inputs[i];Log("INPUT summary candidate=%u binds=%u format=%u depth=%d",i,entry.binds,entry.desc.Format,entry.depth);
        if(!entry.binds || entry.desc.SampleDesc.Count!=1)continue;
        auto desc=entry.desc;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ID3D11Texture2D* staging=nullptr;HRESULT hr=d->CreateTexture2D(&desc,nullptr,&staging);if(FAILED(hr)){Log("INPUT staging hr=%08lx",hr);continue;}
        c->CopyResource(staging,entry.texture);D3D11_MAPPED_SUBRESOURCE mapped{};hr=c->Map(staging,0,D3D11_MAP_READ,0,&mapped);
        if(SUCCEEDED(hr)){
            UINT bpp=desc.Format==DXGI_FORMAT_R32G32_FLOAT || desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT?8:desc.Format==DXGI_FORMAT_R16_TYPELESS || desc.Format==DXGI_FORMAT_D16_UNORM?2:4;
            char file[MAX_PATH];snprintf(file,MAX_PATH,"%s\\input-%u.raw",output,i);FILE* raw=fopen(file,"wb");if(raw){for(UINT y=0;y<desc.Height;++y)fwrite((BYTE*)mapped.pData+y*mapped.RowPitch,bpp,desc.Width,raw);fclose(raw);}
            char metadata[MAX_PATH];snprintf(metadata,MAX_PATH,"%s\\input-%u.json",output,i);FILE* meta=fopen(metadata,"w");if(meta){fprintf(meta,"{\"width\":%u,\"height\":%u,\"format\":%u,\"binds\":%u,\"depth\":%s,\"bytes_per_pixel\":%u}",desc.Width,desc.Height,desc.Format,entry.binds,entry.depth?"true":"false",bpp);fclose(meta);}
            Log("INPUT readback candidate=%u file=%s hr=%08lx",i,file,hr);c->Unmap(staging,0);
        }else Log("INPUT Map candidate=%u hr=%08lx",i,hr);
        staging->Release();
    }
}

// One bounded synchronous readback is opt-in; it never modifies the game buffer.
static void Capture(IDXGISwapChain* swap) {
    if(!captureEnabled || InterlockedCompareExchange(&captured,1,0)) return;
    ID3D11Device* device=nullptr; ID3D11DeviceContext* context=nullptr;
    ID3D11Texture2D *back=nullptr,*copy=nullptr,*staging=nullptr;
    HRESULT hr=swap->GetDevice(__uuidof(ID3D11Device),(void**)&device);
    if(FAILED(hr)) {Log("capture GetDevice hr=%08lx",hr);return;}
    device->GetImmediateContext(&context);
    hr=swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back);
    if(SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
        if(sidecarEnabled)ProbeSidecar(device,context,back,output,Log);
        Log("capture backbuffer=%p %ux%u fmt=%u samples=%u usage=%u bind=%x misc=%x",back,desc.Width,desc.Height,desc.Format,desc.SampleDesc.Count,desc.Usage,desc.BindFlags,desc.MiscFlags);
        if(desc.SampleDesc.Count==1 && (desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) {
            desc.BindFlags=0;desc.MiscFlags=0;desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;
            hr=device->CreateTexture2D(&desc,nullptr,&copy);
            if(SUCCEEDED(hr)) {
                context->CopyResource(copy,back);
                desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                hr=device->CreateTexture2D(&desc,nullptr,&staging);
            }
            if(SUCCEEDED(hr)) {
                context->CopyResource(staging,copy);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr=context->Map(staging,0,D3D11_MAP_READ,0,&mapped);
                if(SUCCEEDED(hr)) {
                    char file[MAX_PATH];snprintf(file,MAX_PATH,"%s\\frame.ppm",output);
                    FILE* f=fopen(file,"wb");
                    if(f) {
                        fprintf(f,"P6\n%u %u\n255\n",desc.Width,desc.Height);
                        unsigned char* row=(unsigned char*)HeapAlloc(GetProcessHeap(),0,desc.Width*3);
                        if(row) {
                            bool bgra=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                            unsigned long long sum=0;
                            for(UINT y=0;y<desc.Height;++y){auto p=(unsigned char*)mapped.pData+y*mapped.RowPitch;for(UINT x=0;x<desc.Width;++x){row[x*3]=p[x*4+(bgra?2:0)];row[x*3+1]=p[x*4+1];row[x*3+2]=p[x*4+(bgra?0:2)];sum+=row[x*3]+row[x*3+1]+row[x*3+2];}fwrite(row,3,desc.Width,f);}
                            HeapFree(GetProcessHeap(),0,row);
                            Log("CAPTURE verified copy -> staging readback %s rgb_sum=%llu",file,sum);
                        }
                        fclose(f);
                    } else Log("capture fopen failed %s",file);
                    context->Unmap(staging,0);
                }
            }
        } else Log("capture unsupported format or MSAA; buffer unchanged");
    }
    Log("capture final hr=%08lx",hr);
    if(staging)staging->Release();if(copy)copy->Release();if(back)back->Release();if(context)context->Release();device->Release();
}

using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using ResizeFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
struct Table {void** table; PresentFn present; Present1Fn present1; ResizeFn resize;};
static Table tables[16];static UINT tableCount;
static Table* Find(void* object) {auto vt=*(void***)object;for(UINT i=0;i<tableCount;++i)if(tables[i].table==vt)return &tables[i];return nullptr;}
static void BeforePresent(IDXGISwapChain* swap,UINT sync,UINT flags,const char* route) {
    if(flags & DXGI_PRESENT_TEST)return;
    auto count=InterlockedIncrement64(&frames);
    auto now=GetTickCount64();
    if(count==1 || now-lastLog>=3000) {
        lastLog=now; DXGI_SWAP_CHAIN_DESC desc{};swap->GetDesc(&desc);
        ID3D11Device* d=nullptr; ID3D11DeviceContext* c=nullptr;
        HRESULT hr=swap->GetDevice(__uuidof(ID3D11Device),(void**)&d);if(d)d->GetImmediateContext(&c);
        Log("PRESENT route=%s frame=%lld swap=%p device11=%p context=%p hr=%08lx %ux%u fmt=%u buffers=%u effect=%u windowed=%d sync=%u flags=%x",route,count,swap,d,c,hr,desc.BufferDesc.Width,desc.BufferDesc.Height,desc.BufferDesc.Format,desc.BufferCount,desc.SwapEffect,desc.Windowed,sync,flags);
        if(c)c->Release();if(d)d->Release();
        if(count==1)Modules();
    }
    if(count>=120)Capture(swap);
    if(inputsEnabled && count==600){ID3D11Device* d=nullptr;ID3D11DeviceContext* c=nullptr;if(SUCCEEDED(swap->GetDevice(__uuidof(ID3D11Device),(void**)&d))){d->GetImmediateContext(&c);CaptureInputs(d,c);c->Release();d->Release();}}
}
static HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swap,UINT sync,UINT flags){auto t=Find(swap);BeforePresent(swap,sync,flags,"Present");HRESULT hr=t->present(swap,sync,flags);if(FAILED(hr))Log("Present failure hr=%08lx",hr);return hr;}
static HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* swap,UINT sync,UINT flags,const DXGI_PRESENT_PARAMETERS* p){auto t=Find(swap);BeforePresent(swap,sync,flags,"Present1");return t->present1(swap,sync,flags,p);}
static HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain* swap,UINT n,UINT w,UINT h,DXGI_FORMAT f,UINT flags){Log("ResizeBuffers swap=%p %ux%u fmt=%u buffers=%u",swap,w,h,f,n);return Find(swap)->resize(swap,n,w,h,f,flags);}
static void HookSwap(IDXGISwapChain* swap) {
    auto vt=*(void***)swap;
    if(Find(swap) || tableCount==16)return;
    auto t=&tables[tableCount++];t->table=vt;
    bool ok=Replace(vt+8,(void*)Present,(void**)&t->present);
    bool resizeOk=!coreEnabled && Replace(vt+13,(void*)Resize,(void**)&t->resize);
    IDXGISwapChain1* s1=nullptr;
    if(!coreEnabled && SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1),(void**)&s1))) {
        if(*(void***)s1==vt)Replace(vt+22,(void*)Present1,(void**)&t->present1);
        else Log("Present1 has separate interface table; base Present is instrumented");
        s1->Release();
    }
    Log("HOOK swap=%p vtable=%p Present=%p original=%p ok=%d Resize=%d",swap,vt,Present,t->present,ok,resizeOk);
}

using CreateFn=decltype(&D3D11CreateDeviceAndSwapChain);
static CreateFn createOriginal;
static HRESULT WINAPI Create(IDXGIAdapter* adapter,D3D_DRIVER_TYPE type,HMODULE sw,UINT flags,const D3D_FEATURE_LEVEL* levels,UINT n,UINT sdk,const DXGI_SWAP_CHAIN_DESC* desc,IDXGISwapChain** swap,ID3D11Device** device,D3D_FEATURE_LEVEL* level,ID3D11DeviceContext** context) {
    Log("CREATE D3D11CreateDeviceAndSwapChain adapter=%p type=%u flags=%x desc=%p",adapter,type,flags,desc);
    if(sidecarEnabled)BootstrapSidecarBefore11(adapter,Log);
    HRESULT hr=createOriginal(adapter,type,sw,flags,levels,n,sdk,desc,swap,device,level,context);
    Log("CREATE result hr=%08lx device=%p context=%p swap=%p feature=%x",hr,device?*device:nullptr,context?*context:nullptr,swap?*swap:nullptr,level?*level:0);
    if(SUCCEEDED(hr) && swap && *swap) {
        if(sidecarEnabled && device && *device)BootstrapSidecar(*device,Log);
        if(desc){screenWidth=desc->BufferDesc.Width;screenHeight=desc->BufferDesc.Height;}
        TemporalAttach(device?*device:nullptr,context?*context:nullptr,*swap);
        if(!coreEnabled)HookInputs(device?*device:nullptr,context?*context:nullptr);
        if(device && *device){IDXGIDevice* dx=nullptr;IDXGIAdapter* ad=nullptr;if(SUCCEEDED((*device)->QueryInterface(__uuidof(IDXGIDevice),(void**)&dx))){dx->GetAdapter(&ad);if(ad){DXGI_ADAPTER_DESC a{};ad->GetDesc(&a);char name[256];WideCharToMultiByte(CP_UTF8,0,a.Description,-1,name,256,nullptr,nullptr);Log("ADAPTER %s vendor=%x device=%x luid=%lx:%lx",name,a.VendorId,a.DeviceId,a.AdapterLuid.HighPart,a.AdapterLuid.LowPart);ad->Release();}dx->Release();}}
        if(!coreEnabled || coreDiagnostics)HookSwap(*swap);
        else Log("CORE observation mode: diagnostic Present slots left to the supplied core");
    }
    return hr;
}
using FactoryFn=HRESULT(WINAPI*)(REFIID,void**);static FactoryFn factoryOriginal;
static HRESULT WINAPI Factory(REFIID iid,void** out){HRESULT hr=factoryOriginal(iid,out);Log("CREATE CreateDXGIFactory hr=%08lx factory=%p",hr,out?*out:nullptr);return hr;}

static bool IsRtssW(LPCWSTR path){if(!path)return false;auto name=wcsrchr(path,L'\\');return !_wcsicmp(name?name+1:path,L"RTSSHooks64.dll");}
using LdrLoadFn=NTSTATUS(NTAPI*)(PWSTR,ULONG*,UNICODE_STRING*,HANDLE*);
static LdrLoadFn ldrLoadOriginal;
static NTSTATUS NTAPI FilterLdrLoad(PWSTR search,ULONG* flags,UNICODE_STRING* name,HANDLE* module){
    wchar_t path[MAX_PATH]{};
    if(name && name->Buffer && name->Length<sizeof(path)){memcpy(path,name->Buffer,name->Length);if(IsRtssW(path)){Log("RTSS comparison: blocked process-local native load %ls",path);if(module)*module=nullptr;return (NTSTATUS)0xC0000135;}}
    return ldrLoadOriginal(search,flags,name,module);
}
static DWORD WINAPI CoreThread(void*) {
    MH_STATUS init=MH_Initialize();
    // Install the native load guard first: Win32 LoadLibrary guards alone miss
    // RTSS's injection path. Refuse NR if RTSS won the startup race.
    if(excludeRtss){
        auto native=(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"LdrLoadDll");
        auto made=MH_CreateHook(native,(void*)FilterLdrLoad,(void**)&ldrLoadOriginal);auto enabled=made==MH_OK?MH_EnableHook(native):made;
        Log("RTSS exclusion LdrLoadDll create=%d enable=%d",made,enabled);
        if(enabled!=MH_OK){Log("FATAL RTSS exclusion could not be installed; core not loaded");return 1;}
        auto already=GetModuleHandleW(L"RTSSHooks64.dll");
        Log("RTSS exclusion already-loaded module=%p",already);
        if(already){Log("FATAL RTSS already loaded before exclusion; core not loaded. Retry test launch or disable RTSS for SkyrimSE.exe.");return 1;}
    }
    // Redirect the core's known-folder logging before loading it.
    HMODULE shell=LoadLibraryExW(L"shell32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto target=shell?(void*)GetProcAddress(shell,"SHGetKnownFolderPath"):nullptr;
    MH_STATUS create=target?MH_CreateHook(target,(void*)ProbeKnownFolder,(void**)&knownFolderOriginal):MH_ERROR_NOT_EXECUTABLE;
    MH_STATUS enable=create==MH_OK?MH_EnableHook(target):create;
    Log("Known-folder isolation MinHook init=%d create=%d enable=%d",init,create,enable);
    if(enable!=MH_OK){Log("FATAL core not loaded: known-folder isolation failed");return 1;}
    wchar_t delayText[32]{};GetEnvironmentVariableW(L"SKYRIM_PROBE_CORE_DELAY_MS",delayText,32);
    DWORD delay=wcstoul(delayText,nullptr,10);
    if(delay){Log("CORE late-load experiment delay=%lu ms",delay);Sleep(delay);}
    if(!delay){
    wchar_t markerName[128];swprintf(markerName,128,L"Local\\Dlss5Quick.EarlyInject.%lu",GetCurrentProcessId());
    // Keep the author's own early-injection marker alive for InitThread.
    HANDLE marker=CreateEventW(nullptr,TRUE,FALSE,markerName);
    Log("CORE early marker=%ls handle=%p",markerName,marker);
    swprintf(markerName,128,L"Local\\DXL.EarlyInject.%lu",GetCurrentProcessId());
    HANDLE dxlMarker=CreateEventW(nullptr,TRUE,FALSE,markerName);
    Log("CORE early marker=%ls handle=%p",markerName,dxlMarker);
    }
    Sleep(300);
    wchar_t path[MAX_PATH];
    if(!GetEnvironmentVariableW(L"SKYRIM_PROBE_CORE_PATH",path,MAX_PATH)){GetModuleFileNameW(selfModule,path,MAX_PATH);wchar_t* slash=wcsrchr(path,L'\\');if(!slash)return 1;wcscpy(slash+1,L"RE_DLSS5_Core.dll");}
    HMODULE core=LoadLibraryW(path);Log("CORE LoadLibrary path=%ls module=%p error=%lu",path,core,core?0:GetLastError());
    Sleep(2000);Modules();TemporalInstallNeuralHook();return 0;
}

// Bink startup proxy exports use ABI-preserving assembly stubs generated at build.
#include "proxy_exports.h"
static INIT_ONCE versionOnce=INIT_ONCE_STATIC_INIT;static HMODULE versionModule;
static BOOL CALLBACK LoadVersion(PINIT_ONCE,PVOID,PVOID*){wchar_t path[MAX_PATH];if(!GetEnvironmentVariableW(L"SKYRIM_PROBE_ORIGINAL_DLL",path,MAX_PATH))return FALSE;versionModule=LoadLibraryW(path);return versionModule!=nullptr;}
extern "C" void* ProbeResolve(UINT index){if(index>=sizeof(exportNames)/sizeof(*exportNames)||!InitOnceExecuteOnce(&versionOnce,LoadVersion,nullptr,nullptr))return nullptr;return (void*)GetProcAddress(versionModule,exportNames[index]);}

BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    selfModule=module;DisableThreadLibraryCalls(module);
    char file[MAX_PATH],expected[MAX_PATH],actual[MAX_PATH],mode[32];
    if(!GetEnvironmentVariableA("SKYRIM_PROBE_LOG",file,MAX_PATH) || !GetEnvironmentVariableA("SKYRIM_PROBE_EXPECTED_EXE",expected,MAX_PATH))return FALSE;
    GetModuleFileNameA(nullptr,actual,MAX_PATH);if(_stricmp(actual,expected))return FALSE;
    logFile=CreateFileA(file,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    GetEnvironmentVariableA("SKYRIM_PROBE_PROFILE",profile,MAX_PATH);GetEnvironmentVariableA("SKYRIM_PROBE_OUTPUT",output,MAX_PATH);
    TemporalConfigure(output,Log);
    GetEnvironmentVariableA("SKYRIM_PROBE_CAPTURE",mode,sizeof(mode));captureEnabled=mode[0]=='1';
    GetEnvironmentVariableA("SKYRIM_PROBE_INPUTS",mode,sizeof(mode));inputsEnabled=mode[0]=='1';
    GetEnvironmentVariableA("SKYRIM_PROBE_SIDECAR",mode,sizeof(mode));sidecarEnabled=mode[0]=='1';
    Log("ATTACH pid=%lu actual=%s profile=%s capture=%d",GetCurrentProcessId(),actual,profile,captureEnabled);
    HMODULE main=GetModuleHandleW(nullptr);
    bool isSkyrim=strstr(actual,"SkyrimSE.exe")!=nullptr;
    bool folders=PatchImport(main,"SHGetFolderPathA",(void*)ProbeFolder,(void**)&folderOriginal);
    if(isSkyrim && (!profile[0] || !folders)){Log("FATAL profile isolation not installed");return FALSE;}
    PatchImport(main,"ShellExecuteA",(void*)ProbeShell,(void**)&shellOriginal);
    PatchImport(main,"CreateDXGIFactory",(void*)Factory,(void**)&factoryOriginal);
    bool graphics=PatchImport(main,"D3D11CreateDeviceAndSwapChain",(void*)Create,(void**)&createOriginal);
    if(!graphics && isSkyrim){Log("FATAL no D3D11 creation import");return FALSE;}
    GetEnvironmentVariableA("SKYRIM_PROBE_CORE",mode,sizeof(mode));
    coreEnabled=mode[0]=='1';
    GetEnvironmentVariableA("SKYRIM_PROBE_CORE_DIAGNOSTICS",mode,sizeof(mode));coreDiagnostics=mode[0]=='1';
    GetEnvironmentVariableA("SKYRIM_PROBE_EXCLUDE_RTSS",mode,sizeof(mode));excludeRtss=mode[0]=='1';
    if(coreEnabled){HANDLE thread=CreateThread(nullptr,0,CoreThread,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    return TRUE;
}
