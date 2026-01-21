# Implementation Status

This document tracks the current implementation status of the AMD GPU Debugger project.

## ✅ Implemented (Ready for Testing on Real Hardware)

The following core subsystems have been implemented and are ready for testing with actual RDNA3 hardware:

### 1. Device Initialization & Management (`src/amdgpu_device.c`)
- DRM device open and context creation
- GPU info query and validation
- Command submission and fence synchronization
- Proper cleanup and resource deallocation

### 2. Buffer Object Management (`src/bo.c`)
- GPU memory allocation (VRAM, GTT, GDS, GWS, OA domains)
- GPU VA range allocation and mapping with custom flags
- Manual IOCTL-based VA mapping for uncached/executable control
- CPU mapping for host-accessible buffers
- Safe upload and cleanup functions with bounds checking

### 3. Register Access Infrastructure (`src/regs.c`)
- debugfs `regs2` file access for privileged MMIO operations
- SRBM/GRBM state setup for VMID/SE/SH selection
- Register read/write helpers with error handling
- TBA/TMA trap handler installation for VMIDs 1-8

### 4. PM4 Command Packet Builders (`src/pm4.c`)
- `PKT3_SET_SH_REG`: Configure shader registers
- `PKT3_DISPATCH_DIRECT`: Issue compute dispatches
- `PKT3_ACQUIRE_MEM`: Pre-shader memory barriers
- `PKT3_RELEASE_MEM`: Post-shader fence writes
- `build_compute_dispatch()`: High-level dispatch helper

### 5. Build System
- Makefile with libdrm dependency detection
- `.gitignore` for build artifacts
- Successful compilation on Ubuntu 24.04 with GCC and libdrm 2.4.122

### 6. Example Shaders
- `examples/nop.gfx11.s`: Minimal no-op shader
- `examples/write_buffer.gfx11.s`: Buffer write test
- `scripts/ll-as.sh`: LLVM assembler wrapper for GFX11

### 7. Documentation
- Comprehensive inline comments with DANGER warnings
- Error handling patterns documented
- Hardware-specific assumptions clearly marked

---

## ⚠️ Partially Implemented (Needs Hardware-Specific Values)

These components are structurally complete but contain **placeholder values** that must be verified/replaced before use on actual hardware:

### 1. GC11 Register Offsets (`src/regs.c`)

**Current State**: Placeholder offsets (e.g., `0x2E00` for TBA_LO)

**CRITICAL ISSUE**: Using incorrect offsets **WILL** cause:
- GPU hangs or resets
- Writes to wrong registers
- System instability

**Required Action**: Verify against:
1. UMR register database for gfx1100/Navi31:
   ```bash
   umr -O bits,follow | grep -E "(TBA|TMA|SQ_CMD)"
   ```
2. Linux kernel `drivers/gpu/drm/amd/include/asic_reg/gc/gc_11_0_*_offset.h`
3. Actual hardware testing via debugfs read operations

**Affected Registers**:
- `REG_SQ_SHADER_TBA_LO`: Currently `0x2E00` (PLACEHOLDER)
- `REG_SQ_SHADER_TBA_HI`: Currently `0x2E01` (PLACEHOLDER)
- `REG_SQ_SHADER_TMA_LO`: Currently `0x2E02` (PLACEHOLDER)
- `REG_SQ_SHADER_TMA_HI`: Currently `0x2E03` (PLACEHOLDER)
- `REG_SQ_CMD`: Currently `0x2D00` (PLACEHOLDER)

### 2. GC Register Base Addresses (`src/amdgpu_device.c`)

**Current State**: Hardcoded to `0x0` (placeholder)

**CRITICAL ISSUE**: Incorrect base address causes all register accesses to fail

**Required Action**: Obtain correct base address via:
1. `amdgpu_query_hw_ip_info()` for IP block base addresses
2. Reading from kernel driver initialization code
3. Device-specific configuration based on `asic_id`

**Code Location**: Line 104 in `src/amdgpu_device.c`
```c
gc_regs_base_addr[0] = 0x0; // PLACEHOLDER - GC base (MUST VERIFY)
```

