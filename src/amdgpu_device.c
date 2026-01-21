#include "bo.h"
#include "regs.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

/**
 * Open DRM device node and initialize AMDGPU device context.
 * 
 * @param device_path: Path to DRM device (e.g., "/dev/dri/card0" or NULL for auto)
 * @param dev: Output device structure
 * @return: 0 on success, negative error code on failure
 * 
 * DANGER: Requires read/write access to DRM device (typically in 'video' group).
 * DANGER: Only one debugger process should be active at a time.
 */
int32_t amdgpu_device_init(const char* device_path, amdgpu_t* dev) {
    int32_t ret = 0;
    int drm_fd = -1;
    amdgpu_device_handle dev_handle = NULL;
    amdgpu_context_handle ctx_handle = NULL;
    int regs2_fd = -1;
    uint32_t drm_major = 0, drm_minor = 0;

    // Default to card0 if no path specified
    const char* path = device_path ? device_path : "/dev/dri/card0";

    // Open DRM device
    drm_fd = open(path, O_RDWR);
    if (drm_fd < 0) {
        fprintf(stderr, "[ERROR] Failed to open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "[ERROR] Make sure you have permissions (video group)\n");
        return -errno;
    }

    // Initialize AMDGPU device via libdrm
    ret = amdgpu_device_initialize(drm_fd, &drm_major, &drm_minor, &dev_handle);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_device_initialize failed: %d\n", ret);
        close(drm_fd);
        return ret;
    }

    fprintf(stdout, "[INFO] AMDGPU device initialized (DRM %u.%u)\n",
            drm_major, drm_minor);

    // Get device info
    struct amdgpu_gpu_info gpu_info = {0};
    ret = amdgpu_query_gpu_info(dev_handle, &gpu_info);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_query_gpu_info failed: %d\n", ret);
        amdgpu_device_deinitialize(dev_handle);
        close(drm_fd);
        return ret;
    }

    fprintf(stdout, "[INFO] GPU: device_id=0x%x chip_rev=0x%x chip_external_rev=0x%x\n",
            gpu_info.asic_id, gpu_info.chip_rev, gpu_info.chip_external_rev);

    // Check for RDNA3 (gfx11)
    // Note: family_id is the actual field name in libdrm's amdgpu_gpu_info
    uint32_t family_id = gpu_info.family_id;
    if (family_id < AMDGPU_FAMILY_GC_11_0_0) {
        fprintf(stderr, "[WARN] This tool is designed for RDNA3 (gfx11)\n");
        fprintf(stderr, "[WARN] Detected family_id: %u\n", family_id);
        fprintf(stderr, "[WARN] Continuing anyway, but behavior may be incorrect\n");
    }

    // Create command submission context
    ret = amdgpu_cs_ctx_create(dev_handle, &ctx_handle);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_cs_ctx_create failed: %d\n", ret);
        amdgpu_device_deinitialize(dev_handle);
        close(drm_fd);
        return ret;
    }

    // Open debugfs regs2 for privileged register access
    // Try card0 first, then card1, etc.
    char regs2_path[256];
    for (int i = 0; i < 8; i++) {
        snprintf(regs2_path, sizeof(regs2_path),
                 "/sys/kernel/debug/dri/%d/regs2", i);
        regs2_fd = open(regs2_path, O_RDWR);
        if (regs2_fd >= 0) {
            fprintf(stdout, "[INFO] Opened debugfs: %s\n", regs2_path);
            break;
        }
    }

    if (regs2_fd < 0) {
        fprintf(stderr, "[WARN] Failed to open debugfs regs2\n");
        fprintf(stderr, "[WARN] Ensure debugfs is mounted: mount -t debugfs none /sys/kernel/debug\n");
        fprintf(stderr, "[WARN] Or run as root / with CAP_SYS_ADMIN\n");
        fprintf(stderr, "[WARN] Register access (TBA/TMA) will not be available\n");
        // Continue without regs2 - some features will be disabled
    }

    // Initialize GC register base addresses
    // DANGER: Hardcoded for typical RDNA3 layout
    // 
    // FIXME: These are PLACEHOLDER values that must be determined from
    //        actual hardware before use. The correct base addresses can be
    //        obtained via:
    //        1. amdgpu_query_hw_ip_info() for IP-specific base addresses
    //        2. Reading from kernel amdgpu driver sources
    //        3. UMR register database for specific ASIC
    // 
    // Using incorrect base addresses WILL cause register access to fail
    // or write to wrong locations, potentially hanging the GPU.
    uint64_t gc_regs_base_addr[16] = {0};
    gc_regs_base_addr[0] = 0x0; // PLACEHOLDER - GC base (MUST VERIFY)

    // Fill output structure
    *dev = (amdgpu_t){
        .drm_fd = drm_fd,
        .dev_handle = dev_handle,
        .ctx_handle = ctx_handle,
        .regs2_fd = regs2_fd,
        .device_id = gpu_info.asic_id,
        .chip_rev = gpu_info.chip_rev,
        .chip_external_rev = gpu_info.chip_external_rev,
    };

    memcpy(dev->gc_regs_base_addr, gc_regs_base_addr, sizeof(gc_regs_base_addr));

    fprintf(stdout, "[INFO] Device context initialized\n");
    return 0;
}

/**
 * Clean up device context and free resources.
 * 
 * @param dev: Device context to clean up
 * 
 * DANGER: Invalidates all BOs, contexts, and file descriptors.
 * DANGER: GPU must be idle before calling this.
 */
