/*
 * RADV (Mesa's Vulkan driver for AMD GPUs) on ReactOS, through libdrm-ros and the Linux amdgpu driver in the kernel.
 *
 * Uses every AMD GPU RADV finds: creates a device, compiles a compute shader (ACO), runs it to write 16384 values
 * and checks every one. Without an AMD GPU (QEMU) RADV must find none: the log shows the DRM devices it was offered
 * (libdrm-ros, LIBGL_DEBUG=verbose) and RADV's own startup messages.
 * Needs vulkan-1.dll, vulkan_radeon.dll and radeon_icd.x86_64.json next to it. Prints "RADVTEST: ... PASS" or "FAIL".
 * Build: x86_64-w64-mingw32-gcc -O1 -I<Vulkan-Headers>/include radvtest.c -o radvtest.exe
 *
 * The shader (GLSL, compiled with glslangValidator -V --target-env vulkan1.0):
 *   #version 450
 *   layout(local_size_x = 64) in;
 *   layout(std430, set = 0, binding = 0) buffer Out { uint v[]; };
 *   void main() { uint i = gl_GlobalInvocationID.x; v[i] = i * 3u + 7u; }
 */
#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <io.h>
#include <fcntl.h>

static const uint32_t fill_spv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000021,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0006000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x0000000b,0x00060010,0x00000004,
    0x00000011,0x00000040,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,0x00040005,
    0x00000004,0x6e69616d,0x00000000,0x00030005,0x00000008,0x00000069,0x00080005,0x0000000b,
    0x475f6c67,0x61626f6c,0x766e496c,0x7461636f,0x496e6f69,0x00000044,0x00030005,0x00000011,
    0x0074754f,0x00040006,0x00000011,0x00000000,0x00000076,0x00030005,0x00000013,0x00000000,
    0x00040047,0x0000000b,0x0000000b,0x0000001c,0x00040047,0x00000010,0x00000006,0x00000004,
    0x00030047,0x00000011,0x00000003,0x00050048,0x00000011,0x00000000,0x00000023,0x00000000,
    0x00040047,0x00000013,0x00000021,0x00000000,0x00040047,0x00000013,0x00000022,0x00000000,
    0x00040047,0x00000020,0x0000000b,0x00000019,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x00040020,0x00000007,0x00000007,
    0x00000006,0x00040017,0x00000009,0x00000006,0x00000003,0x00040020,0x0000000a,0x00000001,
    0x00000009,0x0004003b,0x0000000a,0x0000000b,0x00000001,0x0004002b,0x00000006,0x0000000c,
    0x00000000,0x00040020,0x0000000d,0x00000001,0x00000006,0x0003001d,0x00000010,0x00000006,
    0x0003001e,0x00000011,0x00000010,0x00040020,0x00000012,0x00000002,0x00000011,0x0004003b,
    0x00000012,0x00000013,0x00000002,0x00040015,0x00000014,0x00000020,0x00000001,0x0004002b,
    0x00000014,0x00000015,0x00000000,0x0004002b,0x00000006,0x00000018,0x00000003,0x0004002b,
    0x00000006,0x0000001a,0x00000007,0x00040020,0x0000001c,0x00000002,0x00000006,0x0004002b,
    0x00000006,0x0000001e,0x00000040,0x0004002b,0x00000006,0x0000001f,0x00000001,0x0006002c,
    0x00000009,0x00000020,0x0000001e,0x0000001f,0x0000001f,0x00050036,0x00000002,0x00000004,
    0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000007,0x00000008,0x00000007,
    0x00050041,0x0000000d,0x0000000e,0x0000000b,0x0000000c,0x0004003d,0x00000006,0x0000000f,
    0x0000000e,0x0003003e,0x00000008,0x0000000f,0x0004003d,0x00000006,0x00000016,0x00000008,
    0x0004003d,0x00000006,0x00000017,0x00000008,0x00050084,0x00000006,0x00000019,0x00000017,
    0x00000018,0x00050080,0x00000006,0x0000001b,0x00000019,0x0000001a,0x00060041,0x0000001c,
    0x0000001d,0x00000013,0x00000015,0x00000016,0x0003003e,0x0000001d,0x0000001b,0x000100fd,
    0x00010038
};

