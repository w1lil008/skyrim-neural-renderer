#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Deterministic native-like MRT/TAA sequence. It exercises the capture route;
// it does not establish that an arbitrary Skyrim resource uses this convention.
static constexpr UINT kWidth = 640, kHeight = 360;
// Optional captured-photo stimulus for fixture-only neural sensitivity probes.
// Depth and motion remain the analytic fixture; this is NOT spatial approval.
static const unsigned char* ReplayPixels(){
    static bool attempted=false;static unsigned char* pixels=nullptr;
    if(attempted)return pixels;attempted=true;
    char path[MAX_PATH]{};if(!GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_REPLAY_COLOR",path,MAX_PATH))return nullptr;
    FILE* f=fopen(path,"rb");if(!f)return nullptr;
    const size_t bytes=size_t(kWidth)*kHeight*4;pixels=(unsigned char*)malloc(bytes);
    bool valid=pixels&&fread(pixels,1,bytes,f)==bytes&&fgetc(f)==EOF;fclose(f);
    if(!valid){free(pixels);pixels=nullptr;}return pixels;
}
static const char kShader[] = R"HLSL(
cbuffer Scene : register(b0) {
    float4 centers; // current xy, previous xy; top-left-origin pixel coordinates
    float4 dimensions; // width, height, rectangle half width, half height
    float4 flags; // background pass, frame index, unused, unused
};
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex SceneVS(uint id : SV_VertexID) {
    Vertex v;
    if (flags.x > 0.5) {
        float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
        v.position = float4(p, 0.75, 1.0);
        v.uv = p * float2(0.5, -0.5) + 0.5;
    } else {
        const float2 corners[6] = {
            float2(-1,-1), float2(1,-1), float2(-1,1),
            float2(-1,1), float2(1,-1), float2(1,1)
        };
        float2 p = centers.xy + corners[id] * dimensions.zw;
        v.position = float4(p / dimensions.xy * float2(2,-2) + float2(-1,1), 0.25, 1.0);
        v.uv = corners[id] * 0.5 + 0.5;
    }
    return v;
}
struct SceneOutput { float4 color : SV_Target0; float2 motion : SV_Target1; };
SceneOutput ScenePS(Vertex v) {
    SceneOutput o;
    float checker = fmod(floor(v.uv.x * 16) + floor(v.uv.y * 16), 2);
    if (flags.x > 0.5) {
        o.color = float4(0.12 + v.uv.x * 0.55, 0.15 + v.uv.y * 0.60, 0.30 + checker * 0.10, 1);
        o.motion = float2(0,0);
    } else {
        o.color = float4(0.30 + checker * 0.60, 0.12 + v.uv.x * 0.45, 0.08 + v.uv.y * 0.50, 1);
        o.motion = (centers.zw - centers.xy) / dimensions.xy;
    }
    return o;
}
Vertex FullscreenVS(uint id : SV_VertexID) {
    Vertex v;
    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    v.position = float4(p, 0, 1);
    v.uv = p * float2(0.5, -0.5) + 0.5;
    return v;
}
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> PreviousColor : register(t1);
Texture2D<float2> NativeVelocity : register(t2);
Texture2D<float> NativeDepth : register(t3);
SamplerState PointClamp : register(s0);
float4 TemporalPS(Vertex v) : SV_Target0 {
    float2 motion = NativeVelocity.Sample(PointClamp, v.uv);
    float depth = NativeDepth.Sample(PointClamp, v.uv);
    float3 current = CurrentColor.Sample(PointClamp, v.uv).rgb;
    float3 history = PreviousColor.Sample(PointClamp, v.uv + motion).rgb;
    // All four inputs have a real effect, so their native shader slots survive
    // optimization. The source/history remain original, pre-neural scene color.
    return float4(lerp(current, history, depth < 0.5 ? 0.10 : 0.03), 1);
}
)HLSL";

