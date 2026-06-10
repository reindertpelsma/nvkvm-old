// SPDX-License-Identifier: GPL
/*
 * Mode-2 DEBUG bridge: kernel-side cuMemcpyHtoD uprobes.
 *
 * This is diagnostic plumbing for the UVM external-allocation dataplane.  It
 * copies cuMemcpyHtoD source bytes into guest RAM from kernel context and
 * reports <deviceVA, guestGPA, size> to the nvkvm-gpu-emul BAR0 debug aperture.
 * QEMU then resolves later CE reads from that device VA through the reported
 * guest-RAM shadow.  This replaces the older LD_PRELOAD HtoD shadow proof.
 */
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/pci.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/uprobes.h>
#include <linux/workqueue.h>

#define NVKVM_BAR0_UVM_VA_LO      0xFFF520ul
#define NVKVM_BAR0_UVM_VA_HI      0xFFF524ul
#define NVKVM_BAR0_UVM_GPA_LO     0xFFF528ul
#define NVKVM_BAR0_UVM_GPA_HI     0xFFF52cul
#define NVKVM_BAR0_UVM_SIZE_LO    0xFFF530ul
#define NVKVM_BAR0_UVM_SIZE_HI    0xFFF534ul
#define NVKVM_BAR0_UVM_COMMIT     0xFFF538ul
#define NVKVM_BAR0_RMALLOC_HCLIENT      0xFFF540ul
#define NVKVM_BAR0_RMALLOC_HPARENT      0xFFF544ul
#define NVKVM_BAR0_RMALLOC_HOBJECT      0xFFF548ul
#define NVKVM_BAR0_RMALLOC_HCLASS       0xFFF54cul
#define NVKVM_BAR0_RMALLOC_PARAM_GPA_LO 0xFFF550ul
#define NVKVM_BAR0_RMALLOC_PARAM_GPA_HI 0xFFF554ul
#define NVKVM_BAR0_RMALLOC_PARAM_SIZE   0xFFF558ul
#define NVKVM_BAR0_RMALLOC_COMMIT       0xFFF55cul
#define NVKVM_BAR0_UVM_SPAN       0x40ul

#define NVKVM_BRIDGE_SLOTS        1024
#define NVKVM_RMALLOC_REPORT_TOKEN 0x40000000u
#define NVKVM_RMALLOC_MEM_CLASS    0x0000003eu
#define NVKVM_RMALLOC_LOCAL_USER_CLASS 0x00000040u
#define NVKVM_RMALLOC_MEM_PARAMS_SIZE 128u

static char *libcuda_path = "/usr/local/nvidia-guest/lib/libcuda.so.580.159.04";
module_param(libcuda_path, charp, 0444);
MODULE_PARM_DESC(libcuda_path, "Path to guest libcuda.so used for uprobes");

static char *libc_path = "/lib/x86_64-linux-gnu/libc.so.6";
module_param(libc_path, charp, 0444);
MODULE_PARM_DESC(libc_path, "Path to guest libc.so used for ioctl uprobes");

static unsigned long htod_off;
module_param(htod_off, ulong, 0444);
MODULE_PARM_DESC(htod_off, "ELF symbol offset for cuMemcpyHtoD");

static unsigned long htod_v2_off;
module_param(htod_v2_off, ulong, 0444);
MODULE_PARM_DESC(htod_v2_off, "ELF symbol offset for cuMemcpyHtoD_v2");

static unsigned long ioctl_off;
module_param(ioctl_off, ulong, 0444);
MODULE_PARM_DESC(ioctl_off, "ELF symbol offset for libc ioctl");

static unsigned int max_bytes = PAGE_SIZE;
module_param(max_bytes, uint, 0444);
MODULE_PARM_DESC(max_bytes, "Maximum bytes to shadow per HtoD call");

static bool cmd_shadow = true;
module_param(cmd_shadow, bool, 0444);
MODULE_PARM_DESC(cmd_shadow, "Continuously shadow low UVM external command pages");

static unsigned int cmd_scan_pages = 128;
module_param(cmd_scan_pages, uint, 0444);
MODULE_PARM_DESC(cmd_scan_pages, "Maximum command pages to scan per ioctl probe");

static unsigned long cmd_va_limit = 0x1000000000ul;
module_param(cmd_va_limit, ulong, 0444);
MODULE_PARM_DESC(cmd_va_limit, "Only shadow UVM external ranges below this VA");

static unsigned long cmd_range_max = 64ul << 20;
module_param(cmd_range_max, ulong, 0444);
MODULE_PARM_DESC(cmd_range_max, "Maximum bytes tracked per low command range");

static unsigned int cmd_bg_scan_pages = 128;
module_param(cmd_bg_scan_pages, uint, 0444);
MODULE_PARM_DESC(cmd_bg_scan_pages, "Maximum command pages to scan per background pass");

static unsigned int cmd_scan_period_ms = 100;
module_param(cmd_scan_period_ms, uint, 0444);
MODULE_PARM_DESC(cmd_scan_period_ms, "Background command scan period in milliseconds");

