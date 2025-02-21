// SPDX-License-Identifier: GPL-2.0
#include <perf/evlist.h>
#include <perf/evsel.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <sys/ioctl.h>
#include <internal/evlist.h>
#include <internal/evsel.h>
#include <internal/xyarray.h>
#include <internal/mmap.h>
#include <internal/cpumap.h>
#include <internal/threadmap.h>
#include <internal/lib.h>
#include <linux/zalloc.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <perf/cpumap.h>
#include <perf/threadmap.h>

void perf_evlist__init(struct perf_evlist *evlist)
{
	INIT_LIST_HEAD(&evlist->entries);
	evlist->nr_entries = 0;
	perf_evlist_poll__init(evlist);
	perf_evlist__reset_id_hash(evlist);
}

static void __perf_evlist__propagate_maps(struct perf_evlist *evlist,
					  struct perf_evsel *evsel)
{
	/*
	 * We already have cpus for evsel (via PMU sysfs) so
	 * keep it, if there's no target cpu list defined.
	 */
	if (!evsel->own_cpus || evlist->has_user_cpus) {
		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = perf_cpu_map__get(evlist->cpus);
	} else if (!evsel->system_wide && perf_cpu_map__empty(evlist->cpus)) {
		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = perf_cpu_map__get(evlist->cpus);
	} else if (evsel->cpus != evsel->own_cpus) {
		perf_cpu_map__put(evsel->cpus);
		evsel->cpus = perf_cpu_map__get(evsel->own_cpus);
	}

	perf_thread_map__put(evsel->threads);
	evsel->threads = perf_thread_map__get(evlist->threads);
	evlist->all_cpus = perf_cpu_map__merge(evlist->all_cpus, evsel->cpus);
}

static void perf_evlist__propagate_maps(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_evsel(evlist, evsel)
		__perf_evlist__propagate_maps(evlist, evsel);
}

void perf_evlist__add(struct perf_evlist *evlist,
		      struct perf_evsel *evsel)
{
	evsel->idx = evlist->nr_entries;
	list_add_tail(&evsel->node, &evlist->entries);
	evlist->nr_entries += 1;
	__perf_evlist__propagate_maps(evlist, evsel);
}

void perf_evlist__remove(struct perf_evlist *evlist,
			 struct perf_evsel *evsel)
{
	list_del_init(&evsel->node);
	evlist->nr_entries -= 1;
}

struct perf_evlist *perf_evlist__new(void)
{
	struct perf_evlist *evlist = zalloc(sizeof(*evlist));

	if (evlist != NULL)
		perf_evlist__init(evlist);

	return evlist;
}

struct perf_evsel *
perf_evlist__next(struct perf_evlist *evlist, struct perf_evsel *prev)
{
	struct perf_evsel *next;

	if (!prev) {
		next = list_first_entry(&evlist->entries,
					struct perf_evsel,
					node);
	} else {
		next = list_next_entry(prev, node);
	}

	/* Empty list is noticed here so don't need checking on entry. */
	if (&next->node == &evlist->entries)
		return NULL;

	return next;
}

static void perf_evlist__purge(struct perf_evlist *evlist)
{
	struct perf_evsel *pos, *n;

	perf_evlist__for_each_entry_safe(evlist, n, pos) {
		list_del_init(&pos->node);
		perf_evsel__delete(pos);
	}

	evlist->nr_entries = 0;
}

void perf_evlist__exit(struct perf_evlist *evlist)
{
	perf_cpu_map__put(evlist->cpus);
	perf_cpu_map__put(evlist->all_cpus);
	perf_thread_map__put(evlist->threads);
	evlist->cpus = NULL;
	evlist->all_cpus = NULL;
	evlist->threads = NULL;
}

void perf_evlist__delete(struct perf_evlist *evlist)
{
	if (evlist == NULL)
		return;

	perf_evlist__munmap(evlist);
	perf_evlist__close(evlist);
	perf_evlist__purge(evlist);
	perf_evlist__exit(evlist);
	free(evlist);
}

