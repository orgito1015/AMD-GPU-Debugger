#pragma once

#include "util.h"
#include <stddef.h>
#include <stdint.h>

/**
 * Shader stage type.
 */
typedef enum {
    HDB_SHADER_STAGE_COMPUTE = 0,
    HDB_SHADER_STAGE_VERTEX,
    HDB_SHADER_STAGE_FRAGMENT,
} hdb_shader_stage_t;

/**
 * Compiled shader result.
 * 
 * Contains the GFX11 machine code binary and hardware configuration.
 * Debug info (if available) maps instruction offsets to source lines.
 * 
 * DANGER: bin points to internal RADV memory; do not free directly.
 * DANGER: Must copy bin to GPU buffer before shader object is destroyed.
 */
typedef struct {
    const void*   bin;              // GFX11 machine code binary
    size_t        bin_size;         // Size in bytes
    uint32_t      rsrc1;            // SPI_SHADER_PGM_RSRC1 value
    uint32_t      rsrc2;            // SPI_SHADER_PGM_RSRC2 value
    uint32_t      rsrc3;            // SPI_SHADER_PGM_RSRC3 value
    const void*   debug_info;       // ACO debug info (nullable)
    size_t        debug_info_count; // Number of debug entries
} hdb_shader_t;

/**
 * Compile SPIR-V to GFX11 machine code using RADV/ACO.
 * 
 * @param spirv_binary: SPIR-V bytecode
 * @param size: Size of SPIR-V binary in bytes
 * @param stage: Shader stage (compute, vertex, fragment)
 * @param shader: Output shader structure
 * @return: 0 on success, negative on error
 * 
 * DANGER: Sets RADV_FORCE_FAMILY=navi31 environment variable.
 * DANGER: Requires Mesa RADV/ACO to be built and linkable.
 * DANGER: This is a heavyweight operation (JIT compilation).
 * DANGER: Not thread-safe; use external synchronization if needed.
 * 
 * NOTE: Implementation is optional for minimal build.
 * If RADV is not available, this function should return -ENOSYS.
 */
int32_t hdb_compile_spirv_to_bin(const void* spirv_binary,
                                 size_t size,
                                 hdb_shader_stage_t stage,
                                 hdb_shader_t* shader);
