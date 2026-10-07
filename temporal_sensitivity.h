#pragma once
#include <d3d11.h>
#include <d3d12.h>
#include <stdint.h>

namespace TemporalSensitivity {
using Log = void(*)(const char*, ...);
void Configure(const char* directory, Log);
bool Enabled();
bool ForceReset();
// Fixture-only diagnostic gap: start is inclusive, end is exclusive.
// Returns true when the optional gap is absent or sensitivity is disabled.
bool AllowNativeInputs(uint64_t copySerial);
// Synthetic fixture only. True selects depth (R32_FLOAT), false selects motion
// (R16G16_FLOAT). Failure returns nativeResource and is explicitly logged.
ID3D12Resource* Override(ID3D12Device*, ID3D12GraphicsCommandList*, bool depth,
                        UINT width, UINT height, ID3D12Resource* nativeResource);
// Call after the original D3D11 copy. Supply the SAME bridge serial to a frame's
// Color11 copy-in and Out11 copy-out. Queries/maps never wait for GPU completion.
void AfterCopy(ID3D11DeviceContext*, ID3D11Resource* destination,
               ID3D11Resource* source, uint64_t bridgeSerial, uint64_t taaSerial,
               uint64_t nrCallIndex, uint64_t nativeEpochFirstCall, bool effectiveReset);
}
