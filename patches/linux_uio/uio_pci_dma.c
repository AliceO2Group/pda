/**
 * @author Dominic Eschweiler <dominic@eschweiler.at>
 *
 * @section LICENSE
 *
 * Copyright (c) 2015, Dominic Eschweiler
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */



#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/sort.h>
#include <linux/sysfs.h>
#include <linux/uio_driver.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <asm/cacheflush.h>
#include <linux/vmalloc.h>


#pragma GCC diagnostic pop
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

//#define UIO_PDA_DEBUG
//#define UIO_PDA_DEBUG_SG
#define UIO_PDA_IOMMU
#define UIO_PDA_USE_PAGEFAULT_HANDLER


#include "uio_pci_dma.h"

#define DRIVER_NAME     "uio_pci_dma"
#define DRIVER_DESC     "UIO PCI 2.3 driver with DMA support"
#define DRIVER_LICENSE  "Dual BSD/GPL"
#define DRIVER_AUTHOR   "Dominic Eschweiler"

struct uio_pci_dma_device
{
    struct uio_info       info;
    struct pci_dev       *pdev;
    struct kobject       *dma_kobj;
    struct bin_attribute  attr_bar[PCI_NUM_RESOURCES];
    bool                  msi_enabled;
};

spinlock_t alloc_free_lock;
static inline int
uio_pci_dma_allocate_kernel_memory(struct uio_pci_dma_private *priv);

static inline int
uio_pci_dma_allocate_user_memory(struct uio_pci_dma_private *priv);

static inline int
convert_sg(struct uio_pci_dma_private* priv);

int
sort_sg_compare(const void *item1, const void *item2);

void
sort_sg_swap(void *item1, void *item2, int size);

static inline int
uio_pci_dma_free(struct kobject *kobj);

static inline int
uio_pci_dma_free_user_memory(struct uio_pci_dma_private *priv);

static inline int
uio_pci_dma_free_kernel_memory(struct uio_pci_dma_private *priv);

static inline void
free_memory_lists(struct uio_pci_dma_private *priv);

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
bool
pci_check_and_mask_intx(struct pci_dev *pdev)
{
    UIO_DEBUG_ENTER();
    uint32_t csr;
    uint16_t command;
    uint16_t status;
    uint16_t disable;

    pci_block_user_cfg_access(pdev);

    /* Readout the CSR and split it into command and status part */
    pci_read_config_dword(pdev, PCI_COMMAND, &csr);
    command = csr;
    status  = csr >> 16;

    /* Check that the current device indeed throw the interrupt */
    if(!(status & PCI_STATUS_INTERRUPT) )
    { goto exit; }

    /* Disable interrupt on the device */
    disable = command | PCI_COMMAND_INTX_DISABLE;
    if(disable != command)
    { pci_write_config_word(pdev, PCI_COMMAND, disable); }

exit:
    pci_unblock_user_cfg_access(pdev);
    UIO_DEBUG_RETURN(true);
}



bool
pci_intx_mask_supported(struct pci_dev *pdev)
{
    UIO_DEBUG_ENTER();
    bool     ret        = false;
    uint16_t csr_before = 0;
    uint16_t csr_after  = 0;

    pci_block_user_cfg_access(pdev);
    {
        pci_read_config_word(pdev, PCI_COMMAND, &csr_before);
        pci_write_config_word(pdev, PCI_COMMAND, csr_before ^ PCI_COMMAND_INTX_DISABLE);
        pci_read_config_word(pdev, PCI_COMMAND, &csr_after);

        if( (csr_before ^ csr_after) & ~PCI_COMMAND_INTX_DISABLE)
        { UIO_PDA_ERROR( "CSR changed (intx masking not supported)!\n", exit ) }

        if( (csr_before ^ csr_after) & PCI_COMMAND_INTX_DISABLE)
        {
            ret = true;
            pci_write_config_word(pdev, PCI_COMMAND, csr_before);
        }
    }

exit:
    pci_unblock_user_cfg_access(pdev);
    UIO_DEBUG_RETURN(ret);
}
#endif



/** Device initialization stuff etc. */
static
irqreturn_t
irqhandler
(
    int              irq,
    struct uio_info *info_uio
)
{
    UIO_DEBUG_ENTER();
    struct uio_pci_dma_device *dev =
        container_of(info_uio, struct uio_pci_dma_device, info);

    if(!dev->msi_enabled && !pci_check_and_mask_intx(dev->pdev) )
    { UIO_DEBUG_RETURN(IRQ_NONE); }

    UIO_DEBUG_RETURN(IRQ_HANDLED);
}

