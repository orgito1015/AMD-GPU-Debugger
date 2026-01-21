#include "spirv_compile.h"
#include <errno.h>

/**
 * SPIR-V to GFX11 compilation stub.
 * 
 * RADV/ACO integration requires:
 * - Mesa RADV built with ACO compiler
 * - Vulkan headers and loaders
 * - Complex null_winsys setup
 * 
 * This is a heavyweight dependency not suitable for minimal builds.
 * For actual debugging, users should either:
 * 1. Write GFX11 assembly directly (see examples/)
 * 2. Use ROCm toolchain to compile to GFX11
 * 3. Implement full RADV integration (future work)
 * 
 * For now, this returns -ENOSYS (not implemented).
 */
int32_t hdb_compile_spirv_to_bin(const void* spirv_binary,
                                 size_t size,
                                 hdb_shader_stage_t stage,
                                 hdb_shader_t* shader) {
    (void)spirv_binary;
    (void)size;
    (void)stage;
    (void)shader;

    fprintf(stderr, "[ERROR] SPIR-V compilation not implemented\n");
    fprintf(stderr, "[INFO] Use GFX11 assembly directly (see examples/)\n");
    fprintf(stderr, "[INFO] Or compile with ROCm toolchain\n");

    return -ENOSYS;
}