void perf_evlist__set_maps(struct perf_evlist *evlist,
			   struct perf_cpu_map *cpus,
			   struct perf_thread_map *threads)
{
	/*
	 * Allow for the possibility that one or another of the maps isn't being
	 * changed i.e. don't put it.  Note we are assuming the maps that are
	 * being applied are brand new and evlist is taking ownership of the
	 * original reference count of 1.  If that is not the case it is up to
	 * the caller to increase the reference count.
	 */
	if (cpus != evlist->cpus) {
		perf_cpu_map__put(evlist->cpus);
		evlist->cpus = perf_cpu_map__get(cpus);
	}

	if (threads != evlist->threads) {
		perf_thread_map__put(evlist->threads);
		evlist->threads = perf_thread_map__get(threads);
	}

	if (!evlist->all_cpus && cpus)
		evlist->all_cpus = perf_cpu_map__get(cpus);

	perf_evlist__propagate_maps(evlist);
}

int perf_evlist__open(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int err;

	perf_evlist__for_each_entry(evlist, evsel) {
		err = perf_evsel__open(evsel, evsel->cpus, evsel->threads);
		if (err < 0)
			goto out_err;
	}

	return 0;

out_err:
	perf_evlist__close(evlist);
	return err;
}

void perf_evlist__close(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_entry_reverse(evlist, evsel)
		perf_evsel__close(evsel);
}

void perf_evlist__enable(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_entry(evlist, evsel)
		perf_evsel__enable_group(evsel);
}

void perf_evlist__disable(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	perf_evlist__for_each_entry(evlist, evsel)
		perf_evsel__disable_group(evsel);
}

u64 perf_evlist__read_format(struct perf_evlist *evlist)
{
	struct perf_evsel *first = perf_evlist__first(evlist);

	return first->attr.read_format;
}

#define SID(e, x, y) xyarray__entry(e->sample_id, x, y)

static void perf_evlist__id_hash(struct perf_evlist *evlist,
				 struct perf_evsel *evsel,
				 int cpu, int thread, u64 id)
{
	int hash;
	struct perf_sample_id *sid = SID(evsel, cpu, thread);

	sid->id = id;
	sid->evsel = evsel;
	hash = hash_64(sid->id, PERF_EVLIST__HLIST_BITS);
	hlist_add_head(&sid->node, &evlist->heads[hash]);
}

void perf_evlist__reset_id_hash(struct perf_evlist *evlist)
{
	int i;

	for (i = 0; i < PERF_EVLIST__HLIST_SIZE; ++i)
		INIT_HLIST_HEAD(&evlist->heads[i]);
}

void perf_evlist__id_add(struct perf_evlist *evlist,
			 struct perf_evsel *evsel,
			 int cpu, int thread, u64 id)
{
	perf_evlist__id_hash(evlist, evsel, cpu, thread, id);
	evsel->id[evsel->ids++] = id;
}