#define DMA_MASK( tag )                                                    \
if( (err = pci_set ## tag ## dma_mask(pci_device, DMA_BIT_MASK(64))) )     \
{                                                                          \
    printk(DRIVER_NAME " : Warning: couldn't set 64-bit PCI DMA mask.\n"); \
    if( (err = pci_set_dma_mask(pci_device, DMA_BIT_MASK(32))) )           \
    { UIO_PDA_ERROR("Can't set PCI DMA mask, aborting!\n", exit); }        \
}



BIN_ATTR_MAP_CALLBACK( bar_mmap )
{
    UIO_DEBUG_ENTER();
    struct device *dev                    = container_of(kobj, struct device, kobj);
    struct uio_pci_dma_device *dma_device = dev_get_drvdata(dev);

    int bar_number = -1;

    uint32_t i = 0;
    for(i = 0; i<PCI_NUM_RESOURCES; i++)
    {
        if(&dma_device->attr_bar[i] == attr)
        { bar_number = i; break; }
    }

    if(bar_number == -1)
    {
        printk(DRIVER_NAME " : Can't map BAR memory!\n");
        return -EINVAL;
    }

    uint64_t address = pci_resource_start(dma_device->pdev, bar_number);

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    int ret =
        remap_pfn_range
        (
            vma,
            vma->vm_start,
            address >> PAGE_SHIFT,
            (vma->vm_end-vma->vm_start),
            vma->vm_page_prot
        );

    UIO_DEBUG_RETURN(ret);
}

BIN_ATTR_READ_CALLBACK( mock )
{
    UIO_DEBUG_ENTER();
    UIO_DEBUG_RETURN(count);
}



static
int __devinit
probe
(
    struct pci_dev             *pci_device,
    const struct pci_device_id *id
)
{
    UIO_DEBUG_ENTER();
    struct uio_pci_dma_device *dma_device = NULL;
    struct kobject            *dma_kobj   = NULL;

    int                        err;
    struct kset               *kset;
    bool                       msi_enabled = false;

    spin_lock_init(&alloc_free_lock);

    /* attr_bin_request */
    BIN_ATTR_PDA(request, sizeof(struct uio_pci_dma_private), S_IWUGO, NULL,
        uio_pci_dma_sysfs_request_buffer_write, NULL);

    /* attr_bin_free */
    BIN_ATTR_PDA(free, 0, S_IWUGO, NULL,
        uio_pci_dma_sysfs_delete_buffer_write, NULL);

    /* attr_bin_max_payload_size */
    BIN_ATTR_PDA(max_payload_size, sizeof(int), S_IRUGO,
        uio_pci_dma_sysfs_mps, NULL, NULL);

    /* attr_bin_max_read_request_size */
    BIN_ATTR_PDA(max_read_request_size, sizeof(int), S_IRUGO,
        uio_pci_dma_sysfs_readrq, NULL, NULL);

    UIO_DEBUG_PRINTF("Enabling the PCI Device\n");
    err = pci_enable_device(pci_device);
    if(err)
    { UIO_PDA_ERROR(" Failed to enable PCI device!\n", exit); }

    if(!pci_device->irq)
    {
        err = -ENODEV;
        UIO_PDA_ERROR("Device has no assigned interrupt!\n", exit);
    }

    if(!pci_enable_msi(pci_device))
    { msi_enabled = true; }
    else if(!pci_intx_mask_supported(pci_device))
    {
        err = -ENODEV;
        UIO_PDA_ERROR("Device does not support interrupt masking!\n", exit);
    }

    UIO_DEBUG_PRINTF("Allocating dma_device\n");
    dma_device = kzalloc(sizeof(struct uio_pci_dma_device), GFP_KERNEL);
    if(!dma_device)
    {
        err = -ENOMEM;
        UIO_PDA_ERROR("Allocation failed!\n", exit);
    }

    dma_device->pdev           = pci_device;
    dma_device->msi_enabled    = msi_enabled;

    dma_device->info.name      = DRIVER_NAME;
    dma_device->info.version   = UIO_PCI_DMA_VERSION;
    dma_device->info.irq       = pci_device->irq;
    dma_device->info.handler   = irqhandler;
    dma_device->info.irq_flags = msi_enabled ? 0 : IRQF_SHARED;

    UIO_DEBUG_PRINTF("Set DMA-Master\n");
    pci_set_master(pci_device);
    DMA_MASK( _ );
    DMA_MASK( _consistent_ );

    UIO_DEBUG_PRINTF("Generate a new sysfs folder\n");
    kset                 = kset_create_and_add("dma", NULL, &pci_device->dev.kobj);
    dma_kobj             = &kset->kobj;
    dma_device->dma_kobj = dma_kobj;

    /* Add a sysfs entry for request_buffer. Piping into the related file
     * triggers an allocate and the generation of a new <buffer name>
     * entry in sysfs */
    if( (err=sysfs_create_bin_file(dma_kobj, attr_bin_request)) )
    { UIO_PDA_ERROR("Can't create entry for buffer request!\n", exit_request); }

    /* Add a sysfs entry for delete_buffer, piping <buffer name> into the
     * related file triggers a deletion of bufferN entry in sysfs */
    if( (err = sysfs_create_bin_file(dma_kobj, attr_bin_free)) )
    { UIO_PDA_ERROR("Can't create entry for buffer freeing!\n", exit_free); }

    UIO_DEBUG_PRINTF("Add mps entry\n");
    if( (err=sysfs_create_bin_file(dma_kobj, attr_bin_max_payload_size)) )
    { UIO_PDA_ERROR("Can't create entry for maximum payload size!\n", exit_max_payload_size); }

    UIO_DEBUG_PRINTF("Add mrrs entry\n");
    if( (err = sysfs_create_bin_file(dma_kobj, attr_bin_max_read_request_size)) )
    { UIO_PDA_ERROR("Can't create entry for maximum read request size!\n", exit_max_read_request_size); }

    /* create attributes for BAR mappings */
    if(pci_request_regions(pci_device, DRIVER_NAME))
    {
        printk(DRIVER_NAME " : Can't retrieve PCI BARs!\n");
        goto exit;
    }

    uint32_t i = 0;
    uint32_t j = 0;
    for(i = 0; i<PCI_NUM_RESOURCES; i++)
    {
        dma_device->attr_bar[i].attr.name = NULL;
        dma_device->attr_bar[i].attr.mode = S_IRUGO|S_IWUGO;
        dma_device->attr_bar[i].size      = 0;
        dma_device->attr_bar[i].read      = NULL;
        dma_device->attr_bar[i].write     = NULL;
        dma_device->attr_bar[i].mmap      = NULL;

        if(pci_device->resource[i].flags & IORESOURCE_MEM)
        {
            dma_device->attr_bar[i].attr.name = kmalloc(10*sizeof(char), GFP_KERNEL);
            sprintf((char*)dma_device->attr_bar[i].attr.name, "bar%u", i);

            dma_device->attr_bar[i].size      = pci_resource_len(pci_device, i);
            dma_device->attr_bar[i].read      = uio_pci_dma_sysfs_mock;
            dma_device->attr_bar[i].write     = uio_pci_dma_sysfs_mock;
            dma_device->attr_bar[i].mmap      = uio_pci_dma_sysfs_bar_mmap;

            if(sysfs_create_bin_file(&pci_device->dev.kobj, &dma_device->attr_bar[i]))
            { UIO_PDA_ERROR(" Can't create BAR object!\n", exit_bars); }
        }
    }

    /** Set driver specific data. */
    UIO_DEBUG_PRINTF("Register device and set the driver specific data\n");
    if(uio_register_device(&pci_device->dev, &dma_device->info) )
    { UIO_PDA_ERROR( "Registering device failed!\n", exit_max_read_request_size); }
    pci_set_drvdata(pci_device, dma_device);

    printk(DRIVER_NAME " : initialized new DMA device (%x %x)\n",
           pci_device->vendor, pci_device->device);

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);

exit_bars:
    for(j=i--; j>=0; j--)
    {
        if(pci_device->resource[i].flags & IORESOURCE_MEM)
        { sysfs_remove_bin_file(&pci_device->dev.kobj, &dma_device->attr_bar[i]); }
    }

exit_max_read_request_size:
    sysfs_remove_bin_file(dma_kobj, attr_bin_max_read_request_size);

exit_max_payload_size:
    sysfs_remove_bin_file(dma_kobj, attr_bin_max_payload_size);

exit_free:
    sysfs_remove_bin_file(dma_kobj, attr_bin_free);

exit_request:
    sysfs_remove_bin_file(dma_kobj, attr_bin_request);

exit:
    if(dma_device)
    { kfree(dma_device); }

    if(dma_kobj)
    { kobject_del(dma_kobj); }

    if(msi_enabled)
    { pci_disable_msi(pci_device); }

    if(attr_bin_request)
    { kfree(attr_bin_request); }

    if(attr_bin_free)
    { kfree(attr_bin_free); }

    if(attr_bin_max_payload_size)
    { kfree(attr_bin_max_payload_size); }

    if(attr_bin_max_read_request_size)
    { kfree(attr_bin_max_read_request_size); }

    UIO_DEBUG_RETURN(err);
}



static void
remove(struct pci_dev *pci_device)
{
    UIO_DEBUG_ENTER();
    struct uio_pci_dma_device *dma_device = pci_get_drvdata(pci_device);
    struct kobject            *kobj       = dma_device->dma_kobj;
    struct kobject            *k          = NULL;

    /* Remove all allocated DMA buffers */
    struct kset *kset = to_kset(kobj);
    spin_lock( &kset->list_lock );
    {
        list_for_each_entry(k, &kset->list, entry)
        { uio_pci_dma_free(k); }
    }
    spin_unlock( &kset->list_lock );

    kobject_del(dma_device->dma_kobj);
    kset_unregister(kset);

    /* Remove memory regions */
    uint32_t i = 0;
    for(i = 0; i<PCI_NUM_RESOURCES; i++)
    {
        if(pci_device->resource[i].flags & IORESOURCE_MEM)
        { sysfs_remove_bin_file(&pci_device->dev.kobj, &dma_device->attr_bar[i]); }
        kfree(dma_device->attr_bar[i].attr.name);
    }

    /* Unregister device */
    uio_unregister_device(&dma_device->info);
    if(dma_device->msi_enabled)
    { pci_disable_msi(pci_device); }

    pci_release_regions(pci_device);
    pci_disable_device(pci_device);
    UIO_DEBUG_PRINTF("Freeing dma_device\n");
    kfree(dma_device);

    printk(DRIVER_NAME " : removed DMA device (%x %x)\n", pci_device->vendor, pci_device->device);
    UIO_DEBUG_PRINTF("remove EXIT\n");
}


static const struct pci_device_id id_table[] = {
    {PCI_DEVICE(0x10dc, 0x01a0) }, /* C-RORC PCI ID as registered at CERN */
    {PCI_DEVICE(0x10dc, 0xbeaf) }, /* FLIB intermediate PCI ID */
    { 0, }
};

static
struct pci_driver
driver =
{
    .name     = DRIVER_NAME,
    .id_table = id_table,
    .probe    = probe,
    .remove   = remove
};

static int __init mod_init(void)
{
    UIO_DEBUG_ENTER();
        printk(DRIVER_DESC " version: " UIO_PCI_DMA_VERSION "\n");
        printk(DRIVER_DESC " minor  : " UIO_PCI_DMA_MINOR "\n");
    UIO_DEBUG_RETURN(pci_register_driver(&driver));
}

static void __exit mod_exit(void)
{
    UIO_DEBUG_ENTER();
        pci_unregister_driver(&driver);
        printk("Unloaded " DRIVER_DESC " version: " UIO_PCI_DMA_VERSION "\n");
    UIO_DEBUG_PRINTF("mod_exit EXIT\n");
}

/** CALLBACKS */

/*! \brief uio_pci_dma_request_buffer_write
 *         Callback function that gets called when someone pipes a
 *         uio_pci_dma_private (see header file) into the "request"
 *         attribute of the DMA subsystem. */
BIN_ATTR_WRITE_CALLBACK( request_buffer_write )
{
    UIO_DEBUG_ENTER();
    struct uio_pci_dma_private *request = (struct uio_pci_dma_private*)buffer;
    struct kset                *kset    = container_of(kobj, struct kset, kobj);
    struct uio_pci_dma_private *priv    = NULL;
    struct bin_attribute        attrib;

    /* Check that really a request struct is passed */
    if(count != sizeof(struct uio_pci_dma_private) )
    { UIO_PDA_ERROR("Invalid size of request struct -> aborting!\n", exit); }

    spin_lock(&alloc_free_lock);

    UIO_DEBUG_PRINTF("MB = %zu are requested\n", (request->size/MB_SIZE) );

    /* Allocate and initialize internal bookkeeping */
    priv = kzalloc(sizeof(struct uio_pci_dma_private), GFP_KERNEL);
    if(!priv)
    { UIO_PDA_ERROR("Allocation failed!\n", exit); }
    memcpy(priv, buffer, count);

    priv->device = container_of(kobj->parent, struct device, kobj);

    kobject_init(&priv->kobj, &priv->type);

    struct kobject *tmp_kobj = NULL;
    KSET_FIND(kset, request->name, tmp_kobj);
    if(tmp_kobj != NULL)
    { UIO_PDA_ERROR("Buffer already exists!\n", exit); }

    priv->kobj.kset = kset;
    if(kobject_add(&priv->kobj, NULL, request->name))
    { UIO_PDA_ERROR("Can't create buffer object!\n", exit); }

    /* First try to allocate the memory */
    priv->buffer    = NULL;
    priv->sg        = NULL;
    priv->scatter   = NULL;
    priv->page_list = NULL;
    priv->pfn_list  = NULL;
    priv->length    = 0;
    priv->start     = request->start;

    if(request->start == 0)
    {
        if(uio_pci_dma_allocate_kernel_memory(priv) != UIO_PCI_DMA_SUCCESS)
        { UIO_PDA_ERROR( "Kernel DMA buffer allocation failed!\n", exit);     }
    } else {
        if(uio_pci_dma_allocate_user_memory(priv)   != UIO_PCI_DMA_SUCCESS)
        { UIO_PDA_ERROR( "User space DMA buffer allocation failed!\n", exit); }
    }

    UIO_DEBUG_PRINTF("init priv pointer %p\n", priv);

    /* Define and allocate the struct attr_bin_map */
    BIN_ATTR_PDA(map, priv->size, S_IWUGO | S_IRUGO, uio_pci_dma_sysfs_mock,
                 uio_pci_dma_sysfs_mock, uio_pci_dma_sysfs_map);

    /* Define and allocate the struct attr_bin_sg */
    BIN_ATTR_PDA(sg, (priv->length * sizeof(struct scatter) ), S_IRUGO,
                 uio_pci_dma_sysfs_mock, uio_pci_dma_sysfs_mock, uio_pci_dma_sysfs_map_sg);

    UIO_DEBUG_PRINTF("uio_pci_dma_request_buffer_write size %llu\n",
        (priv->length * sizeof(struct scatterlist) ) );

    /* Add entry for memory mapping of the DMA buffer */
    if(sysfs_create_bin_file(&priv->kobj, attr_bin_map))
    { UIO_PDA_ERROR("Can't create entry for buffer mapping!\n", exit_binfile); }

    /* Add entry to expose the sg-list of the DMA buffer */
    if(sysfs_create_bin_file(&priv->kobj, attr_bin_sg))
    { UIO_PDA_ERROR("Can't create entry for sg list exposing!\n", exit_binfile); }

    kobject_uevent(&priv->kobj, KOBJ_ADD);

    spin_unlock(&alloc_free_lock);

    UIO_DEBUG_RETURN(sizeof(struct uio_pci_dma_private));

exit_binfile:
    attrib.attr.name = "map";
    sysfs_remove_bin_file(&priv->kobj, &attrib);

    attrib.attr.name = "sg";
    sysfs_remove_bin_file(&priv->kobj, &attrib);

    kobject_del(&priv->kobj);

    if(attr_bin_sg)
    { kfree(attr_bin_sg);  }

    if(attr_bin_map)
    { kfree(attr_bin_map); }

exit:
    if(priv)
    {
        free_memory_lists(priv);
        kfree(priv);
        priv = NULL;
    }

    spin_unlock(&alloc_free_lock);

    UIO_DEBUG_RETURN(-1);
}

static inline void
free_memory_lists(struct uio_pci_dma_private *priv)
{
    UIO_DEBUG_ENTER();

    if(priv != NULL)
    {
        if(priv->page_list != NULL)
        {
            UIO_DEBUG_PRINTF("Freeing priv->page_list\n");
            vfree(priv->page_list);
            priv->page_list = NULL;
        }

        if(priv->pfn_list != NULL)
        {
            UIO_DEBUG_PRINTF("Freeing priv->pfn_list\n");
            vfree(priv->pfn_list);
            priv->pfn_list = NULL;
        }

        if(priv->sg != NULL)
        {
            UIO_DEBUG_PRINTF("Freeing priv->sg\n");
            vfree(priv->sg);
            priv->sg = NULL;
        }

        if(priv->scatter != NULL)
        {
            UIO_DEBUG_PRINTF("Freeing priv->scatter\n");
            vfree(priv->scatter);
            priv->scatter = NULL;
        }
    }

    UIO_DEBUG_PRINTF("free_memory_lists EXIT\n");
}

static inline int
program_iommu(struct uio_pci_dma_private* priv)
{
#ifdef UIO_PDA_IOMMU
    UIO_DEBUG_ENTER()
    uint64_t count = dma_map_sg(priv->device, priv->sg, priv->length, DMA_BIDIRECTIONAL);
    if(count == 0)
    {
        UIO_DEBUG_PRINTF("Mapping SG list failed!\n");
        UIO_DEBUG_RETURN(UIO_PCI_DMA_ERROR);
    }

    priv->old_length = priv->length;
    priv->length     = count;
    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);
#endif
}