static unsigned int cmd_seed_pages = 64;
module_param(cmd_seed_pages, uint, 0444);
MODULE_PARM_DESC(cmd_seed_pages, "Initial low UVM external pages to report even when empty");

static unsigned int cmd_stride_seed_pages = 32;
module_param(cmd_stride_seed_pages, uint, 0444);
MODULE_PARM_DESC(cmd_stride_seed_pages, "Additional strided command windows to report immediately");

static unsigned int cmd_pre_stride_seed_pages = 64;
module_param(cmd_pre_stride_seed_pages, uint, 0444);
MODULE_PARM_DESC(cmd_pre_stride_seed_pages, "Pages to seed from the command window immediately before a tracked range");

static unsigned long cmd_stride_step = 2ul << 20;
module_param(cmd_stride_step, ulong, 0444);
MODULE_PARM_DESC(cmd_stride_step, "Byte spacing between immediately seeded command windows");

static unsigned int cmd_stride_window_pages = 64;
module_param(cmd_stride_window_pages, uint, 0444);
MODULE_PARM_DESC(cmd_stride_window_pages, "Pages to seed from each strided command window");

static unsigned int cmd_neighbor_pages = 64;
module_param(cmd_neighbor_pages, uint, 0444);
MODULE_PARM_DESC(cmd_neighbor_pages, "PFN-backed neighbor pages to report after a non-empty command page");

static bool cmd_report_once = true;
module_param(cmd_report_once, bool, 0444);
MODULE_PARM_DESC(cmd_report_once, "Report each PFN-backed command page only once");

struct nvkvm_slot {
    void *page;
    dma_addr_t gpa;
};

struct nvkvm_probe {
    struct uprobe_consumer consumer;
    struct inode *inode;
    unsigned long offset;
    const char *name;
};

static struct nvkvm_slot slots[NVKVM_BRIDGE_SLOTS];
static atomic_t next_slot = ATOMIC_INIT(0);
static void __iomem *bar;
static struct pci_dev *bar_pdev;
static atomic_t report_count = ATOMIC_INIT(0);
static struct nvkvm_probe probe_htod;
static struct nvkvm_probe probe_htod_v2;
static struct nvkvm_probe probe_ioctl;

#define NVKVM_IOCTL_SLOTS 64
#define NVKVM_CMD_RANGES 256
#define NVKVM_CMD_SEEN   8192
#define NVKVM_UVM_MAP_EXTERNAL_ALLOCATION 33ul
#define NVKVM_UVM_MAP_EXT_OFFSET_OFF 16ul
#define NVKVM_UVM_MAP_EXT_RMFD_OFF 9248ul
#define NVKVM_UVM_MAP_EXT_HCLIENT_OFF 9252ul
#define NVKVM_UVM_MAP_EXT_HMEMORY_OFF 9256ul
#define NVKVM_UVM_MAP_EXT_RM_STATUS_OFF 9260ul

struct nvkvm_ioctl_slot {
    pid_t pid;
    unsigned long req;
    unsigned long arg;
    u32 h_client;
    u32 h_parent;
    u32 h_object;
    u32 h_class;
    u32 param_size;
    int param_slot;
    bool rm_alloc;
};

static struct nvkvm_ioctl_slot ioctl_slots[NVKVM_IOCTL_SLOTS];
static DEFINE_SPINLOCK(ioctl_lock);

struct nvkvm_cmd_range {
    pid_t tgid;
    u64 base;
    u64 len;
    u64 cursor;
};

static struct nvkvm_cmd_range cmd_ranges[NVKVM_CMD_RANGES];
static DEFINE_SPINLOCK(cmd_lock);
static int cmd_replace_next;
static int cmd_scan_next;
static atomic_t cmd_report_count = ATOMIC_INIT(0);
static struct delayed_work cmd_scan_work;
static pid_t cmd_scan_tgids[NVKVM_CMD_RANGES];
static DEFINE_MUTEX(cmd_scratch_lock);
static void *cmd_scratch_page;

struct nvkvm_cmd_seen {
    pid_t tgid;
    u64 va_page;
    u64 gpa_page;
    u32 first;
    bool has_data;
};

static struct nvkvm_cmd_seen cmd_seen[NVKVM_CMD_SEEN];
static DEFINE_SPINLOCK(cmd_seen_lock);
static u32 cmd_seen_next;

static void nvkvm_report_shadow(u64 va, u64 gpa, u64 size, u32 token)
{
    if (!bar)
        return;

    iowrite32((u32)va,       bar + (NVKVM_BAR0_UVM_VA_LO   - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)(va >> 32),  bar + (NVKVM_BAR0_UVM_VA_HI   - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)gpa,      bar + (NVKVM_BAR0_UVM_GPA_LO  - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)(gpa >> 32), bar + (NVKVM_BAR0_UVM_GPA_HI  - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)size,     bar + (NVKVM_BAR0_UVM_SIZE_LO - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)(size >> 32), bar + (NVKVM_BAR0_UVM_SIZE_HI - NVKVM_BAR0_UVM_VA_LO));
    wmb();
    iowrite32(token,         bar + (NVKVM_BAR0_UVM_COMMIT  - NVKVM_BAR0_UVM_VA_LO));
}

