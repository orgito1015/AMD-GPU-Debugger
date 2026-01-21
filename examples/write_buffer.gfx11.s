// GFX11 shader: Write a constant value to a buffer.
//
// This shader writes 0xDEADBEEF to the first 4 bytes of a buffer.
// Useful for testing memory writes and buffer access.
//
// Compile with: scripts/ll-as.sh examples/write_buffer.gfx11.s
//
// User SGPRs (passed via dispatch):
// - s[0:1]: Buffer descriptor pointer (64-bit VA)
//
// RSRC1/2/3 requirements:
// - RSRC1: VGPRS=1, SGPRS=2, DX10_CLAMP=1, IEEE_MODE=1
// - RSRC2: SCRATCH_EN=0, USER_SGPR=2, TGID_X_EN=0
// - RSRC3: SHARED_VGPR_CNT=0

.text
.globl _start
.type _start, @function

_start:
    // Load constant value 0xDEADBEEF into v0
    v_mov_b32 v0, 0xDEADBEEF
    
    // Load buffer base address from user SGPRs into s[2:3]
    // For simplicity, assume buffer VA is in s[0:1]
    // In real usage, this would be passed via kernel argument buffer
    
    // Global store: write v0 to address in s[0:1] + offset 0
    // global_store_b32 [s[0:1]], v0
    // Note: GFX11 uses flat addressing, so we need flat_store
    
    // For now, simplified: assume buffer is at known address
    // Write 0xDEADBEEF to offset 0
    s_load_dwordx2 s[2:3], s[0:1], 0x0  // Load buffer base from descriptor
    s_waitcnt lgkmcnt(0)                 // Wait for scalar load
    
    v_mov_b32 v1, s2                     // Move address low to VGPR
    v_mov_b32 v2, s3                     // Move address high to VGPR
    
    flat_store_b32 v[1:2], v0            // Store constant to buffer
    s_waitcnt vmcnt(0)                   // Wait for store
    
    s_endpgm
    
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