#define N 16384

static void L(const char *fmt, ...)
{
    char b[600];
    va_list a;

    va_start(a, fmt);
    vsnprintf(b, sizeof(b) - 2, fmt, a);
    va_end(a);
    strcat(b, "\n");
    OutputDebugStringA(b);
    fputs(b, stdout);
    fflush(stdout);
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
#define IF(name) PFN_##name name = (PFN_##name)gipa(inst, #name)

/* RADV writes its startup log to stderr: send stderr into a pipe (the live CD has no writable disk) and echo what
 * arrives to the debug output */
static HANDLE log_rd;

static void capture_stderr(void)
{
    HANDLE wr;
    int fd;

    if (!CreatePipe(&log_rd, &wr, NULL, 1 << 20))
        return;
    fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
    if (fd < 0 || _dup2(fd, 2) < 0)
        return;
    setvbuf(stderr, NULL, _IONBF, 0);
}

static void echo_log(void)
{
    static char pending[4096];
    static size_t used;
    char buf[2048];
    DWORD avail = 0, got;
    size_t i;

    fflush(stderr);
    while (log_rd && PeekNamedPipe(log_rd, NULL, 0, NULL, &avail, NULL) && avail &&
           ReadFile(log_rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &got, NULL) && got)
    {
        for (i = 0; i < got; i++)
        {
            if (buf[i] == '\n' || used == sizeof(pending) - 1)
            {
                pending[used] = 0;
                if (used && pending[used - 1] == '\r')
                    pending[used - 1] = 0;
                if (pending[0])
                    L("RADVTEST:   | %s", pending);
                used = 0;
            }
            else
                pending[used++] = buf[i];
        }
    }
}

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { L("RADVTEST: FAIL %s -> %d", #x, r_); return 1; } } while (0)

static int run_device(VkPhysicalDevice pd)
{
    IF(vkGetPhysicalDeviceQueueFamilyProperties); IF(vkGetPhysicalDeviceMemoryProperties); IF(vkCreateDevice);
    IF(vkGetDeviceProcAddr);
    VkQueueFamilyProperties qf[8];
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t nqf = 8, q = ~0u, i, mt = ~0u, bad = 0;
    float prio = 1.0f;
    VkDevice dev;

    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
    for (i = 0; i < nqf && q == ~0u; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            q = i;
    if (q == ~0u)
    {
        L("RADVTEST: FAIL no compute queue");
        return 1;
    }
    {
        VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, q, 1, &prio };
        VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci };

        VKCHECK(vkCreateDevice(pd, &dci, NULL, &dev));
        L("RADVTEST: ok   vkCreateDevice (queue family %u)", q);
    }
#define DF(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DF(vkCreateBuffer); DF(vkGetBufferMemoryRequirements); DF(vkAllocateMemory); DF(vkBindBufferMemory); DF(vkMapMemory);
    DF(vkCreateDescriptorSetLayout); DF(vkCreatePipelineLayout); DF(vkCreateShaderModule); DF(vkCreateComputePipelines);
    DF(vkCreateDescriptorPool); DF(vkAllocateDescriptorSets); DF(vkUpdateDescriptorSets); DF(vkCreateCommandPool);
    DF(vkAllocateCommandBuffers); DF(vkBeginCommandBuffer); DF(vkCmdBindPipeline); DF(vkCmdBindDescriptorSets);
    DF(vkCmdDispatch); DF(vkEndCommandBuffer); DF(vkCreateFence); DF(vkQueueSubmit); DF(vkWaitForFences);
    DF(vkGetDeviceQueue); DF(vkDestroyDevice); DF(vkDeviceWaitIdle); DF(vkCmdPipelineBarrier);

    /* the pipeline: ACO compiles the shader for the GPU */
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkShaderModule sm;
    VkPipeline pipe;
    VKCHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl };
    VKCHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fill_spv), fill_spv };
    VKCHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main" }, pl };
    VKCHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    L("RADVTEST: ok   vkCreateComputePipelines: the shader compiled for the GPU (ACO)");

    /* a host-visible buffer, the dispatch, and the check */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer buf;
    VkMemoryRequirements mr;
    VkDeviceMemory mem;
    uint32_t *p;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (i = 0; i < mp.memoryTypeCount && mt == ~0u; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            mt = i;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
    VKCHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));
    VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    memset(p, 0xff, N * 4);
    L("RADVTEST: ok   host-visible buffer mapped at %p (memory type %u)", p, mt);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    VkDescriptorSet ds;
    VKCHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VKCHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               NULL, &dbi };
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, q };
    VkCommandPool pool;
    VkCommandBuffer cb;
    VKCHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VKCHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VKCHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VkQueue queue;
    VKCHECK(vkCreateFence(dev, &fci, NULL, &fence));
    vkGetDeviceQueue(dev, q, 0, &queue);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb };
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
    for (i = 0; i < N; i++)
        bad += p[i] != i * 3u + 7u;
    L("RADVTEST: %s the GPU ran the shader: %u of %u values wrong (first: %u %u %u)", bad ? "FAIL" : "ok  ", bad, N,
      p[0], p[1], p[N - 1]);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    return bad != 0;
}

