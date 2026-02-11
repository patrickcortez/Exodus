#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xea5ac1d9, "class_create" },
	{ 0xf98f93a7, "device_create" },
	{ 0x14fcde53, "class_destroy" },
	{ 0x52b15b3b, "__unregister_chrdev" },
	{ 0xf1de9e85, "vfree" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x6fdeeff0, "device_destroy" },
	{ 0x14fcde53, "class_unregister" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xa53f4e29, "memcpy" },
	{ 0xa53f4e29, "memmove" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0x16ab4215, "__wake_up" },
	{ 0x7851be11, "__SCT__might_resched" },
	{ 0x092a35a2, "_copy_to_user" },
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0x7a5ffe84, "init_wait_entry" },
	{ 0xd272d446, "schedule" },
	{ 0x0db8d68d, "prepare_to_wait_event" },
	{ 0xc87f4bab, "finish_wait" },
	{ 0xd7a59a65, "vmalloc_user_noprof" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfed1e3bc, "kmalloc_caches" },
	{ 0x70db3fe4, "__kmalloc_cache_noprof" },
	{ 0xd7a59a65, "vzalloc_noprof" },
	{ 0xc1e6c71e, "__mutex_init" },
	{ 0x5403c125, "__init_waitqueue_head" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xd272d446, "__fentry__" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xb1ad3f2f, "boot_cpu_data" },
	{ 0xf83d2074, "remap_vmalloc_range" },
	{ 0xe8213e80, "_printk" },
	{ 0x84f07bf7, "cachemode2protval" },
	{ 0x5a844b26, "__x86_indirect_thunk_rax" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0x11169e9e, "__register_chrdev" },
	{ 0xba157484, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xea5ac1d9,
	0xf98f93a7,
	0x14fcde53,
	0x52b15b3b,
	0xf1de9e85,
	0xcb8b6ec6,
	0x6fdeeff0,
	0x14fcde53,
	0x90a48d82,
	0xa53f4e29,
	0xa53f4e29,
	0x092a35a2,
	0x16ab4215,
	0x7851be11,
	0x092a35a2,
	0xd710adbf,
	0x7a5ffe84,
	0xd272d446,
	0x0db8d68d,
	0xc87f4bab,
	0xd7a59a65,
	0xbd03ed67,
	0xfed1e3bc,
	0x70db3fe4,
	0xd7a59a65,
	0xc1e6c71e,
	0x5403c125,
	0xd272d446,
	0xd272d446,
	0xd272d446,
	0xb1ad3f2f,
	0xf83d2074,
	0xe8213e80,
	0x84f07bf7,
	0x5a844b26,
	0xf46d5bf3,
	0xf46d5bf3,
	0x11169e9e,
	0xba157484,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"class_create\0"
	"device_create\0"
	"class_destroy\0"
	"__unregister_chrdev\0"
	"vfree\0"
	"kfree\0"
	"device_destroy\0"
	"class_unregister\0"
	"__ubsan_handle_out_of_bounds\0"
	"memcpy\0"
	"memmove\0"
	"_copy_from_user\0"
	"__wake_up\0"
	"__SCT__might_resched\0"
	"_copy_to_user\0"
	"__kmalloc_noprof\0"
	"init_wait_entry\0"
	"schedule\0"
	"prepare_to_wait_event\0"
	"finish_wait\0"
	"vmalloc_user_noprof\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"vzalloc_noprof\0"
	"__mutex_init\0"
	"__init_waitqueue_head\0"
	"__stack_chk_fail\0"
	"__fentry__\0"
	"__x86_return_thunk\0"
	"boot_cpu_data\0"
	"remap_vmalloc_range\0"
	"_printk\0"
	"cachemode2protval\0"
	"__x86_indirect_thunk_rax\0"
	"mutex_lock\0"
	"mutex_unlock\0"
	"__register_chrdev\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "8B4CBF65EE54ECCF16FA053");
