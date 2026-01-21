#pragma once

#include "bo.h"

/**
 * PM4 Packet Type 3 builders for GFX11 (RDNA3).
 * 
 * PM4 (Programmable Microengine Command Processor 4) packets are the
 * low-level command format for AMD GPUs. Type 3 packets are used for
 * most compute and graphics operations.
 * 
 * DANGER: Incorrect packet values can hang or reset the GPU.
 * DANGER: Register offsets vary by ASIC - these are for gfx1100.
 */

/**
 * PKT3 header construction.
 * 
 * @param op: Packet opcode (e.g., PKT3_SET_SH_REG)
 * @param count: Number of dwords following header
 * @param predicate: Predication enable (usually 0)
 * @return: PKT3 header dword
 */
#define PKT3(op, count, predicate) \
    ((3 << 30) | (((op) & 0xFF) << 8) | ((count) & 0x3FFF) | ((predicate) << 31))

/**
 * PKT3 with shader type selection.
 * 
 * @param shader_type: 0=PS, 1=CS, 2=VS, 3=GS, etc.
 */
#define PKT3_SHADER_TYPE_S(shader_type) \
    (((shader_type) & 0x1) << 1)

/**
 * PKT3 opcodes (partial list for compute).
 */
#define PKT3_NOP                        0x10
#define PKT3_SET_BASE                   0x11
#define PKT3_CLEAR_STATE                0x12
#define PKT3_INDEX_BUFFER_SIZE          0x13
#define PKT3_DISPATCH_DIRECT            0x15
#define PKT3_DISPATCH_INDIRECT          0x16
#define PKT3_ATOMIC_MEM                 0x1E
#define PKT3_EVENT_WRITE                0x46
#define PKT3_ACQUIRE_MEM                0x58
#define PKT3_SET_SH_REG                 0x76
#define PKT3_SET_CONTEXT_REG            0x69
#define PKT3_SET_UCONFIG_REG            0x79
#define PKT3_LOAD_SH_REG                0x30
#define PKT3_LOAD_CONTEXT_REG           0x31
#define PKT3_WAIT_REG_MEM               0x3C
#define PKT3_RELEASE_MEM                0x49

/**
 * Register offset ranges for SET_*_REG packets.
 */
#define SI_SH_REG_OFFSET                0x2C00
#define SI_SH_REG_END                   0x3000
#define SI_CONTEXT_REG_OFFSET           0xA000
#define SI_CONTEXT_REG_END              0xB000
#define SI_UCONFIG_REG_OFFSET           0xC000
#define SI_UCONFIG_REG_END              0xD000

/**
 * Compute shader register offsets (for SET_SH_REG).
 * 
 * DANGER: These are gfx1100-specific. Other RDNA3 ASICs may differ.
 */
#define R_00B848_COMPUTE_PGM_RSRC1      0xB848
#define R_00B84C_COMPUTE_PGM_RSRC2      0xB84C
#define R_00B8A0_COMPUTE_PGM_RSRC3      0xB8A0
#define R_00B830_COMPUTE_PGM_LO         0xB830
#define R_00B834_COMPUTE_PGM_HI         0xB834
#define R_00B860_COMPUTE_TMPRING_SIZE   0xB860
#define R_00B854_COMPUTE_DISPATCH_INITIATOR 0xB854
#define R_00B81C_COMPUTE_NUM_THREAD_X   0xB81C
#define R_00B820_COMPUTE_NUM_THREAD_Y   0xB820
#define R_00B824_COMPUTE_NUM_THREAD_Z   0xB824

/**
 * Dispatch initiator flags.
 */
#define COMPUTE_DISPATCH_INITIATOR_COMPUTE_SHADER_EN (1 << 0)
#define COMPUTE_DISPATCH_INITIATOR_FORCE_START_AT_000 (1 << 1)

/**
 * Append PKT3_SET_SH_REG packet.
 * 
 * Sets a shader register (range 0x2C00-0x3000).
 * 
 * @param packets: Packet array to append to
 * @param reg: Register offset (e.g., R_00B848_COMPUTE_PGM_RSRC1)
 * @param value: Register value
 * 
 * DANGER: Invalid register offset will be caught by HDB_ASSERT.
 * DANGER: Invalid register value can hang shader waves.
 */
void pkt3_set_sh_reg(pkt3_packets_t* packets, uint32_t reg, uint32_t value);

/**
 * Append PKT3_DISPATCH_DIRECT packet.
 * 
 * Dispatches a compute shader with specified workgroup dimensions.
 * 
 * @param packets: Packet array to append to
 * @param dim_x: Number of workgroups in X dimension
 * @param dim_y: Number of workgroups in Y dimension
 * @param dim_z: Number of workgroups in Z dimension
 * @param dispatch_initiator: Dispatch flags
 * 
 * DANGER: Shader must be configured via SET_SH_REG before dispatch.
 * DANGER: PGM_LO/HI, RSRC1/2/3, NUM_THREAD_* must all be set correctly.
 */
void pkt3_dispatch_direct(pkt3_packets_t* packets,
                          uint32_t dim_x,
                          uint32_t dim_y,
                          uint32_t dim_z,
                          uint32_t dispatch_initiator);

/**
 * Append PKT3_ACQUIRE_MEM packet.
 * 
 * Memory barrier / cache flush before shader execution.
 * 
 * @param packets: Packet array to append to
 * 
 * DANGER: Incorrect cache coherency can cause data corruption.
 */
void pkt3_acquire_mem(pkt3_packets_t* packets);

/**
 * Append PKT3_RELEASE_MEM packet.
 * 
 * Write fence value to memory after shader completion.
 * 
 * @param packets: Packet array to append to
 * @param va: GPU virtual address to write fence
 * @param fence_value: Value to write (e.g., sequence number)
 * 
 * DANGER: va must be valid and writable in current VMID's address space.
 */
void pkt3_release_mem(pkt3_packets_t* packets, uint64_t va, uint32_t fence_value);

/**
 * Helper: Build compute shader dispatch command buffer.
 * 
 * This is a high-level helper that sets up all necessary registers
 * and dispatches a compute shader.
 * 
 * @param packets: Packet array to build into (must be initialized)
 * @param code_va: GPU VA of shader code
 * @param rsrc1: COMPUTE_PGM_RSRC1 value
 * @param rsrc2: COMPUTE_PGM_RSRC2 value
 * @param rsrc3: COMPUTE_PGM_RSRC3 value
 * @param threads_x: Workgroup size X (e.g., 64 for 1 wave)
 * @param threads_y: Workgroup size Y
 * @param threads_z: Workgroup size Z
 * @param groups_x: Number of workgroups X
 * @param groups_y: Number of workgroups Y
 * @param groups_z: Number of workgroups Z
 * 
 * DANGER: code_va must point to valid GFX11 shader binary.
 * DANGER: rsrc1/2/3 must match shader requirements (VGPRs, SGPRs, etc.).
 */
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
                            uint32_t groups_z);
