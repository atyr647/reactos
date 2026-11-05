/* D3D11 test: window + swapchain + clear/present. Loads d3d11.dll from the exe directory (DXVK). */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>

static void L(const char *fmt, ...)
{
    char b[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    strcat(b, "\n"); OutputDebugStringA(b);
}
static LONG WINAPI Veh(PEXCEPTION_POINTERS p)
{
    static volatile LONG busy;
    HMODULE m = NULL; char name[260] = "?", b[600]; DWORD w; HANDLE f; int n;
    if (p->ExceptionRecord->ExceptionCode == 0x40010006 || p->ExceptionRecord->ExceptionCode == 0x4001000A) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, p->ExceptionRecord->ExceptionAddress, &m);
    if (m) GetModuleFileNameA(m, name, sizeof name);
    n = snprintf(b, sizeof b, "VEH tid %lu code %08lx at %p (%s +%llx) info %llx %llx\r\n", GetCurrentThreadId(), p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress, name,
      (unsigned long long)((char *)p->ExceptionRecord->ExceptionAddress - (char *)m), (unsigned long long)p->ExceptionRecord->ExceptionInformation[0], (unsigned long long)p->ExceptionRecord->ExceptionInformation[1]);
    f = CreateFileA("C:\\veh.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, b, n, &w, 0); CloseHandle(f); }
    InterlockedExchange(&busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI Uef(PEXCEPTION_POINTERS p)
{
    HMODULE m = NULL; char name[260] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, p->ExceptionRecord->ExceptionAddress, &m);
    if (m) GetModuleFileNameA(m, name, sizeof name);
    L("D3D11T: CRASH %08lx at %p (%s +%llx)", p->ExceptionRecord->ExceptionCode, p->ExceptionRecord->ExceptionAddress, name,
      (unsigned long long)((char *)p->ExceptionRecord->ExceptionAddress - (char *)m));
    return EXCEPTION_EXECUTE_HANDLER;
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}
#define HR(x) do { HRESULT r_ = (x); L("D3D11T: %s -> %08lx", #x, r_); if (FAILED(r_)) return 1; } while (0)

int main(int argc, char **argv)
{
    WNDCLASSA wc = { 0 }; HWND hwnd; MSG msg;
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = 0, want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ID3D11Texture2D *bb = NULL; ID3D11RenderTargetView *rtv = NULL;
    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; int use_tri = 0;
    int frames = argc > 1 ? atoi(argv[1]) : 200, i;
    HMODULE d3d, dxgi;
    HRESULT (WINAPI *create)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                             const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    char path[260];

    SetUnhandledExceptionFilter(Uef);
    AddVectoredExceptionHandler(1, Veh);
    SetEnvironmentVariableA("VK_LOADER_NO_DXGI", "1"); _putenv("VK_LOADER_NO_DXGI=1");
    if (!getenv("VK_DRIVER_FILES")) SetEnvironmentVariableA("VK_DRIVER_FILES", "C:\\lvp_icd.x86_64.json");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info");
    L("D3D11T: start");
    dxgi = LoadLibraryA("dxgi.dll"); GetModuleFileNameA(dxgi, path, sizeof path); L("D3D11T: dxgi.dll = %p %s", dxgi, path);
    d3d = LoadLibraryA("d3d11.dll"); GetModuleFileNameA(d3d, path, sizeof path); L("D3D11T: d3d11.dll = %p %s err %lu", d3d, path, GetLastError());
    if (!d3d) return 2;
    create = (void *)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain");
    L("D3D11T: D3D11CreateDeviceAndSwapChain = %p", create);
    if (!create) return 3;

    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "d3d11t"; wc.hCursor = LoadCursorA(0, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d11t", "d3d11 (DXVK + lavapipe on ReactOS)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 416, 338, 0, 0, wc.hInstance, 0);
    sd.BufferDesc.Width = 400; sd.BufferDesc.Height = 300; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.OutputWindow = hwnd; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    HR(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx));
    L("D3D11T: feature level %x", fl);
    HR(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb));
    HR(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv));
    {
        static const char vs_src[] = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p[3] = { float2(0.0, 0.7), float2(0.7, -0.7), float2(-0.7, -0.7) }; return float4(p[id], 0.0, 1.0); }";
        static const char ps_src[] = "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.x / 400.0, p.y / 300.0, 1.0 - p.x / 400.0, 1.0); }";
        static const char ps_tex[] = "Texture2D t : register(t0); SamplerState s : register(s0); float4 main(float4 p : SV_Position) : SV_Target { return t.Sample(s, p.xy / float2(400.0, 300.0) * 4.0); }";
        int tex = getenv("TEX") != NULL;
        HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
        HRESULT (WINAPI *dc)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
        ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
        L("D3D11T: d3dcompiler_47 = %p", comp);
        use_tri = 0;
        if (comp && (dc = (void *)GetProcAddress(comp, "D3DCompile")))
        {
            HRESULT r1 = dc(vs_src, sizeof vs_src - 1, "vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err);
            if (FAILED(r1) && err) L("D3D11T: vs compile: %s", (char *)ID3D10Blob_GetBufferPointer(err));
            HRESULT r2 = dc(tex ? ps_tex : ps_src, tex ? sizeof ps_tex - 1 : sizeof ps_src - 1, "ps", NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err);
            if (FAILED(r2) && err) L("D3D11T: ps compile: %s", (char *)ID3D10Blob_GetBufferPointer(err));
            L("D3D11T: D3DCompile vs %08lx ps %08lx", r1, r2);
            if (SUCCEEDED(r1) && SUCCEEDED(r2))
            {
                HRESULT a = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
                HRESULT b = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
                L("D3D11T: CreateVertexShader %08lx CreatePixelShader %08lx", a, b);
                use_tri = SUCCEEDED(a) && SUCCEEDED(b);
                if (tex)
                {
                    UINT px[16 * 16]; int x, y;
                    D3D11_TEXTURE2D_DESC td = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE };
                    D3D11_SUBRESOURCE_DATA sr = { px, 16 * 4, 0 };
                    D3D11_SAMPLER_DESC smd = { D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP, 0, 1, D3D11_COMPARISON_NEVER, { 0, 0, 0, 0 }, 0, D3D11_FLOAT32_MAX };
                    ID3D11Texture2D *t2 = NULL; ID3D11ShaderResourceView *srv = NULL; ID3D11SamplerState *ss = NULL;
                    for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) px[y * 16 + x] = ((x ^ y) & 4) ? 0xFF20A0FFu : 0xFFFFC020u;
                    L("D3D11T: CreateTexture2D %08lx", ID3D11Device_CreateTexture2D(dev, &td, &sr, &t2));
                    L("D3D11T: CreateSRV %08lx", ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t2, NULL, &srv));
                    L("D3D11T: CreateSampler %08lx", ID3D11Device_CreateSamplerState(dev, &smd, &ss));
                    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
                    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &ss);
                }
            }
        }
    }
    for (i = 0; i < frames; i++)
    {
        float col[4] = { (i % 60) / 60.f, 0.6f, 1.f - (i % 60) / 60.f, 1.f };
        HRESULT r;
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        if (use_tri)
        {
            D3D11_VIEWPORT vp = { 0, 0, 400, 300, 0, 1 };
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        r = IDXGISwapChain_Present(sc, 1, 0);
        if (i < 3 || FAILED(r)) L("D3D11T: frame %d present -> %08lx", i, r);
    }
    L("D3D11T: PASS %d frames", frames);
    if (argc > 2) while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