int perf_evlist__id_add_fd(struct perf_evlist *evlist,
			   struct perf_evsel *evsel,
			   int cpu, int thread, int fd)
{
	u64 read_data[4] = { 0, };
	int id_idx = 1; /* The first entry is the counter value */
	u64 id;
	int ret;

	ret = ioctl(fd, PERF_EVENT_IOC_ID, &id);
	if (!ret)
		goto add;

	if (errno != ENOTTY)
		return -1;

	/* Legacy way to get event id.. All hail to old kernels! */

	/*
	 * This way does not work with group format read, so bail
	 * out in that case.
	 */
	if (perf_evlist__read_format(evlist) & PERF_FORMAT_GROUP)
		return -1;

	if (!(evsel->attr.read_format & PERF_FORMAT_ID) ||
	    read(fd, &read_data, sizeof(read_data)) == -1)
		return -1;

	if (evsel->attr.read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
		++id_idx;
	if (evsel->attr.read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
		++id_idx;

	id = read_data[id_idx];

add:
	perf_evlist__id_add(evlist, evsel, cpu, thread, id);
	return 0;
}

struct perf_evsel *perf_evlist__id_to_evsel(struct perf_evlist *evlist,
                   uint64_t id, int *pcpu)
{
    int hash;
    struct perf_sample_id *sid;

	hash = hash_64(id, PERF_EVLIST__HLIST_BITS);
    hlist_for_each_entry(sid, &evlist->heads[hash], node) {
        if (sid->id == id) {
            if (pcpu)
                *pcpu = sid->cpu;
            return sid->evsel;
        }
    }
    return NULL;
}

void perf_evlist_poll__init(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	epoll->epfd = -1;
	epoll->maxevents = 0;
	epoll->events = NULL;
	epoll->external = false;
	epoll->nr = 0;
	epoll->nr_alloc = 0;
	epoll->data = NULL;
}

void perf_evlist_poll__external(struct perf_evlist *evlist, bool external)
{
	evlist->epoll.external = external;
}

int perf_evlist_poll__alloc(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	struct perf_cpu_map *cpus = evlist->cpus;
	struct perf_thread_map *threads = evlist->threads;

	if (!epoll->external) {
		epoll->epfd = epoll_create1(EPOLL_CLOEXEC);
		if (epoll->epfd < 0)
			return -errno;

		epoll->maxevents = 64;
		epoll->events = zalloc(epoll->maxevents * sizeof(*epoll->events));
		if (epoll->events == NULL)
			return -ENOMEM;
	}
	epoll->nr = 0;
	epoll->nr_alloc = evlist->nr_entries * perf_cpu_map__nr(cpus) * perf_thread_map__nr(threads);
	epoll->data = zalloc(epoll->nr_alloc * sizeof(*epoll->data));
	if (epoll->data == NULL)
		return -ENOMEM;

	return 0;
}

void perf_evlist_poll__free(struct perf_evlist *evlist)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	if (epoll->events)
		free(epoll->events);
	if (epoll->epfd >= 0)
		close(epoll->epfd);
	if (epoll->data)
		free(epoll->data);
	perf_evlist_poll__init(evlist);
}

int perf_evlist_poll__add(struct perf_evlist *evlist, int fd,
				struct perf_mmap *mmap, unsigned revent)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	struct epoll_event event;

	if (epoll->nr == epoll->nr_alloc)
		return -ENOMEM;

	epoll->data[epoll->nr].fd = fd;
	epoll->data[epoll->nr].events = revent | EPOLLERR | EPOLLHUP | EPOLLET;
	epoll->data[epoll->nr].mmap = mmap;

	if (!epoll->external) {
		event.events = epoll->data[epoll->nr].events;
		event.data.u32 = epoll->nr;
		if (epoll_ctl(epoll->epfd, EPOLL_CTL_ADD, fd, &event) < 0)
			return -errno;
	}
	epoll->nr ++;
	return 0;
}

int perf_evlist_poll__del(struct perf_evlist *evlist, int n)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;

	if (epoll->data[n].mmap) {
		perf_mmap__put(epoll->data[n].mmap);
		epoll->data[n].mmap = NULL;
		if (!epoll->external) {
			if (epoll_ctl(epoll->epfd, EPOLL_CTL_DEL, epoll->data[n].fd, NULL) < 0)
				return -errno;
		}
		epoll->nr --;
	}
	return 0;
}

int perf_evlist_poll__foreach_fd(struct perf_evlist *evlist, foreach_fd fn)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i, err;

	if (!epoll->external)
		return -EINVAL;

	for (i = 0; i < epoll->nr; i++) {
		err = fn(epoll->data[i].fd, epoll->data[i].events, epoll->data[i].mmap);
		if (err)
			return err;
	}
	return 0;
}