void amdgpu_device_cleanup(amdgpu_t* dev) {
    if (dev->ctx_handle != NULL) {
        amdgpu_cs_ctx_free(dev->ctx_handle);
        dev->ctx_handle = NULL;
    }

    if (dev->dev_handle != NULL) {
        amdgpu_device_deinitialize(dev->dev_handle);
        dev->dev_handle = NULL;
    }

    if (dev->regs2_fd >= 0) {
        close(dev->regs2_fd);
        dev->regs2_fd = -1;
    }

    if (dev->drm_fd >= 0) {
        close(dev->drm_fd);
        dev->drm_fd = -1;
    }

    fprintf(stdout, "[INFO] Device context cleaned up\n");
}

/**
 * Submit command buffer to GPU.
 * 
 * @param dev: Device context
 * @param packets: PM4 packet array
 * @param buffers: Array of BO handles to include in submission
 * @param buffers_count: Number of BOs
 * @param submit: Output submission info (for fence wait)
 * @return: 0 on success, negative error code on failure
 * 
 * DANGER: Submits PM4 commands directly to GPU compute queue.
 * DANGER: Malformed packets can hang or reset the GPU.
 */
int32_t dev_submit(amdgpu_t* dev,
                   pkt3_packets_t* packets,
                   amdgpu_bo_handle* buffers,
                   uint32_t buffers_count,
                   amdgpu_submit_t* submit) {
    int32_t ret = -1;
    amdgpu_bo_t ib = {0};

    // Allocate indirect buffer for PM4 packets
    ret = bo_alloc(dev, pkt3_size(packets), AMDGPU_GEM_DOMAIN_GTT, false, &ib);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] Failed to allocate IB: %d\n", ret);
        return ret;
    }

    // Upload packets to IB
    bo_upload(&ib, packets->data, pkt3_size(packets));

    // Build BO list (IB + user BOs)
    amdgpu_bo_handle* bo_handles =
        malloc(sizeof(amdgpu_bo_handle) * (buffers_count + 1));
    if (bo_handles == NULL) {
        fprintf(stderr, "[ERROR] malloc failed for BO list\n");
        bo_free(dev, &ib);
        return -ENOMEM;
    }

    bo_handles[0] = ib.bo_handle;
    for_range(i, 0, buffers_count) {
        bo_handles[i + 1] = buffers[i];
    }

    amdgpu_bo_list_handle bo_list = NULL;
    ret = amdgpu_bo_list_create(dev->dev_handle,
                                buffers_count + 1,
                                bo_handles,
                                NULL,
                                &bo_list);
    free(bo_handles);

    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_bo_list_create failed: %d\n", ret);
        bo_free(dev, &ib);
        return ret;
    }

    // Prepare submission
    struct amdgpu_cs_ib_info ib_info = {
        .flags = 0,
        .ib_mc_address = ib.va_addr,
        .size = packets->count, // Size in dwords
    };

    struct amdgpu_cs_request req = {
        .flags = 0,
        .ip_type = AMDGPU_HW_IP_COMPUTE,
        .ip_instance = 0,
        .ring = 0,
        .resources = bo_list,
        .number_of_dependencies = 0,
        .dependencies = NULL,
        .number_of_ibs = 1,
        .ibs = &ib_info,
        .seq_no = 0,
        .fence_info = {0},
    };

    // Submit
    ret = amdgpu_cs_submit(dev->ctx_handle, 0, &req, 1);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_cs_submit failed: %d\n", ret);
        amdgpu_bo_list_destroy(bo_list);
        bo_free(dev, &ib);
        return ret;
    }

    fprintf(stdout, "[INFO] Command buffer submitted (seq=%lu)\n", req.seq_no);

    // Fill output structure
    *submit = (amdgpu_submit_t){
        .ib = ib,
        .bo_list = bo_list,
        .fence = {
            .context = dev->ctx_handle,
            .ip_type = AMDGPU_HW_IP_COMPUTE,
            .ip_instance = 0,
            .ring = 0,
            .fence = req.seq_no,
        },
    };

    return 0;
}

/**
 * Wait for command submission to complete.
 * 
 * @param dev: Device context
 * @param submit: Submission info from dev_submit
 * @param timeout_ns: Timeout in nanoseconds (0 = infinite)
 * @return: 0 on success, negative error code on timeout/error
 * 
 * DANGER: Infinite timeout can hang if GPU is stuck.
 */
int32_t dev_wait(amdgpu_t* dev, amdgpu_submit_t* submit, uint64_t timeout_ns) {
    (void)dev; // Unused

    uint32_t expired = 0;
    int32_t ret = amdgpu_cs_query_fence_status(&submit->fence,
                                               timeout_ns,
                                               0, // flags
                                               &expired);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_cs_query_fence_status failed: %d\n", ret);
        return ret;
    }

    if (!expired) {
        fprintf(stderr, "[ERROR] Fence timeout\n");
        return -ETIMEDOUT;
    }

    fprintf(stdout, "[INFO] Command buffer completed\n");
    return 0;
}

/**
 * Clean up submission resources.
 * 
 * @param dev: Device context
 * @param submit: Submission to clean up
 */
void dev_submit_cleanup(amdgpu_t* dev, amdgpu_submit_t* submit) {
    if (submit->bo_list != NULL) {
        amdgpu_bo_list_destroy(submit->bo_list);
        submit->bo_list = NULL;
    }

    bo_free(dev, &submit->ib);
}
