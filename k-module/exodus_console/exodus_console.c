/* Build: make -C /lib/modules/$(uname -r)/build M=$(pwd) modules */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/sched.h>

#include "exodus_console_shared.h"

#define DEVICE_NAME "excon"
#define CLASS_NAME  "exodus"
#define MAX_CONSOLES 8

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cortez Architecture");
MODULE_DESCRIPTION("Exodus Console — kernel-owned terminal screen buffer");

typedef struct {
    int                id;
    int                active;

    excon_header_t     *header;
    excon_cell_t       *cells;
    excon_cell_t       *scroll_buf;

    void               *mmap_buf;
    unsigned long      mmap_size;

    uint8_t            current_attr;

    char               input_ring[EXCON_INPUT_BUF_SIZE];
    unsigned int       input_head;
    unsigned int       input_tail;
    wait_queue_head_t  input_wait;
    struct mutex       input_lock;

    struct mutex       buf_lock;
} excon_console_t;

static excon_console_t consoles[MAX_CONSOLES];
static DEFINE_MUTEX(global_lock);

static int major_number;
static struct class *excon_class;

static void rebuild_mmap_buffer(excon_console_t *con) {
    int rows = con->header->rows;
    int cols = con->header->cols;
    int cell_bytes = rows * cols * sizeof(excon_cell_t);

    memcpy(con->mmap_buf, con->header, sizeof(excon_header_t));
    memcpy((char *)con->mmap_buf + sizeof(excon_header_t), con->cells, cell_bytes);
    con->header->dirty_seq++;
    ((excon_header_t *)con->mmap_buf)->dirty_seq = con->header->dirty_seq;
}

static void console_scroll_up(excon_console_t *con, int lines) {
    int rows = con->header->rows;
    int cols = con->header->cols;

    if (lines <= 0 || lines > rows)
        lines = 1;

    if (con->scroll_buf && con->header->scroll_lines < EXCON_MAX_SCROLL) {
        int copy_lines = lines;
        if (con->header->scroll_lines + copy_lines > EXCON_MAX_SCROLL)
            copy_lines = EXCON_MAX_SCROLL - con->header->scroll_lines;

        memcpy(&con->scroll_buf[con->header->scroll_lines * cols],
               con->cells, copy_lines * cols * sizeof(excon_cell_t));
        con->header->scroll_lines += copy_lines;
    }

    int remaining = rows - lines;
    if (remaining > 0) {
        memmove(con->cells,
                &con->cells[lines * cols],
                remaining * cols * sizeof(excon_cell_t));
    }

    for (int r = remaining; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            con->cells[r * cols + c].ch = ' ';
            con->cells[r * cols + c].attr = 0x07;
        }
    }
}

static void console_write_char(excon_console_t *con, char ch) {
    int rows = con->header->rows;
    int cols = con->header->cols;
    int r = con->header->cursor_row;
    int c = con->header->cursor_col;

    if (ch == '\n') {
        con->header->cursor_col = 0;
        con->header->cursor_row++;
        if (con->header->cursor_row >= rows) {
            console_scroll_up(con, 1);
            con->header->cursor_row = rows - 1;
        }
        return;
    }

    if (ch == '\r') {
        con->header->cursor_col = 0;
        return;
    }

    if (ch == '\t') {
        int next_tab = (c + 8) & ~7;
        if (next_tab > cols) next_tab = cols;
        while (con->header->cursor_col < next_tab)
            console_write_char(con, ' ');
        return;
    }

    if (ch == '\b') {
        if (c > 0) {
            con->header->cursor_col--;
            con->cells[r * cols + con->header->cursor_col].ch = ' ';
            con->cells[r * cols + con->header->cursor_col].attr = con->current_attr;
        }
        return;
    }

    if (r >= 0 && r < rows && c >= 0 && c < cols) {
        con->cells[r * cols + c].ch = ch;
        con->cells[r * cols + c].attr = con->current_attr;
    }

    con->header->cursor_col++;
    if (con->header->cursor_col >= cols) {
        if (con->header->flags & EXCON_FLAG_WRAP_MODE) {
            con->header->cursor_col = 0;
            con->header->cursor_row++;
            if (con->header->cursor_row >= rows) {
                console_scroll_up(con, 1);
                con->header->cursor_row = rows - 1;
            }
        } else {
            con->header->cursor_col = cols - 1;
        }
    }
}

static int input_ring_empty(excon_console_t *con) {
    return con->input_head == con->input_tail;
}

