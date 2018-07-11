/*
 * Texas Instruments ADS79XX SPI ADC driver
 *
 * Copyright 2016 David Lechner <david@lechnology.com>
 *
 * Based on iio/ti_ads79xx.c:
 * Copyright 2011 Analog Devices Inc (from AD7923 Driver)
 * Copyright 2012 CS Systemes d'Information
 *
 * And also on hwmon/ads79xx.c
 * Copyright (C) 2013 Texas Instruments Incorporated - http://www.ti.com/
 *	Nishanth Menon
 *
 * Licensed under the GPL-2.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sw_trigger.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>

#define ADS79XX_CR_MANUAL	(1 << 12)
#define ADS79XX_CR_WRITE	(1 << 11)
#define ADS79XX_CR_CHAN(ch)	(ch << 7)
#define ADS79XX_CR_RANGE_5V	(1 << 6)

#define ADS79XX_MAX_CHAN	16

/* val = value, dec = left shift, bits = number of bits of the mask */
#define EXTRACT(val, dec, bits)		((val >> dec) & ((1 << bits) - 1))

struct ti_ads79xx_state {
	struct spi_device	*spi;
	struct spi_transfer	ring_xfer;
	struct spi_transfer	scan_single_xfer[3];
	struct spi_message	ring_msg;
	struct spi_message	scan_single_msg;
	u16			single_tx;
	u16			single_rx;

	struct regulator	*reg;
	struct iio_sw_trigger	*hrtimer_trigger;

	unsigned int		settings;

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	u16 rx_buf[ADS79XX_MAX_CHAN + 10] ____cacheline_aligned;
	u16 tx_buf[ADS79XX_MAX_CHAN + 2];
};

struct ti_ads79xx_chip_info {
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
};

enum ti_ads79xx_id {
	ADS7950,
	ADS7951,
	ADS7952,
	ADS7953,
	ADS7954,
	ADS7955,
	ADS7956,
	ADS7957,
	ADS7958,
	ADS7959,
	ADS7960,
	ADS7961,
};

#define ADS79XX_V_CHAN(index, bits)				\
{								\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = index,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.address = index,					\
	.datasheet_name = "CH"__stringify(index),		\
	.scan_index = index,					\
	.scan_type = {						\
		.sign = 'u',					\
		.realbits = (bits),				\
		.storagebits = 16,				\
		.shift = 12 - (bits),				\
		.endianness = IIO_CPU,				\
	},							\
}

#define DECLARE_ADS79XX_4_CHANNELS(name, bits) \
const struct iio_chan_spec name ## _channels[] = { \
	ADS79XX_V_CHAN(0, bits), \
	ADS79XX_V_CHAN(1, bits), \
	ADS79XX_V_CHAN(2, bits), \
	ADS79XX_V_CHAN(3, bits), \
	IIO_CHAN_SOFT_TIMESTAMP(4), \
}

#define DECLARE_ADS79XX_8_CHANNELS(name, bits) \
const struct iio_chan_spec name ## _channels[] = { \
	ADS79XX_V_CHAN(0, bits), \
	ADS79XX_V_CHAN(1, bits), \
	ADS79XX_V_CHAN(2, bits), \
	ADS79XX_V_CHAN(3, bits), \
	ADS79XX_V_CHAN(4, bits), \
	ADS79XX_V_CHAN(5, bits), \
	ADS79XX_V_CHAN(6, bits), \
	ADS79XX_V_CHAN(7, bits), \
	IIO_CHAN_SOFT_TIMESTAMP(8), \
}

#define DECLARE_ADS79XX_12_CHANNELS(name, bits) \
const struct iio_chan_spec name ## _channels[] = { \
	ADS79XX_V_CHAN(0, bits), \
	ADS79XX_V_CHAN(1, bits), \
	ADS79XX_V_CHAN(2, bits), \
	ADS79XX_V_CHAN(3, bits), \
	ADS79XX_V_CHAN(4, bits), \
	ADS79XX_V_CHAN(5, bits), \
	ADS79XX_V_CHAN(6, bits), \
	ADS79XX_V_CHAN(7, bits), \
	ADS79XX_V_CHAN(8, bits), \
	ADS79XX_V_CHAN(9, bits), \
	ADS79XX_V_CHAN(10, bits), \
	ADS79XX_V_CHAN(11, bits), \
	IIO_CHAN_SOFT_TIMESTAMP(12), \
}

