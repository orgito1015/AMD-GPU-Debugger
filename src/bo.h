#pragma once

#include "util.h"
#include <xf86drm.h>
#include <amdgpu.h>
#include <amdgpu_drm.h>

/**
 * amdgpu_bo_t: Buffer Object wrapper
 * 
 * Represents a GPU buffer object with both GPU virtual address (VA)
 * and optional CPU mapping. All addresses are in the process's GPU
 * address space, which is per-context.
 * 
 * DANGER: bo_handle and va_handle must be freed explicitly.
 * DANGER: host_addr may be NULL if CPU_ACCESS_REQUIRED was not set.
 * DANGER: va_addr is only valid within the owning context/VMID.
 */
typedef struct {
    amdgpu_bo_handle   bo_handle;   // libdrm BO handle
    amdgpu_va_handle   va_handle;   // VA range handle
    uint64_t           va_addr;     // GPU virtual address
    void*              host_addr;   // CPU-mapped address (nullable)
    size_t             size;        // Actual allocated size (aligned)
    uint32_t           kms_handle;  // KMS handle for IOCTL operations
} amdgpu_bo_t;

/**
 * amdgpu_t: Main device context
 * 
 * Encapsulates DRM device, libdrm device handle, command submission context,
 * and debugfs file descriptors for privileged register access.
 * 
 * DANGER: Only one context should program TBA/TMA at a time system-wide.
 * DANGER: regs2_fd requires CAP_SYS_ADMIN or debugfs mount permissions.
 * DANGER: Operations on this context can affect other processes sharing VMIDs.
 */
typedef struct {
    int                      drm_fd;         // DRM device file descriptor
    amdgpu_device_handle     dev_handle;     // libdrm device handle
    amdgpu_context_handle    ctx_handle;     // Command submission context
    int                      regs2_fd;       // debugfs regs2 file descriptor
    uint64_t                 gc_regs_base_addr[16]; // GC register base addresses per SOC block
    uint32_t                 device_id;      // PCI device ID
    uint32_t                 chip_rev;       // Chip revision
    uint32_t                 chip_external_rev; // External chip revision
} amdgpu_t;

/**
 * amdgpu_submit_t: Command submission tracking
 * 
 * Holds the indirect buffer and fence for a submitted command.
 * Must be used to wait for completion and free resources.
 * 
 * DANGER: bo_list must be freed after submission completes.
 * DANGER: ib buffer must be freed to avoid memory leak.
 */
typedef struct {
    amdgpu_bo_t          ib;         // Indirect buffer containing PM4 packets
    amdgpu_bo_list_handle bo_list;   // BO list for submission
    struct amdgpu_cs_fence fence;    // Fence for synchronization
} amdgpu_submit_t;

/**
 * Dynamic array for PM4 packets (uint32_t values).
 * 
 * Grows automatically as packets are appended.
 * Used to build command buffers before uploading to GPU.
 */
typedef struct {
    uint32_t* data;     // Packet data (dwords)
    size_t    count;    // Number of dwords
    size_t    capacity; // Allocated capacity
} pkt3_packets_t;

/**
 * Initialize empty packet array.
 */
static inline void pkt3_init(pkt3_packets_t* packets) {
    packets->data = NULL;
    packets->count = 0;
    packets->capacity = 0;
}

/**
 * Append a dword to the packet array, growing if necessary.
 */
static inline void da_append(pkt3_packets_t* packets, uint32_t value) {
    if (packets->count >= packets->capacity) {
        size_t new_cap = packets->capacity == 0 ? 64 : packets->capacity * 2;
        packets->data = realloc(packets->data, new_cap * sizeof(uint32_t));
        HDB_ASSERT(packets->data != NULL, "realloc failed for packet array");
        packets->capacity = new_cap;
    }
    packets->data[packets->count++] = value;
}

/**
 * Get size in bytes of packet array.
 */
static inline size_t pkt3_size(pkt3_packets_t* packets) {
    return packets->count * sizeof(uint32_t);
}

/**
 * Free packet array resources.
 */
static inline void pkt3_free(pkt3_packets_t* packets) {
    free(packets->data);
    packets->data = NULL;
    packets->count = 0;
    packets->capacity = 0;
}

/**
 * Buffer object allocation function.
 * 
 * @param dev: Device context
 * @param size: Requested size in bytes (will be page-aligned)
 * @param domain: Memory domain (VRAM, GTT, etc.)
 * @param uncached: If true, set uncached flags for GTT
 * @param bo: Output BO structure
 * @return: 0 on success, negative error code on failure
 * 
 * DANGER: Allocates GPU memory that must be freed with bo_free().
 * DANGER: VA mapping is permanent until bo_free() is called.
 * DANGER: Uncached GTT is required for CPU-GPU synchronization but slow.
 */
int32_t bo_alloc(amdgpu_t* dev, size_t size, uint32_t domain, 
                 bool uncached, amdgpu_bo_t* bo);

/**
 * Upload data to buffer object.
 * 
 * @param bo: Target buffer object
 * @param data: Source data
 * @param size: Number of bytes to copy
 * 
 * DANGER: Assumes bo->host_addr is mapped and size <= bo->size.
 * DANGER: No bounds checking performed - caller must ensure validity.
 */
void bo_upload(amdgpu_bo_t* bo, const void* data, size_t size);

/**
 * Free buffer object and all associated resources.
 * 
 * @param dev: Device context
 * @param bo: Buffer object to free
 * 
 * DANGER: Invalidates all GPU VAs and CPU pointers to this BO.
 * DANGER: Must not be called while GPU is still accessing the buffer.
 */
void bo_free(amdgpu_t* dev, amdgpu_bo_t* bo);