static int input_ring_push(excon_console_t *con, const char *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        unsigned int next = (con->input_head + 1) % EXCON_INPUT_BUF_SIZE;
        if (next == con->input_tail)
            break;
        con->input_ring[con->input_head] = data[i];
        con->input_head = next;
    }
    return i;
}

static int input_ring_pop(excon_console_t *con, char *data, int len) {
    int i;
    for (i = 0; i < len; i++) {
        if (con->input_tail == con->input_head)
            break;
        data[i] = con->input_ring[con->input_tail];
        con->input_tail = (con->input_tail + 1) % EXCON_INPUT_BUF_SIZE;
    }
    return i;
}

static int excon_open(struct inode *inodep, struct file *filep) {
    filep->private_data = NULL;
    return 0;
}

static int excon_release(struct inode *inodep, struct file *filep) {
    return 0;
}

static long excon_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
    excon_console_t *con = filep->private_data;

    switch (cmd) {
        case EXCON_CREATE: {
            excon_create_t info;
            if (copy_from_user(&info, (void __user *)arg, sizeof(info)))
                return -EFAULT;

            if (info.rows == 0 || info.cols == 0 ||
                info.rows > EXCON_MAX_ROWS || info.cols > EXCON_MAX_COLS)
                return -EINVAL;

            mutex_lock(&global_lock);

            int slot = -1;
            for (int i = 0; i < MAX_CONSOLES; i++) {
                if (!consoles[i].active) {
                    slot = i;
                    break;
                }
            }

            if (slot < 0) {
                mutex_unlock(&global_lock);
                return -ENOSPC;
            }

            con = &consoles[slot];
            memset(con, 0, sizeof(excon_console_t));

            int cell_count = info.rows * info.cols;
            int scroll_cells = EXCON_MAX_SCROLL * info.cols;

            con->mmap_size = PAGE_ALIGN(sizeof(excon_header_t) + cell_count * sizeof(excon_cell_t));
            con->mmap_buf = vmalloc_user(con->mmap_size);
            if (!con->mmap_buf) {
                mutex_unlock(&global_lock);
                return -ENOMEM;
            }

            con->header = kzalloc(sizeof(excon_header_t), GFP_KERNEL);
            con->cells = kzalloc(cell_count * sizeof(excon_cell_t), GFP_KERNEL);
            con->scroll_buf = vzalloc(scroll_cells * sizeof(excon_cell_t));

            if (!con->header || !con->cells) {
                vfree(con->mmap_buf);
                kfree(con->header);
                kfree(con->cells);
                vfree(con->scroll_buf);
                mutex_unlock(&global_lock);
                return -ENOMEM;
            }

            con->id = slot;
            con->active = 1;
            con->header->rows = info.rows;
            con->header->cols = info.cols;
            con->header->cursor_row = 0;
            con->header->cursor_col = 0;
            con->header->flags = EXCON_FLAG_CURSOR_VISIBLE | EXCON_FLAG_WRAP_MODE;
            con->header->fg_color = 7;
            con->header->bg_color = 0;
            con->header->dirty_seq = 0;
            con->header->scroll_offset = 0;
            con->header->scroll_lines = 0;
            con->current_attr = 0x07;

            for (int i = 0; i < cell_count; i++) {
                con->cells[i].ch = ' ';
                con->cells[i].attr = 0x07;
            }

            mutex_init(&con->buf_lock);
            mutex_init(&con->input_lock);
            init_waitqueue_head(&con->input_wait);
            con->input_head = 0;
            con->input_tail = 0;

            rebuild_mmap_buffer(con);

            filep->private_data = con;
            mutex_unlock(&global_lock);

            printk(KERN_INFO "ExodusConsole: Created console %d (%dx%d)\n",
                   slot, info.cols, info.rows);
            return slot;
        }

        case EXCON_CLEAR: {
            if (!con || !con->active)
                return -EINVAL;

            mutex_lock(&con->buf_lock);

            int total = con->header->rows * con->header->cols;
            for (int i = 0; i < total; i++) {
                con->cells[i].ch = ' ';
                con->cells[i].attr = 0x07;
            }
            con->header->cursor_row = 0;
            con->header->cursor_col = 0;
            con->header->scroll_offset = 0;
            con->header->scroll_lines = 0;

            rebuild_mmap_buffer(con);
            mutex_unlock(&con->buf_lock);
            return 0;
        }

        case EXCON_WRITE_DATA: {
            if (!con || !con->active)
                return -EINVAL;

            excon_write_t wr;
            if (copy_from_user(&wr, (void __user *)arg, sizeof(wr)))
                return -EFAULT;

            if (wr.len > sizeof(wr.data))
                wr.len = sizeof(wr.data);

            mutex_lock(&con->buf_lock);
            for (uint32_t i = 0; i < wr.len; i++)
                console_write_char(con, wr.data[i]);

            rebuild_mmap_buffer(con);
            mutex_unlock(&con->buf_lock);
            return 0;
        }

        case EXCON_SET_CURSOR: {
            if (!con || !con->active)
                return -EINVAL;

            excon_cursor_t cur;
            if (copy_from_user(&cur, (void __user *)arg, sizeof(cur)))
                return -EFAULT;

            mutex_lock(&con->buf_lock);
            if (cur.row < con->header->rows)
                con->header->cursor_row = cur.row;
            if (cur.col < con->header->cols)
                con->header->cursor_col = cur.col;

            rebuild_mmap_buffer(con);
            mutex_unlock(&con->buf_lock);
            return 0;
        }

        case EXCON_GET_SIZE: {
            if (!con || !con->active)
                return -EINVAL;

            excon_create_t sz;
            sz.rows = con->header->rows;
            sz.cols = con->header->cols;

            if (copy_to_user((void __user *)arg, &sz, sizeof(sz)))
                return -EFAULT;
            return 0;
        }

        case EXCON_SCROLL: {
            if (!con || !con->active)
                return -EINVAL;

            excon_scroll_t sc;
            if (copy_from_user(&sc, (void __user *)arg, sizeof(sc)))
                return -EFAULT;

            mutex_lock(&con->buf_lock);
            if (sc.lines > 0)
                console_scroll_up(con, sc.lines);

            rebuild_mmap_buffer(con);
            mutex_unlock(&con->buf_lock);
            return 0;
        }

        case EXCON_SET_ATTR: {
            if (!con || !con->active)
                return -EINVAL;

            excon_attr_t at;
            if (copy_from_user(&at, (void __user *)arg, sizeof(at)))
                return -EFAULT;

            mutex_lock(&con->buf_lock);
            con->current_attr = (at.fg & EXCON_ATTR_FG_MASK) |
                                ((at.bg << EXCON_ATTR_BG_SHIFT) & EXCON_ATTR_BG_MASK);
            if (at.bold)
                con->current_attr |= EXCON_ATTR_BOLD;
            if (at.blink)
                con->current_attr |= EXCON_ATTR_BLINK;

            con->header->fg_color = at.fg;
            con->header->bg_color = at.bg;
            mutex_unlock(&con->buf_lock);
            return 0;
        }

        case EXCON_PUSH_INPUT: {
            if (!con || !con->active)
                return -EINVAL;

            excon_input_t inp;
            if (copy_from_user(&inp, (void __user *)arg, sizeof(inp)))
                return -EFAULT;

            if (inp.len > sizeof(inp.data))
                inp.len = sizeof(inp.data);

            mutex_lock(&con->input_lock);
            int pushed = input_ring_push(con, inp.data, inp.len);
            mutex_unlock(&con->input_lock);

            wake_up_interruptible(&con->input_wait);
            return pushed;
        }

        case EXCON_READ_INPUT: {
            if (!con || !con->active)
                return -EINVAL;

            if (wait_event_interruptible(con->input_wait,
                    !input_ring_empty(con)))
                return -ERESTARTSYS;

            excon_input_t inp;
            mutex_lock(&con->input_lock);
            inp.len = input_ring_pop(con, inp.data, sizeof(inp.data));
            mutex_unlock(&con->input_lock);

            if (copy_to_user((void __user *)arg, &inp, sizeof(inp)))
                return -EFAULT;
            return inp.len;
        }

        case EXCON_RESIZE: {
            if (!con || !con->active)
                return -EINVAL;

            excon_resize_t rs;
            if (copy_from_user(&rs, (void __user *)arg, sizeof(rs)))
                return -EFAULT;

            if (rs.rows == 0 || rs.cols == 0 ||
                rs.rows > EXCON_MAX_ROWS || rs.cols > EXCON_MAX_COLS)
                return -EINVAL;

            mutex_lock(&con->buf_lock);

            int new_cells = rs.rows * rs.cols;
            excon_cell_t *new_buf = kzalloc(new_cells * sizeof(excon_cell_t), GFP_KERNEL);
            if (!new_buf) {
                mutex_unlock(&con->buf_lock);
                return -ENOMEM;
            }

            for (int i = 0; i < new_cells; i++) {
                new_buf[i].ch = ' ';
                new_buf[i].attr = 0x07;
            }

            int copy_rows = con->header->rows < rs.rows ? con->header->rows : rs.rows;
            int copy_cols = con->header->cols < rs.cols ? con->header->cols : rs.cols;

            for (int r = 0; r < copy_rows; r++) {
                for (int c = 0; c < copy_cols; c++) {
                    new_buf[r * rs.cols + c] = con->cells[r * con->header->cols + c];
                }
            }

            kfree(con->cells);
            con->cells = new_buf;
            con->header->rows = rs.rows;
            con->header->cols = rs.cols;

            if (con->header->cursor_row >= rs.rows)
                con->header->cursor_row = rs.rows - 1;
            if (con->header->cursor_col >= rs.cols)
                con->header->cursor_col = rs.cols - 1;

            unsigned long new_mmap_size = PAGE_ALIGN(sizeof(excon_header_t) +
                                                     new_cells * sizeof(excon_cell_t));
            if (new_mmap_size != con->mmap_size) {
                vfree(con->mmap_buf);
                con->mmap_buf = vmalloc_user(new_mmap_size);
                con->mmap_size = new_mmap_size;
            }

            rebuild_mmap_buffer(con);
            mutex_unlock(&con->buf_lock);

            printk(KERN_INFO "ExodusConsole: Console %d resized to %dx%d\n",
                   con->id, rs.cols, rs.rows);
            return 0;
        }

        default:
            return -ENOTTY;
    }
}

