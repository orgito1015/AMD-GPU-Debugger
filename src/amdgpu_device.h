#pragma once

#include "bo.h"

/**
 * Initialize AMDGPU device and create command submission context.
 * 
 * @param device_path: Path to DRM device (NULL for auto-detect)
 * @param dev: Output device structure
 * @return: 0 on success, negative error code on failure
 */
int32_t amdgpu_device_init(const char* device_path, amdgpu_t* dev);

/**
 * Clean up device context and free resources.
 * 
 * @param dev: Device context to clean up
 */
void amdgpu_device_cleanup(amdgpu_t* dev);

/**
 * Submit command buffer to GPU.
 * 
 * @param dev: Device context
 * @param packets: PM4 packet array
 * @param buffers: Array of BO handles to include in submission
 * @param buffers_count: Number of BOs
 * @param submit: Output submission info (for fence wait)
 * @return: 0 on success, negative error code on failure
 */
int32_t dev_submit(amdgpu_t* dev,
                   pkt3_packets_t* packets,
                   amdgpu_bo_handle* buffers,
                   uint32_t buffers_count,
                   amdgpu_submit_t* submit);

/**
 * Wait for command submission to complete.
 * 
 * @param dev: Device context
 * @param submit: Submission info from dev_submit
 * @param timeout_ns: Timeout in nanoseconds (0 = infinite)
 * @return: 0 on success, negative error code on timeout/error
 */
int32_t dev_wait(amdgpu_t* dev, amdgpu_submit_t* submit, uint64_t timeout_ns);

/**
 * Clean up submission resources.
 * 
 * @param dev: Device context
 * @param submit: Submission to clean up
 */
void dev_submit_cleanup(amdgpu_t* dev, amdgpu_submit_t* submit);