#define DECLARE_ADS79XX_16_CHANNELS(name, bits) \
const struct iio_chan_spec name ## _channels[] = { \
	ADS79XX_V_CHAN(0, bits), \
	ADS79XX_V_CHAN(1, bits), \
	ADS79XX_V_CHAN(2, bits), \
	ADS79XX_V_CHAN(3, bits), \
	ADS79XX_V_CHAN(4, bits), \
	ADS79XX_V_CHAN(5, bits), \
	ADS79XX_V_CHAN(6, bits), \
	ADS79XX_V_CHAN(7, bits), \
	ADS79XX_V_CHAN(8, bits), \
	ADS79XX_V_CHAN(9, bits), \
	ADS79XX_V_CHAN(10, bits), \
	ADS79XX_V_CHAN(11, bits), \
	ADS79XX_V_CHAN(12, bits), \
	ADS79XX_V_CHAN(13, bits), \
	ADS79XX_V_CHAN(14, bits), \
	ADS79XX_V_CHAN(15, bits), \
	IIO_CHAN_SOFT_TIMESTAMP(16), \
}

static DECLARE_ADS79XX_4_CHANNELS(ti_ads7950, 12);
static DECLARE_ADS79XX_8_CHANNELS(ti_ads7951, 12);
static DECLARE_ADS79XX_12_CHANNELS(ti_ads7952, 12);
static DECLARE_ADS79XX_16_CHANNELS(ti_ads7953, 12);
static DECLARE_ADS79XX_4_CHANNELS(ti_ads7954, 10);
static DECLARE_ADS79XX_8_CHANNELS(ti_ads7955, 10);
static DECLARE_ADS79XX_12_CHANNELS(ti_ads7956, 10);
static DECLARE_ADS79XX_16_CHANNELS(ti_ads7957, 10);
static DECLARE_ADS79XX_4_CHANNELS(ti_ads7958, 8);
static DECLARE_ADS79XX_8_CHANNELS(ti_ads7959, 8);
static DECLARE_ADS79XX_12_CHANNELS(ti_ads7960, 8);
static DECLARE_ADS79XX_16_CHANNELS(ti_ads7961, 8);

static const struct ti_ads79xx_chip_info ti_ads79xx_chip_info[] = {
	[ADS7950] = {
		.channels = ti_ads7950_channels,
		.num_channels = ARRAY_SIZE(ti_ads7950_channels),
	},
	[ADS7951] = {
		.channels = ti_ads7951_channels,
		.num_channels = ARRAY_SIZE(ti_ads7951_channels),
	},
	[ADS7952] = {
		.channels = ti_ads7952_channels,
		.num_channels = ARRAY_SIZE(ti_ads7952_channels),
	},
	[ADS7953] = {
		.channels = ti_ads7953_channels,
		.num_channels = ARRAY_SIZE(ti_ads7953_channels),
	},
	[ADS7954] = {
		.channels = ti_ads7954_channels,
		.num_channels = ARRAY_SIZE(ti_ads7954_channels),
	},
	[ADS7955] = {
		.channels = ti_ads7955_channels,
		.num_channels = ARRAY_SIZE(ti_ads7955_channels),
	},
	[ADS7956] = {
		.channels = ti_ads7956_channels,
		.num_channels = ARRAY_SIZE(ti_ads7956_channels),
	},
	[ADS7957] = {
		.channels = ti_ads7957_channels,
		.num_channels = ARRAY_SIZE(ti_ads7957_channels),
	},
	[ADS7958] = {
		.channels = ti_ads7958_channels,
		.num_channels = ARRAY_SIZE(ti_ads7958_channels),
	},
	[ADS7959] = {
		.channels = ti_ads7959_channels,
		.num_channels = ARRAY_SIZE(ti_ads7959_channels),
	},
	[ADS7960] = {
		.channels = ti_ads7960_channels,
		.num_channels = ARRAY_SIZE(ti_ads7960_channels),
	},
	[ADS7961] = {
		.channels = ti_ads7961_channels,
		.num_channels = ARRAY_SIZE(ti_ads7961_channels),
	},
};

