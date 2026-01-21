// Minimal GFX11 shader: NOP wave that immediately exits.
//
// This shader does nothing but return, useful for testing
// basic dispatch and trap handler installation.
//
// Compile with: scripts/ll-as.sh examples/nop.gfx11.s
//
// RSRC1/2/3 requirements:
// - RSRC1: VGPRS=0, SGPRS=0, DX10_CLAMP=1, IEEE_MODE=1
// - RSRC2: SCRATCH_EN=0, USER_SGPR=0, TGID_X_EN=0
// - RSRC3: SHARED_VGPR_CNT=0

.text
.globl _start
.type _start, @function

_start:
    // No operation - just end program
    s_endpgm
    
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
