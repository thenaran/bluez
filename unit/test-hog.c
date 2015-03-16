/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2015  Intel Corporation. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <glib.h>

#include "src/shared/util.h"
#include "src/shared/tester.h"

#include "attrib/gattrib.h"

#include "android/hog.h"

struct test_pdu {
	bool valid;
	const uint8_t *data;
	size_t size;
};

struct test_data {
	char *test_name;
	struct test_pdu *pdu_list;
};

struct context {
	GAttrib *attrib;
	struct bt_hog *hog;
	guint source;
	guint process;
	int fd;
	unsigned int pdu_offset;
	const struct test_data *data;
};

#define data(args...) ((const unsigned char[]) { args })

#define raw_pdu(args...)    \
{      \
	.valid = true,		\
	.data = data(args), \
	.size = sizeof(data(args)),\
}

#define false_pdu()	\
{						\
		.valid = false, \
}

#define define_test(name, function, args...)      \
	do {    \
		const struct test_pdu pdus[] = {			\
			args, { }					\
		};		\
		static struct test_data data;      \
		data.test_name = g_strdup(name);   \
		data.pdu_list = g_malloc(sizeof(pdus));			\
		memcpy(data.pdu_list, pdus, sizeof(pdus));		\
		tester_add(name, &data, NULL, function, NULL);     \
	} while (0)

static void test_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_debug("%s%s", prefix, str);
}

static gboolean context_quit(gpointer user_data)
{
	struct context *context = user_data;

	if (context->process > 0)
		g_source_remove(context->process);

	if (context->source > 0)
		g_source_remove(context->source);

	bt_hog_unref(context->hog);

	g_attrib_unref(context->attrib);

	g_free(context);

	tester_test_passed();

	return FALSE;
}

static gboolean send_pdu(gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	ssize_t len;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	len = write(context->fd, pdu->data, pdu->size);

	util_hexdump('<', pdu->data, len, test_debug, "hog: ");

	g_assert_cmpint(len, ==, pdu->size);

	context->process = 0;

	if (!context->data->pdu_list[context->pdu_offset].valid)
		context_quit(context);

	return FALSE;
}

static gboolean test_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	unsigned char buf[512];
	const struct test_pdu *pdu;
	ssize_t len;
	int fd;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		context->source = 0;
		g_print("%s: cond %x\n", __func__, cond);
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));

	g_assert(len > 0);

	util_hexdump('>', buf, len, test_debug, "hog: ");

	g_assert_cmpint(len, ==, pdu->size);

	g_assert(memcmp(buf, pdu->data, pdu->size) == 0);

	context->process = g_idle_add(send_pdu, context);

	return TRUE;
}

static struct context *create_context(gconstpointer data)
{
	struct context *context;
	GIOChannel *channel, *att_io;
	int err, sv[2], fd;
	char name[] = "bluez-hog";
	uint16_t vendor = 0x0002;
	uint16_t product = 0x0001;
	uint16_t version = 0x0001;

	context = g_new0(struct context, 1);
	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	att_io = g_io_channel_unix_new(sv[0]);

	g_io_channel_set_close_on_unref(att_io, TRUE);

	context->attrib = g_attrib_new(att_io, 23);
	g_assert(context->attrib);

	g_io_channel_unref(att_io);

	fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	g_assert(fd > 0);

	context->hog = bt_hog_new(fd, name, vendor, product, version, NULL);
	g_assert(context->hog);

	channel = g_io_channel_unix_new(sv[1]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				test_handler, context);
	g_assert(context->source > 0);

	g_io_channel_unref(channel);

	context->fd = sv[1];
	context->data = data;

	return context;
}