static int excon_mmap(struct file *filp, struct vm_area_struct *vma) {
    excon_console_t *con = filp->private_data;
    unsigned long size;

    if (!con || !con->active)
        return -EINVAL;

    size = vma->vm_end - vma->vm_start;
    if (size > con->mmap_size)
        return -EINVAL;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    if (remap_vmalloc_range(vma, con->mmap_buf, 0)) {
        printk(KERN_ERR "ExodusConsole: mmap failed for console %d\n", con->id);
        return -EAGAIN;
    }

    printk(KERN_INFO "ExodusConsole: Console %d mapped to userspace\n", con->id);
    return 0;
}

static unsigned int excon_poll(struct file *filp, struct poll_table_struct *wait) {
    excon_console_t *con = filp->private_data;
    unsigned int mask = 0;

    if (!con || !con->active)
        return POLLERR;

    poll_wait(filp, &con->input_wait, wait);

    mutex_lock(&con->input_lock);
    if (!input_ring_empty(con))
        mask |= POLLIN | POLLRDNORM;
    mutex_unlock(&con->input_lock);

    mask |= POLLOUT | POLLWRNORM;
    return mask;
}

static struct file_operations excon_fops = {
    .open           = excon_open,
    .release        = excon_release,
    .unlocked_ioctl = excon_ioctl,
    .mmap           = excon_mmap,
    .poll           = excon_poll,
};