static void nvkvm_report_rmalloc(u32 h_client, u32 h_parent, u32 h_object,
                                 u32 h_class, u64 param_gpa, u32 param_size,
                                 u32 token)
{
    if (!bar)
        return;

    iowrite32(h_client,  bar + (NVKVM_BAR0_RMALLOC_HCLIENT - NVKVM_BAR0_UVM_VA_LO));
    iowrite32(h_parent,  bar + (NVKVM_BAR0_RMALLOC_HPARENT - NVKVM_BAR0_UVM_VA_LO));
    iowrite32(h_object,  bar + (NVKVM_BAR0_RMALLOC_HOBJECT - NVKVM_BAR0_UVM_VA_LO));
    iowrite32(h_class,   bar + (NVKVM_BAR0_RMALLOC_HCLASS - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)param_gpa, bar + (NVKVM_BAR0_RMALLOC_PARAM_GPA_LO - NVKVM_BAR0_UVM_VA_LO));
    iowrite32((u32)(param_gpa >> 32), bar + (NVKVM_BAR0_RMALLOC_PARAM_GPA_HI - NVKVM_BAR0_UVM_VA_LO));
    iowrite32(param_size, bar + (NVKVM_BAR0_RMALLOC_PARAM_SIZE - NVKVM_BAR0_UVM_VA_LO));
    wmb();
    iowrite32(token,     bar + (NVKVM_BAR0_RMALLOC_COMMIT - NVKVM_BAR0_UVM_VA_LO));
}

static bool nvkvm_cmd_seen_update(pid_t tgid, u64 va, u64 gpa,
                                  u32 first, bool has_data)
{
    unsigned long flags;
    u64 va_page = va & PAGE_MASK;
    u64 gpa_page = gpa & PAGE_MASK;
    u32 slot;

    if (!cmd_report_once)
        return true;

    spin_lock_irqsave(&cmd_seen_lock, flags);
    for (int i = 0; i < NVKVM_CMD_SEEN; i++) {
        if (cmd_seen[i].tgid == tgid &&
            cmd_seen[i].va_page == va_page &&
            cmd_seen[i].gpa_page == gpa_page) {
            if (cmd_seen[i].first == first &&
                cmd_seen[i].has_data == has_data) {
                spin_unlock_irqrestore(&cmd_seen_lock, flags);
                return false;
            }
            cmd_seen[i].first = first;
            cmd_seen[i].has_data = has_data;
            spin_unlock_irqrestore(&cmd_seen_lock, flags);
            return true;
        }
    }
    slot = cmd_seen_next++ % NVKVM_CMD_SEEN;
    cmd_seen[slot].tgid = tgid;
    cmd_seen[slot].va_page = va_page;
    cmd_seen[slot].gpa_page = gpa_page;
    cmd_seen[slot].first = first;
    cmd_seen[slot].has_data = has_data;
    spin_unlock_irqrestore(&cmd_seen_lock, flags);
    return true;
}

static void nvkvm_cmd_seen_clear_tgid(pid_t tgid)
{
    unsigned long flags;

    if (!tgid)
        return;

    spin_lock_irqsave(&cmd_seen_lock, flags);
    for (int i = 0; i < NVKVM_CMD_SEEN; i++) {
        if (cmd_seen[i].tgid == tgid)
            memset(&cmd_seen[i], 0, sizeof(cmd_seen[i]));
    }
    spin_unlock_irqrestore(&cmd_seen_lock, flags);
}

static bool nvkvm_page_has_data(const void *page, size_t len)
{
    const unsigned long *words = page;
    size_t nwords = len / sizeof(*words);
    size_t i;

    for (i = 0; i < nwords; i++) {
        if (words[i])
            return true;
    }
    for (i = nwords * sizeof(*words); i < len; i++) {
        if (((const u8 *)page)[i])
            return true;
    }
    return false;
}

static bool nvkvm_task_va_to_gpa(struct task_struct *task, u64 va, u64 bytes,
                                 u64 *out_gpa)
{
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    unsigned long addr;
    unsigned long pfn = 0;
    bool ok = false;

    if (!task || !out_gpa || !bytes)
        return false;
    if (va + bytes < va)
        return false;

    mm = get_task_mm(task);
    if (!mm)
        return false;

    addr = (unsigned long)(va & PAGE_MASK);
    mmap_read_lock(mm);
    vma = find_vma(mm, addr);
    if (vma && addr >= vma->vm_start && va + bytes <= vma->vm_end) {
        if (follow_pfn(vma, addr, &pfn) == 0) {
            ok = true;
        } else {
            pte_t *pte = NULL;
            spinlock_t *ptl = NULL;

            if (follow_pte(mm, addr, &pte, &ptl) == 0) {
                if (pte_present(*pte)) {
                    pfn = pte_pfn(*pte);
                    ok = true;
                }
                pte_unmap_unlock(pte, ptl);
            }
        }
    }
    mmap_read_unlock(mm);
    mmput(mm);

    if (!ok)
        return false;
    *out_gpa = ((u64)pfn << PAGE_SHIFT) | (va & ~PAGE_MASK);
    return true;
}

