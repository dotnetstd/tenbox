#include "platform/macos/hypervisor/hvf_platform.h"
#include <sys/sysctl.h>

namespace hvf {

bool IsHypervisorPresent() {
    int hv_support = 0;
    size_t size = sizeof(hv_support);
    if (sysctlbyname("kern.hv_support", &hv_support, &size, nullptr, 0) == 0) {
        return hv_support != 0;
    }
    return false;
}

} // namespace hvf