/*
 * ti_ads79xx_update_scan_mode() setup the spi transfer buffer for the new
 * scan mask
 */
static int ti_ads79xx_update_scan_mode(struct iio_dev *indio_dev,
				       const unsigned long *active_scan_mask)
{
	struct ti_ads79xx_state *st = iio_priv(indio_dev);
	int i, cmd, len;

	len = 0;
	for_each_set_bit(i, active_scan_mask, indio_dev->num_channels) {
		cmd = ADS79XX_CR_WRITE | ADS79XX_CR_CHAN(i) | st->settings;
		st->tx_buf[len++] = cmd;
	}
	st->tx_buf[len++] = 0;
	st->tx_buf[len++] = 0;

	st->ring_xfer.len = len * 2;

	return 0;
}

/**
 * ti_ads79xx_trigger_handler() bh of trigger launched polling to ring buffer
 *
 * Currently there is no option in this driver to disable the saving of
 * timestamps within the ring.
 **/
static irqreturn_t ti_ads79xx_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ti_ads79xx_state *st = iio_priv(indio_dev);
	int err;

	err = spi_sync(st->spi, &st->ring_msg);
	if (err)
		goto done;

	iio_push_to_buffers_with_timestamp(indio_dev, &st->rx_buf[2],
		iio_get_time_ns(indio_dev));

done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ti_ads79xx_scan_direct(struct ti_ads79xx_state *st, unsigned ch)
{
	struct iio_dev *indio_dev = spi_get_drvdata(st->spi);
	int ret;

	mutex_lock(&indio_dev->mlock);

	st->single_tx = ADS79XX_CR_WRITE | ADS79XX_CR_CHAN(ch) | st->settings;

	ret = spi_sync(st->spi, &st->scan_single_msg);
	if (ret)
		goto out;

	/* returned data for wrong channel for some reason? */
	if (EXTRACT(st->single_rx, 12, 4) != ch) {
		ret = -EAGAIN;
		goto out;
	}

	ret = EXTRACT(st->single_rx, 0, 12);
out:
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static int ti_ads79xx_get_range(struct ti_ads79xx_state *st)
{
	int vref;

	vref = regulator_get_voltage(st->reg);
	if (vref < 0)
		return vref;

	vref /= 1000;

	if (st->settings & ADS79XX_CR_RANGE_5V)
		vref *= 2;

	return vref;
}

static int ti_ads79xx_read_raw(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       int *val, int *val2, long m)
{
	struct ti_ads79xx_state *st = iio_priv(indio_dev);
	int ret;

	switch (m) {
	case IIO_CHAN_INFO_RAW:
		ret = ti_ads79xx_scan_direct(st, chan->address);
		if (ret < 0)
			return ret;

		*val = ret >> chan->scan_type.shift;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		ret = ti_ads79xx_get_range(st);
		if (ret < 0)
			return ret;

		*val = ret;
		*val2 = (1 << chan->scan_type.realbits) - 1;

		return IIO_VAL_FRACTIONAL;
	}

	return -EINVAL;
}

static const struct iio_info ti_ads79xx_info = {
	.read_raw = &ti_ads79xx_read_raw,
	.update_scan_mode = ti_ads79xx_update_scan_mode,
	.driver_module = THIS_MODULE,
};

static int ti_ads79xx_probe(struct spi_device *spi)
{
	struct ti_ads79xx_state *st;
	struct iio_dev *indio_dev;
	const struct ti_ads79xx_chip_info *info;
	int ret;

	spi->bits_per_word = 16;
	ret = spi_setup(spi);
	if (ret < 0) {
		dev_err(&spi->dev, "Error in spi setup.\n");
		return ret;
	}

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (indio_dev == NULL)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	spi_set_drvdata(spi, indio_dev);

	st->spi = spi;
	st->settings = ADS79XX_CR_MANUAL | ADS79XX_CR_RANGE_5V;

	info = &ti_ads79xx_chip_info[spi_get_device_id(spi)->driver_data];

	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->dev.parent = &spi->dev;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = info->channels;
	indio_dev->num_channels = info->num_channels;
	indio_dev->info = &ti_ads79xx_info;

	/* build spi ring message */
	spi_message_init(&st->ring_msg);

	st->ring_xfer.tx_buf = &st->tx_buf[0];
	st->ring_xfer.rx_buf = &st->rx_buf[0];
	/* len will be set later */
	st->ring_xfer.cs_change = true;

	spi_message_add_tail(&st->ring_xfer, &st->ring_msg);

	/*
	 * Setup default message. The chip takes one full cycle to convert a
	 * sample. The conversion process is driven by the SPI clock, which
	 * is why we have 3 transfers. The middle one is just dummy data sent
	 * while the chip is converting the sample from first transfer.
	 */

	st->scan_single_xfer[0].tx_buf = &st->single_tx;
	st->scan_single_xfer[0].len = 2;
	st->scan_single_xfer[0].cs_change = 1;
	st->scan_single_xfer[1].tx_buf = &st->single_tx;
	st->scan_single_xfer[1].len = 2;
	st->scan_single_xfer[1].cs_change = 1;
	st->scan_single_xfer[2].rx_buf = &st->single_rx;
	st->scan_single_xfer[2].len = 2;
	st->scan_single_xfer[2].cs_change = 1;

	spi_message_init_with_transfers(&st->scan_single_msg,
					&st->scan_single_xfer[0], 3);

	st->reg = devm_regulator_get(&spi->dev, "vref");
	if (IS_ERR(st->reg)) {
		ret = PTR_ERR(st->reg);
		if (ret != -EPROBE_DEFER)
			dev_err(&spi->dev, "Failed to get regulator \"vref\".\n");
		return ret;
	}

	ret = regulator_enable(st->reg);
	if (ret) {
		dev_err(&spi->dev, "Failed to enable regulator \"vref\".\n");
		return ret;
	}

	ret = iio_triggered_buffer_setup(indio_dev, NULL,
			&ti_ads79xx_trigger_handler, NULL);
	if (ret) {
		dev_err(&spi->dev, "Failed to setup triggered buffer.\n");
		goto error_disable_reg;
	}

	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "Failed to register iio device.\n");
		goto error_cleanup_ring;
	}

	/* Hack to create continuous polling mode */
	st->hrtimer_trigger = iio_sw_trigger_create("hrtimer", dev_name(&spi->dev));
	if (IS_ERR(st->hrtimer_trigger)) {
		ret = PTR_ERR(st->hrtimer_trigger);
		goto error_unregister_iio_device;
	}

	iio_trigger_set_immutable(indio_dev, st->hrtimer_trigger->trigger);

	return 0;