static bool nvkvm_shadow_task_page(struct task_struct *task, u64 va, u64 bytes,
                                   bool report_empty)
{
    int idx;
    u32 token;
    bool has_data = false;
    bool read_ok = false;
    u64 real_gpa = 0;
    bool has_real_gpa;
    u32 first = 0;

    if (!bytes)
        return false;
    if (bytes > PAGE_SIZE)
        bytes = PAGE_SIZE;

    has_real_gpa = nvkvm_task_va_to_gpa(task, va, bytes, &real_gpa);
    mutex_lock(&cmd_scratch_lock);
    if (cmd_scratch_page) {
        memset(cmd_scratch_page, 0, PAGE_SIZE);
        read_ok = access_process_vm(task, (unsigned long)va, cmd_scratch_page,
                                    (int)bytes, 0) == bytes;
        if (read_ok) {
            has_data = nvkvm_page_has_data(cmd_scratch_page, bytes);
            memcpy(&first, cmd_scratch_page, min_t(u64, bytes, sizeof(first)));
        }
    }
    if (!has_real_gpa && read_ok)
        has_real_gpa = nvkvm_task_va_to_gpa(task, va, bytes, &real_gpa);
    if (!has_data && !report_empty)
        goto out_unlock;

    if (has_real_gpa) {
        if (!nvkvm_cmd_seen_update(task->tgid, va, real_gpa,
                                   first, has_data))
            goto out_unlock;
        idx = -1;
        token = (u32)atomic_inc_return(&report_count);
        nvkvm_report_shadow(va, real_gpa, bytes, token);
    } else {
        if (!read_ok)
            goto out_unlock;
        idx = atomic_inc_return(&next_slot) % NVKVM_BRIDGE_SLOTS;
        if (!slots[idx].page)
            goto out_unlock;
        memset(slots[idx].page, 0, PAGE_SIZE);
        memcpy(slots[idx].page, cmd_scratch_page, bytes);
        token = (u32)atomic_inc_return(&report_count);
        nvkvm_report_shadow(va, slots[idx].gpa, bytes, token);
    }

    if (atomic_inc_return(&cmd_report_count) <= 32) {
        pr_info("nvkvm_uvm_bridge: CMD%s%s VA=0x%llx bytes=0x%llx gpa=0x%llx first=0x%08x slot=%d\n",
                has_data ? "" : "-MAP",
                has_real_gpa ? "-PFN" : "",
                (unsigned long long)va, (unsigned long long)bytes,
                (unsigned long long)(has_real_gpa ? real_gpa : slots[idx].gpa),
                first, idx);
    }
    mutex_unlock(&cmd_scratch_lock);
    return true;

out_unlock:
    mutex_unlock(&cmd_scratch_lock);
    return false;
}

static bool nvkvm_cmd_range_interesting(u64 base, u64 len)
{
    if (!cmd_shadow || !base || !len)
        return false;
    if (base >= cmd_va_limit)
        return false;
    return true;
}

static void nvkvm_track_cmd_range(struct task_struct *task, pid_t tgid,
                                  u64 base, u64 len)
{
    unsigned long flags;
    u64 end;
    u64 aligned;
    int slot = -1;
    u64 tracked_base;
    u64 tracked_len;

    if (!nvkvm_cmd_range_interesting(base, len))
        return;
    if (len > cmd_range_max)
        len = cmd_range_max;

    end = base + len;
    if (end < base)
        return;
    aligned = base & PAGE_MASK;
    end = PAGE_ALIGN(end);
    if (end <= aligned)
        return;
    base = aligned;
    len = end - base;

    spin_lock_irqsave(&cmd_lock, flags);
    for (int i = 0; i < NVKVM_CMD_RANGES; i++) {
        u64 rbase = cmd_ranges[i].base;
        u64 rend = rbase + cmd_ranges[i].len;

        if (cmd_ranges[i].tgid == tgid && rbase == base &&
            cmd_ranges[i].len == len) {
            spin_unlock_irqrestore(&cmd_lock, flags);
            return;
        }
        if (cmd_ranges[i].tgid == tgid && rbase <= base &&
            rend >= base + len) {
            spin_unlock_irqrestore(&cmd_lock, flags);
            return;
        }
        if (slot < 0 && cmd_ranges[i].tgid == 0)
            slot = i;
    }
    if (slot < 0) {
        slot = cmd_replace_next++ % NVKVM_CMD_RANGES;
    }

    cmd_ranges[slot].tgid = tgid;
    cmd_ranges[slot].base = base;
    cmd_ranges[slot].len = len;
    cmd_ranges[slot].cursor = 0;
    tracked_base = base;
    tracked_len = len;
    spin_unlock_irqrestore(&cmd_lock, flags);

    pr_info("nvkvm_uvm_bridge: track CMD tgid=%d base=0x%llx len=0x%llx slot=%d\n",
            tgid, (unsigned long long)base, (unsigned long long)len, slot);

    if (task && cmd_seed_pages) {
        u64 pages = min_t(u64, cmd_seed_pages, tracked_len >> PAGE_SHIFT);

        for (u64 i = 0; i < pages; i++)
            nvkvm_shadow_task_page(task, tracked_base + i * PAGE_SIZE,
                                   PAGE_SIZE, true);
    }

    if (task && (cmd_stride_seed_pages || cmd_pre_stride_seed_pages) &&
        tracked_len >= PAGE_SIZE) {
        u64 seeded = 0;
        u64 step = PAGE_ALIGN(cmd_stride_step);
        u64 window_pages = cmd_stride_window_pages ? cmd_stride_window_pages : 1;

        if (step < PAGE_SIZE)
            step = PAGE_SIZE;

        if (cmd_pre_stride_seed_pages && tracked_base >= step) {
            u64 max_pages = min_t(u64, window_pages, cmd_pre_stride_seed_pages);
            u64 pre_base = tracked_base - step;

            for (u64 page = 0; page < max_pages; page++)
                nvkvm_shadow_task_page(task,
                                       pre_base + page * PAGE_SIZE,
                                       PAGE_SIZE, true);
        }

        for (u64 off = step;
             off < tracked_len && seeded < cmd_stride_seed_pages;
             off += step, seeded++) {
            u64 max_pages = min_t(u64, window_pages,
                                  (tracked_len - off) >> PAGE_SHIFT);

            for (u64 page = 0; page < max_pages; page++) {
                nvkvm_shadow_task_page(task,
                                       tracked_base + off + page * PAGE_SIZE,
                                       PAGE_SIZE, true);
            }
        }
    }
}