static inline int
uio_pci_dma_allocate_user_memory
(
    struct uio_pci_dma_private *priv
)
{
    UIO_DEBUG_ENTER();

    if( (priv->size) < 1)
    { UIO_PDA_ERROR("Zero amount of pages requested!\n", exit); }

    UIO_DEBUG_PRINTF("Calculate the amount of pages needed\n");
    int pages = (priv->size / PAGE_SIZE);
    if( (priv->size % PAGE_SIZE) != 0)
    {
        pages++;
        priv->size = pages * PAGE_SIZE;
        UIO_DEBUG_PRINTF("Adjusted page number\n");
    }
    priv->length = pages;
    priv->pages  = pages;

    UIO_DEBUG_PRINTF("Check for page alignment\n");
    if( (priv->start % PAGE_SIZE) != 0 )
    { UIO_PDA_ERROR("Buffer is not page aligned!\n", exit); }

    UIO_DEBUG_PRINTF("Allocating priv->page_list\n");
    priv->page_list = vmalloc_user(pages * sizeof(struct page *));
    if(!priv->page_list)
    { UIO_PDA_ERROR("Page list allocation failed!\n", exit); }

    #ifdef UIO_PDA_USE_PAGEFAULT_HANDLER
    UIO_DEBUG_PRINTF("Allocating priv->pfn_list\n");
    priv->pfn_list = vmalloc_user(pages * sizeof(unsigned long));
    if(!priv->pfn_list)
    { UIO_PDA_ERROR("Page list allocation failed!\n", exit); }
    #endif

    UIO_DEBUG_PRINTF("Get user pages\n");
#if defined(PDA_FIVEARG_GUP)
    int pages_mapped
        = get_user_pages
            (priv->start, pages, FOLL_WRITE, priv->page_list, NULL);
#elif defined(PDA_SIXARG_GUP)
    int pages_mapped
        = get_user_pages
            (priv->start, pages, 1, 0, priv->page_list, NULL);
#else
    int pages_mapped
        = get_user_pages
            (current, current->mm, priv->start, pages, 1, 0, priv->page_list, NULL);
#endif
    if(pages_mapped != pages)
    {
        UIO_DEBUG_PRINTF("pages_mapped = %d pages = %d\n", pages_mapped, pages);
        UIO_PDA_ERROR("User page translation failed!\n", exit)
    }

    UIO_DEBUG_PRINTF("Allocating priv->sg\n");
    if( !(priv->sg = (struct scatterlist*)vmalloc_user(pages * sizeof(struct scatterlist))) )
    { UIO_PDA_ERROR("Unable to allocate memory for SG list!", exit); }

    sg_init_table( (priv->sg), pages);

    UIO_DEBUG_PRINTF("Put pages into scatter gather list\n");
    uint64_t i;
    for(i=0; i<pages; i++)
    {
        struct page *current_page = priv->page_list[i];
        sg_set_page( &priv->sg[i], current_page, PAGE_SIZE, 0);
        #ifdef UIO_PDA_USE_PAGEFAULT_HANDLER
        priv->pfn_list[i] = page_to_pfn(current_page);
        #endif
    }

    if(program_iommu(priv) == UIO_PCI_DMA_ERROR)
    { UIO_PDA_ERROR("SG list conversion failed!\n", exit); }

    UIO_DEBUG_SG( "Scatter:\n", (priv->sg), priv->length );

    if(convert_sg(priv) != UIO_PCI_DMA_SUCCESS)
    { UIO_PDA_ERROR("SG list conversion failed!\n", exit); }

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);

