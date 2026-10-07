#pragma once
#include <d3d11.h>
bool ProbeSidecar(ID3D11Device*,ID3D11DeviceContext*,ID3D11Texture2D*,const char*,void(*)(const char*,...));
void BootstrapSidecar(ID3D11Device*,void(*)(const char*,...));
void BootstrapSidecarBefore11(IDXGIAdapter*,void(*)(const char*,...));