static void nvkvm_clear_cmd_ranges(pid_t tgid)
{
    unsigned long flags;

    spin_lock_irqsave(&cmd_lock, flags);
    for (int i = 0; i < NVKVM_CMD_RANGES; i++) {
        if (cmd_ranges[i].tgid == tgid)
            memset(&cmd_ranges[i], 0, sizeof(cmd_ranges[i]));
    }
    spin_unlock_irqrestore(&cmd_lock, flags);
    nvkvm_cmd_seen_clear_tgid(tgid);
}

static void nvkvm_scan_task_cmd_ranges(struct task_struct *task, pid_t tgid,
                                       unsigned int budget)
{
    unsigned int scanned = 0;
    unsigned int reported = 0;

    if (!cmd_shadow || !budget || !task)
        return;

    while (scanned < budget) {
        unsigned long flags;
        u64 va = 0;
        u64 bytes = 0;
        u64 range_end = 0;
        bool report_empty = false;
        bool found = false;

        spin_lock_irqsave(&cmd_lock, flags);
        for (int n = 0; n < NVKVM_CMD_RANGES; n++) {
            int i = (cmd_scan_next + n) % NVKVM_CMD_RANGES;
            u64 off;

            if (cmd_ranges[i].tgid != tgid || !cmd_ranges[i].len)
                continue;
            off = cmd_ranges[i].cursor;
            if (off >= cmd_ranges[i].len)
                off = 0;
            va = cmd_ranges[i].base + off;
            bytes = min_t(u64, PAGE_SIZE, cmd_ranges[i].len - off);
            range_end = cmd_ranges[i].base + cmd_ranges[i].len;
            report_empty = (off >> PAGE_SHIFT) < cmd_seed_pages;
            cmd_ranges[i].cursor = off + PAGE_SIZE;
            if (cmd_ranges[i].cursor >= cmd_ranges[i].len) {
                cmd_ranges[i].cursor = 0;
                cmd_scan_next = (i + 1) % NVKVM_CMD_RANGES;
            } else {
                cmd_scan_next = i;
            }
            found = true;
            break;
        }
        spin_unlock_irqrestore(&cmd_lock, flags);

        if (!found)
            break;

        scanned++;
        if (nvkvm_shadow_task_page(task, va, bytes, report_empty)) {
            reported++;
            if (!report_empty && cmd_neighbor_pages && range_end > va + PAGE_SIZE) {
                u64 nmax = min_t(u64, cmd_neighbor_pages,
                                 (range_end - (va + PAGE_SIZE)) >> PAGE_SHIFT);

                for (u64 n = 1; n <= nmax; n++) {
                    if (nvkvm_shadow_task_page(task, va + n * PAGE_SIZE,
                                               PAGE_SIZE, true))
                        reported++;
                }
            }
        }
    }

    if (reported && atomic_read(&cmd_report_count) <= 32) {
        pr_info("nvkvm_uvm_bridge: scanned CMD tgid=%d pages=%u reported=%u\n",
                tgid, scanned, reported);
    }
}