int perf_evlist__poll_mmap(struct perf_evlist *evlist, int timeout, handle_mmap handle)
{
	struct perf_evlist_poll *epoll = &evlist->epoll;
	int i, cnt;

	if (epoll->external)
		return -EINVAL;

	if (epoll->epfd < 0) {
		return usleep(timeout * 1000);
	}

	cnt = epoll_wait(epoll->epfd, epoll->events, epoll->maxevents, timeout);
	if (cnt < 0)
		return -errno;

	for (i = 0; i < cnt; i++) {
		unsigned int revents = epoll->events[i].events;
		int n = epoll->events[i].data.u32;
		if (handle)
			handle(epoll->data[n].mmap);
		if (revents & EPOLLHUP)
			perf_evlist_poll__del(evlist, n);
	}
	return epoll->nr ? cnt : -ENOENT;
}

int perf_evlist__poll(struct perf_evlist *evlist, int timeout)
{
	return perf_evlist__poll_mmap(evlist, timeout, NULL);
}

static struct perf_mmap* perf_evlist__alloc_mmap(struct perf_evlist *evlist, bool overwrite)
{
	int i;
	struct perf_mmap *map;

	map = zalloc(evlist->nr_mmaps * sizeof(struct perf_mmap));
	if (!map)
		return NULL;

	for (i = 0; i < evlist->nr_mmaps; i++) {
		struct perf_mmap *prev = i ? &map[i - 1] : NULL;

		/*
		 * When the perf_mmap() call is made we grab one refcount, plus
		 * one extra to let perf_mmap__consume() get the last
		 * events after all real references (perf_mmap__get()) are
		 * dropped.
		 *
		 * Each PERF_EVENT_IOC_SET_OUTPUT points to this mmap and
		 * thus does perf_mmap__get() on it.
		 */
		perf_mmap__init(&map[i], prev, overwrite, NULL);
		map[i].idx = i;
	}

	return map;
}

static void perf_evsel__set_sid_idx(struct perf_evsel *evsel, int idx, int cpu, int thread)
{
	struct perf_sample_id *sid = SID(evsel, cpu, thread);

	sid->idx = idx;
	sid->cpu = perf_cpu_map__cpu(evsel->cpus, cpu);
	sid->tid = perf_thread_map__pid(evsel->threads, thread);
}

static struct perf_mmap*
perf_evlist__mmap_cb_get(struct perf_evlist *evlist, bool overwrite, int idx)
{
	struct perf_mmap *maps;

	maps = overwrite ? evlist->mmap_ovw : evlist->mmap;

	if (!maps) {
		maps = perf_evlist__alloc_mmap(evlist, overwrite);
		if (!maps)
			return NULL;

		if (overwrite)
			evlist->mmap_ovw = maps;
		else
			evlist->mmap = maps;
	}

	return &maps[idx];
}

#define FD(e, x, y) (*(int *) xyarray__entry(e->fd, x, y))

static int
perf_evlist__mmap_cb_mmap(struct perf_mmap *map, struct perf_mmap_param *mp,
			  int output, int cpu)
{
	return perf_mmap__mmap(map, mp, output, cpu);
}

static void perf_evlist__set_mmap_first(struct perf_evlist *evlist, struct perf_mmap *map,
					bool overwrite)
{
	if (overwrite)
		evlist->mmap_ovw_first = map;
	else
		evlist->mmap_first = map;
}

