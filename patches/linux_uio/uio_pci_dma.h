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



#ifndef UIO_PCI_DMA_H
#define UIO_PCI_DMA_H

/** Uncomment this for SLC6 */
/*
#ifdef LINUX_VERSION_CODE
    #undef LINUX_VERSION_CODE
#endif
#define LINUX_VERSION_CODE KERNEL_VERSION(2,6,35)
*/

#define UIO_PCI_DMA_VERSION "0.7.0"
#define UIO_PCI_DMA_MINOR   "0"

#define UIO_PCI_DMA_SUCCESS 0
#define UIO_PCI_DMA_ERROR   -1

#define MB_SIZE 1048576

#ifndef __KERNEL__
#include <limits.h>

#if ( __WORDSIZE == 64 )
   #define BUILD_64   1
#else
   #define BUILD_32
#endif /* __WORDSIZE */

struct kobject
{
    char name[1024];
};

struct kobj_type
{
    char name[1024];
};

#ifdef BUILD_64
    typedef uint64_t dma_addr_t;
#else
    typedef uint32_t dma_addr_t;
#endif /* dma_addr_t */

#endif /** #ifndef __KERNEL__ */

struct
__attribute__((__packed__))
uio_pci_dma_private
{
    char       name[1024];
    size_t     size;
    int32_t    numa_node;
    uint64_t   start;

    /* Kernel internal stuff */
    struct     kobject       kobj;
    struct     kobj_type     type;
    char                    *buffer;
    struct     scatterlist  *sg;
    struct     scatter      *scatter;
    uint64_t                 length;
    uint64_t                 old_length;
    uint64_t                 pages;
    struct     device       *device;
    struct     page        **page_list;
    unsigned long           *pfn_list;
};

struct scatter
{
    unsigned long       page_link;
    unsigned int        offset;
    unsigned int        length;
             dma_addr_t dma_address;
};

#ifdef __KERNEL__

/** Kernel versions >= 3.8.0  don't have the __devinit flag anymore */
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
        #ifndef __devinit
            #define __devinit
        #endif /** __devinit */
    #endif

/** Kernel versions <= 2.6.34 don't get a file pointer passed to the
 *  sysfs-callbacks
 */
    #if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 34)
    #define BIN_ATTR_WRITE_CALLBACK( name )                                            \
    ssize_t                                                                            \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct kobject       *kobj,                                                    \
        struct bin_attribute *attr,                                                    \
        char                 *buffer,                                                  \
        loff_t                offset,                                                  \
        size_t                count                                                    \
    )

    #define BIN_ATTR_READ_CALLBACK( name )                                             \
    ssize_t                                                                            \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct kobject       *kobj,                                                    \
        struct bin_attribute *attr,                                                    \
        char                 *buffer,                                                  \
        loff_t                offset,                                                  \
        size_t                count                                                    \
    )

    #define BIN_ATTR_MAP_CALLBACK( name )                                              \
    int                                                                                \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct kobject        *kobj,                                                   \
        struct bin_attribute  *attr,                                                   \
        struct vm_area_struct *vma                                                     \
    )
    #endif

    #if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 34)
    #define BIN_ATTR_WRITE_CALLBACK( name )                                            \
    ssize_t                                                                            \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct file          *file,                                                    \
        struct kobject       *kobj,                                                    \
        struct bin_attribute *attr,                                                    \
        char                 *buffer,                                                  \
        loff_t                offset,                                                  \
        size_t                count                                                    \
    )

    #define BIN_ATTR_READ_CALLBACK( name )                                             \
    ssize_t                                                                            \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct file          *file,                                                    \
        struct kobject       *kobj,                                                    \
        struct bin_attribute *attr,                                                    \
        char                 *buffer,                                                  \
        loff_t                offset,                                                  \
        size_t                count                                                    \
    )

    #define BIN_ATTR_MAP_CALLBACK( name )                                              \
    int                                                                                \
    uio_pci_dma_sysfs_ ## name                                                         \
    (                                                                                  \
        struct file           *file,                                                   \
        struct kobject        *kobj,                                                   \
        struct bin_attribute  *attr,                                                   \
        struct vm_area_struct *vma                                                     \
    )
    #endif

/** Kernel versions <= 2.6.31 don't have certain defines for PCI config space
 *  adressing */
    #if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 31)
        #ifndef PCI_STATUS_INTERRUPT
            #define PCI_STATUS_INTERRUPT 0x08
        #endif
    #endif

