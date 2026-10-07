// Independent NVSDK_NGX_Parameter ABI proof. No SDK, Windows, or STL headers.
// Compile with -DABI_LAYOUT_ONLY -target x86_64-windows-msvc -S -emit-llvm
// to inspect Microsoft overload ordering; compile without the define for a
// runnable GNU-target check of the temporal shim's typed raw forwarding.
struct ID3D11Resource;
struct ID3D12Resource;
using Result = unsigned int;
using U64 = unsigned long long;
static_assert(sizeof(void*) == 8, "This fixture is for Windows x64 only");
static_assert(sizeof(unsigned int) == 4 && sizeof(int) == 4, "32-bit integers");

// Exact declaration order from NVIDIA nvsdk_ngx_params.h. There is no virtual
// destructor. Microsoft reverses overloads within each same-name group.
struct OfficialParameter {
    virtual void Set(const char*, U64) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual Result Get(const char*, U64*) const = 0;
    virtual Result Get(const char*, float*) const = 0;
    virtual Result Get(const char*, double*) const = 0;
    virtual Result Get(const char*, unsigned int*) const = 0;
    virtual Result Get(const char*, int*) const = 0;
    virtual Result Get(const char*, ID3D11Resource**) const = 0;
    virtual Result Get(const char*, ID3D12Resource**) const = 0;
    virtual Result Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct Payload {
    void* raw;
    ID3D12Resource* resource12;
    ID3D11Resource* resource11;
    int integer;
    unsigned int unsignedInteger;
    double real64;
    float real32;
    U64 integer64;
    unsigned int resets;
};

struct LayoutProbe final : OfficialParameter {
    Payload p{};
    void Set(const char*, U64 v) override { p.integer64 = v; }
    void Set(const char*, float v) override { p.real32 = v; }
    void Set(const char*, double v) override { p.real64 = v; }
    void Set(const char*, unsigned int v) override { p.unsignedInteger = v; }
    void Set(const char*, int v) override { p.integer = v; }
    void Set(const char*, ID3D11Resource* v) override { p.resource11 = v; }
    void Set(const char*, ID3D12Resource* v) override { p.resource12 = v; }
    void Set(const char*, void* v) override { p.raw = v; }
    Result Get(const char*, U64* v) const override { *v = p.integer64; return 1; }
    Result Get(const char*, float* v) const override { *v = p.real32; return 1; }
    Result Get(const char*, double* v) const override { *v = p.real64; return 1; }
    Result Get(const char*, unsigned int* v) const override { *v = p.unsignedInteger; return 1; }
    Result Get(const char*, int* v) const override { *v = p.integer; return 1; }
    Result Get(const char*, ID3D11Resource** v) const override { *v = p.resource11; return 1; }
    Result Get(const char*, ID3D12Resource** v) const override { *v = p.resource12; return 1; }
    Result Get(const char*, void** v) const override { *v = p.raw; return 1; }
    void Reset() override { ++p.resets; }
};
LayoutProbe layoutProbe;
extern "C" OfficialParameter* GetLayoutProbe() { return &layoutProbe; }

#ifndef ABI_LAYOUT_ONLY
extern "C" int printf(const char*, ...);
struct RawBag { void** table; Payload p{}; };
struct ParameterView { void** table; const void* original; };

// Expected Microsoft slots, backed by explicitly typed mock operations.
#define BASE_SET(N, T, FIELD) void baselineSet##N(RawBag* b, const char*, T v) { b->p.FIELD = v; }
BASE_SET(0, void*, raw)
BASE_SET(1, ID3D12Resource*, resource12)
BASE_SET(2, ID3D11Resource*, resource11)
BASE_SET(3, int, integer)
BASE_SET(4, unsigned int, unsignedInteger)
BASE_SET(5, double, real64)
BASE_SET(6, float, real32)
BASE_SET(7, U64, integer64)
#define BASE_GET(N, T, FIELD) Result baselineGet##N(const RawBag* b, const char*, T* v) { *v = b->p.FIELD; return 1; }
BASE_GET(8, void*, raw)
BASE_GET(9, ID3D12Resource*, resource12)
BASE_GET(10, ID3D11Resource*, resource11)
BASE_GET(11, int, integer)
BASE_GET(12, unsigned int, unsignedInteger)
BASE_GET(13, double, real64)
BASE_GET(14, float, real32)
BASE_GET(15, U64, integer64)
void baselineReset(RawBag* b) { ++b->p.resets; }
void* baselineTable[] = {
    (void*)baselineSet0,(void*)baselineSet1,(void*)baselineSet2,(void*)baselineSet3,
    (void*)baselineSet4,(void*)baselineSet5,(void*)baselineSet6,(void*)baselineSet7,
    (void*)baselineGet8,(void*)baselineGet9,(void*)baselineGet10,(void*)baselineGet11,
    (void*)baselineGet12,(void*)baselineGet13,(void*)baselineGet14,(void*)baselineGet15,
    (void*)baselineReset
};

// Mirrors temporal.cpp's forwarding ABI: typed Set preserves XMM arguments;
// every Get's output is a pointer, so one raw forwarding prototype is valid.
#define FORWARD_SET(N, T) void forwardSet##N(ParameterView* p, const char* k, T v) { \
    ((void(*)(const void*, const char*, T))(*(void***)p->original)[N])(p->original,k,v); }
