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
	{ 0xc5d6d7e7, "i2c_del_driver" },
	{ 0xfecd548f, "i2c_register_driver" },
	{ 0x838b64e7, "dev_err" },
	{ 0x3ba16ce2, "dev_warn" },
	{ 0x2273e70e, "dev_smartio_register_node" },
	{ 0xac62d28f, "smartio_unregister_node" },
	{ 0x8a8fb613, "dev_get_drvdata" },
	{ 0x27e1a049, "printk" },
	{ 0xbdfb6dbb, "__fentry__" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=smartio-core";

MODULE_ALIAS("i2c:smart1");
MODULE_ALIAS("i2c:smart2");

MODULE_INFO(srcversion, "FB7E893D1B892CE313EA95B");
