#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
int main() {
    wchar_t corePath[MAX_PATH]{}, readyName[128];
    GetEnvironmentVariableW(L"SKYRIM_PROBE_CORE_PATH",corePath,MAX_PATH);
    swprintf(readyName,128,wcsstr(corePath,L"DXL-core.dll")?L"Local\\DXL.Ready.%lu":L"Local\\Dlss5Quick.Ready.%lu",GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName);
    if(!LoadLibraryW(L"bink2w64.dll")){printf("probe load failed %lu\n",GetLastError());return 3;}
    char coreMode[8];GetEnvironmentVariableA("SKYRIM_PROBE_CORE",coreMode,sizeof(coreMode));
    if(coreMode[0]=='1')printf("core ready wait=%lu\n",WaitForSingleObject(ready,5000));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SkyrimProbeFixture";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"D3D11 diagnostic fixture",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Width=640;desc.BufferDesc.Height=360;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;IDXGISwapChain* swap=nullptr;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap,&device,nullptr,&context);
    printf("fixture create hr=%08lx\n",hr);if(FAILED(hr))return 1;
    ID3D11Texture2D* back=nullptr;ID3D11RenderTargetView* view=nullptr;
    swap->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&back);device->CreateRenderTargetView(back,nullptr,&view);back->Release();
    for(int i=0;i<240;++i){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}float color[4]={0.125f,0.5f,0.875f,1.0f};context->ClearRenderTargetView(view,color);hr=swap->Present(0,0);if(FAILED(hr)){printf("fixture Present hr=%08lx\n",hr);return 2;}Sleep(16);}
    view->Release();context->ClearState();context->Flush();swap->Release();context->Release();device->Release();DestroyWindow(window);printf("fixture completed 240 presents\n");return 0;
}
