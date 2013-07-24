/*  
 * FLS front panel lcd driver
 */
#include <linux/module.h>
#include <linux/kernel.h>

int lcd_init(void)
{
	// start up msg
	printk(KERN_INFO "FLS LCD driver started\n");

	return 0;
}

void lcd_cleanup(void)
{
	// shutdown msg
	printk(KERN_INFO "FLS LCD driver done\n");
}

module_init(lcd_init);
module_exit(lcd_cleanup);