---

## ❌ Not Yet Implemented (Future Work)

The following critical components are **not implemented** and are required for a functional debugger:

### 1. Trap Handler Shader (GPU-side)

**Purpose**: Save and restore wave state when trap fires

**Requirements**:
- GFX11 assembly to save VGPRs, SGPRs, STATUS, EXEC, VCC, PC to TMA buffer
- TTMP register usage for privileged operations (TTMP0/1 contains PC on trap)
- CPU-GPU handshake protocol via uncached TMA buffer
- Wait loop for CPU signal before resuming
- `s_rfe_b64` to return from exception
- PC adjustment for `s_trap` vs hardware traps

**Reference**: See README section 5 for detailed architecture

**Estimated Effort**: 200-300 lines of carefully crafted GFX11 assembly

### 2. CPU-GPU Synchronization Loop

**Purpose**: Orchestrate trap handling from CPU side

**Requirements**:
- Allocate uncached GTT buffer for TMA (CPU-GPU shared memory)
- Poll TMA buffer for trap readiness flag with timeout
- Decode hardware IDs (`HW_ID1`, `HW_ID2`) to identify trapped wave
- Issue `SQ_CMD` halt command via debugfs to freeze wave
- Read saved register state from TMA buffer
- Present state to user (CLI/TUI)
- Modify registers if requested
- Clear readiness flag
- Issue `SQ_CMD` resume command
- Wait for wave to continue or complete

**Estimated Effort**: 400-500 lines of C code

### 3. Debugger CLI/TUI

**Purpose**: User interface for debugging operations

**Required Commands**:
- `step`: Single-step execution (set `DEBUG_MODE` in RSRC1, dispatch, wait for trap)
- `continue`: Resume from breakpoint/trap
- `breakpoint <addr>`: Set PC-based breakpoint (modify code or use trap table)
- `watchpoint <addr>`: Set data watchpoint (use SQ watch registers)
- `registers [vgpr|sgpr]`: Display register state from TMA buffer
- `disasm [<addr>]`: Disassemble shader code at PC or given address
- `source`: Map PC to source line via ACO debug info
- `info waves`: List all trapped waves
- `select <wave_id>`: Switch active wave context

**Estimated Effort**: 600-800 lines with GNU readline integration

### 4. SPIR-V → GFX11 Compilation (Optional)

**Purpose**: Allow debugging of SPIR-V shaders without manual assembly

**Current State**: Stub returning `-ENOSYS`

**Requirements**:
- Link against Mesa RADV library
- Setup Vulkan instance/device in `null_winsys` mode
- Call `radv_compile_cs()` for compute shaders
- Extract binary, `rsrc1/2/3`, and debug info
- Map SPIR-V line numbers to instruction offsets

**Dependencies**:
- Mesa built with RADV/ACO enabled
- Vulkan headers (`vulkan/vulkan.h`)
- RADV internal headers (from Mesa tree)

**Estimated Effort**: 300-400 lines, but heavyweight Mesa dependency

### 5. Page Table Walker (Optional)

**Purpose**: Decode GPU VA → PA for debugging memory issues

**Requirements**:
- Read `GCMC_VM_FB_*` registers for page table base
- Read `GCVM_CONTEXT*_PAGE_TABLE_*` for VMID-specific tables
- Walk 4-level page table (PDE, PTE) to translate address
- Display PDE/PTE flags (valid, system, coherent, R/W/X)

**Use Cases**:
- Debug page faults
- Validate buffer VA mappings
- Inspect memory attributes

**Estimated Effort**: 200-300 lines

---

## 🛠 Next Steps for Contributors

To bring this project from proof-of-concept to functional debugger:

### Step 1: Obtain Correct Register Values (CRITICAL)

**On a system with Navi31/32/33 (RDNA3)**:

1. Install UMR (User Mode Register debugger):
   ```bash
   sudo apt install umr
   # OR build from source: https://gitlab.freedesktop.org/tomstdenis/umr
   ```

2. Dump register offsets for SQ (Shader Processor):
   ```bash
   sudo umr -O bits,follow | grep -E "(SQ_SHADER_TBA|SQ_SHADER_TMA|SQ_CMD)"
   ```

