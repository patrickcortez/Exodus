#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include "cortez_cokernel.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cortez Architecture");
MODULE_DESCRIPTION("Cortez Co-Kernel Ring 0 Module");

static int major_number;
static struct class* cokernel_class = NULL;
static struct device* cokernel_device = NULL;
static DEFINE_MUTEX(ck_mutex);

// --- Interrupt Circular Buffer ---
#define IRQ_BUF_SIZE 128

typedef struct {
    ck_interrupt_t buffer[IRQ_BUF_SIZE];
    int head;
    int tail;
    int count;
    wait_queue_head_t wait_q;
} ck_irq_queue_t;

static ck_irq_queue_t g_irq_queue;

static void irq_queue_init(void) {
    g_irq_queue.head = 0;
    g_irq_queue.tail = 0;
    g_irq_queue.count = 0;
    init_waitqueue_head(&g_irq_queue.wait_q);
}

static int irq_enqueue(ck_interrupt_t* irq) {
    mutex_lock(&ck_mutex);
    if (g_irq_queue.count >= IRQ_BUF_SIZE) {
        mutex_unlock(&ck_mutex);
        return -1; // Buffer full
    }
    g_irq_queue.buffer[g_irq_queue.tail] = *irq;
    g_irq_queue.tail = (g_irq_queue.tail + 1) % IRQ_BUF_SIZE;
    g_irq_queue.count++;
    mutex_unlock(&ck_mutex);
    
    wake_up_interruptible(&g_irq_queue.wait_q);
    return 0;
}

static int irq_dequeue(ck_interrupt_t* irq) {
    mutex_lock(&ck_mutex);
    if (g_irq_queue.count == 0) {
        mutex_unlock(&ck_mutex);
        return -1; // Buffer empty
    }
    *irq = g_irq_queue.buffer[g_irq_queue.head];
    g_irq_queue.head = (g_irq_queue.head + 1) % IRQ_BUF_SIZE;
    g_irq_queue.count--;
    mutex_unlock(&ck_mutex);
    return 0;
}

// --- Syscall Implementation ---

static long handle_syscall(ck_syscall_args_t* args) {
    switch (args->sys_id) {
        case CK_SYS_PRINT: {
            char* msg = kmalloc(4096, GFP_KERNEL);
            if (!msg) return -ENOMEM;
            
            if (copy_from_user(msg, (void __user*)args->arg1, 4096)) {
                kfree(msg);
                return -EFAULT;
            }
            msg[4095] = '\0';
            printk(KERN_INFO "[CoKernel Ring0] PRINT: %s\n", msg);
            kfree(msg);
            return 0;
        }
        case CK_SYS_EXIT:
            printk(KERN_INFO "[CoKernel Ring0] Process exit request: %lu\n", args->arg1);
            return 0;
        default:
            printk(KERN_INFO "[CoKernel Ring0] Unknown syscall: %d\n", args->sys_id);
            return -EINVAL;
    }
}

// --- File Operations ---

static int ck_open(struct inode *inodep, struct file *filep) {
    return 0;
}

static int ck_release(struct inode *inodep, struct file *filep) {
    return 0;
}

static ssize_t ck_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {
    ck_interrupt_t irq;
    int ret;

    if (len < sizeof(ck_interrupt_t)) return -EINVAL;

    // Wait for data
    if (wait_event_interruptible(g_irq_queue.wait_q, g_irq_queue.count > 0)) {
        return -ERESTARTSYS;
    }

    if (irq_dequeue(&irq) < 0) return 0; // Should not happen after wait

    if (copy_to_user(buffer, &irq, sizeof(ck_interrupt_t))) {
        return -EFAULT;
    }

    return sizeof(ck_interrupt_t);
}

static ssize_t ck_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {
    // Allows writing to inject interrupts (simulating hardware)
    ck_interrupt_t irq;
    
    // If writing raw data, treat as generic data interrupt
    if (len == sizeof(ck_interrupt_t)) {
        if (copy_from_user(&irq, buffer, sizeof(ck_interrupt_t))) return -EFAULT;
    } else {
        // Simple injection: first byte is IRQ, rest is data (truncated)
        char kbuf[16];
        int copy_len = len > 16 ? 16 : len;
        if (copy_from_user(kbuf, buffer, copy_len)) return -EFAULT;
        
        irq.irq = kbuf[0]; // First byte is IRQ number
        irq.data = 0;      // Placeholder
        irq.timestamp = jiffies;
    }

    if (irq_enqueue(&irq) < 0) return -EAGAIN;
    
    return len;
}

static __poll_t ck_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;
    poll_wait(filep, &g_irq_queue.wait_q, wait);
    if (g_irq_queue.count > 0) mask |= EPOLLIN | EPOLLRDNORM;
    return mask;
}

static long ck_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
    long ret = 0;
    switch (cmd) {
        case CK_IOCTL_SYSCALL: {
            ck_syscall_args_t kargs;
            if (copy_from_user(&kargs, (void __user*)arg, sizeof(kargs))) return -EFAULT;
            
            // Note: We don't lock the whole syscall, only critical sections inside
            ret = handle_syscall(&kargs);
            
            kargs.result = ret;
            if (copy_to_user((void __user*)arg, &kargs, sizeof(kargs))) return -EFAULT;
            break;
        }
        default:
            return -ENOTTY;
    }
    return ret;
}

static struct file_operations fops = {
    .open = ck_open,
    .release = ck_release,
    .read = ck_read,
    .write = ck_write,
    .poll = ck_poll,
    .unlocked_ioctl = ck_ioctl,
};

// --- Init / Exit ---

static int __init cokernel_init(void) {
    printk(KERN_INFO "CortezCoKernel: Initializing Ring 0 Co-Kernel...\n");

    irq_queue_init();

    major_number = register_chrdev(0, COKERNEL_DEVICE_NAME, &fops);
    if (major_number < 0) {
        printk(KERN_ALERT "CortezCoKernel: Failed to register a major number\n");
        return major_number;
    }

    cokernel_class = class_create(COKERNEL_CLASS_NAME);
    if (IS_ERR(cokernel_class)) {
        unregister_chrdev(major_number, COKERNEL_DEVICE_NAME);
        return PTR_ERR(cokernel_class);
    }

    cokernel_device = device_create(cokernel_class, NULL, MKDEV(major_number, 0), NULL, COKERNEL_DEVICE_NAME);
    if (IS_ERR(cokernel_device)) {
        class_destroy(cokernel_class);
        unregister_chrdev(major_number, COKERNEL_DEVICE_NAME);
        return PTR_ERR(cokernel_device);
    }

    printk(KERN_INFO "CortezCoKernel: Module loaded correctly with major number %d\n", major_number);
    return 0;
}

static void __exit cokernel_exit(void) {
    device_destroy(cokernel_class, MKDEV(major_number, 0));
    class_unregister(cokernel_class);
    class_destroy(cokernel_class);
    unregister_chrdev(major_number, COKERNEL_DEVICE_NAME);
    printk(KERN_INFO "CortezCoKernel: Module unloaded\n");
}

module_init(cokernel_init);
module_exit(cokernel_exit);