static void test_hog(gconstpointer data)
{
	struct context *context = create_context(data);

	g_assert(bt_hog_attach(context->hog, context->attrib));
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	define_test("/TP/HGRF/RH/BV-01-I", test_hog,
		raw_pdu(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28),
		raw_pdu(0x11, 0x06, 0x01, 0x00, 0x04, 0x00, 0x12,
			0x18, 0x05, 0x00, 0x08, 0x00, 0x12, 0x18),
		raw_pdu(0x10, 0x09, 0x00, 0xff, 0xff, 0x00, 0x28),
		raw_pdu(0x01, 0x10, 0x09, 0x00, 0x0a),
		raw_pdu(0x08, 0x01, 0x00, 0x04, 0x00, 0x03, 0x28),
		raw_pdu(0x09, 0x07, 0x03, 0x00, 0x02, 0x04, 0x00,
			0x4b, 0x2a),
		raw_pdu(0x08, 0x01, 0x00, 0x04, 0x00, 0x02, 0x28),
		raw_pdu(0x01, 0x08, 0x01, 0x00, 0x0a),
		raw_pdu(0x08, 0x05, 0x00, 0x08, 0x00, 0x02, 0x28),
		raw_pdu(0x01, 0x08, 0x05, 0x00, 0x0a),
		raw_pdu(0x08, 0x05, 0x00, 0x08, 0x00, 0x03, 0x28),
		raw_pdu(0x09, 0x07, 0x07, 0x00, 0x02, 0x08, 0x00,
			0x4b, 0x2a),
		raw_pdu(0x0a, 0x04, 0x00),
		raw_pdu(0x0b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
			0x15, 0x16),
		raw_pdu(0x0a, 0x08, 0x00),
		raw_pdu(0x0b, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
			0x15, 0x16),
		raw_pdu(0x0c, 0x04, 0x00, 0x16, 0x00),
		raw_pdu(0x0d, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
			0x15, 0x16),
		raw_pdu(0x0c, 0x08, 0x00, 0x16, 0x00),
		raw_pdu(0x0d, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
			0x15, 0x16),
		raw_pdu(0x0c, 0x04, 0x00, 0x2c, 0x00),
		raw_pdu(0x0d, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13),
		raw_pdu(0x0c, 0x08, 0x00, 0x2c, 0x00),
		raw_pdu(0x0d, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
			0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
			0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13));

	define_test("/TP/HGRF/RH/BV-08-I", test_hog,
		raw_pdu(0x10, 0x01, 0x00, 0xff, 0xff, 0x00, 0x28),
		raw_pdu(0x11, 0x06, 0x01, 0x00, 0x05, 0x00, 0x12,
			0x18, 0x06, 0x00, 0x0a, 0x00, 0x12, 0x18),
		raw_pdu(0x10, 0x0b, 0x00, 0xff, 0xff, 0x00, 0x28),
		raw_pdu(0x01, 0x10, 0x0b, 0x00, 0x0a),
		raw_pdu(0x08, 0x01, 0x00, 0x05, 0x00, 0x03, 0x28),
		raw_pdu(0x09, 0x07, 0x03, 0x00, 0x0a, 0x04, 0x00,
			0x4d, 0x2a),
		raw_pdu(0x08, 0x01, 0x00, 0x05, 0x00, 0x02, 0x28),
		raw_pdu(0x01, 0x08, 0x01, 0x00, 0x0a),
		raw_pdu(0x08, 0x06, 0x00, 0x0a, 0x00, 0x02, 0x28),
		raw_pdu(0x01, 0x08, 0x06, 0x00, 0x0a),
		raw_pdu(0x08, 0x06, 0x00, 0x0a, 0x00, 0x03, 0x28),
		raw_pdu(0x09, 0x07, 0x08, 0x00, 0x0a, 0x09, 0x00,
			0x4d, 0x2a),
		raw_pdu(0x08, 0x04, 0x00, 0x05, 0x00, 0x03, 0x28),
		raw_pdu(0x01, 0x08, 0x04, 0x00, 0x0a),
		raw_pdu(0x08, 0x09, 0x00, 0x0a, 0x00, 0x03, 0x28),
		raw_pdu(0x01, 0x08, 0x09, 0x00, 0x0a),
		raw_pdu(0x0a, 0x04, 0x00),
		raw_pdu(0x0b, 0xee, 0xee, 0xff, 0xff),
		raw_pdu(0x04, 0x05, 0x00, 0x05, 0x00),
		raw_pdu(0x05, 0x01, 0x05, 0x00, 0x08, 0x29),
		raw_pdu(0x0a, 0x09, 0x00),
		raw_pdu(0x0b, 0xff, 0xff, 0xee, 0xee),
		raw_pdu(0x04, 0x0a, 0x00, 0x0a, 0x00),
		raw_pdu(0x05, 0x01, 0x0a, 0x00, 0x08, 0x29),
		raw_pdu(0x0a, 0x05, 0x00),
		raw_pdu(0x0b, 0x01, 0x03),
		raw_pdu(0x0a, 0x0a, 0x00),
		raw_pdu(0x0b, 0x02, 0x03));

	return tester_run();
}
