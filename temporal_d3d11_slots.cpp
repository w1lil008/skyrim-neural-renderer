#define CINTERFACE
#include <d3d11_1.h>
#include <stddef.h>

// Derive the method address from the SDK's actual interface layout. Native
// hooks continue to work after the context restores its internal state object.
static_assert(offsetof(ID3D11DeviceContext1Vtbl,SwapDeviceContextState)/sizeof(void*)==131,"SDK state swap slot");
extern "C" void* TemporalSwapStateTarget(ID3D11DeviceContext1* context){
    return context?(void*)context->lpVtbl->SwapDeviceContextState:nullptr;
}