static int
mmap_per_evsel(struct perf_evlist *evlist, struct perf_evlist_mmap_ops *ops,
	       int idx, struct perf_mmap_param *mp, int cpu_idx,
	       int thread, int *_output, int *_output_overwrite)
{
	int evlist_cpu = perf_cpu_map__cpu(evlist->cpus, cpu_idx);
	struct perf_evsel *evsel;
	unsigned revent;

	perf_evlist__for_each_entry(evlist, evsel) {
		bool overwrite = evsel->attr.write_backward;
		struct perf_mmap *map;
		int *output, fd, cpu;

		if (evsel->system_wide && thread)
			continue;

		cpu = perf_cpu_map__idx(evsel->cpus, evlist_cpu);
		if (cpu == -1)
			continue;

		map = ops->get(evlist, overwrite, idx);
		if (map == NULL)
			return -ENOMEM;

		if (overwrite) {
			mp->prot = PROT_READ;
			output   = _output_overwrite;
		} else {
			mp->prot = PROT_READ | PROT_WRITE;
			output   = _output;
		}

		fd = FD(evsel, cpu, thread);

		if (*output == -1) {
			*output = fd;

			/*
			 * The last one will be done at perf_mmap__consume(), so that we
			 * make sure we don't prevent tools from consuming every last event in
			 * the ring buffer.
			 *
			 * I.e. we can get the POLLHUP meaning that the fd doesn't exist
			 * anymore, but the last events for it are still in the ring buffer,
			 * waiting to be consumed.
			 */
			refcount_set(&map->refcnt, 2);

			if (ops->mmap(map, mp, *output, evlist_cpu) < 0)
				return -1;

			if (!idx)
				perf_evlist__set_mmap_first(evlist, map, overwrite);
		} else {
			if (ioctl(fd, PERF_EVENT_IOC_SET_OUTPUT, *output) != 0)
				return -1;

			perf_mmap__get(map);
		}

		revent = !overwrite ? EPOLLIN : 0;

		if (!evsel->system_wide &&
		    perf_evlist_poll__add(evlist, fd, map, revent) < 0) {
			perf_mmap__put(map);
			return -1;
		}

		if (evsel->attr.read_format & PERF_FORMAT_ID) {
			if (perf_evlist__id_add_fd(evlist, evsel, cpu, thread,
						   fd) < 0)
				return -1;
			perf_evsel__set_sid_idx(evsel, idx, cpu, thread);
		}
	}

	return 0;
}

static int
mmap_per_thread(struct perf_evlist *evlist, struct perf_evlist_mmap_ops *ops,
		struct perf_mmap_param *mp)
{
	int thread;
	int nr_threads = perf_thread_map__nr(evlist->threads);

	for (thread = 0; thread < nr_threads; thread++) {
		int output = -1;
		int output_overwrite = -1;

		if (ops->idx)
			ops->idx(evlist, mp, thread, false);

		if (mmap_per_evsel(evlist, ops, thread, mp, 0, thread,
				   &output, &output_overwrite))
			goto out_unmap;
	}

	return 0;

out_unmap:
	perf_evlist__munmap(evlist);
	return -1;
}

static int
mmap_per_cpu(struct perf_evlist *evlist, struct perf_evlist_mmap_ops *ops,
	     struct perf_mmap_param *mp)
{
	int nr_threads = perf_thread_map__nr(evlist->threads);
	int nr_cpus    = perf_cpu_map__nr(evlist->cpus);
	int cpu, thread;

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		int output = -1;
		int output_overwrite = -1;

		if (ops->idx)
			ops->idx(evlist, mp, cpu, true);

		for (thread = 0; thread < nr_threads; thread++) {
			if (mmap_per_evsel(evlist, ops, cpu, mp, cpu,
					   thread, &output, &output_overwrite))
				goto out_unmap;
		}
	}

	return 0;

out_unmap:
	perf_evlist__munmap(evlist);
	return -1;
}

static int perf_evlist__nr_mmaps(struct perf_evlist *evlist)
{
	int nr_mmaps;

	nr_mmaps = perf_cpu_map__nr(evlist->cpus);
	if (perf_cpu_map__empty(evlist->cpus))
		nr_mmaps = perf_thread_map__nr(evlist->threads);

	return nr_mmaps;
}