exit:
    UIO_DEBUG_RETURN(UIO_PCI_DMA_ERROR);
}


/* Allocate a private structure for secure mapping into the user space */
static inline int
convert_sg(struct uio_pci_dma_private* priv)
{
    UIO_DEBUG_ENTER();
    int ret = UIO_PCI_DMA_SUCCESS;

    UIO_DEBUG_PRINTF("Allocating priv->scatter\n");
    if( !(priv->scatter = (struct scatter*)vmalloc_user(priv->length * sizeof(struct scatter))) )
    { ret = UIO_PCI_DMA_ERROR; UIO_PDA_ERROR("Allocation failed!\n", exit); }

    uint64_t i;
    for(i = 0; i < (priv->length); i++)
    {
        priv->scatter[i].page_link   = priv->sg[i].page_link;
        priv->scatter[i].offset      = priv->sg[i].offset;
        priv->scatter[i].length      = sg_dma_len(&(priv->sg[i]));
        priv->scatter[i].dma_address = sg_dma_address(&(priv->sg[i]));
    }

    printk(DRIVER_NAME " : %llu pages requested (in %llu entries), buffer "
                       "%s is allocated\n", priv->pages, priv->length, priv->name);

exit:
    UIO_DEBUG_RETURN(ret);
}


static inline int
uio_pci_dma_allocate_kernel_memory
(
    struct uio_pci_dma_private *priv
)
{
    UIO_DEBUG_ENTER();

    if( (priv->size) < 1)
    { UIO_PDA_ERROR("Zero amount of pages requested!\n", exit); }

    priv->pages = (priv->size / PAGE_SIZE);
    if( (priv->size % PAGE_SIZE) != 0)
    {
        priv->pages++;
        priv->size = priv->pages * PAGE_SIZE;
        UIO_DEBUG_PRINTF("Adjusted page number\n");
    }

    UIO_DEBUG_PRINTF("Allocating priv->sg\n");
    if( !(priv->sg = (struct scatterlist*)vmalloc_user(priv->pages * sizeof(struct scatterlist))) )
    { UIO_PDA_ERROR("Unable to allocate memory for SG list!", exit); }

    sg_init_table( (priv->sg), priv->pages);

    /* Get the order for chunks which can be allocated in a consecutive buffer.
     * We allocate chunks with the size of MAX_ORDER, which is the maximum that
     * we can get in a consecutive buffer. */
    uint8_t order = 0;
    if( (order = get_order(priv->size)) > (MAX_ORDER-1) )
    { order = (MAX_ORDER - 1); }
    UIO_DEBUG_PRINTF("Page order = %u (MAX_ORDER = %u) Pages to allocate = %llu\n",
                     order, MAX_ORDER, priv->pages);

    uint64_t pages_left   = priv->pages;
    priv->length = 0;
    while(pages_left > 0)
    {
        while( (1<<order) > pages_left)
        { order--; }

        struct page *page = NULL;
        #if CONFIG_NUMA == 1
            if(priv->numa_node == -1)
            { page = alloc_pages( GFP_HIGHUSER, order ); }
            else
            { page = alloc_pages_node(priv->numa_node, GFP_HIGHUSER, order); }
        #else
            page = alloc_pages( GFP_HIGHUSER, order );
        #endif

        if(!page)
        {
            printk(DRIVER_NAME " : Failed to allocate DMA memory!\n");
            goto exit;
        }

        uint64_t pages        = (1 << order);
        size_t   chunk_length = pages * PAGE_SIZE;

        uint64_t i;
        for(i = 0; i < pages; i++)
        { SetPageReserved(&page[i]); }

        sg_set_page(&priv->sg[priv->length], page, chunk_length, 0);
        priv->length++;
        pages_left = pages_left - pages;
    }

    if(program_iommu(priv) == UIO_PCI_DMA_ERROR)
    { UIO_PDA_ERROR("SG list conversion failed!\n", exit); }

    UIO_DEBUG_SG( "Scatter:\n", (priv->sg), priv->length );
    sort(priv->sg, priv->length, sizeof(struct scatterlist), sort_sg_compare, sort_sg_swap);
    UIO_DEBUG_SG( "Sorted Scatter:\n", (priv->sg), priv->length );

    if(convert_sg(priv) != UIO_PCI_DMA_SUCCESS)
    { UIO_PDA_ERROR("SG list conversion failed!\n", exit); }

#ifdef UIO_PDA_USE_PAGEFAULT_HANDLER
        UIO_DEBUG_PRINTF("Allocating priv->page_list\n");
        priv->page_list = vmalloc_user(priv->pages * sizeof(struct page *));
        if(!priv->page_list)
        { UIO_PDA_ERROR("Page list allocation failed!\n", exit); }

        UIO_DEBUG_PRINTF("Allocating priv->pfn_list\n");
        priv->pfn_list = vmalloc_user(priv->pages * sizeof(unsigned long));
        if(!priv->pfn_list)
        { UIO_PDA_ERROR("Page list allocation failed!\n", exit); }

        uint64_t page_index = 0;
        uint64_t j, i;
        for(i = 0; i < priv->length; i++)
        {
            struct page *page = sg_page(&(priv->sg[i]));
            uint64_t sg_size  =
            #ifdef UIO_PDA_IOMMU
                sg_dma_len(&(priv->sg[i]));
            #else
                priv->sg[i].length;
            #endif

            uint64_t pages = sg_size >> PAGE_SHIFT;
            for(j=0; j<pages; j++)
            {
                struct page *current_page = nth_page(page, j);
                priv->page_list[page_index] = current_page;
                priv->pfn_list[page_index] = page_to_pfn(current_page);
                page_index++;
            }
        }
#endif

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);