static int __init excon_init(void) {
    printk(KERN_INFO "ExodusConsole: Initializing...\n");

    for (int i = 0; i < MAX_CONSOLES; i++)
        consoles[i].active = 0;

    major_number = register_chrdev(0, DEVICE_NAME, &excon_fops);
    if (major_number < 0)
        return major_number;

    excon_class = class_create(CLASS_NAME);
    if (IS_ERR(excon_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(excon_class);
    }

    if (IS_ERR(device_create(excon_class, NULL, MKDEV(major_number, 0), NULL, "excon0"))) {
        class_destroy(excon_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return -1;
    }

    printk(KERN_INFO "ExodusConsole: Module loaded (major=%d)\n", major_number);
    return 0;
}

static void __exit excon_exit(void) {
    for (int i = 0; i < MAX_CONSOLES; i++) {
        if (consoles[i].active) {
            vfree(consoles[i].mmap_buf);
            kfree(consoles[i].header);
            kfree(consoles[i].cells);
            vfree(consoles[i].scroll_buf);
            consoles[i].active = 0;
        }
    }

    device_destroy(excon_class, MKDEV(major_number, 0));
    class_unregister(excon_class);
    class_destroy(excon_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "ExodusConsole: Module unloaded\n");
}

module_init(excon_init);
module_exit(excon_exit);
