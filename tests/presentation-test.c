/*
 * Copyright © 2014 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "weston-test-client-helper.h"
#include "presentation_timing-client-protocol.h"

static struct presentation *
get_presentation(struct client *client)
{
	struct global *g;
	struct global *global_pres = NULL;
	static struct presentation *pres;

	if (pres)
		return pres;

	wl_list_for_each(g, &client->global_list, link) {
		if (strcmp(g->interface, "presentation"))
			continue;

		if (global_pres)
			assert(0 && "multiple presentation objects");

		global_pres = g;
	}

	assert(global_pres && "no presentation found");

	assert(global_pres->version == 1);

	pres = wl_registry_bind(client->wl_registry, global_pres->name,
				&presentation_interface, 1);
	assert(pres);

	return pres;
}

struct feedback {
	struct client *client;
	struct presentation_feedback *obj;

	enum {
		FB_PENDING = 0,
		FB_PRESENTED,
		FB_DISCARDED
	} result;

	struct wl_output *sync_output;
	uint64_t seq;
	struct timespec time;
	uint32_t refresh_nsec;
	uint32_t flags;
};

static void
timespec_from_proto(struct timespec *tm, uint32_t tv_sec_hi,
		    uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	tm->tv_sec = ((uint64_t)tv_sec_hi << 32) + tv_sec_lo;
	tm->tv_nsec = tv_nsec;
}

static void
feedback_sync_output(void *data,
		     struct presentation_feedback *presentation_feedback,
		     struct wl_output *output)
{
	struct feedback *fb = data;

	assert(fb->result == FB_PENDING);

	if (output)
		fb->sync_output = output;
}

static void
feedback_presented(void *data,
		   struct presentation_feedback *presentation_feedback,
		   uint32_t tv_sec_hi,
		   uint32_t tv_sec_lo,
		   uint32_t tv_nsec,
		   uint32_t refresh_nsec,
		   uint32_t seq_hi,
		   uint32_t seq_lo,
		   uint32_t flags)
{
	struct feedback *fb = data;

	assert(fb->result == FB_PENDING);
	fb->result = FB_PRESENTED;
	fb->seq = ((uint64_t)seq_hi << 32) + seq_lo;
	timespec_from_proto(&fb->time, tv_sec_hi, tv_sec_lo, tv_nsec);
	fb->refresh_nsec = refresh_nsec;
	fb->flags = flags;
}

static void
feedback_discarded(void *data,
		   struct presentation_feedback *presentation_feedback)
{
	struct feedback *fb = data;

	assert(fb->result == FB_PENDING);
	fb->result = FB_DISCARDED;
}

static const struct presentation_feedback_listener feedback_listener = {
	feedback_sync_output,
	feedback_presented,
	feedback_discarded
};

static struct feedback *
feedback_create(struct client *client, struct wl_surface *surface)
{
	struct feedback *fb;

	fb = xzalloc(sizeof *fb);
	fb->client = client;
	fb->obj = presentation_feedback(get_presentation(client), surface);
	presentation_feedback_add_listener(fb->obj, &feedback_listener, fb);

	return fb;
}

static void
feedback_wait(struct feedback *fb)
{
	while (fb->result == FB_PENDING) {
		assert(wl_display_dispatch(fb->client->wl_display) >= 0);
	}
}

static char *
pflags_to_str(uint32_t flags, char *str, unsigned len)
{
	static const struct {
		uint32_t flag;
		char sym;
	} desc[] = {
		{ PRESENTATION_FEEDBACK_KIND_VSYNC, 's' },
		{ PRESENTATION_FEEDBACK_KIND_HW_CLOCK, 'c' },
		{ PRESENTATION_FEEDBACK_KIND_HW_COMPLETION, 'e' },
		{ PRESENTATION_FEEDBACK_KIND_ZERO_COPY, 'z' },
	};
	unsigned i;

	*str = '\0';
	if (len < ARRAY_LENGTH(desc) + 1)
		return str;

	for (i = 0; i < ARRAY_LENGTH(desc); i++)
		str[i] = flags & desc[i].flag ? desc[i].sym : '_';
	str[ARRAY_LENGTH(desc)] = '\0';

	return str;
}

static void
feedback_print(struct feedback *fb)
{
	char str[10];

	switch (fb->result) {
	case FB_PENDING:
		printf("pending");
		return;
	case FB_DISCARDED:
		printf("discarded");
		return;
	case FB_PRESENTED:
		break;
	}

	pflags_to_str(fb->flags, str, sizeof str);
	printf("presented %lld.%09lld, refresh %u us, [%s] seq %" PRIu64,
		(long long)fb->time.tv_sec, (long long)fb->time.tv_nsec,
		fb->refresh_nsec / 1000, str, fb->seq);
}

static void
feedback_destroy(struct feedback *fb)
{
	presentation_feedback_destroy(fb->obj);
	free(fb);
}

TEST(test_presentation_feedback_simple)
{
	struct client *client;
	struct feedback *fb;

	client = client_create(100, 50, 123, 77);
	assert(client);

	wl_surface_attach(client->surface->wl_surface,
			  client->surface->wl_buffer, 0, 0);
	fb = feedback_create(client, client->surface->wl_surface);
	wl_surface_damage(client->surface->wl_surface, 0, 0, 100, 100);
	wl_surface_commit(client->surface->wl_surface);

	client_roundtrip(client);

	feedback_wait(fb);

	printf("%s feedback:", __func__);
	feedback_print(fb);
	printf("\n");

	feedback_destroy(fb);
}