error_unregister_iio_device:
	iio_device_unregister(indio_dev);
error_cleanup_ring:
	iio_triggered_buffer_cleanup(indio_dev);
error_disable_reg:
	regulator_disable(st->reg);

	return ret;
}

static int ti_ads79xx_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct ti_ads79xx_state *st = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	iio_sw_trigger_destroy(st->hrtimer_trigger);
	iio_triggered_buffer_cleanup(indio_dev);
	regulator_disable(st->reg);

	return 0;
}

static const struct spi_device_id ti_ads79xx_id[] = {
	{"ti-ads7950", ADS7950},
	{"ti-ads7951", ADS7951},
	{"ti-ads7952", ADS7952},
	{"ti-ads7953", ADS7953},
	{"ti-ads7954", ADS7954},
	{"ti-ads7955", ADS7955},
	{"ti-ads7956", ADS7956},
	{"ti-ads7957", ADS7957},
	{"ti-ads7958", ADS7958},
	{"ti-ads7959", ADS7959},
	{"ti-ads7960", ADS7960},
	{"ti-ads7961", ADS7961},
	{ }
};
MODULE_DEVICE_TABLE(spi, ti_ads79xx_id);

static struct spi_driver ti_ads79xx_driver = {
	.driver = {
		.name	= "ti-ads79xx",
		.owner	= THIS_MODULE,
	},
	.probe		= ti_ads79xx_probe,
	.remove		= ti_ads79xx_remove,
	.id_table	= ti_ads79xx_id,
};
module_spi_driver(ti_ads79xx_driver);

MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_DESCRIPTION("TI ADS795X/ADC796X ADC");
MODULE_LICENSE("GPL v2");