static void nvkvm_cmd_scan_workfn(struct work_struct *work)
{
    pid_t *tgids = cmd_scan_tgids;
    int ntgids = 0;
    unsigned long flags;
    unsigned int delay;

    if (!cmd_shadow)
        return;

    spin_lock_irqsave(&cmd_lock, flags);
    for (int i = 0; i < NVKVM_CMD_RANGES; i++) {
        bool seen = false;

        if (!cmd_ranges[i].tgid)
            continue;
        for (int j = 0; j < ntgids; j++) {
            if (tgids[j] == cmd_ranges[i].tgid) {
                seen = true;
                break;
            }
        }
        if (!seen && ntgids < NVKVM_CMD_RANGES)
            tgids[ntgids++] = cmd_ranges[i].tgid;
    }
    spin_unlock_irqrestore(&cmd_lock, flags);

    for (int i = 0; i < ntgids; i++) {
        struct pid *pid;
        struct task_struct *task;

        pid = find_get_pid(tgids[i]);
        if (!pid) {
            nvkvm_clear_cmd_ranges(tgids[i]);
            continue;
        }
        task = get_pid_task(pid, PIDTYPE_PID);
        put_pid(pid);
        if (!task) {
            nvkvm_clear_cmd_ranges(tgids[i]);
            continue;
        }
        nvkvm_scan_task_cmd_ranges(task, tgids[i], cmd_bg_scan_pages);
        put_task_struct(task);
    }

    delay = cmd_scan_period_ms ? cmd_scan_period_ms : 50;
    schedule_delayed_work(&cmd_scan_work, msecs_to_jiffies(delay));
}

static int nvkvm_htod_handler(struct uprobe_consumer *self, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
    u64 dst = regs->di;
    const char __user *src = (const char __user *)regs->si;
    u64 bytes = regs->dx;
    u64 n;
    u64 off;
    int idx;
    u32 token;

    if (!dst || !src || !bytes)
        return 0;

    n = bytes;
    if (n > max_bytes)
        n = max_bytes;

    for (off = 0; off < n; off += PAGE_SIZE) {
        u64 chunk = min_t(u64, PAGE_SIZE, n - off);

        idx = atomic_inc_return(&next_slot) % NVKVM_BRIDGE_SLOTS;
        if (!slots[idx].page)
            return 0;

        memset(slots[idx].page, 0, PAGE_SIZE);
        if (copy_from_user(slots[idx].page, src + off, chunk) != 0)
            continue;

        token = (u32)atomic_inc_return(&report_count);
        nvkvm_report_shadow(dst + off, slots[idx].gpa, chunk, token);

        if (token <= 64) {
            u32 first = 0;
            memcpy(&first, slots[idx].page, min_t(u64, chunk, sizeof(first)));
            pr_info("nvkvm_uvm_bridge: HtoD dst=0x%llx bytes=0x%llx gpa=0x%llx first=0x%08x slot=%d\n",
                    (unsigned long long)(dst + off), (unsigned long long)chunk,
                    (unsigned long long)slots[idx].gpa, first, idx);
        }
    }
#endif
    return 0;
}

static int nvkvm_ioctl_handler(struct uprobe_consumer *self, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
    unsigned long req = regs->si;
    unsigned long arg = regs->dx;
    unsigned long type = (req >> 8) & 0xfful;
    unsigned long nr = req & 0xfful;
    int slot = -1;
    struct nvkvm_ioctl_slot rec = { 0 };
    unsigned long flags;

    nvkvm_scan_task_cmd_ranges(current, current->tgid, cmd_scan_pages);

    if (!arg)
        return 0;

    if (type == 0 && nr == NVKVM_UVM_MAP_EXTERNAL_ALLOCATION) {
        /* existing UVM_MAP_EXTERNAL tracking */
    } else if (type == 'F' && nr == 0x2bul) {
        u64 pptr = 0;
        u32 h_class = 0;

        if (copy_from_user(&h_class, (void __user *)(arg + 12), sizeof(h_class)) != 0 ||
            (h_class != NVKVM_RMALLOC_MEM_CLASS &&
             h_class != NVKVM_RMALLOC_LOCAL_USER_CLASS) ||
            copy_from_user(&pptr, (void __user *)(arg + 16), sizeof(pptr)) != 0 ||
            !pptr) {
            return 0;
        }

        rec.param_slot = atomic_inc_return(&next_slot) % NVKVM_BRIDGE_SLOTS;
        if (!slots[rec.param_slot].page)
            return 0;
        memset(slots[rec.param_slot].page, 0, PAGE_SIZE);
        if (copy_from_user(slots[rec.param_slot].page,
                           (void __user *)pptr,
                           NVKVM_RMALLOC_MEM_PARAMS_SIZE) != 0) {
            return 0;
        }
        if (copy_from_user(&rec.h_client, (void __user *)(arg + 0), sizeof(rec.h_client)) != 0 ||
            copy_from_user(&rec.h_parent, (void __user *)(arg + 4), sizeof(rec.h_parent)) != 0 ||
            copy_from_user(&rec.h_object, (void __user *)(arg + 8), sizeof(rec.h_object)) != 0) {
            return 0;
        }
        rec.h_class = h_class;
        rec.param_size = NVKVM_RMALLOC_MEM_PARAMS_SIZE;
        rec.rm_alloc = true;
    } else {
        return 0;
    }

    rec.pid = current->pid;
    rec.req = req;
    rec.arg = arg;

    spin_lock_irqsave(&ioctl_lock, flags);
    for (int i = 0; i < NVKVM_IOCTL_SLOTS; i++) {
        if (ioctl_slots[i].pid == current->pid) {
            slot = i;
            break;
        }
        if (slot < 0 && ioctl_slots[i].pid == 0)
            slot = i;
    }
    if (slot >= 0) {
        ioctl_slots[slot] = rec;
    }
    spin_unlock_irqrestore(&ioctl_lock, flags);
#endif
    return 0;
}

