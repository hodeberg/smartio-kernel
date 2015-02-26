#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0x5358b4ed, "module_layout" },
	{ 0xfe1c648f, "kmalloc_caches" },
	{ 0xf3315dc7, "driver_register" },
	{ 0xef5ddd35, "__bus_register" },
	{ 0xe4ee7cc4, "dev_set_drvdata" },
	{ 0x43a53735, "__alloc_workqueue_key" },
	{ 0xc8b57c27, "autoremove_wake_function" },
	{ 0x1bfee7ad, "queue_work" },
	{ 0x181af9d0, "mutex_unlock" },
	{ 0x838b64e7, "dev_err" },
	{ 0x353d11fe, "current_task" },
	{ 0x27e1a049, "printk" },
	{ 0x75ab2578, "class_unregister" },
	{ 0x9b0d45f7, "driver_unregister" },
	{ 0xb264abed, "mutex_lock" },
	{ 0x994d5afc, "device_add" },
	{ 0x663a4cd5, "__class_register" },
	{ 0xdd771c6a, "bus_unregister" },
	{ 0xf11543ff, "find_first_zero_bit" },
	{ 0x4b7a8cb8, "idr_pre_get" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x1000e51, "schedule" },
	{ 0xbdfb6dbb, "__fentry__" },
	{ 0xf2b09e86, "kmem_cache_alloc_trace" },
	{ 0x37a7e107, "get_device" },
	{ 0xcf21d241, "__wake_up" },
	{ 0x37a0cba, "kfree" },
	{ 0x69acdf38, "memcpy" },
	{ 0x5c8b5ce8, "prepare_to_wait" },
	{ 0xa6eed33b, "device_initialize" },
	{ 0xfa66f77c, "finish_wait" },
	{ 0x3ba16ce2, "dev_warn" },
	{ 0xd029fbdd, "device_unregister" },
	{ 0xd9b9386f, "dev_set_name" },
	{ 0x14f3c2a, "idr_get_new" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";


MODULE_INFO(srcversion, "378EC78725D57BE6033AEF1");
