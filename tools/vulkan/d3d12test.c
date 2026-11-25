/* D3D12 test: vkd3d-proton d3d12.dll + DXVK dxgi.dll (from the exe directory) on lavapipe. Clear + present. */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdarg.h>

static void L(const char *fmt, ...)
{
    char b[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b - 2, fmt, ap); va_end(ap);
    strcat(b, "\n"); OutputDebugStringA(b);
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}
#define HR(x) do { HRESULT r_ = (x); L("D3D12T: %s -> %08lx", #x, r_); if (FAILED(r_)) return 1; } while (0)
#define FRAMES 2

int main(int argc, char **argv)
{
    WNDCLASSA wc = { 0 }; HWND hwnd; MSG msg;
    HMODULE d3d12 = NULL, dxgi = NULL;
    HRESULT (WINAPI *create_device)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HRESULT (WINAPI *create_factory)(REFIID, void **);
    ID3D12Device *dev = NULL; ID3D12CommandQueue *q = NULL; IDXGIFactory4 *fac = NULL; IDXGISwapChain1 *sc1 = NULL;
    IDXGISwapChain3 *sc = NULL; ID3D12DescriptorHeap *heap = NULL; ID3D12Resource *bb[FRAMES] = { 0 };
    ID3D12CommandAllocator *alloc = NULL; ID3D12GraphicsCommandList *cl = NULL; ID3D12Fence *fence = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAMES };
    DXGI_SWAP_CHAIN_DESC1 sd = { 400, 300, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, FRAMES, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD };
    UINT rtv_size, i; UINT64 fv = 0; HANDLE ev;
    int frames = argc > 1 ? atoi(argv[1]) : 200, f;
    char path[260];

    SetEnvironmentVariableA("VK_LOADER_NO_DXGI", "1"); _putenv("VK_LOADER_NO_DXGI=1");
    if (!getenv("VK_DRIVER_FILES")) { SetEnvironmentVariableA("VK_DRIVER_FILES", "C:\\lvp_icd.x86_64.json"); _putenv("VK_DRIVER_FILES=C:\\lvp_icd.x86_64.json"); }
    L("D3D12T: start");
    dxgi = LoadLibraryA("dxgi.dll"); GetModuleFileNameA(dxgi, path, sizeof path); L("D3D12T: dxgi.dll = %p %s", dxgi, path);
    d3d12 = LoadLibraryA("d3d12.dll"); GetModuleFileNameA(d3d12, path, sizeof path); L("D3D12T: d3d12.dll = %p %s err %lu", d3d12, path, GetLastError());
    if (!d3d12 || !dxgi) return 2;
    create_device = (void *)GetProcAddress(d3d12, "D3D12CreateDevice");
    create_factory = (void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    L("D3D12T: D3D12CreateDevice %p CreateDXGIFactory1 %p", create_device, create_factory);
    if (!create_device || !create_factory) return 3;

    HR(create_device(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    HR(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    HR(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);

    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "d3d12t"; wc.hCursor = LoadCursorA(0, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowA("d3d12t", "d3d12 (vkd3d-proton + DXVK + lavapipe on ReactOS)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 416, 338, 0, 0, wc.hInstance, 0);
    HR(create_factory(&IID_IDXGIFactory4, (void **)&fac));
    HR(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
    HR(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
    HR(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h);
        for (i = 0; i < FRAMES; i++)
        {
            HR(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bb[i]));
            ID3D12Device_CreateRenderTargetView(dev, bb[i], NULL, h);
            h.ptr += rtv_size;
        }
    }
    HR(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    HR(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    HR(ID3D12GraphicsCommandList_Close(cl));

    for (f = 0; f < frames; f++)
    {
        UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        float col[4] = { (f % 60) / 60.f, 0.7f, 1.f - (f % 60) / 60.f, 1.f };
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        ID3D12CommandList *lists[1];
        HRESULT r;

        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        r = ID3D12CommandAllocator_Reset(alloc); if (FAILED(r)) { L("D3D12T: alloc reset %08lx", r); return 1; }
        r = ID3D12GraphicsCommandList_Reset(cl, alloc, NULL); if (FAILED(r)) { L("D3D12T: list reset %08lx", r); return 1; }
        b.Transition.pResource = bb[idx]; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &h); h.ptr += idx * rtv_size;
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, col, 0, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        r = ID3D12GraphicsCommandList_Close(cl); if (FAILED(r)) { L("D3D12T: close %08lx", r); return 1; }
        lists[0] = (ID3D12CommandList *)cl;
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, lists);
        r = IDXGISwapChain3_Present(sc, 1, 0);
        if (f < 3 || FAILED(r)) L("D3D12T: frame %d present -> %08lx", f, r);
        ID3D12CommandQueue_Signal(q, fence, ++fv);
        if (ID3D12Fence_GetCompletedValue(fence) < fv) { ID3D12Fence_SetEventOnCompletion(fence, fv, ev); WaitForSingleObject(ev, 10000); }
    }
    L("D3D12T: PASS %d frames", frames);
    if (argc > 2) while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