static int nvkvm_ioctl_ret_handler(struct uprobe_consumer *self,
                                   unsigned long func, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
    struct nvkvm_ioctl_slot saved = { 0 };
    unsigned long flags;
    unsigned long ret = regs_return_value(regs);
    u64 base = 0, len = 0, offset = 0;
    u32 h_client = 0, h_memory = 0;
    u32 rm_status = 0xffffffffu;
    u32 token;

    spin_lock_irqsave(&ioctl_lock, flags);
    for (int i = 0; i < NVKVM_IOCTL_SLOTS; i++) {
        if (ioctl_slots[i].pid == current->pid) {
            saved = ioctl_slots[i];
            memset(&ioctl_slots[i], 0, sizeof(ioctl_slots[i]));
            break;
        }
    }
    spin_unlock_irqrestore(&ioctl_lock, flags);

    if (!saved.arg || ret != 0)
        return 0;

    if (saved.rm_alloc) {
        u32 status = 0xffffffffu;

        if (saved.param_slot < 0 || saved.param_slot >= NVKVM_BRIDGE_SLOTS ||
            !slots[saved.param_slot].page ||
            copy_from_user(&status, (void __user *)(saved.arg + 28),
                           sizeof(status)) != 0 ||
            status != 0) {
            return 0;
        }

        token = (u32)atomic_inc_return(&report_count) |
                NVKVM_RMALLOC_REPORT_TOKEN;
        nvkvm_report_rmalloc(saved.h_client, saved.h_parent, saved.h_object,
                             saved.h_class, slots[saved.param_slot].gpa,
                             saved.param_size, token);
        if ((token & ~NVKVM_RMALLOC_REPORT_TOKEN) <= 96) {
            pr_info("nvkvm_uvm_bridge: RMALLOC hClient=0x%08x hParent=0x%08x hObject=0x%08x hClass=0x%08x gpa=0x%llx size=%u token=0x%08x\n",
                    saved.h_client, saved.h_parent, saved.h_object,
                    saved.h_class,
                    (unsigned long long)slots[saved.param_slot].gpa,
                    saved.param_size, token);
        }
        return 0;
    }

    if (copy_from_user(&base, (void __user *)saved.arg, sizeof(base)) != 0 ||
        copy_from_user(&len, (void __user *)(saved.arg + 8), sizeof(len)) != 0 ||
        copy_from_user(&offset,
                       (void __user *)(saved.arg + NVKVM_UVM_MAP_EXT_OFFSET_OFF),
                       sizeof(offset)) != 0 ||
        copy_from_user(&h_client,
                       (void __user *)(saved.arg + NVKVM_UVM_MAP_EXT_HCLIENT_OFF),
                       sizeof(h_client)) != 0 ||
        copy_from_user(&h_memory,
                       (void __user *)(saved.arg + NVKVM_UVM_MAP_EXT_HMEMORY_OFF),
                       sizeof(h_memory)) != 0 ||
        copy_from_user(&rm_status,
                       (void __user *)(saved.arg + NVKVM_UVM_MAP_EXT_RM_STATUS_OFF),
                       sizeof(rm_status)) != 0) {
        return 0;
    }
    if (!base || !len || rm_status != 0)
        return 0;

    token = (u32)atomic_inc_return(&report_count) | 0x80000000u;
    nvkvm_report_shadow(base, ((u64)h_client << 32) | h_memory, len, token);
    nvkvm_track_cmd_range(current, current->tgid, base, len);
    nvkvm_scan_task_cmd_ranges(current, current->tgid, cmd_scan_pages);
    if ((token & 0x7fffffffU) <= 96) {
        pr_info("nvkvm_uvm_bridge: UVM_MAP_EXTERNAL base=0x%llx len=0x%llx off=0x%llx hClient=0x%08x hMemory=0x%08x token=0x%08x\n",
                (unsigned long long)base, (unsigned long long)len,
                (unsigned long long)offset, h_client, h_memory, token);
    }
#endif
    return 0;
}

static int nvkvm_register_probe(struct nvkvm_probe *probe, unsigned long offset,
                                const char *path_name, const char *obj_path,
                                const char *name,
                                int (*handler)(struct uprobe_consumer *, struct pt_regs *),
                                int (*ret_handler)(struct uprobe_consumer *,
                                                   unsigned long, struct pt_regs *))
{
    struct path kpath;
    int ret;

    memset(probe, 0, sizeof(*probe));
    probe->offset = offset;
    probe->name = name;
    probe->consumer.handler = handler;
    probe->consumer.ret_handler = ret_handler;

    if (!offset)
        return 0;

    ret = kern_path(obj_path, LOOKUP_FOLLOW, &kpath);
    if (ret)
        return ret;

    probe->inode = igrab(d_inode(kpath.dentry));
    path_put(&kpath);
    if (!probe->inode)
        return -ENOENT;

    ret = uprobe_register(probe->inode, probe->offset, &probe->consumer);
    if (ret) {
        iput(probe->inode);
        probe->inode = NULL;
        return ret;
    }

    pr_info("nvkvm_uvm_bridge: registered %s at %s+0x%lx\n",
            name, path_name, offset);
    return 0;
}

