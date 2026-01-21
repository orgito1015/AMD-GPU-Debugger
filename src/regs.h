#pragma once

#include "util.h"
#include "bo.h"
#include <linux/ioctl.h>

/**
 * Register operation type.
 */
typedef enum {
    REG_OP_READ = 0,
    REG_OP_WRITE = 1,
} reg_32_op_t;

/**
 * Register information type.
 */
typedef enum {
    REG_MMIO = 0,      // Memory-mapped I/O register
    REG_INDIRECT = 1,  // Indirect register (requires index/data pair)
} reg_type_t;

/**
 * Register metadata.
 * 
 * soc_index: Index into gc_regs_base_addr array for this register's block.
 * type: MMIO or indirect register type.
 */
typedef struct {
    uint32_t   soc_index;
    reg_type_t type;
} reg_info_t;

/**
 * GC 11 (RDNA3) register enumeration.
 * 
 * This is a minimal subset for trap handler setup and wave control.
 * Full register database would be thousands of entries (see UMR project).
 * 
 * DANGER: Register offsets are hardware-specific and may vary by ASIC.
 * DANGER: Writing wrong values can hang or reset the GPU.
 */
typedef enum {
    REG_SQ_SHADER_TBA_LO = 0,  // Trap Base Address (low 32 bits)
    REG_SQ_SHADER_TBA_HI,      // Trap Base Address (high 32 bits)
    REG_SQ_SHADER_TMA_LO,      // Trap Memory Address (low 32 bits)
    REG_SQ_SHADER_TMA_HI,      // Trap Memory Address (high 32 bits)
    REG_SQ_CMD,                // SQ command register (halt/resume waves)
    REG_MAX,
} gc_11_reg_t;

/**
 * SQ_SHADER_TBA_LO register layout.
 * 
 * Trap handler base address bits [39:8] (256-byte aligned).
 * Actual address is (tba_lo << 8) | (tba_hi << 40).
 */
typedef union {
    struct {
        uint32_t base_addr : 32;  // Bits [39:8] of trap handler address
    };
    uint32_t raw;
} reg_sq_shader_tba_lo_t;

/**
 * SQ_SHADER_TBA_HI register layout.
 * 
 * DANGER: trap_en bit enables trap handler globally for this VMID.
 * DANGER: Setting trap_en=1 with invalid TBA can hang waves or crash GPU.
 */
typedef union {
    struct {
        uint32_t base_addr : 8;   // Bits [47:40] of trap handler address
        uint32_t reserved  : 23;
        uint32_t trap_en   : 1;   // Trap enable (1=enabled, 0=disabled)
    };
    uint32_t raw;
} reg_sq_shader_tba_hi_t;

/**
 * SQ_SHADER_TMA_LO register layout.
 * 
 * Trap scratch buffer address (low 32 bits).
 */
typedef union {
    struct {
        uint32_t base_addr : 32;  // Bits [31:0] of trap memory address
    };
    uint32_t raw;
} reg_sq_shader_tma_lo_t;

/**
 * SQ_SHADER_TMA_HI register layout.
 * 
 * Trap scratch buffer address (high 32 bits).
 */
typedef union {
    struct {
        uint32_t base_addr : 32;  // Bits [63:32] of trap memory address
    };
    uint32_t raw;
} reg_sq_shader_tma_hi_t;

/**
 * SQ_CMD register layout.
 * 
 * Used to halt, resume, or step waves by hardware ID.
 * 
 * DANGER: Incorrect wave_id or mode can affect wrong waves.
 * DANGER: Halting waves indefinitely can deadlock GPU.
 */
typedef union {
    struct {
        uint32_t cmd     : 4;   // Command (1=halt/resume/step)
        uint32_t mode    : 4;   // Mode (0=resume, 1=halt, 2=step)
        uint32_t data    : 24;  // Wave ID or other data
    };
    uint32_t raw;
} reg_sq_cmd_t;

/**
 * debugfs regs2 IOCTL data structure (v2).
 * 
 * Used to set SRBM/GRBM state before register access.
 * Allows targeting specific SEs, SHs, CUs, VMIDs, etc.
 * 
 * DANGER: use_srbm=1 with vmid=X affects that VMID's registers.
 * DANGER: xcc_id=(uint32_t)-1 means "all XCCs" (multi-die GPUs).
 */
typedef struct amdgpu_debugfs_regs2_iocdata_v2 {
    uint32_t use_srbm, use_grbm, pg_lock;
    struct {
        uint32_t se, sh, instance;
    } grbm;
    struct {
        uint32_t me, pipe, queue, vmid;
    } srbm;
    uint32_t xcc_id;
} regs2_ioc_data_t;

/**
 * debugfs regs2 IOCTL magic numbers.
 */
#define AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE \
    _IOW(0x20, 0x1, struct amdgpu_debugfs_regs2_iocdata)
#define AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2 \
    _IOW(0x20, 0x2, struct amdgpu_debugfs_regs2_iocdata_v2)

/**
 * Register offset table (populated per-ASIC).
 * 
 * DANGER: Hardcoded for gfx1100 (Navi31). Other gfx11 variants may differ.
 * DANGER: Using wrong offsets can write to unintended registers.
 */
extern const uint64_t gc_11_regs_offsets[REG_MAX];

/**
 * Register info table.
 */
extern const reg_info_t gc_11_regs_infos[REG_MAX];

/**
 * Register access helper with SRBM/GRBM state setup.
 * 
 * @param dev: Device context
 * @param reg: Register to access
 * @param ioc_data: SRBM/GRBM/XCC state
 * @param op: Read or write
 * @param value: Pointer to value (read/write depending on op)
 * 
 * DANGER: Requires root or CAP_SYS_ADMIN to open debugfs regs2.
 * DANGER: Writes to MMIO registers take effect immediately.
 * DANGER: Can affect other processes if VMID is shared.
 */
void dev_op_reg32(amdgpu_t* dev,
                  gc_11_reg_t reg,
                  regs2_ioc_data_t ioc_data,
                  reg_32_op_t op,
                  uint32_t* value);

/**
 * Setup trap handler for all user VMIDs (1-8).
 * 
 * @param dev: Device context
 * @param tba: Trap handler code address (GPU VA, 256-byte aligned)
 * @param tma: Trap scratch buffer address (GPU VA)
 * 
 * DANGER: Affects VMIDs 1-8 globally on the GPU.
 * DANGER: Other processes using these VMIDs will have trap handler enabled.
 * DANGER: If TBA/TMA addresses are invalid in another process's VA space, 
 *         that process's waves will fault or hang when trap fires.
 * DANGER: Only one debugger instance should call this at a time.
 */
void dev_setup_trap_handler(amdgpu_t* dev, uint64_t tba, uint64_t tma);
