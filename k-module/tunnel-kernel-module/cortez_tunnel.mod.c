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
	{ 0xcb8b6ec6, "kfree" },
	{ 0xb1ad3f2f, "boot_cpu_data" },
	{ 0xf83d2074, "remap_vmalloc_range" },
	{ 0x84f07bf7, "cachemode2protval" },
	{ 0x11169e9e, "__register_chrdev" },
	{ 0xea5ac1d9, "class_create" },
	{ 0xf98f93a7, "device_create" },
	{ 0x14fcde53, "class_destroy" },
	{ 0x52b15b3b, "__unregister_chrdev" },
	{ 0x6fdeeff0, "device_destroy" },
	{ 0x14fcde53, "class_unregister" },
	{ 0x092a35a2, "_copy_from_user" },
	{ 0x2435d559, "strncmp" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xfed1e3bc, "kmalloc_caches" },
	{ 0x70db3fe4, "__kmalloc_cache_noprof" },
	{ 0xd7a59a65, "vmalloc_user_noprof" },
	{ 0xc609ff70, "strncpy" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xd272d446, "__fentry__" },
	{ 0xe8213e80, "_printk" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xf46d5bf3, "mutex_lock" },
	{ 0xf46d5bf3, "mutex_unlock" },
	{ 0xf1de9e85, "vfree" },
	{ 0xba157484, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0xcb8b6ec6,
	0xb1ad3f2f,
	0xf83d2074,
	0x84f07bf7,
	0x11169e9e,
	0xea5ac1d9,
	0xf98f93a7,
	0x14fcde53,
	0x52b15b3b,
	0x6fdeeff0,
	0x14fcde53,
	0x092a35a2,
	0x2435d559,
	0xbd03ed67,
	0xfed1e3bc,
	0x70db3fe4,
	0xd7a59a65,
	0xc609ff70,
	0xd272d446,
	0xd272d446,
	0xe8213e80,
	0xd272d446,
	0xf46d5bf3,
	0xf46d5bf3,
	0xf1de9e85,
	0xba157484,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"kfree\0"
	"boot_cpu_data\0"
	"remap_vmalloc_range\0"
	"cachemode2protval\0"
	"__register_chrdev\0"
	"class_create\0"
	"device_create\0"
	"class_destroy\0"
	"__unregister_chrdev\0"
	"device_destroy\0"
	"class_unregister\0"
	"_copy_from_user\0"
	"strncmp\0"
	"random_kmalloc_seed\0"
	"kmalloc_caches\0"
	"__kmalloc_cache_noprof\0"
	"vmalloc_user_noprof\0"
	"strncpy\0"
	"__stack_chk_fail\0"
	"__fentry__\0"
	"_printk\0"
	"__x86_return_thunk\0"
	"mutex_lock\0"
	"mutex_unlock\0"
	"vfree\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "BA3504C43851FFA5BF0C84F");
