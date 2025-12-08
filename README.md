# AMD GPU Debugger (Experimental RDNA3 Wavefront Debugger)

> **Status**: Research prototype / PoC  
> **Target**: AMD RDNA3 (gfx11, e.g. RX 7900 XTX) on Linux  
> **Goal**: CPU-like debugging experience for GPU compute waves (pause, inspect, step, resume)

## Why this exists

CPU debugging is a solved problem: you can single-step, inspect registers, set breakpoints, and watch memory changes.  
On GPUs, especially with massively parallel wavefronts, this experience is still painful.

While tools like **rocgdb** exist for AMD’s ROCm environment, they are highly scoped to a specific stack.  
This repository explores a **low-level debugger for AMD GPUs** by:

- Talking directly to `amdgpu` via **DRM + libdrm**
- Using **debugfs** (`regs2`) and the **UMR** register database
- Installing a **trap handler** via `TBA` / `TMA` on RDNA3
- Implementing a CPU–GPU handshake loop that:
  - Traps a wave
  - Saves its state to memory
  - Lets the CPU inspect / modify that state
  - Restores and resumes execution

The design is heavily inspired by the public work of Marcell Kiss and AMD’s own driver stack (RADV, amdgpu, amdkfd, UMR).

---

## High-Level Architecture

The debugger is split into several layers:

1. **Device / Context Setup (libdrm + DRM IOCTLs)**
2. **Buffer Objects (BOs) and GPU/CPU VA Mapping**
3. **Command Submission (PM4 packets, indirect buffers)**
4. **Trap Handler (GPU-side shader that saves/restores context)**
5. **CPU–GPU Synchronization Loop**
6. **SPIR-V → GFX11 Binary Compilation (via RADV/ACO null winsys)**
7. **Debugger Features (step, breakpoints, watchpoints, source mapping)**
8. **Page Table Walking (optional: GFX11 VA → PA decoding)**

A rough data flow:

![Debugger Architecture](image.png)

```

---

## 1. Device & Context Setup

The entry point is opening the DRM node and initializing the AMDGPU device:

- Open `/dev/dri/cardX` with `open(2)`
- Call `amdgpu_device_initialize` from **libdrm** to get a `amdgpu_device_handle`
- Create a command submission context via `amdgpu_cs_ctx_create`

This establishes a user-mode context (UMD) that maps down to a **VMID** in the kernel mode driver (KMD) and sets up GPU page tables for this process.

---

## 2. Buffer Objects (BOs) and Mapping

We need at least:

- A **code buffer** for GPU machine code
- A **command buffer** hosting PM4 packets (indirect buffer)
- One or more **data buffers** (for scratch, trap buffer, output, etc.)

### Allocation

We allocate BOs via `amdgpu_bo_alloc` with different domains and flags depending on usage:

```c
void bo_alloc(amdgpu_t* dev, size_t size, uint32_t domain, bool uncached, amdgpu_bo_t* bo) {
    int32_t ret        = -1;
    uint32_t alignment = 0;
    uint32_t flags     = 0;
    size_t actual_size = 0;

    amdgpu_bo_handle bo_handle = NULL;
    amdgpu_va_handle va_handle = NULL;
    uint64_t va_addr           = 0;
    void* host_addr            = NULL;

    if (domain != AMDGPU_GEM_DOMAIN_GWS &&
        domain != AMDGPU_GEM_DOMAIN_GDS &&
        domain != AMDGPU_GEM_DOMAIN_OA) {
        actual_size = (size + 4096 - 1) & 0xFFFFFFFFFFFFF000ULL;
        alignment   = 4096;
        flags       = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
                      AMDGPU_GEM_CREATE_VRAM_CLEARED |
                      AMDGPU_GEM_CREATE_VM_ALWAYS_VALID;

        if (uncached && domain == AMDGPU_GEM_DOMAIN_GTT)
            flags |= AMDGPU_GEM_CREATE_CPU_GTT_USWC;
    } else {
        actual_size = size;
        alignment   = 1;
        flags       = AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
    }

    struct amdgpu_bo_alloc_request req = {
        .alloc_size     = actual_size,
        .phys_alignment = alignment,
        .preferred_heap = domain,
        .flags          = flags,
    };

    ret = amdgpu_bo_alloc(dev->dev_handle, &req, &bo_handle);
    HDB_ASSERT(!ret, "can't allocate bo");

    /* GPU VA mapping and optional CPU mapping handled below… */

    ...
}
```

### GPU VA Mapping (manual IOCTL)

Instead of `amdgpu_bo_va_op`, we directly issue `DRM_AMDGPU_GEM_VA` to control page attributes (e.g. uncached):

```c
uint32_t kms_handle = 0;
amdgpu_bo_export(bo_handle, amdgpu_bo_handle_type_kms, &kms_handle);