int perf_evlist__mmap_ops(struct perf_evlist *evlist,
			  struct perf_evlist_mmap_ops *ops,
			  struct perf_mmap_param *mp)
{
	struct perf_evsel *evsel;
	struct perf_cpu_map *cpus = evlist->cpus;
	struct perf_thread_map *threads = evlist->threads;

	if (!ops || !ops->get || !ops->mmap)
		return -EINVAL;

	mp->mask = evlist->mmap_len - page_size - 1;

	evlist->nr_mmaps = perf_evlist__nr_mmaps(evlist);

	perf_evlist__for_each_entry(evlist, evsel) {
		if ((evsel->attr.read_format & PERF_FORMAT_ID) &&
		    evsel->sample_id == NULL &&
		    perf_evsel__alloc_id(evsel, perf_cpu_map__nr(cpus), perf_thread_map__nr(threads)) < 0)
			return -ENOMEM;
	}

	if (evlist->epoll.epfd == -1 && perf_evlist_poll__alloc(evlist) < 0)
		return -ENOMEM;

	if (perf_cpu_map__empty(cpus))
		return mmap_per_thread(evlist, ops, mp);

	return mmap_per_cpu(evlist, ops, mp);
}

int perf_evlist__mmap(struct perf_evlist *evlist, int pages)
{
	struct perf_mmap_param mp;
	struct perf_evlist_mmap_ops ops = {
		.get  = perf_evlist__mmap_cb_get,
		.mmap = perf_evlist__mmap_cb_mmap,
	};

	evlist->mmap_len = (pages + 1) * page_size;

	return perf_evlist__mmap_ops(evlist, &ops, &mp);
}

void perf_evlist__munmap(struct perf_evlist *evlist)
{
    struct perf_evsel *evsel;
	int i;

	if (evlist->epoll.nr) {
		for (i = 0; i < evlist->epoll.nr_alloc; i++)
			perf_evlist_poll__del(evlist, i);
	}
	perf_evlist_poll__free(evlist);

	if (evlist->mmap) {
		for (i = 0; i < evlist->nr_mmaps; i++)
			perf_mmap__munmap(&evlist->mmap[i]);
	}

	if (evlist->mmap_ovw) {
		for (i = 0; i < evlist->nr_mmaps; i++)
			perf_mmap__munmap(&evlist->mmap_ovw[i]);
	}

	zfree(&evlist->mmap);
	zfree(&evlist->mmap_ovw);

    perf_evlist__reset_id_hash(evlist);

    perf_evlist__for_each_entry(evlist, evsel) {
		if (evsel->attr.read_format & PERF_FORMAT_ID)
		    perf_evsel__free_id(evsel);
	}
    
}

struct perf_mmap*
perf_evlist__next_mmap(struct perf_evlist *evlist, struct perf_mmap *map,
		       bool overwrite)
{
	if (map)
		return map->next;

	return overwrite ? evlist->mmap_ovw_first : evlist->mmap_first;
}

void __perf_evlist__set_leader(struct list_head *list)
{
	struct perf_evsel *evsel, *leader;

	leader = list_entry(list->next, struct perf_evsel, node);
	evsel = list_entry(list->prev, struct perf_evsel, node);

	leader->nr_members = evsel->idx - leader->idx + 1;

	__perf_evlist__for_each_entry(list, evsel)
		evsel->leader = leader;
}

void perf_evlist__set_leader(struct perf_evlist *evlist)
{
	if (evlist->nr_entries) {
		evlist->nr_groups = evlist->nr_entries > 1 ? 1 : 0;
		__perf_evlist__set_leader(&evlist->entries);
	}
}

int perf_evlist__max_read_size(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;
	int max_size = sizeof(struct perf_counts_values);
	int read_size;

	perf_evlist__for_each_evsel(evlist, evsel) {
		read_size = perf_evsel__read_size(evsel);
		if (read_size > max_size)
			max_size = read_size;
	}
	return max_size;
}
