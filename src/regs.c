#include "regs.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/**
 * hdb_ioctl: Wrapper for ioctl with error handling.
 */
static int32_t hdb_ioctl(int fd, unsigned long request, void* arg) {
    int ret = ioctl(fd, request, arg);
    if (ret < 0) {
        return -errno;
    }
    return 0;
}

/**
 * GC 11 register offset table (gfx1100 / Navi31).
 * 
 * DANGER: These are hardware-specific MMIO offsets.
 * DANGER: Different RDNA3 ASICs may have different offsets.
 * 
 * NOTE: These are PLACEHOLDER offsets for initial implementation.
 * FIXME: These values must be verified against actual RDNA3 hardware
 *        documentation or extracted from UMR register database before
 *        use on real hardware. Using incorrect offsets WILL cause:
 *        - GPU hangs or resets
 *        - Writes to wrong registers
 *        - System instability
 * 
 * Recommended approach:
 * 1. Use UMR (AMD's register dumper) to extract correct offsets
 * 2. Cross-reference with Linux kernel amdgpu driver sources
 * 3. Test on actual RDNA3 hardware with debugfs validation
 */
const uint64_t gc_11_regs_offsets[REG_MAX] = {
    [REG_SQ_SHADER_TBA_LO] = 0x2E00,  // PLACEHOLDER - MUST VERIFY
    [REG_SQ_SHADER_TBA_HI] = 0x2E01,  // PLACEHOLDER - MUST VERIFY
    [REG_SQ_SHADER_TMA_LO] = 0x2E02,  // PLACEHOLDER - MUST VERIFY
    [REG_SQ_SHADER_TMA_HI] = 0x2E03,  // PLACEHOLDER - MUST VERIFY
    [REG_SQ_CMD]           = 0x2D00,  // PLACEHOLDER - MUST VERIFY
};

/**
 * Register info table.
 * 
 * All registers in this minimal set are MMIO (not indirect).
 * soc_index 0 refers to the first GC register block.
 */
const reg_info_t gc_11_regs_infos[REG_MAX] = {
    [REG_SQ_SHADER_TBA_LO] = { .soc_index = 0, .type = REG_MMIO },
    [REG_SQ_SHADER_TBA_HI] = { .soc_index = 0, .type = REG_MMIO },
    [REG_SQ_SHADER_TMA_LO] = { .soc_index = 0, .type = REG_MMIO },
    [REG_SQ_SHADER_TMA_HI] = { .soc_index = 0, .type = REG_MMIO },
    [REG_SQ_CMD]           = { .soc_index = 0, .type = REG_MMIO },
};

void dev_op_reg32(amdgpu_t* dev,
                  gc_11_reg_t reg,
                  regs2_ioc_data_t ioc_data,
                  reg_32_op_t op,
                  uint32_t* value) {
    int32_t ret = 0;

    HDB_ASSERT(dev->regs2_fd >= 0, "regs2_fd not open");
    HDB_ASSERT(reg < REG_MAX, "invalid register enum");
    HDB_ASSERT(value != NULL, "value pointer is NULL");

    reg_info_t reg_info = gc_11_regs_infos[reg];
    uint64_t reg_offset = gc_11_regs_offsets[reg];
    uint64_t base_offset = dev->gc_regs_base_addr[reg_info.soc_index];
    uint64_t total_offset = reg_offset + base_offset;

    // MMIO registers are accessed at 4-byte intervals
    if (reg_info.type == REG_MMIO) {
        total_offset *= 4;
    }

    // Set SRBM/GRBM state for this access
    ret = hdb_ioctl(dev->regs2_fd,
                    AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2,
                    &ioc_data);
    if (ret != 0) {
        fprintf(stderr, "[ERROR] Failed to set register state: %d\n", ret);
        HDB_ASSERT(false, "AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2 failed");
    }

    // Seek to register offset
    off_t pos = lseek(dev->regs2_fd, total_offset, SEEK_SET);
    if (pos != (off_t)total_offset) {
        fprintf(stderr, "[ERROR] Failed to seek to register offset 0x%lx: %s\n",
                total_offset, strerror(errno));
        HDB_ASSERT(false, "lseek failed");
    }

    // Perform read or write
    ssize_t size = 0;
    switch (op) {
    case REG_OP_READ:
        size = read(dev->regs2_fd, value, 4);
        break;
    case REG_OP_WRITE:
        size = write(dev->regs2_fd, value, 4);
        break;
    default:
        HDB_ASSERT(false, "unsupported register operation");
    }

    if (size != 4) {
        fprintf(stderr, "[ERROR] Register access failed (expected 4 bytes, got %zd)\n",
                size);
        HDB_ASSERT(false, "register read/write failed");
    }
}

void dev_setup_trap_handler(amdgpu_t* dev, uint64_t tba, uint64_t tma) {
    HDB_ASSERT((tba & 0xFF) == 0, "TBA must be 256-byte aligned");

    // Prepare register values
    reg_sq_shader_tma_lo_t tma_lo = { .raw = (uint32_t)(tma) };
    reg_sq_shader_tma_hi_t tma_hi = { .raw = (uint32_t)(tma >> 32) };

    // TBA is shifted: [39:8] in low, [47:40] in high
    reg_sq_shader_tba_lo_t tba_lo = { .raw = (uint32_t)(tba >> 8) };
    reg_sq_shader_tba_hi_t tba_hi = { .raw = (uint32_t)(tba >> 40) };

    // Enable trap handler
    tba_hi.trap_en = 1;

    // SRBM state: target all XCCs, use SRBM to select VMID
    regs2_ioc_data_t ioc_data = {
        .use_srbm = 1,
        .use_grbm = 0,
        .pg_lock = 0,
        .grbm = {0},
        .srbm = {
            .me = 0,
            .pipe = 0,
            .queue = 0,
            .vmid = 0, // Will be set per-VMID below
        },
        .xcc_id = (uint32_t)-1, // All XCCs
    };

    // DANGER: Program VMIDs 1-8 (user VMIDs, VMID 0 is kernel)
    // DANGER: This affects ALL processes using these VMIDs system-wide
    // DANGER: If TBA/TMA are invalid in another process's VA space, that
    //         process will hang or crash when a trap fires
    fprintf(stdout, "[WARN] Installing trap handler for VMIDs 1-8 (INVASIVE)\n");
    fprintf(stdout, "[WARN] TBA=0x%lx TMA=0x%lx\n", tba, tma);

    for_range(i, 1, 9) {
        ioc_data.srbm.vmid = i;

        dev_op_reg32(dev, REG_SQ_SHADER_TBA_LO, ioc_data, REG_OP_WRITE, &tba_lo.raw);
        dev_op_reg32(dev, REG_SQ_SHADER_TBA_HI, ioc_data, REG_OP_WRITE, &tba_hi.raw);

        dev_op_reg32(dev, REG_SQ_SHADER_TMA_LO, ioc_data, REG_OP_WRITE, &tma_lo.raw);
        dev_op_reg32(dev, REG_SQ_SHADER_TMA_HI, ioc_data, REG_OP_WRITE, &tma_hi.raw);

        fprintf(stdout, "[INFO] VMID %zu: TBA/TMA installed\n", i);
    }

    fprintf(stdout, "[INFO] Trap handler setup complete\n");
}
