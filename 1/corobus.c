#include "corobus.h"
#include "libcoro.h"
#include "rlist.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

struct wakeup_queue {
	struct rlist coros;
};

struct coro_bus_channel {
	size_t size_limit;
	struct wakeup_queue send_queue;
	struct wakeup_queue recv_queue;
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

static void
dv_append_many(struct data_vector *v, const unsigned *src, size_t cnt)
{
	size_t need = v->size + cnt;
	if (need > v->capacity) {
		size_t cap = v->capacity == 0 ? 4 : v->capacity * 2;
		while (cap < need) cap *= 2;
		v->capacity = cap;
		v->data = realloc(v->data, v->capacity * sizeof(unsigned));
	}
	memcpy(v->data + v->size, src, cnt * sizeof(unsigned));
	v->size += cnt;
}

static void
dv_append(struct data_vector *v, unsigned x)
{
	dv_append_many(v, &x, 1);
}

static void
dv_pop_many(struct data_vector *v, unsigned *dst, size_t cnt)
{
	assert(cnt <= v->size);
	memcpy(dst, v->data, cnt * sizeof(unsigned));
	v->size -= cnt;
	memmove(v->data, v->data + cnt, v->size * sizeof(unsigned));
}

static unsigned
dv_pop_one(struct data_vector *v)
{
	unsigned t;
	dv_pop_many(v, &t, 1);
	return t;
}

static void
wq_suspend(struct wakeup_queue *q)
{
	struct wakeup_entry e;
	e.coro = coro_this();
	rlist_add_tail_entry(&q->coros, &e, base);
	coro_suspend();
	rlist_del_entry(&e, base);
}

static void
wq_wakeup_first(struct wakeup_queue *q)
{
	if (!rlist_empty(&q->coros)) {
		struct wakeup_entry *e = rlist_first_entry(&q->coros,
			struct wakeup_entry, base);
		coro_wakeup(e->coro);
	}
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *b = malloc(sizeof(*b));
	b->channels = NULL;
	b->channel_count = 0;
	return b;
}

void
coro_bus_delete(struct coro_bus *b)
{
	for (int i = 0; i < b->channel_count; i++) {
		if (b->channels[i]) {
			free(b->channels[i]->data.data);
			free(b->channels[i]);
		}
	}
	free(b->channels);
	free(b);
}

int
coro_bus_channel_open(struct coro_bus *b, size_t size_limit)
{
	int idx = -1;
	for (int i = 0; i < b->channel_count; i++) {
		if (b->channels[i] == NULL) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		int new_cnt = b->channel_count + 1;
		b->channels = realloc(b->channels,
			new_cnt * sizeof(struct coro_bus_channel*));
		b->channels[new_cnt - 1] = NULL;
		idx = b->channel_count;
		b->channel_count = new_cnt;
	}
	struct coro_bus_channel *c = malloc(sizeof(*c));
	c->size_limit = size_limit;
	c->data.data = NULL;
	c->data.size = 0;
	c->data.capacity = 0;
	rlist_create(&c->send_queue.coros);
	rlist_create(&c->recv_queue.coros);
	b->channels[idx] = c;
	return idx;
}

void
coro_bus_channel_close(struct coro_bus *b, int chn)
{
    if (chn < 0 || chn >= b->channel_count) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return;
    }
    struct coro_bus_channel *c = b->channels[chn];
    if (!c) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return;
    }
    b->channels[chn] = NULL;


    while (!rlist_empty(&c->send_queue.coros)) {
        wq_wakeup_first(&c->send_queue);
        coro_yield(); 
    }


    while (!rlist_empty(&c->recv_queue.coros)) {
        wq_wakeup_first(&c->recv_queue);
        coro_yield(); 
    }

    free(c->data.data);
    free(c);
}


int
coro_bus_send(struct coro_bus *b, int chn, unsigned val)
{
	for (;;) {
		int r = coro_bus_try_send(b, chn, val);
		if (r == 0) return 0;
		enum coro_bus_error_code e = coro_bus_errno();
		if (e == CORO_BUS_ERR_NO_CHANNEL) return -1;
		if (e == CORO_BUS_ERR_WOULD_BLOCK) {
			struct coro_bus_channel *c = (chn >= 0 && chn < b->channel_count)
				? b->channels[chn] : NULL;
			if (!c) {
				coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
				return -1;
			}
			wq_suspend(&c->send_queue);
			continue;
		}
		return -1;
	}
}