int main(void)
{
    char dir[MAX_PATH], path[MAX_PATH + 64], *slash;
    int failures = 0;
    HMODULE m;
    uint32_t n = 8, i;
    VkPhysicalDevice pd[8];
    VkResult r;

    GetModuleFileNameA(NULL, dir, MAX_PATH);
    slash = strrchr(dir, '\\');
    if (slash)
        *slash = 0;
    snprintf(path, sizeof(path), "VK_DRIVER_FILES=%s\\radeon_icd.x86_64.json", dir);
    _putenv(path);
    _putenv("VK_LOADER_NO_DXGI=1");
    _putenv("RADV_DEBUG=startup");
    _putenv("LIBGL_DEBUG=verbose");        /* libdrm-ros lists the devices it finds */
    capture_stderr();

    m = LoadLibraryA("vulkan-1.dll");
    gipa = m ? (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr") : NULL;
    if (!gipa)
    {
        L("RADVTEST: FAIL cannot load vulkan-1.dll (%lu)", GetLastError());
        return 1;
    }
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "radvtest", 1, "none", 1, VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
        PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");

        r = vkCreateInstance(&ici, NULL, &inst);
        if (r != VK_SUCCESS)
        {
            echo_log();
            L("RADVTEST: FAIL vkCreateInstance -> %d", r);
            return 1;
        }
    }
    IF(vkEnumeratePhysicalDevices); IF(vkGetPhysicalDeviceProperties2); IF(vkDestroyInstance);
    r = vkEnumeratePhysicalDevices(inst, &n, pd);
    echo_log();
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        n = 0;
    L("RADVTEST: vkEnumeratePhysicalDevices -> %d, %u device(s)", r, n);
    for (i = 0; i < n; i++)
    {
        VkPhysicalDeviceDriverProperties drv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv };

        vkGetPhysicalDeviceProperties2(pd[i], &p2);
        L("RADVTEST:   device %u: '%s' %04x:%04x api %u.%u.%u, driver '%s' '%s'", i, p2.properties.deviceName,
          p2.properties.vendorID, p2.properties.deviceID, VK_VERSION_MAJOR(p2.properties.apiVersion),
          VK_VERSION_MINOR(p2.properties.apiVersion), VK_VERSION_PATCH(p2.properties.apiVersion), drv.driverName,
          drv.driverInfo);
        failures += run_device(pd[i]);
        echo_log();
    }
    vkDestroyInstance(inst, NULL);
    if (!n)
        L("RADVTEST: no AMD GPU (expected without one, e.g. in QEMU)");
    L("RADVTEST: %s", failures ? "FAIL" : "PASS");
    return failures != 0;
}