3. Update `src/regs.c` with actual offsets

4. Query GC register base:
   ```bash
   sudo umr -wa gfx.0.0
   # Look for base address in output
   ```

5. Update `src/amdgpu_device.c` with actual base address

**Alternatively**, extract from kernel sources:
```bash
# In Linux kernel tree
grep -r "mmSQ_SHADER_TBA" drivers/gpu/drm/amd/include/asic_reg/gc/
```

### Step 2: Implement Minimal Trap Handler

1. Create `src/trap_handler.gfx11.s` with basic save/restore:
   ```asm
   // Minimal trap handler example
   .text
   .globl trap_handler
   trap_handler:
       // Save STATUS to TTMP12
       s_getreg_b32 ttmp12, hwreg(HW_REG_STATUS)
       
       // Save EXEC to TTMP[10:11]
       s_mov_b64 ttmp[10:11], exec
       
       // Save VGPRs to TMA buffer (needs TMA query)
       // ... (complex, see README for details)
       
       // Set flag: wave is trapped
       // ... (write to TMA buffer)
       
       // Spin-wait for CPU to signal resume
   spin:
       s_load_dword s0, [TMA_RESUME_FLAG_ADDR]
       s_waitcnt lgkmcnt(0)
       s_cmp_eq_u32 s0, 0
       s_cbranch_scc1 spin
       
       // Restore state
       // ...
       
       // Return from exception
       s_rfe_b64 [ttmp0, ttmp1]  // PC is in TTMP[0:1]
   ```

2. Compile with:
   ```bash
   ./scripts/ll-as.sh src/trap_handler.gfx11.s
   ```

3. Load trap handler into GPU BO and install via TBA/TMA

### Step 3: Test on Real Hardware

1. Build project:
   ```bash
   make clean && make
   ```

2. Run as root (for debugfs access):
   ```bash
   sudo ./hdb --test-init
   ```

3. Test minimal dispatch with trap:
   ```c
   // In debugger_main.c
   // 1. Allocate trap handler BO
   // 2. Upload trap_handler.bin
   // 3. Call dev_setup_trap_handler(dev, handler_va, tma_va)
   // 4. Dispatch nop.gfx11.s with TRAP_ON_START in RSRC3
   // 5. Poll TMA buffer for trap flag
   ```

4. Validate:
   - Trap handler fires (flag written to TMA)
   - Wave halts (does not complete immediately)
   - Resume works (clear flag, wave continues, s_endpgm)

### Step 4: Iterate and Expand

1. Add more trap handler features (VGPR/SGPR save)
2. Implement basic CLI loop
3. Test step/continue commands
4. Add breakpoint support
5. Integrate ACO debug info

---

## Known Issues & Limitations

### Security & System Impact

1. **Global VMID Impact**: TBA/TMA are set for VMIDs 1-8, affecting ALL processes
   - Other GPU workloads will have trap handler enabled
   - If TBA/TMA are invalid in another process's VA space, that process crashes

2. **Root Required**: debugfs `regs2` requires `CAP_SYS_ADMIN`
   - Cannot run as unprivileged user
   - Consider adding capability-based wrapper

3. **GPU Hang Risk**: Incorrect PM4 packets or register writes can:
   - Hang the GPU indefinitely
   - Require system reboot to recover
   - Affect all GPU users on the system

4. **No Concurrent Debuggers**: Only one debugger instance should run at a time
   - No locking mechanism implemented
   - Multiple debuggers will conflict on TBA/TMA

### Functional Limitations

1. **Single-Wave Debugging**: Current design targets one wave at a time
   - Multi-wave dispatches not well-supported
   - HW_ID filtering needed to select specific wave

2. **No Source-Level Debug**: ACO debug info integration not implemented
   - Only assembly-level debugging available
   - Line number mapping requires SPIR-V compilation

3. **No Graphics Shaders**: Compute-only
   - Vertex/fragment shaders not supported
   - Would require different PM4 setup

