/*  
 * FLS front panel lcd driver
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <asm/io.h>

#define MODULE_NAME "FLS front panel LCD"

#define LCD_SPLASH_MSG "                 SPLASH  SCREEN "

// hw layout
#define SYSCON_BASE (0x80004000)
#define RS	(1 << 6)
#define RW	(1 << 7)
#define E	(1 << 8)
#define D4	(1 << 0)
#define D5	(1 << 1)
#define D6	(1 << 4)
#define D7	(1 << 5)

// mapping for dram address to position on screen
#define LINE_LENGTH (0x10)
#define LINE1_START (0x00)
#define LINE2_START (0x40)
#define LINE3_START (0x10)
#define LINE4_START (0x50)

// timings from datasheet (plus tm to add a little margin so we are safe)
#define Tpor0     (50)
#define Tpor1     (5)
#define Tc        (500)
#define Tpw       (230)
#define Tr        (20)
#define Tf        (20)
#define Tsp1      (40)
#define Tsp2      (80)
#define Td        (120)
#define Tm        (50)

struct dio_reg_t {
	unsigned long paddr;
	size_t size;
	struct resource *res;
	void __iomem *vaddr;
};

static struct dio_t {
	struct dio_reg_t dir;	
	struct dio_reg_t in;	
	struct dio_reg_t out;	
} lcd_dio = {
	.dir = {.paddr = SYSCON_BASE + 0x1e, .size = 2},
	.in  = {.paddr = SYSCON_BASE + 0x26, .size = 2},
	.out = {.paddr = SYSCON_BASE + 0x16, .size = 2},
};

enum lcd_busy_state {
	lcd_idle = 0x00,
	lcd_busy = 0x80,
};

enum lcd_display {
	lcd_display_off = 0x00,
	lcd_display_on = 0x04,
};

enum lcd_cursor {
	lcd_cursor_off = 0x00,
	lcd_cursor_on = 0x02,
};

enum lcd_blink {
	lcd_blink_off = 0x00,
	lcd_blink_on = 0x01,
};

enum lcd_lines {
	lcd_lines_1 = 0x00,
	lcd_lines_2 = 0x08,
};

enum lcd_font {
	lcd_font_5by8 = 0x00,
	lcd_font_5by11 = 0x04,
};

enum lcd_data_len {
	lcd_4bit = 0x00,
	lcd_8bit = 0x10,
};

enum lcd_id {
	lcd_id_left = 0x00,
	lcd_id_right = 0x02,
};

enum lcd_sh {
	lcd_sh_off = 0x00,
	lcd_sh_on = 0x01,
};

static struct lcd_t {
	struct dio_t *dio;
	int pos;
	enum lcd_display display_state;
	enum lcd_cursor cursor_state;
	enum lcd_blink blink_state;
} lcd = {
	.dio = &lcd_dio,
};


static void dio_set(struct dio_t *dio, unsigned int set_mask, unsigned int clear_mask)
{
	unsigned int dir, out;
	unsigned long flags;
	unsigned int output_mask = set_mask | clear_mask;

	// make these operations seemly atomic (at least
	// on our single core system)
	local_irq_save(flags);

	// set and clear output state
	out = ioread16(dio->out.vaddr);
	out |= set_mask;
	out &= ~clear_mask;
	iowrite16(out, dio->out.vaddr);

	// ensure these pins are outputs (if already inputs they will
	// all switch together, if some were inputs and some where
	// outputs there might be a slight glitch between some pins
	// and if all were outputs this step does nothing effectively
	dir = ioread16(dio->dir.vaddr);
	dir |= output_mask; // 1 = output, 0 = input
	iowrite16(dir, dio->dir.vaddr);

	local_irq_restore(flags);
}

static unsigned int dio_get(struct dio_t *dio, unsigned int get_mask)
{
	unsigned int dir, in;
	unsigned long flags;

	// make these operations seemly atomic (at least
	// on our single core system)
	local_irq_save(flags);

	// ensure these pins are inputs
	dir = ioread16(dio->dir.vaddr);
	dir &= ~get_mask; // 1 = output, 0 = input
	iowrite16(dir, dio->dir.vaddr);

	// set and clear output state
	in = ioread16(dio->in.vaddr);
	in &= get_mask;
	
	local_irq_restore(flags);
	return in;
}

static int dio_init(struct dio_t *dio)
{
	// sanity check (can remove after devel phase)
	if (!dio) {
		printk(KERN_ERR "invalid parameter\n");
		return -EFAULT;
	}

	// request dir, in, out regions
	dio->dir.res = request_region(dio->dir.paddr, dio->dir.size, MODULE_NAME);
	if (!dio->dir.res) {
		printk(KERN_ERR "requested io region (%.8lx) is in use\n", dio->dir.paddr);
		return -EBUSY;
	}
	dio->in.res = request_region(dio->in.paddr, dio->in.size, MODULE_NAME);
	if (!dio->in.res) {
		printk(KERN_ERR "requested io region (%.8lx) is in use\n", dio->in.paddr);
		return -EBUSY;
	}
	dio->out.res = request_region(dio->out.paddr, dio->out.size, MODULE_NAME);
	if (!dio->out.res) {
		printk(KERN_ERR "requested io region (%.8lx) is in use\n", dio->out.paddr);
		return -EBUSY;
	}
	
	// get virtual address of dir, in, out from phys address so we may use them
	dio->dir.vaddr = ioremap_nocache(dio->dir.paddr, dio->dir.size);
	if (!dio->dir.vaddr) {
		printk(KERN_ERR "unable to remap io region (%.8lx)\n", dio->dir.paddr);
		return -EFAULT;
	}
	dio->in.vaddr = ioremap_nocache(dio->in.paddr, dio->in.size);
	if (!dio->in.vaddr) {
		printk(KERN_ERR "unable to remap io region (%.8lx)\n", dio->in.paddr);
		return -EFAULT;
	}
	dio->out.vaddr = ioremap_nocache(dio->out.paddr, dio->out.size);
	if (!dio->out.vaddr) {
		printk(KERN_ERR "unable to remap io region (%.8lx)\n", dio->out.paddr);
		return -EFAULT;
	}
	
	return 0;
}

static void dio_deinit(struct dio_t *dio)
{
	// unmap virtual addresses of dir, in, out
	if (dio->dir.vaddr)
		iounmap(dio->dir.vaddr);
	dio->dir.vaddr = NULL;
	if (dio->in.vaddr)
		iounmap(dio->in.vaddr);
	dio->in.vaddr = NULL;
	if (dio->out.vaddr)
		iounmap(dio->out.vaddr);
	dio->out.vaddr = NULL;

	// release memory regions
	if (dio->dir.res)
		release_region(dio->dir.paddr, dio->dir.size);
	dio->dir.res = NULL;
	if (dio->in.res)
		release_region(dio->in.paddr, dio->in.size);
	dio->in.res = NULL;
	if (dio->out.res)
		release_region(dio->out.paddr, dio->out.size);
	dio->out.res = NULL;
}

#define cond_to_dio_masks(cond, set, clear, bit) {if (cond) set |= bit; else clear |= bit;}
static void lcd_write4(struct lcd_t *lcd, uint8_t rs, uint8_t db)
{
	unsigned int set = 0;
	unsigned int clear = 0;

	// set rw = 0 (write), and rs
	cond_to_dio_masks(rs, set, clear, RS);
	dio_set(lcd->dio, set, clear | RW);

	// wait for >= tsp1
	ndelay(Tsp1 - Tr + Tm);

	// set e hi
	dio_set(lcd->dio, E, 0);
	ndelay(Tr + Tm);

	// hold e hi for >= tpw - tsp2
	ndelay(Tpw - Tsp2 + Tm);

	// set/clear db
	set = 0;
	clear = 0;
	cond_to_dio_masks((db & (1 << 4)), set, clear, D4);
	cond_to_dio_masks((db & (1 << 5)), set, clear, D5);
	cond_to_dio_masks((db & (1 << 6)), set, clear, D6);
	cond_to_dio_masks((db & (1 << 7)), set, clear, D7);
	dio_set(lcd->dio, set, clear);
	
	// hack for TS8500: even though u10 is powered off it still adds a lot of capacitance
	// the d5 line, this takes 5us to die away so we add a 10us delay here to handle that
	// on the real fls this should not be needed as there is no u10
	udelay(10);
	
	// hold db and enable for >= tps2
	ndelay(Tsp2 + Tm);

	// set e lo
	dio_set(lcd->dio, 0, E);
	ndelay(Tf + Tm);

	// wait for >= thd1 + tf
	ndelay(Tc - Tr - Tpw - Tf + Tm);
}

static void lcd_write8(struct lcd_t *lcd, uint8_t rs, uint8_t db)
{
	lcd_write4(lcd, rs, db);        // upper nibble first
	lcd_write4(lcd, rs, db << 4);   // then lower nibble
}

static uint8_t lcd_read4(struct lcd_t *lcd, uint8_t rs)
{
	uint8_t db = 0, tmp;
	unsigned int set = 0;
	unsigned int clear = 0;

	// set rw = 1 (read), and rs
	cond_to_dio_masks(rs, set, clear, RS);
	dio_set(lcd->dio, set | RW, clear);

	// wait for >= tsp1
	ndelay(Tsp1 - Tr + Tm);

	// set e hi
	dio_set(lcd->dio, E, 0);
	ndelay(Tr + Tm);

	// hold e hi for >= tpw - tsp2
	ndelay(Td - Tr + Tm);

	// hack for TS8500: even though u10 is powered off it still adds a lot of capacitance
	// the d5 line, this takes 5us to die away so we add a 10us delay here to handle that
	// on the real fls this should not be needed as there is no u10
	udelay(10);

	// set/clear db
	tmp = dio_get(lcd->dio, D4 | D5 | D6 | D7);
	db |= tmp & D4 ? (1 << 4): 0;
	db |= tmp & D5 ? (1 << 5): 0;
	db |= tmp & D6 ? (1 << 6): 0;
	db |= tmp & D7 ? (1 << 7): 0;
	
	// hold db and enable for >= tps2
	ndelay(Tpw + Tr - Td + Tm);
	
	// set e lo
	dio_set(lcd->dio, 0, E);
	ndelay(Tf + Tm);

	// wait for >= thd1 + tf
	ndelay(Tc - Tr - Tpw - Tf + Tm);

	return db;
}

static uint8_t lcd_read8(struct lcd_t *lcd, uint8_t rs)
{
	uint8_t db = 0;
	db |= (lcd_read4(lcd, rs) >> 0) & 0xf0;
	db |= (lcd_read4(lcd, rs) >> 4) & 0x0f;
	return db;
}

static enum lcd_busy_state lcd_is_busy(struct lcd_t *lcd, uint8_t *addr)
{
	uint8_t db;
	db = lcd_read8(lcd, 0);
	if (addr)
		*addr = db & 0x7f;
	return db & 0x80;
}

void lcd_display_control(struct lcd_t *lcd, enum lcd_display d, enum lcd_cursor c, enum lcd_blink b)
{
	uint8_t db = 0x08;	// display control 

	// build command
	db |= d | c | b;

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);

	// update states
	lcd->display_state = d;
	lcd->cursor_state = c;
	lcd->blink_state = b;
}

static void lcd_function_set(struct lcd_t *lcd, enum lcd_lines n, enum lcd_font f)
{
	// warning I think this function (or parts of it) may only work at power on
	// see the datasheet (section 4-bit interface mode, p16)
	uint8_t db = 0x20;	// function set

	// build command
	db |= lcd_4bit | n | f;

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);
}

void lcd_cursor(struct lcd_t *lcd, bool enable)
{
	lcd->cursor_state = enable ? lcd_cursor_on: lcd_cursor_off;
	lcd_display_control(lcd, lcd->display_state, lcd->cursor_state, lcd->blink_state);
}

void lcd_blink(struct lcd_t *lcd, bool enable)
{
	lcd->blink_state = enable ? lcd_blink_on: lcd_blink_off;
	lcd_display_control(lcd, lcd->display_state, lcd->cursor_state, lcd->blink_state);
}

static void lcd_clear(struct lcd_t *lcd)
{
	uint8_t db = 0x01;	// display clear

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);
}

static void lcd_home(struct lcd_t *lcd)
{
	uint8_t db = 0x02;	// home 

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);
	lcd->pos = 0;
}

static void lcd_entry_mode(struct lcd_t *lcd, enum lcd_id id, enum lcd_sh sh)
{
	uint8_t db = 0x04;	// entry mode

	// build command
	db |= id | sh;

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);
}

static void lcd_set_dram_addr(struct lcd_t *lcd, uint8_t addr)
{
	uint8_t db = 0x80;	// set dram address 

	// build command
	addr &= 0x7f;
	db |= addr;

	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 0, db);
	lcd->pos = addr;
}

static void lcd_new_line(struct lcd_t *lcd)
{
	if (lcd->pos >= LINE1_START && lcd->pos < (LINE1_START + LINE_LENGTH))
		// we are in line 1, goto line 2
		lcd_set_dram_addr(lcd, LINE2_START);
	else if (lcd->pos >= LINE2_START && lcd->pos < (LINE2_START + LINE_LENGTH))
		// we are in line 2, goto line 3
		lcd_set_dram_addr(lcd, LINE3_START);
	else if (lcd->pos >= LINE3_START && lcd->pos < (LINE3_START + LINE_LENGTH))
		// we are in line 3, goto line 4
		lcd_set_dram_addr(lcd, LINE4_START);
	else if (lcd->pos >= LINE4_START && lcd->pos < (LINE4_START + LINE_LENGTH))
		// we are in line 4, goto line 1
		lcd_set_dram_addr(lcd, LINE1_START);
	else
		// god knows where we are but go back to start
		lcd_set_dram_addr(lcd, LINE1_START);
}

static void lcd_inc_pos(struct lcd_t *lcd)
{
	switch (++lcd->pos) {
		case LINE1_START + LINE_LENGTH:
			// end of line 1, goto line 2
			lcd_set_dram_addr(lcd, LINE2_START);
			break;

		case LINE2_START + LINE_LENGTH:
			// end of line 2, goto line 3
			lcd_set_dram_addr(lcd, LINE3_START);
			break;

		case LINE3_START + LINE_LENGTH:
			// end of line 3, goto line 4
			lcd_set_dram_addr(lcd, LINE4_START);
			break;

		case LINE4_START + LINE_LENGTH:
			// end of line 4, goto line 1
			lcd_set_dram_addr(lcd, LINE1_START);
			break;

		default:
			break;
	}
}

static void lcd_dec_pos(struct lcd_t *lcd)
{
	switch (--lcd->pos) {
		case LINE1_START - 1:
			// start of line 1, goto line 4
			lcd_set_dram_addr(lcd, LINE4_START + LINE_LENGTH - 1);
			break;

		case LINE2_START - 1:
			// start of line 2, goto line 1
			lcd_set_dram_addr(lcd, LINE1_START + LINE_LENGTH - 1);
			break;

		case LINE3_START - 1:
			// start of line 3, goto line 2
			lcd_set_dram_addr(lcd, LINE2_START + LINE_LENGTH - 1);
			break;

		case LINE4_START - 1:
			// start of line 4, goto line 3
			lcd_set_dram_addr(lcd, LINE3_START + LINE_LENGTH - 1);
			break;

		default:
			break;
	}
}

#define LINE_MASK (LINE1_START | LINE2_START | LINE3_START | LINE4_START)
void lcd_getxy(struct lcd_t *lcd, int *x, int *y)
{
	*x = lcd->pos & 0x0f;
	switch (lcd->pos & LINE_MASK) {
		case LINE1_START:
			*y = 0;
			break;
		case LINE2_START:
			*y = 1;
			break;
		case LINE3_START:
			*y = 2;
			break;
		case LINE4_START:
			*y = 3;
			break;
		default:
			// how did we get here !!
			*y = 0;
			break;
	}
}

enum whence_t {WHENCE_ABS, WHENCE_REL};
int lcd_gotoxy(struct lcd_t *lcd, int x, int y, enum whence_t whence)
{
	int r = 0;
	int dp;

	switch (whence) {
		case WHENCE_ABS:
			// bounds check x,y
			if (y * LINE_LENGTH + x > 4 * LINE_LENGTH)
				return -1;
			lcd_home(lcd);
			// now we are at 0 fall through to relative moves

		case WHENCE_REL:
			// x,y are not bounds check on relative moves (they just wrap)
			dp = y * LINE_LENGTH + x;
			if (dp < 0) {
				while (dp++ != 0)
					lcd_dec_pos(lcd);
			}
			else if (dp > 0) {
				while (dp-- != 0)
					lcd_inc_pos(lcd);
			}
			break;
	}

	lcd_set_dram_addr(lcd, lcd->pos);
	return r;
}

static void lcd_putchar(struct lcd_t *lcd, char c)
{
	// wait for the lcd to be ready before sending the command
	while (lcd_is_busy(lcd, NULL) == lcd_busy);
	lcd_write8(lcd, 1, c);
	lcd_inc_pos(lcd);
}

static void lcd_puts(struct lcd_t *lcd, const char *s)
{
	int k;

	// for security we limit this to LINE_LENGTH (overflows)
	for (k = 0; k < LINE_LENGTH * 4; k++, s++) {
		if (*s == 0)
			break;
		lcd_putchar(lcd, *s);
	}
}

static void lcd_4bit_init(struct lcd_t *lcd, enum lcd_lines lines, enum lcd_font font)
{
	// power on
	///@todo turn the power switch on when available
	mdelay(Tpor0);

	// clear all pins on power up and let lcd boot
	// note datasheet says set db to 0x30 but working 
	// example code uses 0x00 so I will follow that (seems
	// to work)
	dio_set(lcd->dio, 0, RS | RW | E | D7 | D6 | D5 | D4);
	mdelay(Tpor1);

	// set 4 bit mode
	lcd_write4(lcd, 0, 0x20);
	while (lcd_is_busy(lcd, NULL) == lcd_busy);

	// set initial startup settings recommended in the datasheet
	lcd_display_control(lcd, lcd_display_off, lcd_cursor_off, lcd_blink_off); // turn everything off
	lcd_function_set(lcd, lines, font); // these can only be set after power on (see datasheet p16)
}

int lcd_init(void)
{
	int ret = 0;

	// start up msg
	printk(KERN_INFO "FLS LCD driver started\n");

	// init the registers etc
	ret = dio_init(lcd.dio);
	if (ret < 0) {
		printk(KERN_ERR "lcd module unable to init dio, bailing out\n");
		goto fail;
	}

	// set RS, RW, and E as lo outputs (these remain outputs throughout lcd operation)
	dio_set(lcd.dio, 0, (RS | RW | E));

	// do 4 bit init sequence (see datasheet, p16)
	// we cannot change the number of lines or font after this (see datasheet, p16)
	lcd_4bit_init(&lcd, lcd_lines_2, lcd_font_5by8);

	// now do our init
	lcd_clear(&lcd);
	lcd_home(&lcd);
	lcd_display_control(&lcd, lcd_display_on, lcd_cursor_off, lcd_blink_off);

	// show initial splash screen
	lcd_puts(&lcd, LCD_SPLASH_MSG);

	return 0;

fail:
	// deinit registers etc
	dio_deinit(lcd.dio);
	return ret;
}

void lcd_cleanup(void)
{
	// deinit registers etc
	dio_deinit(lcd.dio);

	// shutdown msg
	printk(KERN_INFO "FLS LCD driver done\n");
}

#ifdef MODULE
module_init(lcd_init);
#else
early_initcall(lcd_init);
#endif
module_exit(lcd_cleanup);