int
coro_bus_try_send(struct coro_bus *b, int chn, unsigned val)
{
	if (chn < 0 || chn >= b->channel_count) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *c = b->channels[chn];
	if (!c) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	if (c->data.size >= c->size_limit) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}
	dv_append(&c->data, val);
	wq_wakeup_first(&c->recv_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

int
coro_bus_recv(struct coro_bus *b, int chn, unsigned *dst)
{
	for (;;) {
		int r = coro_bus_try_recv(b, chn, dst);
		if (r == 0) return 0;
		enum coro_bus_error_code e = coro_bus_errno();
		if (e == CORO_BUS_ERR_NO_CHANNEL) return -1;
		if (e == CORO_BUS_ERR_WOULD_BLOCK) {
			struct coro_bus_channel *c = (chn >= 0 && chn < b->channel_count)
				? b->channels[chn] : NULL;
			if (!c) {
				coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
				return -1;
			}
			wq_suspend(&c->recv_queue);
			continue;
		}
		return -1;
	}
}

int
coro_bus_try_recv(struct coro_bus *b, int chn, unsigned *dst)
{
	if (chn < 0 || chn >= b->channel_count) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *c = b->channels[chn];
	if (!c) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	if (c->data.size == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}
	unsigned tmp = dv_pop_one(&c->data);
	*dst = tmp;
	wq_wakeup_first(&c->send_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *b, unsigned val)
{
	for (;;) {
		int r = coro_bus_try_broadcast(b, val);
		if (r == 0) return 0;
		enum coro_bus_error_code e = coro_bus_errno();
		if (e != CORO_BUS_ERR_WOULD_BLOCK) return -1;
		int any = 0;
		for (int i = 0; i < b->channel_count; i++) {
			if (b->channels[i]) {
				any = 1;
				break;
			}
		}
		if (!any) {
			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
			return -1;
		}
		/* no dedicated queue for broadcast here, so can't suspend properly */
		return -1;
	}
}

int
coro_bus_try_broadcast(struct coro_bus *b, unsigned val)
{
	int found = 0;
	int block = 0;
	for (int i = 0; i < b->channel_count; i++) {
		struct coro_bus_channel *c = b->channels[i];
		if (!c) continue;
		found = 1;
		if (c->data.size >= c->size_limit) block = 1;
	}
	if (!found) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	if (block) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}
	for (int i = 0; i < b->channel_count; i++) {
		struct coro_bus_channel *c = b->channels[i];
		if (c) {
			dv_append(&c->data, val);
			wq_wakeup_first(&c->recv_queue);
		}
	}
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *b, int chn, const unsigned *vals, unsigned cnt)
{
	unsigned s = 0;
	for (;;) {
		int r = coro_bus_try_send_v(b, chn, vals + s, cnt - s);
		if (r > 0) {
			s += r;
			if (s == cnt) return s;
			continue;
		}
		enum coro_bus_error_code e = coro_bus_errno();
		if (e == CORO_BUS_ERR_NO_CHANNEL) return -1;
		if (e == CORO_BUS_ERR_WOULD_BLOCK) {
			if (s > 0) return s;
			struct coro_bus_channel *c = (chn >= 0 && chn < b->channel_count)
				? b->channels[chn] : NULL;
			if (!c) {
				coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
				return -1;
			}
			wq_suspend(&c->send_queue);
			continue;
		}
		return -1;
	}
}

int
coro_bus_try_send_v(struct coro_bus *b, int chn, const unsigned *vals, unsigned cnt)
{
	if (chn < 0 || chn >= b->channel_count) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *c = b->channels[chn];
	if (!c) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	size_t space = c->size_limit - c->data.size;
	if (!space) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}
	size_t n = cnt < space ? cnt : space;
	dv_append_many(&c->data, vals, n);
	wq_wakeup_first(&c->recv_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return n;
}

int
coro_bus_recv_v(struct coro_bus *b, int chn, unsigned *dst, unsigned cap)
{
	unsigned got = 0;
	for (;;) {
		int r = coro_bus_try_recv_v(b, chn, dst + got, cap - got);
		if (r > 0) {
			got += r;
			return got;
		}
		enum coro_bus_error_code e = coro_bus_errno();
		if (e == CORO_BUS_ERR_NO_CHANNEL) return -1;
		if (e == CORO_BUS_ERR_WOULD_BLOCK) {
			struct coro_bus_channel *c = (chn >= 0 && chn < b->channel_count)
				? b->channels[chn] : NULL;
			if (!c) {
				coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
				return -1;
			}
			wq_suspend(&c->recv_queue);
			continue;
		}
		return -1;
	}
}

int
coro_bus_try_recv_v(struct coro_bus *b, int chn, unsigned *dst, unsigned cap)
{
	if (chn < 0 || chn >= b->channel_count) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *c = b->channels[chn];
	if (!c) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	if (!c->data.size) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}
	unsigned n = c->data.size;
	if (n > cap) n = cap;
	dv_pop_many(&c->data, dst, n);
	wq_wakeup_first(&c->send_queue);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return n;
}

#endif