ret = amdgpu_va_range_alloc(dev->dev_handle,
                            amdgpu_gpu_va_range_general,
                            actual_size,
                            4096,
                            0,
                            &va_addr,
                            &va_handle,
                            0);
HDB_ASSERT(!ret, "can't allocate VA");

uint64_t map_flags =
    AMDGPU_VM_PAGE_EXECUTABLE |
    AMDGPU_VM_PAGE_READABLE   |
    AMDGPU_VM_PAGE_WRITEABLE;

if (uncached)
    map_flags |= AMDGPU_VM_MTYPE_UC | AMDGPU_VM_PAGE_NOALLOC;

struct drm_amdgpu_gem_va va = {
    .handle       = kms_handle,
    .operation    = AMDGPU_VA_OP_MAP,
    .flags        = map_flags,
    .va_address   = va_addr,
    .offset_in_bo = 0,
    .map_size     = actual_size,
};

ret = drm_ioctl_write_read(dev->drm_fd, DRM_AMDGPU_GEM_VA, &va, sizeof(va));
HDB_ASSERT(!ret, "can't map bo in GPU space");

if (flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED) {
    ret = amdgpu_bo_cpu_map(bo_handle, &host_addr);
    HDB_ASSERT(!ret, "can't map bo in CPU space");
    memset(host_addr, 0x0, actual_size);
}
```

---

## 3. Command Submission (PM4 Packets)

The RADV driver uses **PM4 packets** to program registers and dispatch workloads.  
We do the same manually.

### Set Shader Registers

Shader configuration registers (`rsrc1`, `rsrc2`, `rsrc3`, `pgm_lo`, `pgm_hi`, `num_thread_*`) are programmed with `PKT3_SET_SH_REG` packets:

```c
void pkt3_set_sh_reg(pkt3_packets_t* packets, uint32_t reg, uint32_t value) {
    HDB_ASSERT(reg >= SI_SH_REG_OFFSET && reg < SI_SH_REG_END,
               "can't set register outside sh registers span");

    da_append(packets, PKT3(PKT3_SET_SH_REG, 1, 0));          // header
    da_append(packets, (reg - SI_SH_REG_OFFSET) / 4);         // register offset
    da_append(packets, value);                                // value
}
```

### Dispatch

For the minimal case (one wave, one workgroup):

```c
da_append(&pkt3_packets, PKT3(PKT3_DISPATCH_DIRECT, 3, 0) | PKT3_SHADER_TYPE_S(1));
da_append(&pkt3_packets, 1u); // x
da_append(&pkt3_packets, 1u); // y
da_append(&pkt3_packets, 1u); // z
da_append(&pkt3_packets, dispatch_initiator);
```

### Submit Indirect Buffer

The PM4 packet stream is uploaded into an **indirect buffer** (`IB`) and submitted via `amdgpu_cs_submit`:

```c
void dev_submit(amdgpu_t* dev,
                pkt3_packets_t* packets,
                amdgpu_bo_handle* buffers,
                uint32_t buffers_count,
                amdgpu_submit_t* submit) {

    int32_t ret = -1;
    amdgpu_bo_t ib = {0};

    bo_alloc(dev, pkt3_size(packets), AMDGPU_GEM_DOMAIN_GTT, false, &ib);
    bo_upload(&ib, packets->data, pkt3_size(packets));

    amdgpu_bo_handle* bo_handles =
        malloc(sizeof(amdgpu_bo_handle) * (buffers_count + 1));

    bo_handles[0] = ib.bo_handle;
    for_range(i, 0, buffers_count) {
        bo_handles[i + 1] = buffers[i];
    }

    amdgpu_bo_list_handle bo_list = NULL;
    ret = amdgpu_bo_list_create(dev->dev_handle,
                                buffers_count + 1,
                                bo_handles,
                                NULL,
                                &bo_list);
    HDB_ASSERT(!ret, "can't create a bo list");
    free(bo_handles);

    struct amdgpu_cs_ib_info ib_info = {
        .flags         = 0,
        .ib_mc_address = ib.va_addr,
        .size          = packets->count,
    };

    struct amdgpu_cs_request req = {
        .flags                  = 0,
        .ip_type                = AMDGPU_HW_IP_COMPUTE,
        .ip_instance            = 0,
        .ring                   = 0,
        .resources              = bo_list,
        .number_of_dependencies = 0,
        .dependencies           = NULL,
        .number_of_ibs          = 1,
        .ibs                    = &ib_info,
        .seq_no                 = 0,
        .fence_info             = {0},
    };

    ret = amdgpu_cs_submit(dev->ctx_handle, 0, &req, 1);
    HDB_ASSERT(!ret, "can't submit indirect buffer request");

    *submit = (amdgpu_submit_t){
        .ib      = ib,
        .bo_list = bo_list,
        .fence   = {
            .context    = dev->ctx_handle,
            .ip_type    = AMDGPU_HW_IP_COMPUTE,
            .ip_instance= 0,
            .ring       = 0,
            .fence      = req.seq_no,
        },
    };
}
```

---

## 4. Trap Handler via TBA/TMA (RDNA3)

RDNA3 exposes two important shader trap registers:

- **TBA** (Trap Base Address): pointer to trap handler program. Bit 63 = trap enabled.
- **TMA** (Trap Memory Address): pointer to scratch buffer used by the trap.

These are **privileged** registers, so user space cannot directly write them.  
Instead, we use:

- `debugfs`: `/sys/kernel/debug/dri/<idx>/regs2`
- `ioctl(AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2)` with:

```c
typedef struct amdgpu_debugfs_regs2_iocdata_v2 {
    __u32 use_srbm, use_grbm, pg_lock;
    struct {
        __u32 se, sh, instance;
    } grbm;
    struct {
        __u32 me, pipe, queue, vmid;
    } srbm;
    __u32 xcc_id;
} regs2_ioc_data_t;
```

We then seek inside `regs2` to the MMIO offset of the desired register and read/write it.

### Register Access Helper

```c
void dev_op_reg32(amdgpu_t* dev,
                  gc_11_reg_t reg,
                  regs2_ioc_data_t ioc_data,
                  reg_32_op_t op,
                  uint32_t* value) {
    int32_t ret = 0;

    reg_info_t reg_info    = gc_11_regs_infos[reg];
    uint64_t reg_offset    = gc_11_regs_offsets[reg];
    uint64_t base_offset   = dev->gc_regs_base_addr[reg_info.soc_index];
    uint64_t total_offset  = reg_offset + base_offset;

    if (reg_info.type == REG_MMIO)
        total_offset *= 4;

    ret = hdb_ioctl(dev->regs2_fd,
                    AMDGPU_DEBUGFS_REGS2_IOC_SET_STATE_V2,
                    &ioc_data);
    HDB_ASSERT(!ret, "Failed to set registers state");

    size_t size = lseek(dev->regs2_fd, total_offset, SEEK_SET);
    HDB_ASSERT(size == total_offset, "Failed to seek register address");

    switch (op) {
    case REG_OP_READ:
        size = read(dev->regs2_fd, value, 4);
        break;
    case REG_OP_WRITE:
        size = write(dev->regs2_fd, value, 4);
        break;
    default:
        HDB_ASSERT(false, "unsupported op");
    }

    HDB_ASSERT(size == 4, "Failed to write/read the values to/from the register");
}
```

### Installing Trap Handler (TBA/TMA for all VMIDs)

VMIDs are assigned at dispatch time, so we pessimistically program multiple VMIDs:

```c
void dev_setup_trap_handler(amdgpu_t* dev, uint64_t tba, uint64_t tma) {
    reg_sq_shader_tma_lo_t tma_lo = { .raw = (uint32_t)(tma) };
    reg_sq_shader_tma_hi_t tma_hi = { .raw = (uint32_t)(tma >> 32) };

    reg_sq_shader_tba_lo_t tba_lo = { .raw = (uint32_t)(tba >> 8) };
    reg_sq_shader_tba_hi_t tba_hi = { .raw = (uint32_t)(tba >> 40) };

    tba_hi.trap_en = 1;

    regs2_ioc_data_t ioc_data = {
        .use_srbm = 1,
        .xcc_id   = (uint32_t)-1,
    };

    for_range(i, 1, 9) {
        ioc_data.srbm.vmid = i;

        dev_op_reg32(dev, REG_SQ_SHADER_TBA_LO, ioc_data, REG_OP_WRITE, &tba_lo.raw);
        dev_op_reg32(dev, REG_SQ_SHADER_TBA_HI, ioc_data, REG_OP_WRITE, &tba_hi.raw);

        dev_op_reg32(dev, REG_SQ_SHADER_TMA_LO, ioc_data, REG_OP_WRITE, &tma_lo.raw);
        dev_op_reg32(dev, REG_SQ_SHADER_TMA_HI, ioc_data, REG_OP_WRITE, &tma_hi.raw);
    }
}
```

> ⚠️ This can affect other processes sharing those VMIDs by enabling trap handlers with addresses valid only in our VA space. In practice most workloads don’t use `s_trap` or exception-based traps, but it’s still invasive.

---

## 5. Trap Handler Shader

The trap handler is a **privileged compute shader**:

- Runs when:
  - `s_trap` is executed, or
  - Certain exceptions occur, or
  - `TRAP_ON_START` / `DEBUG_MODE` bits are set in `RSRC`.
- Uses **TTMP registers** to hold scratch state.
- Saves:
  - `STATUS`, `EXEC`, `VCC`
  - VGPRs and SGPRs into TMA buffer
  - Hardware IDs and program counter
- Spins, waiting for CPU to signal continuation.

### Core Idea

1. Save stable state (STATUS, EXEC, VCC).
2. Query TMA pointer via `s_sendmsg_rtn_b64`.
3. Save VGPRs using `global_store_addtid_b32` (each lane gets its own slot).
4. Save SGPRs with `global_store_b32` (single lane).
5. Write HW IDs, original masks, and PC to TMA.
6. Spin until CPU writes a resume flag.
7. Restore all registers.
8. Adjust PC for `s_trap` vs hardware traps.
9. Return via `s_rfe_b64`.

(See `src/trap_handler.s` for the full assembly.)

---

## 6. CPU–GPU Synchronization Loop

On the CPU side, we create a tight loop:

1. Enable trap handler (program TBA/TMA).
2. Dispatch shader.
3. Poll uncached TMA buffer for “ready” flag.
4. Once written:
   - Decode HW IDs (`HW_ID1`, `HW_ID2`).
   - Halt the wave via `SQ_CMD`.
   - Read saved VGPRs/SGPRs/PC from TMA.
   - Present state to the user (CLI / TUI).
5. When the user chooses to continue:
   - Optionally modify registers, masks, or buffer contents.
   - Clear the “ready” flag.
   - Write resume command:
     ```c
     reg_sq_cmd_t halt_cmd = {
         .cmd  = 1,
         .mode = 0, // resume
         .data = 1,
     };
     dev_op_reg32(&amdgpu, REG_SQ_CMD, ioc_data, REG_OP_WRITE, &halt_cmd.raw);
     ```
   - The trap handler sees the change, restores context, and returns.

---

## 7. SPIR-V → GFX11 Binary via RADV / ACO

We don’t want users to manually build GFX11 binaries.  
Instead, we reuse RADV’s ACO compiler in `null_winsys` mode:

- Set `RADV_FORCE_FAMILY=navi31` to simulate a Navi31 device.
- Use RADV instance/device creation APIs.
- Compile a SPIR-V compute module with `radv_compile_cs`.
- Extract:
  - Code binary, `rsrc1/2/3`, debug info.

Example helper:

```c
int32_t hdb_compile_spirv_to_bin(const void* spirv_binary,
                                 size_t size,
                                 hdb_shader_stage_t stage,
                                 hdb_shader_t* shader) {
    setenv("RADV_FORCE_FAMILY", "navi31", 1);
    setenv("ACO_DEBUG", "nocache,noopt", 1);

    VkInstanceCreateInfo i_cinfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &(VkApplicationInfo){
            .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName   = "HDB Shader Compiler",
            .applicationVersion = 1,
            .pEngineName        = "HDB",
            .engineVersion      = 1,
            .apiVersion         = VK_API_VERSION_1_4,
        },
    };

    VkInstance vk_instance = {};
    radv_CreateInstance(&i_cinfo, NULL, &vk_instance);
    struct radv_instance* instance = radv_instance_from_handle(vk_instance);

    instance->debug_flags |= RADV_DEBUG_NIR_DEBUG_INFO |
                             RADV_DEBUG_NO_CACHE        |
                             RADV_DEBUG_INFO;

    uint32_t n = 1;
    VkPhysicalDevice vk_pdev = {};
    instance->vk.dispatch_table.EnumeratePhysicalDevices(vk_instance, &n, &vk_pdev);

    struct radv_physical_device* pdev = radv_physical_device_from_handle(vk_pdev);
    pdev->use_llvm = false;

    VkDeviceCreateInfo d_cinfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkDevice vk_dev = {};
    pdev->vk.dispatch_table.CreateDevice(vk_pdev, &d_cinfo, NULL, &vk_dev);

    struct radv_device* dev = radv_device_from_handle(vk_dev);

    struct radv_shader_stage radv_stage = {
        .spirv.data = spirv_binary,
        .spirv.size = size,
        .entrypoint = "main",
        .stage      = MESA_SHADER_COMPUTE,
        .layout = {
            .push_constant_size = 16,
        },
        .key = {
            .optimisations_disabled = true,
        },
    };

    struct radv_shader_binary* cs_bin = NULL;
    struct radv_shader* cs_shader =
        radv_compile_cs(dev, NULL, &radv_stage, true, true, false, true, &cs_bin);

    *shader = (hdb_shader_t){
        .bin              = cs_shader->code,
        .bin_size         = cs_shader->code_size,
        .rsrc1            = cs_shader->config.rsrc1,
        .rsrc2            = cs_shader->config.rsrc2,
        .rsrc3            = cs_shader->config.rsrc3,
        .debug_info       = cs_shader->debug_info,
        .debug_info_count = cs_shader->debug_info_count,
    };

    return 0;
}
```

This allows `hdb` to accept SPIR-V modules as input instead of hand-written GFX assembly.

---

## 8. Debugger Features (Planned / Partial)

### Stepping

- Use `DEBUG_MODE` (RSRC1) + `TRAP_ON_START` (RSRC3):
  - `DEBUG_MODE` → trap after every instruction.
  - `TRAP_ON_START` → trap before first instruction.
- Implementation strategy:
  - For single-step: set `DEBUG_MODE`, resume, trap again after one instruction, clear flag if needed.

### Breakpoints

- We know the base address of the code buffer and each instruction size.
- Compute PC offsets for breakpoint addresses.
- Maintain a table of breakpoints in TMA or another buffer.
- In trap handler, do a binary search on the PC offset to decide if we should pause.

### Source Mapping

- ACO emits detailed debug info mapping instruction offsets ↔ source lines.
- Given:
  - `pc` (from TTMP[0:1])
  - `code_base` (code buffer GPU VA)
- Compute offset: `pc - code_base` and look it up in debug info.
- Present:
  - Disassembled instruction
  - Source file:line

### Watchpoints

- Two approaches:
  1. **Page-based**: mark pages as protected; GPU faults go through trap handler.
  2. **SQ_* watch registers**: e.g.
     ```c
     typedef union {
         struct {
             uint32_t addr: 16;
         };
         uint32_t raw;
     } reg_sq_watch0_addr_h_t;
     ```
  - Details TBD; hardware supports native watch locations.

### Vulkan Integration (Future)

The long-term vision is to integrate with **RADV** as a Vulkan UMD:

- Hook into RADV’s dispatch paths (compute/graphics).
- Let the debugger:
  - Pause on frame boundaries.
  - Attach to specific dispatches.
  - Access descriptors, buffers, textures, push constants directly.

For now, this PoC focuses on **compute shaders on a custom UMD**.

---

## 9. Bonus: GFX11 Page Table Walking

The repository also includes a partial VA → PA decoder for GFX11, inspired by UMR, for debugging virtual memory issues.

- PDE/PTE bitfields:
  - `valid`, `system`, `coherent`, `execute`, `read`, `write`, fragments, etc.
- Uses:
  - `GCMC_VM_FB_*` registers
  - `GCVM_CONTEXT*_PAGE_TABLE_*` registers
- Walks the page tables to compute final physical addresses for a given virtual address and VMID.

---

## 10. Building & Running

> **Note:** These are indicative steps; adjust to your environment.

### Dependencies

- Linux (with AMDGPU kernel driver and debugfs enabled)
- RDNA3 GPU (gfx11, e.g. RX 7900 XTX)
- `libdrm` with AMDGPU support
- Mesa built with RADV + ACO
- `clang` (for GFX11 assembly)
- Access to debugfs: `/sys/kernel/debug/dri/*`

### Example Makefile (minimal)

```makefile
CC      := gcc
CFLAGS  := -O2 -g -Wall -Wextra -std=gnu11 \
           $(shell pkg-config --cflags libdrm_amdgpu)
LDFLAGS := $(shell pkg-config --libs libdrm_amdgpu)

SRC := src/amdgpu_device.c src/bo.c src/regs.c src/spirv_compile.c src/debugger_main.c
OBJ := $(SRC:.c=.o)

all: hdb

hdb: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(OBJ) hdb
```

---

## 11. Roadmap

- [ ] Minimal CLI:
  - `hdb run shader.spv`
  - `hdb step`
  - `hdb regs`
  - `hdb disasm`
- [ ] Per-wave selection and filtering via HW_ID
- [ ] Proper breakpoint infrastructure
- [ ] Watchpoints via SQ watch registers
- [ ] Better integration with ACO debug info (types, variable names)
- [ ] Experimental Vulkan integration via RADV

---

## 12. Disclaimer

This is experimental low-level code that:

- Pokes debugfs and MMIO registers
- Can interfere with other GPU contexts
- May hang or reset the GPU while you’re experimenting

Use on **non-production machines** and at your own risk.

---

## License

See [LICENSE](./LICENSE) for details.
