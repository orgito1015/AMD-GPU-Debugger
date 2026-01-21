#include "amdgpu_device.h"
#include "bo.h"
#include "regs.h"
#include "spirv_compile.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * AMD GPU Debugger - Main Entry Point
 * 
 * This is an experimental RDNA3 wavefront debugger that interfaces
 * directly with the AMDGPU kernel driver via DRM IOCTLs and debugfs.
 * 
 * DANGER: This tool performs invasive hardware operations:
 * - Programs TBA/TMA registers globally for VMIDs 1-8
 * - Submits raw PM4 packets to the GPU
 * - Accesses privileged MMIO registers via debugfs
 * - Can interfere with other GPU processes
 * 
 * Requirements:
 * - RDNA3 GPU (gfx11, e.g., RX 7900 XTX)
 * - Linux with AMDGPU driver
 * - debugfs mounted at /sys/kernel/debug
 * - Root or CAP_SYS_ADMIN for register access
 * - User in 'video' group for DRM access
 */

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --device <path>    DRM device path (default: /dev/dri/card0)\n");
    fprintf(stderr, "  --test-init        Test device initialization only\n");
    fprintf(stderr, "  --help             Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "WARNING: This is experimental low-level code.\n");
    fprintf(stderr, "         Run on non-production machines only.\n");
    fprintf(stderr, "         Requires root or special permissions.\n");
}

int main(int argc, char** argv) {
    const char* device_path = NULL;
    bool test_init = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_path = argv[++i];
        } else if (strcmp(argv[i], "--test-init") == 0) {
            test_init = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    fprintf(stdout, "\n");
    fprintf(stdout, "==================================================\n");
    fprintf(stdout, "AMD GPU Debugger (Experimental RDNA3 PoC)\n");
    fprintf(stdout, "==================================================\n");
    fprintf(stdout, "\n");

    // Initialize device
    amdgpu_t dev = {0};
    int32_t ret = amdgpu_device_init(device_path, &dev);
    if (ret != 0) {
        fprintf(stderr, "[FATAL] Device initialization failed: %d\n", ret);
        return 1;
    }

    if (test_init) {
        fprintf(stdout, "\n");
        fprintf(stdout, "[SUCCESS] Device initialization test passed\n");
        fprintf(stdout, "          Device is ready for debugging operations\n");
        fprintf(stdout, "\n");
        amdgpu_device_cleanup(&dev);
        return 0;
    }

    // Example: Allocate a test buffer
    fprintf(stdout, "\n");
    fprintf(stdout, "Testing buffer object allocation...\n");
    amdgpu_bo_t test_bo = {0};
    ret = bo_alloc(&dev, 4096, AMDGPU_GEM_DOMAIN_GTT, false, &test_bo);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] Buffer allocation failed: %d\n", ret);
        amdgpu_device_cleanup(&dev);
        return 1;
    }
    fprintf(stdout, "[SUCCESS] Allocated BO: VA=0x%lx size=%zu\n",
            test_bo.va_addr, test_bo.size);

    // Write test data
    uint32_t test_data[4] = {0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0x87654321};
    bo_upload(&test_bo, test_data, sizeof(test_data));
    fprintf(stdout, "[SUCCESS] Uploaded test data to BO\n");

    // Clean up
    bo_free(&dev, &test_bo);
    fprintf(stdout, "[SUCCESS] Freed test BO\n");

    fprintf(stdout, "\n");
    fprintf(stdout, "==================================================\n");
    fprintf(stdout, "Next Steps:\n");
    fprintf(stdout, "==================================================\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "1. Add trap handler assembly (src/trap_handler.s)\n");
    fprintf(stdout, "2. Implement PM4 packet builders for compute dispatch\n");
    fprintf(stdout, "3. Setup TBA/TMA with trap handler code\n");
    fprintf(stdout, "4. Implement CPU-GPU synchronization loop\n");
    fprintf(stdout, "5. Add debugger CLI (step, breakpoints, register inspection)\n");
    fprintf(stdout, "\n");
    fprintf(stdout, "See README.md for detailed architecture and examples.\n");
    fprintf(stdout, "\n");

    // Cleanup
    amdgpu_device_cleanup(&dev);

    return 0;
}