template<class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void Name(ID3D11DeviceChild* p, const char* name) {
    if (p) p->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
}
using CompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR,
    const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
    ID3DBlob**, ID3DBlob**);
static HRESULT Compile(CompileFn fn, const char* entry, const char* target, ID3DBlob** blob) {
    ID3DBlob* error = nullptr;
    HRESULT hr = fn(kShader, sizeof(kShader) - 1, "temporal_fixture", nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &error);
    if (error) {
        printf("shader %s: %.*s\n", entry, (int)error->GetBufferSize(), (char*)error->GetBufferPointer());
        error->Release();
    }
    return hr;
}
struct SceneConstants { float centers[4], dimensions[4], flags[4]; };

struct Fixture {
    HWND window = nullptr;
    HMODULE compiler = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap = nullptr;
    ID3D11Texture2D* back = nullptr;
    ID3D11RenderTargetView* backView = nullptr;
    ID3D11Texture2D *color = nullptr, *history = nullptr, *motion = nullptr, *depth = nullptr;
    ID3D11RenderTargetView *colorView = nullptr, *motionView = nullptr;
    ID3D11ShaderResourceView *colorSrv = nullptr, *historySrv = nullptr, *motionSrv = nullptr, *depthSrv = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    ID3D11VertexShader *sceneVs = nullptr, *fullscreenVs = nullptr;
    ID3D11PixelShader *scenePs = nullptr, *temporalPs = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11DepthStencilState *depthWrite = nullptr, *depthOff = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ~Fixture() {
        if (context) { context->ClearState(); context->Flush(); }
        Release(sampler); Release(depthOff); Release(depthWrite); Release(rasterizer); Release(constants);
        Release(temporalPs); Release(scenePs); Release(fullscreenVs); Release(sceneVs);
        Release(depthView); Release(depthSrv); Release(motionSrv); Release(historySrv); Release(colorSrv);
        Release(motionView); Release(colorView); Release(depth); Release(motion); Release(history); Release(color);
        Release(backView); Release(back); Release(swap); Release(context); Release(device);
        if (window) DestroyWindow(window);
        if (compiler) FreeLibrary(compiler);
    }
    HRESULT Setup() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SkyrimTemporalFixture";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
        RECT rect{0,0,(LONG)kWidth,(LONG)kHeight};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowW(wc.lpszClassName, L"D3D11 temporal capture fixture", WS_OVERLAPPEDWINDOW,
            0, 0, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) return HRESULT_FROM_WIN32(GetLastError());
        char visible[8]{};
        GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_VISIBLE", visible, sizeof(visible));
        if (visible[0] == '1') ShowWindow(window, SW_SHOW);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = kWidth; desc.BufferDesc.Height = kHeight;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1; desc.OutputWindow = window; desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &desc, &swap, &device, nullptr, &context);
        if (FAILED(hr)) return hr;
        hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
        if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(back, nullptr, &backView);
        if (FAILED(hr)) return hr;
        D3D11_TEXTURE2D_DESC tex{};
        tex.Width = kWidth; tex.Height = kHeight; tex.MipLevels = 1; tex.ArraySize = 1;
        tex.SampleDesc.Count = 1; tex.Usage = D3D11_USAGE_DEFAULT;
        tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&tex, nullptr, &color); if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(color, nullptr, &colorView); if (FAILED(hr)) return hr;
        hr = device->CreateShaderResourceView(color, nullptr, &colorSrv); if (FAILED(hr)) return hr;
        // Allocate history as an RTV too solely to initialize it once; it is
        // never bound with motion/depth and cannot resemble the scene MRT.
        hr = device->CreateTexture2D(&tex, nullptr, &history); if (FAILED(hr)) return hr;
        hr = device->CreateShaderResourceView(history, nullptr, &historySrv); if (FAILED(hr)) return hr;
        ID3D11RenderTargetView* initializeHistory = nullptr;
        hr = device->CreateRenderTargetView(history, nullptr, &initializeHistory); if (FAILED(hr)) return hr;
        const float black[4] = {0,0,0,1};
        context->ClearRenderTargetView(initializeHistory, black);
        initializeHistory->Release();
        tex.Format = DXGI_FORMAT_R16G16_FLOAT;
        hr = device->CreateTexture2D(&tex, nullptr, &motion); if (FAILED(hr)) return hr;
        hr = device->CreateRenderTargetView(motion, nullptr, &motionView); if (FAILED(hr)) return hr;
        hr = device->CreateShaderResourceView(motion, nullptr, &motionSrv); if (FAILED(hr)) return hr;
        tex.Format = DXGI_FORMAT_R24G8_TYPELESS;
        tex.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        hr = device->CreateTexture2D(&tex, nullptr, &depth); if (FAILED(hr)) return hr;
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = device->CreateDepthStencilView(depth, &dsv, &depthView); if (FAILED(hr)) return hr;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(depth, &srv, &depthSrv); if (FAILED(hr)) return hr;
        Name(color, "TemporalFixture.SceneColor"); Name(history, "TemporalFixture.PreviousSceneColor");
        Name(motion, "TemporalFixture.NativeUVVelocity"); Name(depth, "TemporalFixture.MainD24Depth");
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(SceneConstants); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device->CreateBuffer(&cb, nullptr, &constants); if (FAILED(hr)) return hr;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        hr = device->CreateRasterizerState(&rs, &rasterizer); if (FAILED(hr)) return hr;
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_LESS;
        ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS; ds.BackFace = ds.FrontFace;
        hr = device->CreateDepthStencilState(&ds, &depthWrite); if (FAILED(hr)) return hr;
        ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&ds, &depthOff); if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sample{};
        sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxAnisotropy = 1;
        sample.ComparisonFunc = D3D11_COMPARISON_NEVER; sample.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sample, &sampler); if (FAILED(hr)) return hr;
        compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler) return HRESULT_FROM_WIN32(GetLastError());
        CompileFn compile = (CompileFn)GetProcAddress(compiler, "D3DCompile");
        if (!compile) return HRESULT_FROM_WIN32(GetLastError());
        ID3DBlob* code = nullptr;
        hr = Compile(compile, "SceneVS", "vs_5_0", &code);
        if (SUCCEEDED(hr)) hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &sceneVs);
        Release(code); if (FAILED(hr)) return hr;
        hr = Compile(compile, "ScenePS", "ps_5_0", &code);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &scenePs);
        Release(code); if (FAILED(hr)) return hr;
        hr = Compile(compile, "FullscreenVS", "vs_5_0", &code);
        if (SUCCEEDED(hr)) hr = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &fullscreenVs);
        Release(code); if (FAILED(hr)) return hr;
        hr = Compile(compile, "TemporalPS", "ps_5_0", &code);
        if (SUCCEEDED(hr)) hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &temporalPs);
        Release(code); if (FAILED(hr)) return hr;
        Name(scenePs, "TemporalFixture.SceneMRTShader"); Name(temporalPs, "TemporalFixture.NativeLikeTAA_t2Motion_t3Depth");
        return S_OK;
    }
    static float CenterX(UINT frame) {
        UINT phase = frame % 400; return 220.0f + (float)(phase <= 200 ? phase : 400 - phase);
    }
    static float CenterY(UINT frame) {
        UINT phase = frame % 320; return 140.0f + 0.5f * (float)(phase <= 160 ? phase : 320 - phase);
    }
    HRESULT Draw(UINT frame, UINT presentation) {
        ID3D11ShaderResourceView* empty[4]{};
        context->PSSetShaderResources(0, 4, empty);
        const float black[4] = {0,0,0,1}, zero[4] = {0,0,0,0};
        context->ClearRenderTargetView(colorView, black);
        context->ClearRenderTargetView(motionView, zero);
        context->ClearDepthStencilView(depthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT viewport{0,0,(float)kWidth,(float)kHeight,0,1};
        context->RSSetViewports(1, &viewport); context->RSSetState(rasterizer);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetConstantBuffers(0, 1, &constants); context->PSSetConstantBuffers(0, 1, &constants);
        context->VSSetShader(sceneVs, nullptr, 0); context->PSSetShader(scenePs, nullptr, 0);
        context->OMSetDepthStencilState(depthWrite, 0);
        ID3D11RenderTargetView* sceneTargets[2] = {colorView,motionView};
        context->OMSetRenderTargets(2, sceneTargets, depthView);
        UINT previous = frame ? frame - 1 : frame;
        SceneConstants data{{CenterX(frame), CenterY(frame), CenterX(previous), CenterY(previous)},
            {(float)kWidth, (float)kHeight, 64, 80}, {1,(float)frame,0,0}};
        context->UpdateSubresource(constants, 0, nullptr, &data, 0, 0);
        context->Draw(3, 0);
        data.flags[0] = 0;
        context->UpdateSubresource(constants, 0, nullptr, &data, 0, 0);
        context->Draw(6, 0);
        // This transition is a critical part of the test: inputs have finished
        // their scene writes; the following full-screen pass has no DSV.
        context->OMSetRenderTargets(1, &backView, nullptr);
        if(auto pixels=ReplayPixels())context->UpdateSubresource(color,0,nullptr,pixels,kWidth*4,kWidth*kHeight*4);
        context->OMSetDepthStencilState(depthOff, 0);
        context->VSSetShader(fullscreenVs, nullptr, 0); context->PSSetShader(temporalPs, nullptr, 0);
        ID3D11ShaderResourceView* temporalInputs[4] = {colorSrv,historySrv,motionSrv,depthSrv};
        context->PSSetShaderResources(0, 4, temporalInputs); context->PSSetSamplers(0, 1, &sampler);
        context->Draw(3, 0);
        context->PSSetShaderResources(0, 4, empty);
        context->CopyResource(history, color);
        if (presentation % 120 == 0) {
            printf("presentation=%u scene_frame=%u center=(%.3f,%.3f) bounds=(%.3f,%.3f)-(%.3f,%.3f) "
                "foregroundDepth=0.25 backgroundDepth=0.75 mvUV=(%.9f,%.9f) mvPixels=(%.3f,%.3f)\n",
                presentation, frame, data.centers[0], data.centers[1], data.centers[0]-64, data.centers[1]-80,
                data.centers[0]+64, data.centers[1]+80,
                (data.centers[2]-data.centers[0])/kWidth, (data.centers[3]-data.centers[1])/kHeight,
                data.centers[2]-data.centers[0], data.centers[3]-data.centers[1]);
            fflush(stdout);
        }
        return swap->Present(0, 0);
    }
};

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    UINT frames = 720;
    char duration[64]{};
    if (GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_FRAMES", duration, sizeof(duration))) {
        int value = atoi(duration); if (value > 0 && value <= 360000) frames = (UINT)value;
    }
    if (GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_SECONDS", duration, sizeof(duration))) {
        int value = atoi(duration); if (value > 0 && value <= 6000) frames = (UINT)value * 60;
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]); if (value > 0 && value <= 360000) frames = (UINT)value;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]); if (value > 0 && value <= 6000) frames = (UINT)value * 60;
        }
    }
    bool freezeScene = false;
    UINT frozenSceneFrame = 0;
    char freezeValue[64]{};
    if (GetEnvironmentVariableA("SKYRIM_TEMPORAL_FIXTURE_FREEZE_FRAME", freezeValue, sizeof(freezeValue))) {
        char* end = nullptr;
        unsigned long value = strtoul(freezeValue, &end, 10);
        if (end != freezeValue && *end == 0 && freezeValue[0] != '-' && value <= 360000) {
            freezeScene = true; frozenSceneFrame = (UINT)value;
        } else printf("invalid fixture freeze frame; using normal animation\n");
    }
    printf("fixture frozen=%s scene_frame=%u motion_reference=%u; frozen vectors are an artificial sensitivity probe\n",
        freezeScene ? "true" : "false", frozenSceneFrame, frozenSceneFrame ? frozenSceneFrame - 1 : 0);
    char output[MAX_PATH]{};
    if (GetEnvironmentVariableA("SKYRIM_PROBE_OUTPUT", output, MAX_PATH)) {
        char path[MAX_PATH]{};
        if (snprintf(path, MAX_PATH, "%s\\fixture_scene.json", output) < MAX_PATH) {
            FILE* proof = fopen(path, "w");
            if (proof) {
                fprintf(proof, "{\"fixture_type\":\"deterministic_d3d11_temporal\",\"width\":%u,\"height\":%u,"
                    "\"frozen\":%s,\"frozen_scene_frame\":%u,\"frozen_motion_reference_frame\":%u,"
                    "\"total_presentations_planned\":%u,\"scene_frame_clock\":\"%s\","
                    "\"scene_shader_source_unchanged\":true,\"replay_color_active\":%s,\"replay_notice\":\"Optional captured color uses synthetic analytic depth/motion solely as a sensitivity stimulus; never spatial validation.\",\"motion_probe_notice\":"
                    "\"Frozen RGB repeats retain vectors from N and N-1; these artificial vectors do not describe repeated-frame motion.\"}",
                    kWidth, kHeight, freezeScene ? "true" : "false", frozenSceneFrame,
                    frozenSceneFrame ? frozenSceneFrame - 1 : 0, frames,
                    freezeScene ? "SKYRIM_TEMPORAL_FIXTURE_FREEZE_FRAME" : "presentation_index",ReplayPixels()?"true":"false");
                fclose(proof);
            }
        }
    }
    wchar_t corePath[MAX_PATH]{}, readyName[128]{};
    GetEnvironmentVariableW(L"SKYRIM_PROBE_CORE_PATH", corePath, MAX_PATH);
    swprintf(readyName, 128, wcsstr(corePath,L"DXL-core.dll") ? L"Local\\DXL.Ready.%lu" : L"Local\\Dlss5Quick.Ready.%lu", GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
    HMODULE probe = LoadLibraryW(L"bink2w64.dll");
    if (!probe) { printf("probe load failed %lu\n", GetLastError()); if (ready) CloseHandle(ready); return 3; }
    char coreMode[8]{};
    GetEnvironmentVariableA("SKYRIM_PROBE_CORE", coreMode, sizeof(coreMode));
    if (coreMode[0] == '1' && ready) printf("core ready wait=%lu\n", WaitForSingleObject(ready,5000));
    int result = 0;
    {
        Fixture fixture;
        HRESULT hr = fixture.Setup();
        printf("temporal fixture create hr=%08lx frames=%u size=%ux%u\n", hr, frames, kWidth, kHeight);
        if (FAILED(hr)) result = 1;
        else {
            UINT completed = 0;
            for (UINT frame = 0; frame < frames; ++frame) {
                MSG message{}; bool quit = false;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) quit = true;
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if (quit || !IsWindow(fixture.window)) break;
                hr = fixture.Draw(freezeScene ? frozenSceneFrame : frame, frame);
                if (FAILED(hr)) { printf("temporal fixture Present hr=%08lx frame=%u\n", hr, frame); result = 2; break; }
                ++completed;
                Sleep(16);
            }
            printf("temporal fixture completed %u presents\n", completed);
        }
    }
    if (ready) CloseHandle(ready);
    // The proxy owns asynchronous hooks/threads and intentionally remains
    // loaded until process exit, as in the baseline fixture.
    return result;
}