4. **Placeholder Hardware Values**: Cannot use on real hardware without updates
   - Register offsets are placeholders
   - Will hang GPU if used as-is

### Build Dependencies

- **libdrm-dev**: Required for compilation (`sudo apt install libdrm-dev`)
- **RDNA3 GPU**: Required for execution (Navi31/32/33)
- **Linux kernel 5.15+**: For modern amdgpu driver
- **debugfs mounted**: `mount -t debugfs none /sys/kernel/debug`

---

## Testing Strategy

### Without RDNA3 Hardware (CI-friendly)

- ✅ Build test: Compiles without errors
- ✅ Link test: All symbols resolve
- ✅ Basic execution: `./hdb --help` runs
- ✅ Init test: `./hdb --test-init` handles missing GPU gracefully

### With RDNA3 Hardware (Manual)

1. **Phase 1: Device Init**
   - DRM open succeeds
   - Device info query works
   - Context creation succeeds

2. **Phase 2: BO Allocation**
   - Allocate VRAM BO
   - Allocate GTT BO (uncached)
   - CPU mapping works
   - Upload test data

3. **Phase 3: Register Access**
   - debugfs regs2 opens successfully
   - Read known register (e.g., GRBM_STATUS)
   - Write/read-back test on safe register

4. **Phase 4: Compute Dispatch**
   - Upload nop.gfx11.s binary to BO
   - Build PM4 packet stream
   - Submit indirect buffer
   - Wait for fence (should complete immediately)

5. **Phase 5: Trap Handler**
   - Install TBA/TMA
   - Dispatch shader with TRAP_ON_START
   - Verify trap flag written to TMA
   - Issue resume command
   - Verify wave completes

---

## Performance Considerations

- **debugfs register access**: Slow (kernel context switch per access)
  - Minimize register reads/writes in hot paths
  - Consider batching if API allows

- **TMA buffer polling**: Busy-wait on uncached GTT
  - Add configurable poll interval
  - Use futex or semaphore for CPU-GPU sync (future)

- **PM4 packet construction**: Dynamic allocation in `da_append`
  - Pre-allocate packet buffer if size known
  - Reuse buffers across dispatches

---

## Future Enhancements

### Performance

- [ ] Use `RELEASE_MEM` event writes for fence signaling instead of polling
- [ ] Batch register operations when possible
- [ ] Cache register reads for read-modify-write operations

### Features

- [ ] Multi-wave debugging with HW_ID filtering
- [ ] Conditional breakpoints (break if VGPR[0] == value)
- [ ] Data watchpoints using SQ watch registers
- [ ] DWARF-like source mapping via ACO debug info
- [ ] Remote debugging over TCP/IP
- [ ] Integration with existing debuggers (GDB protocol)

### Usability

- [ ] TUI with ncurses (split-screen assembly/registers)
- [ ] Automatic register offset detection from UMR database
- [ ] Configuration file for device-specific values
- [ ] Launch multiple waves and debug race conditions
- [ ] Record/replay of wave execution

### Safety

- [ ] Detect and warn if other GPU processes are active
- [ ] Implement VMID locking/reservation
- [ ] Graceful GPU reset recovery
- [ ] Timeout on all GPU waits
- [ ] Validate PM4 packets before submission

---

## Resources

- **UMR**: https://gitlab.freedesktop.org/tomstdenis/umr
- **AMDGPU Driver**: https://gitlab.freedesktop.org/drm/amd/-/tree/amd-mainline/drivers/gpu/drm/amd
- **Mesa RADV**: https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/amd/vulkan
- **RDNA3 ISA**: AMD RDNA™ 3 Instruction Set Architecture (Reference Guide)
- **Linux DRM**: https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html

---

## Contact & Contributing

This is an experimental research project. Contributions welcome!

- Open issues for bugs or feature requests
- Submit PRs for fixes or enhancements
- Document any hardware-specific findings

**IMPORTANT**: Before merging hardware-specific values (register offsets, base addresses), include:
1. ASIC name (e.g., "Navi31, device_id 0x744C")
2. Driver version
3. How values were obtained (UMR dump, kernel source, etc.)
4. Test results on actual hardware