/** Generic stuff */
    #ifdef UIO_PDA_DEBUG
        #define UIO_DEBUG_PRINTF( __F_STRING__, ... )                                  \
        {                                                                              \
            printk(DRIVER_NAME " : line %d " __F_STRING__, __LINE__, ## __VA_ARGS__ ); \
        }

        #define UIO_DEBUG_ENTER()                                                      \
        {                                                                              \
            printk(DRIVER_NAME " : ENTER %s:%d\n" , __func__, __LINE__);               \
        }

        #define UIO_DEBUG_RETURN( __RETURNVALUE__ )                                    \
        {                                                                              \
            printk(DRIVER_NAME " : EXIT  %s:%d\n" , __func__, __LINE__);               \
            return( __RETURNVALUE__ );                                                 \
        }
    #endif

    #ifndef UIO_PDA_DEBUG
        #define UIO_DEBUG_PRINTF( __F_STRING__, ... )
        #define UIO_DEBUG_ENTER()
        #define UIO_DEBUG_RETURN( __RETURNVALUE__ ){ return( __RETURNVALUE__ ); }
    #endif

    #define UIO_PDA_ERROR( __F_STRING__, __JUMP__ )                                    \
    {                                                                                  \
        printk(DRIVER_NAME " : ERROR %s:%d \"" __F_STRING__ "\"", __func__, __LINE__); \
        goto __JUMP__;                                                                 \
    }

    #ifdef UIO_PDA_DEBUG_SG
        #define UIO_DEBUG_SG( __N_STRING__, __SG, __LENGTH )                           \
        {                                                                              \
            printk( __N_STRING__ );                                                    \
            for(i = 0; i < (__LENGTH); i++)                                            \
            { printk("a %llu l %u\n", __SG[i].dma_address, __SG[i].length); }          \
        }
    #endif /* UIO_PDA_DEBUG_SG */

    #ifndef UIO_PDA_DEBUG_SG
        #define UIO_DEBUG_SG( __N_STRING__, __SG, __LENGTH )
    #endif

    #define KSET_FIND( _kset, _name, _ret )                                            \
    {                                                                                  \
        struct kobject *k;                                                             \
        spin_lock( &_kset->list_lock );                                                \
        list_for_each_entry(k, &_kset->list, entry)                                    \
        {                                                                              \
            if(kobject_name(k) && !strcmp(kobject_name(k), _name) )                    \
            { _ret = kobject_get(k); break; }                                          \
        }                                                                              \
        spin_unlock( &_kset->list_lock );                                              \
    }

    #define PRINT_SG( _sg, _length, _str)                                              \
    {                                                                                  \
        struct scatterlist *sgp;                                                       \
        uint64_t            i;                                                         \
        for_each_sg(_sg, sgp, _length - 1, i)                                          \
        { printk( "%s %llu: %lx\n", _str, i, sgp->page_link ); }                       \
    }

    #define BIN_ATTR_PDA(_name, _size, _mode, _read, _write, _mmap)                    \
    struct bin_attribute *attr_bin_ ## _name =                                         \
        (struct bin_attribute*)kmalloc(sizeof(struct bin_attribute), GFP_KERNEL);      \
    attr_bin_ ## _name->attr.name = __stringify(_name);                                \
    attr_bin_ ## _name->attr.mode = _mode;                                             \
    attr_bin_ ## _name->size      = _size;                                             \
    attr_bin_ ## _name->read      = _read;                                             \
    attr_bin_ ## _name->write     = _write;                                            \
    attr_bin_ ## _name->mmap      = _mmap;


/** Attribute callback definitions */
BIN_ATTR_READ_CALLBACK( mps );
BIN_ATTR_READ_CALLBACK( readrq );

BIN_ATTR_WRITE_CALLBACK( request_buffer_write );
BIN_ATTR_WRITE_CALLBACK( delete_buffer_write );

BIN_ATTR_MAP_CALLBACK( map );
BIN_ATTR_MAP_CALLBACK( map_sg );

/** Version dependend definitions */


/**
 * Kernel 4.5 introduces a different management of pages by PFN, see
 * https://lwn.net/Articles/654396/
 * https://lwn.net/Articles/656197/
 * https://lwn.net/Articles/672457/
 * These changes are back-ported at least into CentOS 7.3,
 * kernel 3.10.0-514.6.1.el7.x86_64
 **/
#if defined(RHEL_RELEASE_CODE)
#if RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7, 3)
#define PDA_PFN_T_PAGES
#endif
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
#define PDA_PFN_T_PAGES
#endif

#ifdef PDA_PFN_T_PAGES
#include <linux/pfn_t.h>
#endif

/**
 * Kernel 4.6 introduced six-argument get_user_pages()
 * https://github.com/torvalds/linux/commit/c12d2da56d0e07d230968ee2305aaa86b93a6832
 *
 * Kernel 4.9 replaced six-argument get_user_pages() with five-argument version,
 * replacing write/force parameters with gup_flags:
 * https://github.com/torvalds/linux/commit/768ae309a96103ed02eb1e111e838c87854d8b51
 **/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
#define PDA_FIVEARG_GUP
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 6, 0)
#define PDA_SIXARG_GUP
#endif

/**
 * Kernel 4.17 joins struct fault_env and vm_fault:
 * https://github.com/torvalds/linux/commit/82b0f8c39a3869b6fd2a10e180a862248736ec6f
 * It also introduces vmf_insert_page() of vm_fault_t return type, replacing vm_insert_page()
 * https://github.com/torvalds/linux/commit/1c8f422059ae5da07db7406ab916203f9417e396
 **/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 17, 0)
#define PDA_VMF_T
#endif

#endif /** __KERNEL__ */

#endif /** UIO_PCI_DMA_H */
