#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Counter using GPIO buttons and LEDs");
MODULE_AUTHOR("Spiro Michaylov");
MODULE_VERSION("0.1");

/**
 * Set up the module parameter for enabling GPIO usage
 */

static bool enable_gpio = false;
module_param(enable_gpio, bool, 0);
MODULE_PARM_DESC(enable_gpio, "Enable/disable GPIO access (for debugging)");

/**
 * Set up LEDs -- one per binary digit, low bit first
 */

#define MAX_LEDS 8
static uint8_t led_count = 0;
static struct {
	bool on;
	uint8_t gpio;
} led_values[MAX_LEDS];
static uint8_t gpio_increment_button = 0;

/**
 * Counter state
 */

static uint8_t value = 0; // displayed in LEDs
static uint8_t max_value = 0; // not displayed
static uint8_t max_possible = 0; // max possible with current LEDs

/**
 * Increment the value, setting max_value if needed, and 
 * wrapping to 0 if needed -- wrapping does not impact the max_value
 * @return true if wrapped
 */
static bool
increment_maybe_wrap(void) {
	if (value < max_possible) {
		value++;
		if (value > max_value) {
			max_value++;
		}
		return false;
	} else {
		value = 0;
		return true;
	}
}

static void
zero_counters(void) 
{
	value = 0;
	// let max_value stay as a record
	max_possible = 0;
}

static void
setup_max_possible(void)
{
	max_possible = 0;
	for (int i = 0; i < led_count; i++) {
		max_possible = (max_possible << 1) | 1; 
	}
	if (value > max_possible) {
		value = 0;
	}
	printk(KERN_INFO "gpiocount: set max_possible = %u\n", max_possible);
	printk(KERN_INFO "gpiocount: new value = %u\n", value);
}

#define GPIO_MAX_DIGITS 3

/**
 * Parse a LED digit GPIO assignment string and validate, 
 * then set up structures and initialize the LEDs 
 * (if GPIO is enabled) -- must be called with no digits assigned
 */
static int 
assign_leds(const char *led_desc) 
{
	if (led_count > 0) {
		printk(KERN_INFO "gpiocount: cannot assign LEDs when assigned\n");
		return -EPERM; 
	} 
	const char *curr = led_desc;
	char gpio_digits[GPIO_MAX_DIGITS + 1];
	uint8_t next_digit = 0;
	for (;;) {
		char c = *curr;
		if (c == ',' || c == '\0') {
			// end of a number -- process it
			if (next_digit == 0) {
				led_count = 0;
				printk(KERN_INFO "gpiocount: empty LED GPIO at %u\n", led_count);
				return -EINVAL;
			} else if (led_count >= MAX_LEDS) {
				printk(KERN_INFO "gpiocount: too many LED GPIOs -- skipping rest \n");
				break;
			} else {
				// go ahead
			}
			gpio_digits[next_digit] = '\0';
			// parse and add
			uint32_t ttt;
			sscanf(gpio_digits, "%u", &ttt);
			led_values[led_count].gpio = ttt;
			led_values[led_count].on = false;
			led_count++;
			next_digit = 0;
		} else {
			// add the digit unless it's too many
			if (next_digit >=  GPIO_MAX_DIGITS) {
				led_count = 0;
				printk(KERN_INFO "gpiocount: LED GPIO with too many digits\n");
				return -EINVAL;
			} 
			gpio_digits[next_digit++] = c;
		}
		if (*curr == '\0') {
			break;
		}
		curr++;
	}
	setup_max_possible();
	if (enable_gpio) {
		for (uint8_t i = 0; i < led_count; i++) {
			printk(KERN_INFO "gpiocount: initializing LED on GPIO %d\n", 
				led_values[i].gpio);
			if (!gpio_is_valid(led_values[i].gpio)){
				printk(KERN_INFO "gpiocount: invalid LED GPIO %u -- releasing all\n", led_values[i].gpio);
				// assumption: all the prior ones were successful
				// so we can and should release them
				for (uint8_t j = j; j < i; j++) {
					gpio_set_value(led_values[j].gpio, 0);
					gpio_free(led_values[j].gpio);
				}
				zero_counters();
				return -ENODEV;
			}

			gpio_direction_output(led_values[i].gpio, 0);
		}
	}
	return 0;
}

/**
 * Unassign any dynamically assigned LED digits, disassociate from their GPIOs
 * and finalzie the GPIOs (ig GPIO is enabled) -- call this before 
 * subsequent to assign_leds() 
 */
static int 
unassign_leds(void) 
{
	if (enable_gpio) {
		for (uint8_t i = 0; i < led_count; i++) {
			printk(KERN_INFO "gpiocount: releasing LED on GPIO %d\n", 
				led_values[i].gpio);
			gpio_set_value(led_values[i].gpio, 0);
			gpio_unexport(led_values[i].gpio);
			gpio_free(led_values[i].gpio);
		}
	}
	led_count = 0;
	return 0;
}

