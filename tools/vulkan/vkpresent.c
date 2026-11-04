/* Win32 WSI test: window + swapchain + clear colour animation, logging to the debug port. */
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <vulkan/vulkan.h>

static void L(const char *fmt, ...)
{
    char b[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    strcat(b, "\n"); OutputDebugStringA(b); fputs(b, stdout); fflush(stdout);
}
#define CK(x) do { VkResult r_ = (x); L("VKPRES: %s -> %d", #x, r_); if (r_ < 0) return 1; } while (0)
#define W 400
#define H 300

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv)
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    const char *iexts[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vkpresent", 1, 0, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iexts };
    VkInstance inst; VkPhysicalDevice pd; uint32_t n = 1;
    WNDCLASSA wc = { 0 }; HWND hwnd; VkSurfaceKHR surf;
    VkBool32 supported = 0; VkSurfaceCapabilitiesKHR caps;
    float prio = 1.f; const char *dexts[] = { "VK_KHR_swapchain" };
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 1, dexts };
    VkDevice dev; VkQueue q; VkSwapchainKHR sc; VkImage imgs[8]; uint32_t nimg = 8;
    uint32_t nfmt = 1; VkSurfaceFormatKHR fmt;
    VkCommandPool pool; VkCommandBuffer cb; VkSemaphore sem_acq, sem_rel; VkFence fence;
    int frame, frames = argc > 1 ? atoi(argv[1]) : 120;
    MSG msg;

    SetEnvironmentVariableA("VK_LOADER_NO_DXGI", "1");
    if (!getenv("VK_DRIVER_FILES")) SetEnvironmentVariableA("VK_DRIVER_FILES", "C:\\lvp_icd.x86_64.json");
    L("VKPRES: start, vulkan-1=%p", vk);
    CK(((PFN_vkCreateInstance)gipa(0, "vkCreateInstance"))(&ici, 0, &inst));