exit:
    uio_pci_dma_free_kernel_memory(priv);
    UIO_DEBUG_RETURN(UIO_PCI_DMA_ERROR);
}

int
sort_sg_compare
(
    const void *item1,
    const void *item2
)
{
    struct scatterlist *sg1 = (struct scatterlist*)item1;
    struct scatterlist *sg2 = (struct scatterlist*)item2;

    return( (sg1->dma_address > sg2->dma_address) ? 1 : -1 );
}

void
sort_sg_swap
(
    void *item1,
    void *item2,
    int   size
)
{
    struct scatterlist sg_tmp;

    memcpy(&sg_tmp, item1, sizeof(struct scatterlist) );
    memcpy(item1, item2, sizeof(struct scatterlist)   );
    memcpy(item2, &sg_tmp, sizeof(struct scatterlist) );
}



/*! \brief uio_pci_dma_delete_buffer_write
 *         Callback function that gets called when someone pipes a string
 *         into the "free" attribute of the DMA subsystem. The string should
 *         contain buffer name. */
BIN_ATTR_WRITE_CALLBACK( delete_buffer_write )
{
    UIO_DEBUG_ENTER();

    spin_lock(&alloc_free_lock);

    struct bin_attribute attrib;
    char                 tmp_string[count+1];
    struct kset         *kset_pointer = to_kset(kobj);
    struct kobject      *buffer_kobj  = NULL;
    attrib.attr.name                  = "map";

    snprintf(tmp_string, count, "%s", buffer);
    KSET_FIND( kset_pointer, tmp_string, buffer_kobj );
    if(buffer_kobj != NULL)
    {
        attrib.attr.name = "map";
        sysfs_remove_bin_file(buffer_kobj, &attrib);

        attrib.attr.name = "sg";
        sysfs_remove_bin_file(buffer_kobj, &attrib);

        uio_pci_dma_free(buffer_kobj);
        kobject_del(buffer_kobj);
    }
    else
    { printk(DRIVER_NAME " : freeing of buffer %s failed!\n", tmp_string); }

    spin_unlock(&alloc_free_lock);

    UIO_DEBUG_RETURN(count);
}