/**
 * Use the current value to set the boolean values of all the LEDs and 
 * then, if GPIO is enabled, make sure the actual LEDs reflect these settings
 */
static int 
set_leds_from_value(void) {
	// since the low bits are first, just shift each low bit out 
	// of the value and use it 
	printk(KERN_INFO "gpiocount: representing value %u\n", value); 
	uint8_t bits = value;
	for (int i = 0; i < led_count; i++) {
		uint8_t bit = bits & 0x1;
		bits = bits >> 1;
		led_values[i].on = (bit == 0x1);
		printk(KERN_INFO "gpiocount: bit %d is %s\n", 
				i, led_values[i].on ? "on" : "off");
	}
	if (enable_gpio) {
		for (int i = 0; i < led_count; i++) {
			gpio_set_value(led_values[i].gpio, led_values[i].on ? 1 : 0);
		}
	}
	return 0;
}

/**
 * Buttond ebouncing logic
 */
static unsigned int last_interrupt_time_msec = 0;
static uint64_t epoch_msec; // to keep numbers small

/**
 * Initialize epoch time
 */
static void 
init_debounce(void) 
{
	struct timeval tv;
  	do_gettimeofday(&tv);
	epoch_msec = (uint64_t)tv.tv_sec * (uint64_t)1000 + (uint64_t)(tv.tv_usec / 1000);
}

/**
 * Time (in msec) since epoch, for debouncing
 */
static unsigned int since_epoch_msec(void)
{
  struct timeval tv;
  uint64_t now;

  do_gettimeofday(&tv);
  now = (uint64_t)tv.tv_sec * (uint64_t)1000 + (uint64_t)(tv.tv_usec / 1000);

  return (uint32_t)(now - epoch_msec);
}

/**
 * Button handler
 */

static unsigned int increment_irq = 0;

static irq_handler_t 
button_irq_handler(unsigned int irq, void *dev_id, struct pt_regs *regs) { 
	printk(KERN_INFO "gpiocount: entering handler\n");
	unsigned int interrupt_time_msec = since_epoch_msec();

   	if (interrupt_time_msec - last_interrupt_time_msec < 200) 
   	{
     	printk(KERN_INFO "gpiocount: ignored interrupt [%d]%s \n",
          irq, (char *) dev_id);
     	return (irq_handler_t) IRQ_HANDLED;
   	}
   	last_interrupt_time_msec = interrupt_time_msec;
	increment_maybe_wrap();
	set_leds_from_value();
	printk(KERN_INFO "gpiocount: exiting handler\n");
   	return (irq_handler_t) IRQ_HANDLED;
}

/** 
 * Invariant: no button is currently set up
 */
static int 
assign_increment_button(void)
{
	if (enable_gpio) {

		if (!gpio_is_valid(gpio_increment_button)) {
			printk(KERN_INFO "gpiocount: invalid button GPIO\n");
			return -EINVAL;
		}
		gpio_direction_input(gpio_increment_button);
		// TODO: seems like this made it worse!
		int result = gpio_set_debounce(gpio_increment_button, 200);
		if (result) {
			printk(KERN_INFO "gpiocount: attempt to debounce returned %d\n", result); 
		} else {
			printk(KERN_INFO "gpiocount: debounce ok\n"); 
		}

		increment_irq = gpio_to_irq(gpio_increment_button);
   		printk(KERN_INFO "gpiocount: The button is mapped to IRQ: %d\n", increment_irq);

		result = request_irq(increment_irq,
                        (irq_handler_t) button_irq_handler,
                        IRQF_TRIGGER_RISING,
                        "gpiocount_handler",
                        NULL);

		if (result) {
			printk(KERN_INFO "gpiocount: The interrupt request result is: %d\n", result);   
			return result;
		}
	}
	
	return 0;
}

static int
unassign_increment_button(void) 
{
	if (enable_gpio) {
		if (gpio_increment_button > 0) {
			printk(KERN_INFO "gpiocount: releasing increment button on GPIO %d\n", 
				gpio_increment_button);
			free_irq(increment_irq, NULL);
			gpio_unexport(gpio_increment_button);
			gpio_free(gpio_increment_button);
		}
	}
	return 0;
}

/**
 * Unassign all dyanmically defined buttons
 */
static int
unassign_buttons(void) 
{
	int result = unassign_increment_button();
	if (result) return result;
	return 0;
}

/**
 * Set up sysfs integration
 */

static struct kobject *gpiocount_kobj = NULL; 

static ssize_t value_show(struct kobject *kobj, 
	struct kobj_attribute *attr, char *buf)
{
   	return sprintf(buf, "%u\n", value);
}

static ssize_t value_store(struct kobject *kobj, 
	struct kobj_attribute *attr,
    const char *buf, size_t count)
{
	uint32_t t;
   	sscanf(buf, "%u", &t);
	value = t;
	printk(KERN_INFO "gpiocount: 'value' set to %d via sysfs\n", value);
	set_leds_from_value();
   	return count;
}

static ssize_t max_value_show(struct kobject *kobj, 
	struct kobj_attribute *attr, char *buf)
{
   	return sprintf(buf, "%u\n", max_value);
}

