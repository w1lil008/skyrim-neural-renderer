#pragma once
#include <d3d11.h>
#include <dxgi.h>
using TemporalLog = void(*)(const char*, ...);
void TemporalConfigure(const char* output, TemporalLog log);
bool TemporalEnabled();
void TemporalAttach(ID3D11Device*, ID3D11DeviceContext*, IDXGISwapChain*);
void TemporalInstallNeuralHook();