static inline int
uio_pci_dma_free(struct kobject *kobj)
{
    UIO_DEBUG_ENTER();

    if(!kobj)
    { UIO_PDA_ERROR("No kobject!\n", exit); }

    struct uio_pci_dma_private *priv;
    if( !(priv=container_of(kobj, struct uio_pci_dma_private, kobj)) )
    { UIO_PDA_ERROR("Getting container failed!\n", exit); }

    char kobj_name[1024];
    strncpy(kobj_name, kobject_name(kobj), 1024);
    printk(DRIVER_NAME " : Freeing buffer %s\n", kobj_name);

#ifdef UIO_PDA_IOMMU
    if(!priv->sg)
    { UIO_PDA_ERROR("This is no valid scatter gather list!\n", exit); }

    if(priv->device)
    { dma_unmap_sg(priv->device, priv->sg, priv->old_length, DMA_BIDIRECTIONAL); }
#endif

    if(priv->start == 0)
    {
        if(uio_pci_dma_free_kernel_memory(priv) != UIO_PCI_DMA_SUCCESS)
        { UIO_PDA_ERROR("Freeing failed!\n", exit);   }
    } else {
        if(uio_pci_dma_free_user_memory(priv) != UIO_PCI_DMA_SUCCESS)
        { UIO_PDA_ERROR("Releasing failed!\n", exit); }
    }

    if(priv)
    {
        free_memory_lists(priv);
        kfree(priv);
    }

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);