static ssize_t max_value_store(struct kobject *kobj, 
	struct kobj_attribute *attr,
    const char *buf, size_t count)
{
	uint32_t t;
   	sscanf(buf, "%u", &t);
	max_value = t;
	printk(KERN_INFO "gpiocount: 'max_value' set to %d via sysfs\n", 
		max_value);
   	return count;
}

static ssize_t gpio_leds_show(struct kobject *kobj, 
	struct kobj_attribute *attr, char *buf)
{
	int length = 0;
	for (int i = 0; i < led_count; i++) {
		if (i != 0) {
			length += sprintf(buf + length, ",");
		}
		length += sprintf(buf + length, "%u", led_values[i].gpio);
	}
	length += sprintf(buf + length, "\n");
   	return length;
}

static ssize_t gpio_leds_store(struct kobject *kobj, 
	struct kobj_attribute *attr,
    const char *buf, size_t count)
{
	printk(KERN_INFO "gpiocount: reloading LED GPIOs\n");
	unassign_leds();
	assign_leds(buf);
	set_leds_from_value();
   	return count;
}

static ssize_t increment_store(struct kobject *kobj, 
	struct kobj_attribute *attr,
    const char *buf, size_t count)
{
	printk(KERN_INFO "gpiocount: incrementing counter\n");
	increment_maybe_wrap();
	set_leds_from_value();
   	return count;
}

static ssize_t gpio_button_increment_show(struct kobject *kobj, 
	struct kobj_attribute *attr, char *buf)
{
   	return sprintf(buf, "%u\n", gpio_increment_button);
}

static ssize_t gpio_button_increment_store(struct kobject *kobj, 
	struct kobj_attribute *attr,
    const char *buf, size_t count)
{
	uint32_t t;
   	sscanf(buf, "%u", &t);
	unassign_increment_button(); // in case we already have one
	// don't assign until after we've disabled the previous one
	gpio_increment_button = t;
	assign_increment_button();
   	return count;
}

static struct kobj_attribute value_attr = 
	__ATTR(value, 0644, value_show, value_store);
static struct kobj_attribute max_value_attr = 
	__ATTR(max_value, 0644, max_value_show, max_value_store);
static struct kobj_attribute gpio_leds_attr = 
	__ATTR(gpio_leds, 0644, gpio_leds_show, gpio_leds_store);
static struct kobj_attribute increment_attr = 
	__ATTR_WO(increment);
static struct kobj_attribute gpio_button_increment_attr = 
	__ATTR(gpio_button_increment, 0644, 
		gpio_button_increment_show, gpio_button_increment_store);

static struct attribute *gpiocount_attrs[] = {
      &value_attr.attr,                  
      &max_value_attr.attr,
	  &gpio_leds_attr.attr,  
	  &increment_attr.attr,
	  &gpio_button_increment_attr.attr,
      NULL,
};

static struct attribute_group gpiocount_attr_grp = {
	.attrs = gpiocount_attrs,
};

/**
 * Initialization
 */
int gpiocount_init(void)
{
	printk(KERN_INFO "gpiocount: initializing\n");
   
	value = 0u;
	max_value = 0u;

	printk(KERN_INFO "gpiocount: value = %d, max_value = %d", value, max_value);

	init_debounce();

	// initialize the hardware first

	if (enable_gpio) {

		printk(KERN_INFO "gpiocount: GPIO enabled -- setting up\n");

		printk(KERN_INFO "gpiocount: GPIO setup completed\n");
	} else {
		printk(KERN_INFO "gpiocount: GPIO disabled\n");
	}

	// initialize sysfs only after the hardware is available to use

	gpiocount_kobj = 
		kobject_create_and_add("gpiocount", kernel_kobj);
	if (!gpiocount_kobj) {
		printk(KERN_ALERT "gpiocount: failed to create kobject\n");
      	return -ENOMEM;
	}

	int result = sysfs_create_group(gpiocount_kobj, &gpiocount_attr_grp);
	if (result) {
		kobject_put(gpiocount_kobj);
		return result;
	} 

    printk(KERN_INFO "gpiocount: initialized\n");

	return 0;
}

/**
 * Cleanup
 */
void gpiocount_exit(void)
{
	printk(KERN_INFO "gpiocount: exiting\n");
	
	unassign_leds();
	unassign_buttons();

	if (gpiocount_kobj != NULL) {
		printk(KERN_INFO "gpiocount: finalizing sysfs\n");
		kobject_put(gpiocount_kobj);
	}

	// finalize the hardware last

	if (enable_gpio) {
		printk(KERN_INFO "gpiocount: finalizing GPIO\n");

		printk(KERN_INFO "gpiocount: finished finalizing GPIO\n");
	} else {
		printk(KERN_INFO "gpiocount: no need to finalize GPIO\n");
	}


	printk(KERN_INFO "gpiocount: exited\n");
}

module_init(gpiocount_init);
module_exit(gpiocount_exit);


