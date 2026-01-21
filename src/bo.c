#include "bo.h"
#include <sys/ioctl.h>
#include <unistd.h>

/**
 * drm_ioctl_write_read: Helper for DRM IOCTLs with write+read.
 * 
 * @param fd: DRM device file descriptor
 * @param request: IOCTL request number
 * @param arg: IOCTL argument structure
 * @param size: Size of argument structure
 * @return: 0 on success, negative errno on failure
 */
static int32_t drm_ioctl_write_read(int fd, unsigned long request,
                                    void* arg, size_t size) {
    (void)size; // Size is embedded in request
    int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        return -errno;
    }
    return 0;
}

int32_t bo_alloc(amdgpu_t* dev, size_t size, uint32_t domain,
                 bool uncached, amdgpu_bo_t* bo) {
    int32_t ret = -1;
    uint32_t alignment = 0;
    uint32_t flags = 0;
    size_t actual_size = 0;

    amdgpu_bo_handle bo_handle = NULL;
    amdgpu_va_handle va_handle = NULL;
    uint64_t va_addr = 0;
    void* host_addr = NULL;
    uint32_t kms_handle = 0;

    // Special domains (GWS, GDS, OA) don't have CPU access
    if (domain != AMDGPU_GEM_DOMAIN_GWS &&
        domain != AMDGPU_GEM_DOMAIN_GDS &&
        domain != AMDGPU_GEM_DOMAIN_OA) {
        // Align to page boundary
        actual_size = ALIGN_UP(size, PAGE_SIZE);
        alignment = PAGE_SIZE;
        
        // Standard flags for CPU-accessible memory
        flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
                AMDGPU_GEM_CREATE_VRAM_CLEARED |
                AMDGPU_GEM_CREATE_VM_ALWAYS_VALID;

        // Uncached write-combined for GTT (needed for CPU-GPU sync)
        if (uncached && domain == AMDGPU_GEM_DOMAIN_GTT) {
            flags |= AMDGPU_GEM_CREATE_CPU_GTT_USWC;
        }
    } else {
        // Special domains: no alignment, no CPU access
        actual_size = size;
        alignment = 1;
        flags = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
    }

    // Allocate BO via libdrm
    struct amdgpu_bo_alloc_request req = {
        .alloc_size = actual_size,
        .phys_alignment = alignment,
        .preferred_heap = domain,
        .flags = flags,
    };

    ret = amdgpu_bo_alloc(dev->dev_handle, &req, &bo_handle);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_bo_alloc failed: %d (%s)\n",
                ret, strerror(-ret));
        return ret;
    }

    // Export KMS handle for manual VA mapping IOCTL
    ret = amdgpu_bo_export(bo_handle, amdgpu_bo_handle_type_kms, &kms_handle);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_bo_export failed: %d\n", ret);
        amdgpu_bo_free(bo_handle);
        return ret;
    }

    // Allocate GPU VA range
    ret = amdgpu_va_range_alloc(dev->dev_handle,
                                amdgpu_gpu_va_range_general,
                                actual_size,
                                PAGE_SIZE,
                                0, // base_addr (0 = auto)
                                &va_addr,
                                &va_handle,
                                0); // flags
    if (ret != 0) {
        fprintf(stderr, "[ERROR] amdgpu_va_range_alloc failed: %d\n", ret);
        amdgpu_bo_free(bo_handle);
        return ret;
    }

    // Manually map GPU VA with custom flags (uncached, executable, etc.)
    uint64_t map_flags =
        AMDGPU_VM_PAGE_EXECUTABLE |
        AMDGPU_VM_PAGE_READABLE |
        AMDGPU_VM_PAGE_WRITEABLE;

    if (uncached) {
        map_flags |= AMDGPU_VM_MTYPE_UC | AMDGPU_VM_PAGE_NOALLOC;
    }

    struct drm_amdgpu_gem_va va = {
        .handle = kms_handle,
        .operation = AMDGPU_VA_OP_MAP,
        .flags = map_flags,
        .va_address = va_addr,
        .offset_in_bo = 0,
        .map_size = actual_size,
    };

    ret = drm_ioctl_write_read(dev->drm_fd, DRM_IOCTL_AMDGPU_GEM_VA,
                               &va, sizeof(va));
    if (ret != 0) {
        fprintf(stderr, "[ERROR] DRM_IOCTL_AMDGPU_GEM_VA failed: %d\n", ret);
        amdgpu_va_range_free(va_handle);
        amdgpu_bo_free(bo_handle);
        return ret;
    }

    // CPU mapping if required
    if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) {
        ret = amdgpu_bo_cpu_map(bo_handle, &host_addr);
        if (ret != 0) {
            fprintf(stderr, "[ERROR] amdgpu_bo_cpu_map failed: %d\n", ret);
            // Clean up VA mapping
            va.operation = AMDGPU_VA_OP_UNMAP;
            drm_ioctl_write_read(dev->drm_fd, DRM_IOCTL_AMDGPU_GEM_VA,
                                &va, sizeof(va));
            amdgpu_va_range_free(va_handle);
            amdgpu_bo_free(bo_handle);
            return ret;
        }
        
        // Zero the buffer (VRAM_CLEARED flag may not always work)
        memset(host_addr, 0x0, actual_size);
    }

    // Fill output structure
    *bo = (amdgpu_bo_t){
        .bo_handle = bo_handle,
        .va_handle = va_handle,
        .va_addr = va_addr,
        .host_addr = host_addr,
        .size = actual_size,
        .kms_handle = kms_handle,
    };

    return 0;
}

void bo_upload(amdgpu_bo_t* bo, const void* data, size_t size) {
    HDB_ASSERT(bo->host_addr != NULL, "BO is not CPU-mapped");
    HDB_ASSERT(size <= bo->size, "Upload size exceeds BO size");
    
    memcpy(bo->host_addr, data, size);
}

void bo_free(amdgpu_t* dev, amdgpu_bo_t* bo) {
    if (bo->bo_handle == NULL) {
        return; // Already freed or never allocated
    }

    // Unmap CPU if mapped
    if (bo->host_addr != NULL) {
        amdgpu_bo_cpu_unmap(bo->bo_handle);
        bo->host_addr = NULL;
    }

    // Unmap GPU VA
    if (bo->va_handle != NULL) {
        struct drm_amdgpu_gem_va va = {
            .handle = bo->kms_handle,
            .operation = AMDGPU_VA_OP_UNMAP,
            .flags = 0,
            .va_address = bo->va_addr,
            .offset_in_bo = 0,
            .map_size = bo->size,
        };
        
        int ret = drm_ioctl_write_read(dev->drm_fd, DRM_IOCTL_AMDGPU_GEM_VA,
                                       &va, sizeof(va));
        if (ret != 0) {
            fprintf(stderr, "[WARN] GPU VA unmap failed: %d\n", ret);
        }

        amdgpu_va_range_free(bo->va_handle);
        bo->va_handle = NULL;
    }

    // Free BO
    amdgpu_bo_free(bo->bo_handle);
    bo->bo_handle = NULL;
}
