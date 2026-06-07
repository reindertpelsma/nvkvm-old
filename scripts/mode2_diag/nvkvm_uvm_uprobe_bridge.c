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
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uprobes.h>

#define NVKVM_BAR0_UVM_VA_LO      0xFFF520ul
#define NVKVM_BAR0_UVM_VA_HI      0xFFF524ul
#define NVKVM_BAR0_UVM_GPA_LO     0xFFF528ul
#define NVKVM_BAR0_UVM_GPA_HI     0xFFF52cul
#define NVKVM_BAR0_UVM_SIZE_LO    0xFFF530ul
#define NVKVM_BAR0_UVM_SIZE_HI    0xFFF534ul
#define NVKVM_BAR0_UVM_COMMIT     0xFFF538ul
#define NVKVM_BAR0_UVM_SPAN       0x20ul

#define NVKVM_BRIDGE_SLOTS        32

static char *libcuda_path = "/usr/local/nvidia-guest/lib/libcuda.so.580.159.04";
module_param(libcuda_path, charp, 0444);
MODULE_PARM_DESC(libcuda_path, "Path to guest libcuda.so used for uprobes");

static unsigned long htod_off;
module_param(htod_off, ulong, 0444);
MODULE_PARM_DESC(htod_off, "ELF symbol offset for cuMemcpyHtoD");

static unsigned long htod_v2_off;
module_param(htod_v2_off, ulong, 0444);
MODULE_PARM_DESC(htod_v2_off, "ELF symbol offset for cuMemcpyHtoD_v2");

static unsigned int max_bytes = PAGE_SIZE;
module_param(max_bytes, uint, 0444);
MODULE_PARM_DESC(max_bytes, "Maximum bytes to shadow per HtoD call");

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

static int nvkvm_htod_handler(struct uprobe_consumer *self, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
    u64 dst = regs->di;
    const void __user *src = (const void __user *)regs->si;
    u64 bytes = regs->dx;
    u64 n;
    int idx;
    u32 token;

    if (!dst || !src || !bytes)
        return 0;

    n = bytes;
    if (n > max_bytes)
        n = max_bytes;
    if (n > PAGE_SIZE)
        n = PAGE_SIZE;

    idx = atomic_inc_return(&next_slot) % NVKVM_BRIDGE_SLOTS;
    if (!slots[idx].page)
        return 0;

    memset(slots[idx].page, 0, PAGE_SIZE);
    if (copy_from_user(slots[idx].page, src, n) != 0)
        return 0;

    token = (u32)atomic_inc_return(&report_count);
    nvkvm_report_shadow(dst, slots[idx].gpa, n, token);

    if (token <= 32) {
        u32 first = 0;
        memcpy(&first, slots[idx].page, min_t(u64, n, sizeof(first)));
        pr_info("nvkvm_uvm_bridge: HtoD dst=0x%llx bytes=0x%llx gpa=0x%llx first=0x%08x slot=%d\n",
                (unsigned long long)dst, (unsigned long long)n,
                (unsigned long long)slots[idx].gpa, first, idx);
    }
#endif
    return 0;
}

static int nvkvm_register_probe(struct nvkvm_probe *probe, unsigned long offset,
                                const char *name)
{
    struct path path;
    int ret;

    memset(probe, 0, sizeof(*probe));
    probe->offset = offset;
    probe->name = name;
    probe->consumer.handler = nvkvm_htod_handler;

    if (!offset)
        return 0;

    ret = kern_path(libcuda_path, LOOKUP_FOLLOW, &path);
    if (ret)
        return ret;

    probe->inode = igrab(d_inode(path.dentry));
    path_put(&path);
    if (!probe->inode)
        return -ENOENT;

    ret = uprobe_register(probe->inode, probe->offset, &probe->consumer);
    if (ret) {
        iput(probe->inode);
        probe->inode = NULL;
        return ret;
    }

    pr_info("nvkvm_uvm_bridge: registered %s at %s+0x%lx\n",
            name, libcuda_path, offset);
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

        if (!start || len <= NVKVM_BAR0_UVM_COMMIT)
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

    if (!htod_off && !htod_v2_off)
        return -EINVAL;
    if (max_bytes > PAGE_SIZE)
        max_bytes = PAGE_SIZE;
    if (!max_bytes)
        max_bytes = PAGE_SIZE;

    ret = nvkvm_map_bar();
    if (ret)
        return ret;

    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        slots[i].page = (void *)get_zeroed_page(GFP_KERNEL);
        if (!slots[i].page) {
            ret = -ENOMEM;
            goto fail;
        }
        slots[i].gpa = virt_to_phys(slots[i].page);
    }

    ret = nvkvm_register_probe(&probe_htod, htod_off, "cuMemcpyHtoD");
    if (ret)
        goto fail;

    ret = nvkvm_register_probe(&probe_htod_v2, htod_v2_off, "cuMemcpyHtoD_v2");
    if (ret)
        goto fail;

    pr_info("nvkvm_uvm_bridge: loaded max_bytes=%u\n", max_bytes);
    return 0;

fail:
    nvkvm_unregister_probe(&probe_htod_v2);
    nvkvm_unregister_probe(&probe_htod);
    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        if (slots[i].page) {
            free_page((unsigned long)slots[i].page);
            slots[i].page = NULL;
        }
    }
    nvkvm_unmap_bar();
    return ret;
}

static void __exit nvkvm_uvm_bridge_exit(void)
{
    int i;

    nvkvm_unregister_probe(&probe_htod_v2);
    nvkvm_unregister_probe(&probe_htod);
    for (i = 0; i < NVKVM_BRIDGE_SLOTS; i++) {
        if (slots[i].page) {
            free_page((unsigned long)slots[i].page);
            slots[i].page = NULL;
        }
    }
    nvkvm_unmap_bar();
    pr_info("nvkvm_uvm_bridge: unloaded reports=%d\n", atomic_read(&report_count));
}

module_init(nvkvm_uvm_bridge_init);
module_exit(nvkvm_uvm_bridge_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVKVM Mode-2 debug UVM HtoD uprobe bridge");
