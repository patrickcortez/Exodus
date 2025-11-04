#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#include "cortez_tunnel_shared.h"

#define DEVICE_NAME "cortez_tunnel"
#define CLASS_NAME  "cortez"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cortez Architecture");
MODULE_DESCRIPTION("A zero-copy, shared-memory IPC tunnel device");

// --- Data Structures ---

// Represents a single tunnel instance
typedef struct {
    char name[32];
    void *buffer;
    unsigned long size;
    atomic_t ref_count;
    struct list_head list;
} cortez_tunnel_t;

// Global list of all tunnels, protected by a mutex
static LIST_HEAD(tunnels_list);
static DEFINE_MUTEX(tunnels_mutex);

// --- Device Setup ---
static int major_number;
static struct class* cortez_class = NULL;

// --- Forward Declarations ---
static long pipe_ioctl(struct file *, unsigned int, unsigned long);
static int pipe_mmap(struct file *filp, struct vm_area_struct *vma);
static int pipe_open(struct inode *, struct file *);
static int pipe_release(struct inode *, struct file *);

static struct file_operations fops = {
    .open = pipe_open,
    .release = pipe_release,
    .unlocked_ioctl = pipe_ioctl,
    .mmap = pipe_mmap,
};

// --- Helper Functions ---
static cortez_tunnel_t* find_tunnel(const char* name) {
    cortez_tunnel_t *tunnel;
    list_for_each_entry(tunnel, &tunnels_list, list) {
        if (strncmp(name, tunnel->name, sizeof(tunnel->name)) == 0) {
            return tunnel;
        }
    }
    return NULL;
}

// --- File Operations Implementation ---

static int pipe_open(struct inode *inodep, struct file *filep) {
    // We don't attach to a tunnel until ioctl is called
    filep->private_data = NULL;
    printk(KERN_INFO "CortezTunnel: Device opened.\n");
    return 0;
}

static int pipe_release(struct inode *inodep, struct file *filep) {
    // Decrement reference count of the tunnel this FD was connected to
    cortez_tunnel_t *tunnel = (cortez_tunnel_t *)filep->private_data;
    if (tunnel) {
        if (atomic_dec_and_test(&tunnel->ref_count)) {
            // Last user is gone, so clean up the tunnel
            printk(KERN_INFO "CortezTunnel: Destroying tunnel '%s'.\n", tunnel->name);
            mutex_lock(&tunnels_mutex);
            list_del(&tunnel->list);
            mutex_unlock(&tunnels_mutex);
            vfree(tunnel->buffer); // Use vfree for vmalloc'd memory
            kfree(tunnel);
        }
    }
    printk(KERN_INFO "CortezTunnel: Device closed.\n");
    return 0;
}

static long pipe_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
    cortez_tunnel_t *tunnel;

    switch (cmd) {
        case TUNNEL_CREATE: {
            tunnel_create_t create_info;
            if (copy_from_user(&create_info, (tunnel_create_t *)arg, sizeof(create_info)))
                return -EFAULT;

            // Ensure name is null-terminated
            create_info.name[sizeof(create_info.name) - 1] = '\0';
            
            // Round size up to the nearest page size for mmap safety
            create_info.size = PAGE_ALIGN(create_info.size);
            if (create_info.size == 0 || create_info.size > (16 * 1024 * 1024)) // Limit to 16MB
                 return -EINVAL;

            mutex_lock(&tunnels_mutex);
            if (find_tunnel(create_info.name)) {
                mutex_unlock(&tunnels_mutex);
                return -EEXIST; // Tunnel already exists
            }

            tunnel = kzalloc(sizeof(cortez_tunnel_t), GFP_KERNEL);
            if (!tunnel) {
                mutex_unlock(&tunnels_mutex);
                return -ENOMEM;
            }
            // Use vmalloc to get contiguous virtual memory for large allocations
            tunnel->buffer = vmalloc_user(create_info.size);
            if (!tunnel->buffer) {
                 kfree(tunnel);
                 mutex_unlock(&tunnels_mutex);
                 return -ENOMEM;
            }

            strncpy(tunnel->name, create_info.name, sizeof(tunnel->name));
            tunnel->size = create_info.size;
            atomic_set(&tunnel->ref_count, 1);
            
            list_add(&tunnel->list, &tunnels_list);
            filep->private_data = tunnel; // Attach this new tunnel to the file descriptor
            mutex_unlock(&tunnels_mutex);

            printk(KERN_INFO "CortezTunnel: Created tunnel '%s' with size %lu.\n", tunnel->name, tunnel->size);
            break;
        }
        case TUNNEL_CONNECT: {
            char name_buf[32];
            if (copy_from_user(name_buf, (char *)arg, sizeof(name_buf)))
                return -EFAULT;
            
            name_buf[sizeof(name_buf) - 1] = '\0';

            mutex_lock(&tunnels_mutex);
            tunnel = find_tunnel(name_buf);
            if (!tunnel) {
                mutex_unlock(&tunnels_mutex);
                return -ENOENT; // Tunnel not found
            }

            atomic_inc(&tunnel->ref_count); // New user connected
            filep->private_data = tunnel; // Attach this tunnel to the file descriptor
            mutex_unlock(&tunnels_mutex);

            printk(KERN_INFO "CortezTunnel: Process connected to tunnel '%s'.\n", tunnel->name);
            break;
        }
        default:
            return -ENOTTY;
    }
    return 0;
}

static int pipe_mmap(struct file *filp, struct vm_area_struct *vma) {
    cortez_tunnel_t *tunnel = filp->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (!tunnel)
        return -EINVAL;
    if (size > tunnel->size)
        return -EINVAL;

    // Marks the memory region as uncached to solve the cache coherency issue.
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    // Maps the kernel buffer into the user's address space
    int ret = remap_vmalloc_range(vma, tunnel->buffer, 0);
    if (ret) {
        printk(KERN_ERR "CortezTunnel: remap_vmalloc_range failed.\n");
        return ret;
    }
    
    printk(KERN_INFO "CortezTunnel: Mapped tunnel '%s' to userspace.\n", tunnel->name);
    return 0;
}


// --- Module Init and Exit ---
static int __init cortez_tunnel_init(void) {
    printk(KERN_INFO "CortezTunnel: Initializing...\n");

    major_number = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_number < 0) return major_number;

    cortez_class = class_create(CLASS_NAME);
    if (IS_ERR(cortez_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(cortez_class);
    }

    if (IS_ERR(device_create(cortez_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME))) {
        class_destroy(cortez_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return -1;
    }
    
    printk(KERN_INFO "CortezTunnel: Module loaded with major number %d.\n", major_number);
    return 0;
}

static void __exit cortez_tunnel_exit(void) {
    device_destroy(cortez_class, MKDEV(major_number, 0));
    class_unregister(cortez_class);
    class_destroy(cortez_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "CortezTunnel: Module unloaded.\n");
}

module_init(cortez_tunnel_init);
module_exit(cortez_tunnel_exit);