FORWARD_SET(0,void*)
FORWARD_SET(1,ID3D12Resource*)
FORWARD_SET(2,ID3D11Resource*)
FORWARD_SET(3,int)
FORWARD_SET(4,unsigned int)
FORWARD_SET(5,double)
FORWARD_SET(6,float)
FORWARD_SET(7,U64)
using GetFn = Result(*)(const void*, const char*, void*);
#define FORWARD_GET(N) Result forwardGet##N(const ParameterView* p, const char* k, void* v) { \
    return ((GetFn)(*(void***)p->original)[N])(p->original,k,v); }
FORWARD_GET(8) FORWARD_GET(9) FORWARD_GET(10) FORWARD_GET(11)
FORWARD_GET(12) FORWARD_GET(13) FORWARD_GET(14) FORWARD_GET(15)
void forwardReset(ParameterView* p) { ((void(*)(const void*))(*(void***)p->original)[16])(p->original); }
void* forwardingTable[] = {
    (void*)forwardSet0,(void*)forwardSet1,(void*)forwardSet2,(void*)forwardSet3,
    (void*)forwardSet4,(void*)forwardSet5,(void*)forwardSet6,(void*)forwardSet7,
    (void*)forwardGet8,(void*)forwardGet9,(void*)forwardGet10,(void*)forwardGet11,
    (void*)forwardGet12,(void*)forwardGet13,(void*)forwardGet14,(void*)forwardGet15,
    (void*)forwardReset
};

template<class T> bool Check(ParameterView& view, unsigned int setSlot,
                             unsigned int getSlot, T input, const char* type) {
    ((void(*)(ParameterView*,const char*,T))view.table[setSlot])(&view,"fixture",input);
    T output{};
    auto result = ((Result(*)(const ParameterView*,const char*,T*))view.table[getSlot])(&view,"fixture",&output);
    bool okay = result == 1 && output == input;
    printf("%s Set[%u] -> Get[%u]: %s\n",type,setSlot,getSlot,okay?"PASS":"FAIL");
    return okay;
}

int main() {
    RawBag baseline{baselineTable}; ParameterView view{forwardingTable,&baseline};
    int anchor0=0,anchor1=0,anchor2=0;
    bool okay=true;
    okay &= Check(view,0,8,(void*)&anchor0,"void*");
    okay &= Check(view,1,9,(ID3D12Resource*)&anchor1,"ID3D12Resource*");
    okay &= Check(view,2,10,(ID3D11Resource*)&anchor2,"ID3D11Resource*");
    okay &= Check(view,3,11,-2147483647,"int");
    okay &= Check(view,4,12,4294967293U,"unsigned int");
    okay &= Check(view,5,13,-123456789.125,"double");
    okay &= Check(view,6,14,1.234567f,"float");
    okay &= Check(view,7,15,0xfedcba9876543210ULL,"unsigned long long");
    ((void(*)(ParameterView*))view.table[16])(&view);
    okay &= baseline.p.resets==1;
    printf("Reset[16]: %s\n",baseline.p.resets==1?"PASS":"FAIL");
    printf("Windows x64 raw parameter forwarding: %s\n",okay?"PASS":"FAIL");
    return okay?0:1;
}
#endif