#define IP(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    IP(vkEnumeratePhysicalDevices); IP(vkCreateWin32SurfaceKHR); IP(vkGetPhysicalDeviceSurfaceSupportKHR);
    IP(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); IP(vkGetPhysicalDeviceSurfaceFormatsKHR); IP(vkCreateDevice);
    IP(vkGetDeviceProcAddr);
    CK(vkEnumeratePhysicalDevices(inst, &n, &pd));

    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(0); wc.lpszClassName = "vkpresent";
    wc.hCursor = LoadCursorA(0, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowA("vkpresent", "vkpresent (lavapipe on ReactOS)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, W + 16, H + 38, 0, 0, wc.hInstance, 0);
    L("VKPRES: window %p", hwnd);
    {
        VkWin32SurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, wc.hInstance, hwnd };
        CK(vkCreateWin32SurfaceKHR(inst, &si, 0, &surf));
    }
    CK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &supported)); L("VKPRES: surface support %u", supported);
    CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    L("VKPRES: caps images %u..%u extent %ux%u usage %x", caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height, caps.supportedUsageFlags);
    CK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nfmt, &fmt)); L("VKPRES: format %d", fmt.format);
    CK(vkCreateDevice(pd, &dci, 0, &dev));
    {
        PFN_vkGetDeviceQueue gdq = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(dev, "vkGetDeviceQueue");
        PFN_vkCreateSwapchainKHR csc = (PFN_vkCreateSwapchainKHR)vkGetDeviceProcAddr(dev, "vkCreateSwapchainKHR");
        PFN_vkGetSwapchainImagesKHR gsi = (PFN_vkGetSwapchainImagesKHR)vkGetDeviceProcAddr(dev, "vkGetSwapchainImagesKHR");
        PFN_vkAcquireNextImageKHR acq = (PFN_vkAcquireNextImageKHR)vkGetDeviceProcAddr(dev, "vkAcquireNextImageKHR");
        PFN_vkQueuePresentKHR pres = (PFN_vkQueuePresentKHR)vkGetDeviceProcAddr(dev, "vkQueuePresentKHR");
        PFN_vkCreateCommandPool ccp = (PFN_vkCreateCommandPool)vkGetDeviceProcAddr(dev, "vkCreateCommandPool");
        PFN_vkAllocateCommandBuffers acb = (PFN_vkAllocateCommandBuffers)vkGetDeviceProcAddr(dev, "vkAllocateCommandBuffers");
        PFN_vkBeginCommandBuffer bcb = (PFN_vkBeginCommandBuffer)vkGetDeviceProcAddr(dev, "vkBeginCommandBuffer");
        PFN_vkEndCommandBuffer ecb = (PFN_vkEndCommandBuffer)vkGetDeviceProcAddr(dev, "vkEndCommandBuffer");
        PFN_vkCmdClearColorImage clr = (PFN_vkCmdClearColorImage)vkGetDeviceProcAddr(dev, "vkCmdClearColorImage");
        PFN_vkCmdPipelineBarrier bar = (PFN_vkCmdPipelineBarrier)vkGetDeviceProcAddr(dev, "vkCmdPipelineBarrier");
        PFN_vkQueueSubmit sub = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(dev, "vkQueueSubmit");
        PFN_vkCreateSemaphore csem = (PFN_vkCreateSemaphore)vkGetDeviceProcAddr(dev, "vkCreateSemaphore");
        PFN_vkCreateFence cf = (PFN_vkCreateFence)vkGetDeviceProcAddr(dev, "vkCreateFence");
        PFN_vkWaitForFences wf = (PFN_vkWaitForFences)vkGetDeviceProcAddr(dev, "vkWaitForFences");
        PFN_vkResetFences rf = (PFN_vkResetFences)vkGetDeviceProcAddr(dev, "vkResetFences");
        PFN_vkResetCommandBuffer rcb = (PFN_vkResetCommandBuffer)vkGetDeviceProcAddr(dev, "vkResetCommandBuffer");
        VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
        VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

        gdq(dev, 0, 0, &q);
        sci.surface = surf; sci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
        sci.imageFormat = fmt.format; sci.imageColorSpace = fmt.colorSpace;
        sci.imageExtent = caps.currentExtent.width != 0xFFFFFFFF ? caps.currentExtent : (VkExtent2D){ W, H };
        sci.imageArrayLayers = 1; sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; sci.clipped = VK_TRUE;
        CK(csc(dev, &sci, 0, &sc));
        CK(gsi(dev, sc, &nimg, imgs)); L("VKPRES: %u swapchain images", nimg);
        CK(ccp(dev, &pci, 0, &pool));
        { VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
          CK(acb(dev, &ai, &cb)); }
        CK(csem(dev, &semi, 0, &sem_acq)); CK(csem(dev, &semi, 0, &sem_rel));
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT; CK(cf(dev, &fi, 0, &fence));

        for (frame = 0; frame < frames; frame++)
        {
            uint32_t idx; VkResult r;
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkClearColorValue col = { { (frame % 60) / 60.f, 0.2f, 1.f - (frame % 60) / 60.f, 1.f } };
            VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &sem_acq, &ws, 1, &cb, 1, &sem_rel };
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &sem_rel, 1, &sc, &idx };
            VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ~0u, ~0u, 0, rng };
            VkImageMemoryBarrier b2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, ~0u, ~0u, 0, rng };

            while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            wf(dev, 1, &fence, VK_TRUE, ~0ull); rf(dev, 1, &fence);
            r = acq(dev, sc, ~0ull, sem_acq, 0, &idx);
            if (frame < 3 || r) L("VKPRES: frame %d acquire -> %d idx %u", frame, r, idx);
            if (r < 0) return 1;
            b1.image = b2.image = imgs[idx];
            rcb(cb, 0); bcb(cb, &bi);
            bar(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
            clr(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
            bar(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b2);
            ecb(cb);
            r = sub(q, 1, &si, fence); if (frame < 3 || r) L("VKPRES: frame %d submit -> %d", frame, r);
            r = pres(q, &pi); if (frame < 3 || r) L("VKPRES: frame %d present -> %d", frame, r);
        }
    }
    L("VKPRES: PASS %d frames", frames);
    if (argc > 2) { while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); } }
    return 0;
}
