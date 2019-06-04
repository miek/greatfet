/*
 * This file is part of GreatFET
 *
 * Code for using the Scorzonera neighbor.
 */

#include <debug.h>

#include <drivers/comms.h>
#include <drivers/gpio.h>
#include <drivers/dac/ad970x.h>
#include <drivers/sgpio.h>
#include <drivers/timer.h>

#include "../pin_manager.h"
#include "../usb_streaming.h"

#define CLASS_NUMBER_SELF (0x199)

// Store the state of the TAXI initialization.
static bool taxi_initialized = false;

/**
 * Data capture pins for the TAXI receiver.
 */
static sgpio_pin_configuration_t taxi_data_pins[] = {
	{ .sgpio_pin = 0,  .scu_group = 0, .scu_pin =  0, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 1,  .scu_group = 0, .scu_pin =  1, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 2,  .scu_group = 1, .scu_pin = 15, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 3,  .scu_group = 1, .scu_pin = 16, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 4,  .scu_group = 7, .scu_pin =  0, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 5,  .scu_group = 6, .scu_pin =  6, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 6,  .scu_group = 6, .scu_pin =  7, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 7,  .scu_group = 6, .scu_pin =  8, .pull_resistors = SCU_NO_PULL},
/*	{ .sgpio_pin = 10, .scu_group = 4, .scu_pin =  4, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 11, .scu_group = 4, .scu_pin =  5, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 12, .scu_group = 4, .scu_pin =  6, .pull_resistors = SCU_NO_PULL},
	{ .sgpio_pin = 13, .scu_group = 4, .scu_pin =  8, .pull_resistors = SCU_NO_PULL},*/
};

/**
 * Clock generation pin for the ADC clock.
 */
static sgpio_pin_configuration_t taxi_cstrb_pin =
	{ .sgpio_pin = 8,  .scu_group = 4, .scu_pin =  2, .pull_resistors = SCU_NO_PULL};

static sgpio_pin_configuration_t taxi_dstrb_pin =
	{ .sgpio_pin = 9,  .scu_group = 4, .scu_pin =  3, .pull_resistors = SCU_NO_PULL};
static sgpio_pin_configuration_t clkout_pin =
	{ .sgpio_pin = 15,  .scu_group = 4, .scu_pin =  10, .pull_resistors = SCU_NO_PULL};

/**
 * Definition of a set of logic analyzer functions.
 */
static sgpio_function_t taxi_functions[] = {
	{
		.enabled		 = true,

		// We're observing only; and not generating a pattern.
		.mode		    = SGPIO_MODE_STREAM_DATA_IN,

		// Bind each of the lower eight pins to their proper places,
		// and by deafault sample the eight of them.
		.pin_configurations      = taxi_data_pins,
		.bus_width	       = ARRAY_SIZE(taxi_data_pins),

		// Shift on DSTRB on SGPIO9
		.shift_clock_source      = SGPIO_CLOCK_SOURCE_SGPIO09,
		.shift_clock_edge	= SGPIO_CLOCK_EDGE_RISING,
		.shift_clock_qualifier   = SGPIO_ALWAYS_SHIFT_ON_SHIFT_CLOCK,

		// Capture our data into the USB bulk buffer, all ready to be sent up to the host.
		.buffer		  = usb_bulk_buffer,
		.buffer_order	    = 15, // 16384 * 2 (size of the USB streaming buffer)

		.shift_clock_output = &clkout_pin,
	},
};

/**
 * Logic analyzer configuration using SGPIO.
 */
static sgpio_t taxi  = {
	.functions      = taxi_functions,
	.function_count = ARRAY_SIZE(taxi_functions),
};

static gpio_pin_t u4_oe  = {2, 11};
static gpio_pin_t u5_oe  = {3, 3};
static gpio_pin_t trigger  = {0, 4};

/**
 * Configures a given GPIO port/pin to be used for TAXI purposes.
 */
static int set_up_taxi_gpio(gpio_pin_t pin)
{
	int rc;
	uint8_t scu_group, scu_pin;

	// Identify the SCU group and pin for the relevant GPIO pin.
	scu_group = gpio_get_group_number(pin);
	scu_pin   = gpio_get_pin_number(pin);

	// If this port/pin doesn't correspond to a valid physical pin,
	// fail out.
	if ((scu_group == 0xff) || (scu_pin == 0xff)) {
		return EINVAL;
	}

	// Reserve the pin for this class. If we can't get a hold of the pin, fail out.
	if (!pin_ensure_reservation(scu_group, scu_pin, CLASS_NUMBER_SELF)) {
		pr_warning("sdir: couldn't reserve busy pin GPIO%d[%d]!\n", pin.port, pin.pin);
		return EBUSY;
	}

	// Configure the pin to be used as a GPIO in the SCU.
	rc = gpio_configure_pinmux(pin);
	if (rc) {
		pr_warning("sdir: couldn't configure pinmux for GPIO%d[%d]!\n", pin.port, pin.pin);
		return rc;
	}

	return 0;
}

