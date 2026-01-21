#include "pm4.h"

void pkt3_set_sh_reg(pkt3_packets_t* packets, uint32_t reg, uint32_t value) {
    HDB_ASSERT(reg >= SI_SH_REG_OFFSET && reg < SI_SH_REG_END,
               "register offset outside SH register range");

    // PKT3_SET_SH_REG header (1 dword following)
    da_append(packets, PKT3(PKT3_SET_SH_REG, 1, 0));
    
    // Register offset (dword offset relative to SI_SH_REG_OFFSET)
    da_append(packets, (reg - SI_SH_REG_OFFSET) / 4);
    
    // Register value
    da_append(packets, value);
}

void pkt3_dispatch_direct(pkt3_packets_t* packets,
                          uint32_t dim_x,
                          uint32_t dim_y,
                          uint32_t dim_z,
                          uint32_t dispatch_initiator) {
    // PKT3_DISPATCH_DIRECT header (3 dwords following)
    // Set shader type to compute (1)
    da_append(packets, PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | PKT3_SHADER_TYPE_S(1));
    
    // Workgroup dimensions
    da_append(packets, dim_x);
    da_append(packets, dim_y);
    da_append(packets, dim_z);
    
    // Dispatch initiator
    da_append(packets, dispatch_initiator);
}

void pkt3_acquire_mem(pkt3_packets_t* packets) {
    // PKT3_ACQUIRE_MEM: Memory barrier and cache flush
    // This ensures previous writes are visible to shader
    
    // Header (5 dwords following for full barrier)
    da_append(packets, PKT3(PKT3_ACQUIRE_MEM, 5, 0));
    
    // CP coher cntl: Invalidate L1, L2, flush L2
    uint32_t cp_coher_cntl = 
        (1 << 0) |  // SH_ICACHE_ACTION_ENA
        (1 << 1) |  // SH_KCACHE_ACTION_ENA
        (1 << 3) |  // TC_ACTION_ENA
        (1 << 4);   // TCL1_ACTION_ENA
    da_append(packets, cp_coher_cntl);
    
    // CP coher size (entire range)
    da_append(packets, 0xFFFFFFFF);
    
    // CP coher size HI
    da_append(packets, 0xFF);
    
    // CP coher base LO
    da_append(packets, 0);
    
    // CP coher base HI
    da_append(packets, 0);
    
    // Poll interval (unused)
    da_append(packets, 0);
}

void pkt3_release_mem(pkt3_packets_t* packets, uint64_t va, uint32_t fence_value) {
    // PKT3_RELEASE_MEM: Write fence value after shader completes
    // This provides CPU-GPU synchronization
    
    // Header (6 dwords following)
    da_append(packets, PKT3(PKT3_RELEASE_MEM, 6, 0));
    
    // Event type and flags
    // EVENT_TYPE=CACHE_FLUSH_AND_INV_EVENT, EVENT_INDEX=0x5
    uint32_t event_cntl = 
        (0x5 << 0) |   // EVENT_INDEX
        (0x2E << 8);   // EVENT_TYPE (CS_DONE or similar)
    da_append(packets, event_cntl);
    
    // Data selection: Send 32-bit fence value
    uint32_t data_sel = 1; // SEL_32_BIT
    da_append(packets, data_sel);
    
    // Address LO
    da_append(packets, (uint32_t)(va & 0xFFFFFFFF));
    
    // Address HI
    da_append(packets, (uint32_t)(va >> 32));
    
    // Data LO (fence value)
    da_append(packets, fence_value);
    
    // Data HI
    da_append(packets, 0);
    
    // INT_SEL (no interrupt)
    da_append(packets, 0);
}

void build_compute_dispatch(pkt3_packets_t* packets,
                            uint64_t code_va,
                            uint32_t rsrc1,
                            uint32_t rsrc2,
                            uint32_t rsrc3,
                            uint32_t threads_x,
                            uint32_t threads_y,
                            uint32_t threads_z,
                            uint32_t groups_x,
                            uint32_t groups_y,
                            uint32_t groups_z) {
    HDB_ASSERT((code_va & 0xFF) == 0, "shader code address must be 256-byte aligned");

    // Memory barrier before shader execution
    pkt3_acquire_mem(packets);

    // Set shader program address (low/high)
    uint32_t pgm_lo = (uint32_t)(code_va >> 8);  // Bits [39:8]
    uint32_t pgm_hi = (uint32_t)(code_va >> 40); // Bits [47:40]
    
    pkt3_set_sh_reg(packets, R_00B830_COMPUTE_PGM_LO, pgm_lo);
    pkt3_set_sh_reg(packets, R_00B834_COMPUTE_PGM_HI, pgm_hi);

    // Set resource configuration
    pkt3_set_sh_reg(packets, R_00B848_COMPUTE_PGM_RSRC1, rsrc1);
    pkt3_set_sh_reg(packets, R_00B84C_COMPUTE_PGM_RSRC2, rsrc2);
    pkt3_set_sh_reg(packets, R_00B8A0_COMPUTE_PGM_RSRC3, rsrc3);

    // Set workgroup thread dimensions (threads per group)
    pkt3_set_sh_reg(packets, R_00B81C_COMPUTE_NUM_THREAD_X, threads_x);
    pkt3_set_sh_reg(packets, R_00B820_COMPUTE_NUM_THREAD_Y, threads_y);
    pkt3_set_sh_reg(packets, R_00B824_COMPUTE_NUM_THREAD_Z, threads_z);

    // Dispatch initiator: enable compute shader, force start at 000
    uint32_t dispatch_initiator = 
        COMPUTE_DISPATCH_INITIATOR_COMPUTE_SHADER_EN |
        COMPUTE_DISPATCH_INITIATOR_FORCE_START_AT_000;

    // Issue dispatch
    pkt3_dispatch_direct(packets, groups_x, groups_y, groups_z, dispatch_initiator);
}
