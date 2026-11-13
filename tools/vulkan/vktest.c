#include <windows.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

static void L(const char *fmt, ...)
{
    char buf[512]; va_list ap;
    va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    strcat(buf, "\n");
    OutputDebugStringA(buf);
    fputs(buf, stdout); fflush(stdout);
}
#define CHECK(x) do { VkResult r_ = (x); L("VKTEST: %s -> %d", #x, r_); if (r_ != VK_SUCCESS) return 1; } while (0)

int main(int argc, char **argv)
{
    HMODULE m;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst; VkPhysicalDevice pd[4]; uint32_t n = 4;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vktest", 1, "none", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkPhysicalDeviceProperties props;
    VkDevice dev; float prio = 1.f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci };
    uint32_t qn = 0;

    L("VKTEST: start");
    SetEnvironmentVariableA("VK_DRIVER_FILES", argc > 1 ? argv[1] : "lvp_icd.x86_64.json");
    SetEnvironmentVariableA("VK_LOADER_DEBUG", "error,warn,driver");
    m = LoadLibraryA("vulkan-1.dll");
    L("VKTEST: LoadLibrary(vulkan-1.dll) = %p err %lu", m, GetLastError());
    if (!m) return 2;
    gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
    L("VKTEST: vkGetInstanceProcAddr = %p", gipa);
    if (!gipa) return 3;
    CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
    CHECK(((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, pd));
    L("VKTEST: %u physical device(s)", n);
    if (!n) return 4;
    ((PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"))(pd[0], &props);
    L("VKTEST: device '%s' api %u.%u.%u driver %x type %d", props.deviceName,
      VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
      props.driverVersion, props.deviceType);
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(pd[0], &qn, NULL);
    L("VKTEST: %u queue families", qn);
    CHECK(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd[0], &dci, NULL, &dev));
    {
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
        VkQueue q; PFN_vkGetDeviceQueue gdq = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
        gdq(dev, 0, 0, &q);
        CHECK(((PFN_vkQueueWaitIdle)gdpa(dev, "vkQueueWaitIdle"))(q));
    }
    L("VKTEST: PASS");
    return 0;
}