/**
 * Releases a given GPIO port/pin
 */
static int tear_down_taxi_gpio(gpio_pin_t pin)
{
	uint8_t scu_group, scu_pin;

	// Identify the SCU group and pin for the relevant GPIO pin.
	scu_group = gpio_get_group_number(pin);
	scu_pin   = gpio_get_pin_number(pin);

	// If this port/pin doesn't correspond to a valid physical pin,
	// fail out.
	if ((scu_group == 0xff) || (scu_pin == 0xff)) {
		return EINVAL;
	}

    // Place the input pin back into a high-Z state by disconnecting
    // its output driver.  TODO: should this be done in the SCU, as well?
    gpio_set_pin_direction(pin, false);

    // Finally, release the relevant reservation.
    return pin_release_reservation(scu_group, scu_pin);
}

/**
 * Set up the TAXI API.
 *
 * @return 0 on success, or an error code on failure.
 */
static int initialize_taxi(void)
{
	int rc;
	gpio_pin_t gpio_pins[] = {
		u4_oe, u5_oe, trigger
	};


	// Set up each of the GPIO we're using
	for (unsigned i = 0; i < ARRAY_SIZE(gpio_pins); ++i) {
		rc = set_up_taxi_gpio(gpio_pins[i]);
		if (rc) {
			return rc;
		}
		gpio_set_pin_direction(gpio_pins[i], true);
		gpio_set_pin(gpio_pins[i]);
	}

	pr_info("TAXI initialized.\n");
	taxi_initialized = true;
	return 0;
}


/**
 * Tear down the TAXI API, releasing the relevant resources.
 */
static int terminate_taxi(void)
{
	int rc;
	gpio_pin_t gpio_pins[] = {
		u4_oe, u5_oe, trigger
	};

	// Ensure our TAXI functionality is no longer running.
	sgpio_halt(&taxi);

	// Release each of our GPIO pins.
	for (unsigned i = 0; i < ARRAY_SIZE(gpio_pins); ++i) {
		rc = tear_down_taxi_gpio(gpio_pins[i]);
		if (rc) {
			return rc;
		}
	}

	taxi_initialized = false;
	return 0;
}

static void frame_sync_wait(sgpio_t *sgpio)
{
	const uint32_t duration = 1000;
	uint32_t base = get_time();
	bool prev = false;
	bool dstrb = false;
	while (get_time_since(base) < duration) {
		dstrb = !!(sgpio->reg->sgpio_pin_state & (1<<9));
		if (!prev && dstrb) {
			base = get_time();
		}
		prev = dstrb;
	}
}

static int verb_start_receive(struct command_transaction *trans)
{
	int rc;
	(void)trans;

	// Set up our TAXI frontend...
	rc = initialize_taxi();
	if (rc) {
		pr_error("taxi: couldn't initialize TAXI! (%d)\n", rc);
		return rc;
	}

	sgpio_pin_configuration_t *pin_config = &taxi_dstrb_pin;
	platform_scu_configure_pin_fast_io(pin_config->scu_group, pin_config->scu_pin, 7, pin_config->pull_resistors);
	rc = sgpio_set_up_functions(&taxi);
	if (rc) {
		return rc;
	}

	// Finally, start the SGPIO streaming for the relevant buffer.
	usb_streaming_start_streaming_to_host(&taxi_functions[0].position_in_buffer, &taxi_functions[0].data_in_buffer);

	frame_sync_wait(&taxi);
	frame_sync_wait(&taxi);

	sgpio_run(&taxi);

	return 0;
}


static int verb_stop(struct command_transaction *trans)
{
	(void)trans;

	// Halt transmission / reciept.
	usb_streaming_stop_streaming_to_host();
	return terminate_taxi();
}


static struct comms_verb _verbs[] = {
		{  .name = "start_receive", .handler = verb_start_receive, .in_signature = "", .out_signature = "",
	   .doc = "Start reciept of TAXI data on the primary bulk comms pipe." },
		{  .name = "stop", .handler = verb_stop, .in_signature = "", .out_signature = "",
	   .doc = "Halt TAXI communications; termianting any active communications" },

		// Sentinel.
		{}
};
COMMS_DEFINE_SIMPLE_CLASS(taxi, CLASS_NUMBER_SELF, "taxi", _verbs,
	"functionality for TAXIChip");
