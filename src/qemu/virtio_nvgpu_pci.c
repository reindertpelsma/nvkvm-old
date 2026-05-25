/*
 * virtio_nvgpu_pci.c — PCI transport wrapper for virtio-nvgpu
 *
 * Registers the "virtio-nvgpu-pci" device name so that QEMU guests can
 * instantiate the virtio-nvgpu device over PCI (the standard transport for
 * x86_64 KVM guests).
 *
 * This is a thin VirtIOPCIProxy wrapper — all device logic lives in
 * virtio_nvgpu.c.  The pattern mirrors hw/virtio/virtio-balloon-pci.c.
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#include "virtio_nvgpu.h"

typedef struct VirtIONvgpuPCI VirtIONvgpuPCI;

#define TYPE_VIRTIO_NVGPU_PCI "virtio-nvgpu-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIONvgpuPCI, VIRTIO_NVGPU_PCI,
                         TYPE_VIRTIO_NVGPU_PCI)

struct VirtIONvgpuPCI {
	VirtIOPCIProxy parent_obj;
	VirtIONvgpu    vdev;
};

static void virtio_nvgpu_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
	VirtIONvgpuPCI *dev  = VIRTIO_NVGPU_PCI(vpci_dev);
	DeviceState    *vdev = DEVICE(&dev->vdev);

	vpci_dev->class_code = PCI_CLASS_OTHERS;
	qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_nvgpu_pci_class_init(ObjectClass *klass, void *data)
{
	DeviceClass     *dc       = DEVICE_CLASS(klass);
	VirtioPCIClass  *k        = VIRTIO_PCI_CLASS(klass);
	PCIDeviceClass  *pcidev_k = PCI_DEVICE_CLASS(klass);

	k->realize = virtio_nvgpu_pci_realize;
	set_bit(DEVICE_CATEGORY_MISC, dc->categories);

	/* virtio modern PCI device IDs: vendor 0x1AF4, device 0x1040 + type_id */
	pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
	pcidev_k->device_id = 0x1040 + VIRTIO_ID_NVGPU; /* 0x1072 */
	pcidev_k->revision  = VIRTIO_PCI_ABI_VERSION;
	pcidev_k->class_id  = PCI_CLASS_OTHERS;
}

static void virtio_nvgpu_pci_instance_init(Object *obj)
{
	VirtIONvgpuPCI *dev = VIRTIO_NVGPU_PCI(obj);

	virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
				    TYPE_VIRTIO_NVGPU);
}

static const VirtioPCIDeviceTypeInfo virtio_nvgpu_pci_info = {
	.base_name             = TYPE_VIRTIO_NVGPU_PCI,
	.generic_name          = "virtio-nvgpu-pci",
	.transitional_name     = "virtio-nvgpu-pci-transitional",
	.non_transitional_name = "virtio-nvgpu-pci-non-transitional",
	.instance_size         = sizeof(VirtIONvgpuPCI),
	.instance_init         = virtio_nvgpu_pci_instance_init,
	.class_init            = virtio_nvgpu_pci_class_init,
};

static void virtio_nvgpu_pci_register(void)
{
	virtio_pci_types_register(&virtio_nvgpu_pci_info);
}
type_init(virtio_nvgpu_pci_register)