exit:
    UIO_DEBUG_RETURN(UIO_PCI_DMA_ERROR);
}

static inline int
uio_pci_dma_free_user_memory(struct uio_pci_dma_private *priv)
{
    UIO_DEBUG_ENTER();

    uint64_t i;
    for(i=0; i<(priv->pages); i++)
    { put_page(priv->page_list[i]); }

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);
}

static inline int
uio_pci_dma_free_kernel_memory(struct uio_pci_dma_private *priv)
{
    UIO_DEBUG_ENTER();

    uint64_t            i   = 0;
    struct scatterlist *sgp = NULL;
    for_each_sg(priv->sg, sgp, priv->length, i)
    {
        size_t       chunk_length = sgp->length;
        struct page *page         = sg_page(sgp);
        size_t       reserve      = chunk_length + PAGE_SIZE;
        while(reserve -= PAGE_SIZE)
        { ClearPageReserved(page++); }

        __free_pages( sg_page(sgp), get_order(chunk_length) );
    }

    UIO_DEBUG_RETURN(UIO_PCI_DMA_SUCCESS);
}


#ifdef UIO_PDA_USE_PAGEFAULT_HANDLER
#ifdef PDA_VMF_T
static vm_fault_t
#else
static int
#endif
page_fault_handler
(
#ifndef PDA_VMF_T
    struct vm_area_struct *vma,
#endif
    struct vm_fault       *vmf
)
{
    UIO_DEBUG_ENTER();

#ifdef PDA_VMF_T
    struct vm_area_struct *vma = vmf->vma;
#endif
    struct uio_pci_dma_private *priv = vma->vm_private_data;

#ifdef PDA_PFN_T_PAGES
#ifdef PDA_VMF_T
    return vmf_insert_mixed(vma, (unsigned long)vmf->address,
                              pfn_to_pfn_t(priv->pfn_list[vmf->pgoff % priv->pages]));
#else
    int ret = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address,
                              pfn_to_pfn_t(priv->pfn_list[vmf->pgoff % priv->pages]));
#endif
#else
#ifdef PDA_VMF_T
    return vmf_insert_mixed(vma, (unsigned long)vmf->address,
                              priv->pfn_list[vmf->pgoff % priv->pages]);
#else
    int ret = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address,
                              priv->pfn_list[vmf->pgoff % priv->pages]);
#endif
#endif

#ifndef PDA_VMF_T
    switch(ret)
    {
        case 0:
        case -ERESTARTSYS :
        case -EINTR       : { UIO_DEBUG_RETURN(VM_FAULT_NOPAGE); }
        case -ENOMEM      : { UIO_DEBUG_RETURN(VM_FAULT_OOM);    }
        default           : { UIO_DEBUG_RETURN(VM_FAULT_SIGBUS); }
    }
#endif

}

static struct vm_operations_struct
uio_mmap_ops =
{
   .fault = page_fault_handler
};
#endif

/*! \brief uio_pci_dma_map
 *         Callback function that gets called when someone mmaps the related
 *         map attribute. The user process needs to map the whole file. The
 *         right size is the file size of the map attribute. */
BIN_ATTR_MAP_CALLBACK( map )
{
    UIO_DEBUG_ENTER();

    int ret = 0;
    if(kobj == NULL)
    {
        ret = UIO_PCI_DMA_ERROR;
        UIO_PDA_ERROR("Kobject does not exist anymore, race-condition?\n", exit)
    }

    struct uio_pci_dma_private *priv =
        container_of(kobj, struct uio_pci_dma_private, kobj);

    if(priv == NULL)
    {
        ret = UIO_PCI_DMA_ERROR;
        UIO_PDA_ERROR("Can't get private structure, race-condition?\n", exit)
    }

    if( (vma->vm_flags & (VM_SHARED | VM_MAYWRITE) ) == VM_MAYWRITE )
    {
        ret = UIO_PCI_DMA_ERROR;
        UIO_PDA_ERROR( "You try to map COW memory. This can't be done with "
                       "this buffer, because the map insertion is scattered. "
                       "Please try to alter your mmap flags from MAP_PRIVATE "
                       "to MAP_SHARED or something similar.\n", exit);
    }

#ifndef UIO_PDA_USE_PAGEFAULT_HANDLER
    unsigned long       start        = vma->vm_start;
    vma->vm_page_prot                = pgprot_noncached(vma->vm_page_prot);
    uint64_t            request_size = (vma->vm_end - vma->vm_start);
    size_t              buffer_size  = priv->size;
    struct scatterlist *sgp          = priv->sg;
    size_t              mapped_size  = 0;
    for(mapped_size = 0; mapped_size<request_size; )
    {
        unsigned long pfn = page_to_pfn( sg_page(sgp) );
        uint64_t sg_size  =
        #ifdef UIO_PDA_IOMMU
            sg_dma_len(sgp);
        #else
            sgp->length;
        #endif
        if( (ret = remap_pfn_range(vma, start, pfn, sg_size, vma->vm_page_prot)) < 0)
        { ret = UIO_PCI_DMA_ERROR; UIO_PDA_ERROR("Buffer mapping failed!\n", exit); }

        start        = start + sg_size;
        sgp          = sg_next(sgp);
        mapped_size += sg_size;

        /** Wrap if needed */
        if( (mapped_size % buffer_size) == 0 )
        { sgp = priv->sg; }
    }
#else
    vma->vm_private_data  = priv;
    vma->vm_flags        &= ~VM_PFNMAP;
    vma->vm_flags        |= VM_MIXEDMAP;
    vma->vm_ops           = &uio_mmap_ops;
#endif

exit:
    UIO_DEBUG_RETURN(ret);
}



