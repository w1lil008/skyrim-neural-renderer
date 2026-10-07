#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

int main() {
    wchar_t readyName[128];swprintf(readyName,128,L"Local\\Dlss5Quick.Ready.%lu",GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName);
    HMODULE probe=LoadLibraryW(L"bink2w64.dll");
    if(!probe){printf("probe load failed %lu\n",GetLastError());return 1;}
    // Let the core register ordinary DXGI hooks before creating a real D3D12 chain.
    WaitForSingleObject(ready,5000);CloseHandle(ready);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"SkyrimProbeFixture12";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"D3D12 sidecar diagnostic fixture",WS_OVERLAPPEDWINDOW,0,0,640,360,nullptr,nullptr,wc.hInstance,nullptr);
    ShowWindow(window,SW_SHOW);UpdateWindow(window);
    ID3D12Device* device=nullptr;ID3D12CommandQueue* queue=nullptr;IDXGIFactory4* factory=nullptr;IDXGISwapChain1* chain1=nullptr;IDXGISwapChain3* swap=nullptr;
    HRESULT hr=D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,__uuidof(ID3D12Device),(void**)&device);
    printf("D3D12CreateDevice hr=%08lx device=%p\n",hr,device);if(FAILED(hr))return 2;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr=device->CreateCommandQueue(&q,__uuidof(ID3D12CommandQueue),(void**)&queue);if(FAILED(hr))return 3;
    hr=CreateDXGIFactory1(__uuidof(IDXGIFactory4),(void**)&factory);if(FAILED(hr))return 4;
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=640;desc.Height=360;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr=factory->CreateSwapChainForHwnd(queue,window,&desc,nullptr,nullptr,&chain1);printf("CreateSwapChainForHwnd hr=%08lx chain=%p\n",hr,chain1);if(FAILED(hr))return 5;
    chain1->QueryInterface(__uuidof(IDXGISwapChain3),(void**)&swap);chain1->Release();factory->Release();
    ID3D12CommandAllocator* alloc=nullptr;ID3D12GraphicsCommandList* list=nullptr;ID3D12DescriptorHeap* heap=nullptr;ID3D12Fence* fence=nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,__uuidof(ID3D12CommandAllocator),(void**)&alloc);
    device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,__uuidof(ID3D12GraphicsCommandList),(void**)&list);list->Close();
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;device->CreateDescriptorHeap(&hd,__uuidof(ID3D12DescriptorHeap),(void**)&heap);
    device->CreateFence(0,D3D12_FENCE_FLAG_NONE,__uuidof(ID3D12Fence),(void**)&fence);HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    ID3D12Resource* buffers[2]{};auto start=heap->GetCPUDescriptorHandleForHeapStart();UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(UINT i=0;i<2;++i){swap->GetBuffer(i,__uuidof(ID3D12Resource),(void**)&buffers[i]);D3D12_CPU_DESCRIPTOR_HANDLE handle{start.ptr+i*stride};device->CreateRenderTargetView(buffers[i],nullptr,handle);}
    for(UINT frame=0;frame<300;++frame){
        MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        UINT i=swap->GetCurrentBackBufferIndex();alloc->Reset();list->Reset(alloc,nullptr);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=buffers[i];barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;list->ResourceBarrier(1,&barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE handle{start.ptr+i*stride};float color[4]={0.125f,0.5f,0.875f,1.0f};list->ClearRenderTargetView(handle,color,0,nullptr);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;list->ResourceBarrier(1,&barrier);list->Close();ID3D12CommandList* commands[]={list};queue->ExecuteCommandLists(1,commands);
        hr=swap->Present(0,0);if(FAILED(hr)){printf("Present hr=%08lx\n",hr);return 6;}
        queue->Signal(fence,frame+1);if(fence->GetCompletedValue()<frame+1){fence->SetEventOnCompletion(frame+1,done);if(WaitForSingleObject(done,5000)!=WAIT_OBJECT_0){printf("GPU timeout\n");return 7;}}
        Sleep(30);
    }
    printf("fixture12 completed 300 presents device removed reason=%08lx\n",device->GetDeviceRemovedReason());
    CloseHandle(done);for(auto b:buffers)b->Release();fence->Release();heap->Release();list->Release();alloc->Release();swap->Release();queue->Release();device->Release();DestroyWindow(window);return 0;
}