static void nvkvm_unregister_probe(struct nvkvm_probe *probe)
{
    if (!probe->inode)
        return;
    uprobe_unregister(probe->inode, probe->offset, &probe->consumer);
    iput(probe->inode);
    probe->inode = NULL;
}

static int nvkvm_map_bar(void)
{
    struct pci_dev *pdev = NULL;

    while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev))) {
        resource_size_t start = pci_resource_start(pdev, 0);
        resource_size_t len = pci_resource_len(pdev, 0);

        if (!start || len <= NVKVM_BAR0_RMALLOC_COMMIT)
            continue;

        bar = ioremap(start + NVKVM_BAR0_UVM_VA_LO, NVKVM_BAR0_UVM_SPAN);
        if (!bar)
            continue;

        bar_pdev = pdev;
        pr_info("nvkvm_uvm_bridge: mapped BAR0 debug aperture at %s start=0x%llx\n",
                pci_name(pdev), (unsigned long long)start);
        return 0;
    }

    return -ENODEV;
}

static void nvkvm_unmap_bar(void)
{
    if (bar) {
        iounmap(bar);
        bar = NULL;
    }
    if (bar_pdev) {
        pci_dev_put(bar_pdev);
        bar_pdev = NULL;
    }
}

static int __init nvkvm_uvm_bridge_init(void)
{
    int ret;
    int i;

    if (!htod_off && !htod_v2_off && !ioctl_off)
        return -EINVAL;
    if (max_bytes > NVKVM_BRIDGE_SLOTS * PAGE_SIZE)
        max_bytes = NVKVM_BRIDGE_SLOTS * PAGE_SIZE;
    if (!max_bytes)
        max_bytes = PAGE_SIZE;

    cmd_scratch_page = (void *)get_zeroed_page(GFP_KERNEL);
    if (!cmd_scratch_page)
        return -ENOMEM;

    ret = nvkvm_map_bar();
    if (ret)
        goto fail_scratch;
    INIT_DELAYED_WORK(&cmd_scan_work, nvkvm_cmd_scan_workfn);

    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        slots[i].page = (void *)get_zeroed_page(GFP_KERNEL);
        if (!slots[i].page) {
            ret = -ENOMEM;
            goto fail;
        }
        slots[i].gpa = virt_to_phys(slots[i].page);
    }

    ret = nvkvm_register_probe(&probe_htod, htod_off, libcuda_path, libcuda_path,
                               "cuMemcpyHtoD", nvkvm_htod_handler, NULL);
    if (ret)
        goto fail;

    ret = nvkvm_register_probe(&probe_htod_v2, htod_v2_off, libcuda_path, libcuda_path,
                               "cuMemcpyHtoD_v2", nvkvm_htod_handler, NULL);
    if (ret)
        goto fail;

    ret = nvkvm_register_probe(&probe_ioctl, ioctl_off, libc_path, libc_path,
                               "ioctl", nvkvm_ioctl_handler,
                               nvkvm_ioctl_ret_handler);
    if (ret)
        goto fail;

    pr_info("nvkvm_uvm_bridge: loaded max_bytes=%u\n", max_bytes);
    if (cmd_shadow)
        schedule_delayed_work(&cmd_scan_work, msecs_to_jiffies(cmd_scan_period_ms ? cmd_scan_period_ms : 50));
    return 0;

fail:
    nvkvm_unregister_probe(&probe_ioctl);
    nvkvm_unregister_probe(&probe_htod_v2);
    nvkvm_unregister_probe(&probe_htod);
    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        if (slots[i].page) {
            free_page((unsigned long)slots[i].page);
            slots[i].page = NULL;
        }
    }
    nvkvm_unmap_bar();
fail_scratch:
    if (cmd_scratch_page) {
        free_page((unsigned long)cmd_scratch_page);
        cmd_scratch_page = NULL;
    }
    return ret;
}

static void __exit nvkvm_uvm_bridge_exit(void)
{
    int i;

    cancel_delayed_work_sync(&cmd_scan_work);
    nvkvm_unregister_probe(&probe_ioctl);
    nvkvm_unregister_probe(&probe_htod_v2);
    nvkvm_unregister_probe(&probe_htod);
    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        if (slots[i].page) {
            free_page((unsigned long)slots[i].page);
            slots[i].page = NULL;
        }
    }
    if (cmd_scratch_page) {
        free_page((unsigned long)cmd_scratch_page);
        cmd_scratch_page = NULL;
    }
    nvkvm_unmap_bar();
    pr_info("nvkvm_uvm_bridge: unloaded reports=%d\n", atomic_read(&report_count));
}

module_init(nvkvm_uvm_bridge_init);
module_exit(nvkvm_uvm_bridge_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVKVM Mode-2 debug UVM HtoD uprobe bridge");