/*! \brief uio_pci_dma_map_sg
 *         Callback function that gets called when someone mmaps the related
 *         sg attribute to get the scatter gather list. The user process needs
 *         to map the whole file. The right size is the file size of the map
 *         attribute. */
BIN_ATTR_MAP_CALLBACK( map_sg )
{
    UIO_DEBUG_ENTER();

    struct uio_pci_dma_private *priv =
        container_of(kobj, struct uio_pci_dma_private, kobj);
    vma->vm_private_data = priv;
    vma->vm_ops          = NULL;
    int ret              = 0;

    /** Mapping ranges can only be aligned to page sizes. */
    size_t sg_size = (priv->length) * sizeof(struct scatter);
    if( (sg_size % PAGE_SIZE)  != 0)
    { sg_size = ( (sg_size/PAGE_SIZE) + 1) * PAGE_SIZE; }

    if( (vma->vm_end - vma->vm_start) != sg_size)
    {
        UIO_DEBUG_PRINTF("Mapping sizes (%lu != %llu)\n",
                         (vma->vm_end - vma->vm_start),
                         (priv->length * sizeof(struct scatter)));
        UIO_PDA_ERROR("Requested mapping does not have the same size", exit);
    }

    ret = remap_vmalloc_range(vma, priv->scatter, 0);

exit:
    UIO_DEBUG_RETURN(ret);
}



static inline
int pcie_get_cap_offset(struct pci_dev *pci_device)
{
    UIO_DEBUG_ENTER();

    int pcie_cap = 0;

#if LINUX_VERSION_CODE <  KERNEL_VERSION(3, 0, 0)
    pcie_cap = pci_find_capability(pci_device, PCI_CAP_ID_EXP);
#endif

#if LINUX_VERSION_CODE <  KERNEL_VERSION(3, 13, 0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 0, 0)
    pcie_cap = pci_device->pcie_cap;
#endif
#endif

    UIO_DEBUG_RETURN(pcie_cap);
}

static inline
uint16_t pcie_get_ctrl(struct pci_dev *pci_device)
{
    UIO_DEBUG_ENTER();
    int pcie_cap = pcie_get_cap_offset(pci_device);
    if(pcie_cap == 0)
    { UIO_DEBUG_RETURN(0); }
    uint16_t ctrl;
    pci_read_config_word(pci_device, (pcie_cap+PCI_EXP_DEVCTL), &ctrl);
    UIO_DEBUG_RETURN(ctrl);
}

static inline
int pcie_get_max_payload(struct pci_dev *pci_device)
{
    UIO_DEBUG_ENTER();

#if LINUX_VERSION_CODE <  KERNEL_VERSION(3, 13, 0)
    uint16_t ctrl = pcie_get_ctrl(pci_device);
    if(ctrl == 0){ UIO_DEBUG_RETURN(0); }
    UIO_DEBUG_RETURN( 1 << ( ( (ctrl>>5) & 0x07) + 7) );
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
    UIO_DEBUG_RETURN(pcie_get_mps(pci_device));
#endif
}

static inline
int pcie_get_max_read_request(struct pci_dev *pci_device)
{
    UIO_DEBUG_ENTER();

#if LINUX_VERSION_CODE <  KERNEL_VERSION(3, 13, 0)
    uint16_t ctrl = pcie_get_ctrl(pci_device);
    if(ctrl == 0){ UIO_DEBUG_RETURN(0); }
    UIO_DEBUG_RETURN( 128 << ((ctrl & PCI_EXP_DEVCTL_READRQ) >> 12) );
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
    UIO_DEBUG_RETURN(pcie_get_readrq(pci_device));
#endif
}

/*! \brief uio_pci_dma_sysfs_mps
 *
 */
BIN_ATTR_READ_CALLBACK( mps )
{
    UIO_DEBUG_ENTER();

    int *mps = (int*)buffer;

    struct device *device
        = container_of(kobj->parent, struct device, kobj);
    struct pci_dev *pci_device
        = container_of(device, struct pci_dev, dev);

    mps[0] = pcie_get_max_payload(pci_device);

    UIO_DEBUG_PRINTF( "max payload size = %d\n", mps[0]);
    UIO_DEBUG_RETURN(sizeof(int));
}

/*! \brief uio_pci_dma_sysfs_readrq
 *
 */
BIN_ATTR_READ_CALLBACK( readrq )
{
    UIO_DEBUG_ENTER();

    int *readrq = (int*)buffer;

    struct device *device
        = container_of(kobj->parent, struct device, kobj);
    struct pci_dev *pci_device
        = container_of(device, struct pci_dev, dev);

    readrq[0] = pcie_get_max_read_request(pci_device);

    UIO_DEBUG_PRINTF( "max read request size = %d\n", readrq[0]);
    UIO_DEBUG_RETURN(sizeof(int));
}


module_init(mod_init);
module_exit(mod_exit);

MODULE_VERSION(UIO_PCI_DMA_VERSION);
MODULE_LICENSE(DRIVER_LICENSE);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
