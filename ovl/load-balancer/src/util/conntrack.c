/*
  SPDX-License-Identifier: MIT License
  Copyright (c) 2021 Nordix Foundation
*/

#include "util.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define Dx(x) x
#define D(x)

#ifdef THREAD_SAFE
#include <pthread.h>
#define MUTEX(x) pthread_mutex_t x
#define LOCK(x) pthread_mutex_lock(x)
#define UNLOCK(x) pthread_mutex_unlock(x)
#define MUTEX_DESTROY(x) pthread_mutex_destroy(x)
#else
#define MUTEX(x)
#define LOCK(x)
#define UNLOCK(x)
#define MUTEX_DESTROY(x)
#endif

#define HASH djb2_hash
struct ctBucket {
	struct ctBucket* next;
	struct ctKey key;
	void* data;
	uint64_t refered;			/* Last time refered in nanoS */
	MUTEX(mutex);
};
struct ct {
	uint64_t ttl;
	ctFree freefn;
	struct ctStats stats;
	struct ctBucket* bucket;
	MUTEX(mutex);
};

#ifdef MEMDEBUG
#undef BUCKET_ALLOC
#undef BUCKET_FREE
long nAllocatedBuckets = 0;
static void* BUCKET_ALLOC(void) {
	nAllocatedBuckets++;
	return calloc(1,sizeof(struct ctBucket));
}
static void BUCKET_FREE(void* b) {
	nAllocatedBuckets--;
	free(b);
}
#endif

static int keyEqual(struct ctKey const* key1, struct ctKey const* key2)
{
	return memcmp(key1, key2, sizeof(struct ctKey));
}
static uint64_t toNanos(struct timespec* t)
{
	return t->tv_sec * 1000000000 + t->tv_nsec;
}
// Remove stale entries. Returns number of active buckets
// The bucket must be locked on call.
static ctCounter
bucketGC(struct ct* ct, struct ctBucket* b, uint64_t nowNanos)
{
	ctCounter count = 0;
	if (b->data != NULL && (nowNanos - b->refered) > ct->ttl) {
		if (ct->freefn != NULL)
			ct->freefn(b->data);
		b->data = NULL;
	}
	if (b->data != NULL)
		count++;

	struct ctBucket* prev = b;
	struct ctBucket* item = prev->next;
	while (item != NULL) {
		if ((nowNanos - item->refered) > ct->ttl) {
			prev->next = item->next;
			if (item->data != NULL && ct->freefn != NULL)
				ct->freefn(item->data);
			BUCKET_FREE(item);
		} else {
			prev = item;
			count++;
		}
		item = prev->next;
	}
	return count;
}
struct ct* ctCreate(ctCounter hsize, uint64_t ttlNanos, ctFree freefn)
{
	struct ct* ct = calloc(1, sizeof(*ct));
	ct->stats.size = hsize;
	ct->ttl = ttlNanos;
	ct->freefn = freefn;
	ct->bucket = calloc(hsize, sizeof(struct ctBucket));
#ifdef THREAD_SAFE
	pthread_mutex_init(&ct->mutex, NULL);
	for (ctCounter i = 0; i < hsize; i++) {
		pthread_mutex_init(&ct->bucket[i].mutex, NULL);
	}
#endif
	return ct;
}

// "ct" must be locked on call.
// The bucket is locked in the call.
static struct ctBucket* ctLookupBucket(
	struct ct* ct, struct timespec* now, struct ctKey const* key)
{
	uint32_t hash = HASH((uint8_t const*)key, sizeof(*key));
	struct ctBucket* b = ct->bucket + (hash % ct->stats.size);
	LOCK(&b->mutex);
	uint64_t nowNanos = toNanos(now);

	/* Is the main bucket stale? */
	if (b->data != NULL && (nowNanos - b->refered) > ct->ttl) {
		if (ct->freefn != NULL)
			ct->freefn(b->data);
		b->data = NULL;
	}
	if (b->next != NULL) {
		/*
		  We have had collisions and have allocated additional
		  buckets. This is assumed to be a very rare case under normal
		  circumstances and could be indicating a DoS attack.
		 */
		bucketGC(ct, b, nowNanos);
	}
	return b;
}
void* ctLookup(
	struct ct* ct, struct timespec* now, struct ctKey const* key)
{
	LOCK(&ct->mutex);
	struct ctBucket* B = ctLookupBucket(ct, now, key);
	ct->stats.lookups++;
	UNLOCK(&ct->mutex);
	struct ctBucket* b;
	for (b = B; b != NULL; b = b->next) {
		if (keyEqual(key, &b->key) == 0) {
			b->refered = toNanos(now);
			UNLOCK(&B->mutex);
			return b->data;		/* Found! */
		}
	}
	UNLOCK(&B->mutex);
	return NULL;				/* Not found */
}
int ctInsert(
	struct ct* ct, struct timespec* now, struct ctKey const* key, void* data)
{
	LOCK(&ct->mutex);
	struct ctBucket* b = ctLookupBucket(ct, now, key);
	ct->stats.inserts++;
	UNLOCK(&ct->mutex);

	// Check if the entry already exists
	struct ctBucket* item;
	for (item = b; item != NULL; item = item->next) {
		if (item->data == NULL)
			continue;
		if (keyEqual(key, &item->key) == 0) {
			item->refered = toNanos(now);
			item->data = data;
			UNLOCK(&b->mutex);
			return 1;				/* Existing item updated */
		}
	}

	if (b->data == NULL) {
		// The main bucket is free
		b->data = data;
		b->key = *key;
		b->refered = toNanos(now);
		if (b->next != NULL)
			ct->stats.collisions++;
		UNLOCK(&b->mutex);
		return 0;
	}

	// We must allocate a new bucket
	ct->stats.collisions++;
	struct ctBucket* x = BUCKET_ALLOC();
	if (x == NULL) {
		UNLOCK(&b->mutex);
		return -1;
	}
	x->data = data;
	x->key = *key;
	x->refered = toNanos(now);
	x->next = b->next;
	b->next = x;
	UNLOCK(&b->mutex);
	return 0;
}

void ctRemove(
	struct ct* ct, struct timespec* now, struct ctKey const* key)
{
	LOCK(&ct->mutex);
	struct ctBucket* b = ctLookupBucket(ct, now, key);
	UNLOCK(&ct->mutex);
	if (keyEqual(key, &b->key) == 0) {
		if (b->data != NULL && ct->freefn != NULL)
			ct->freefn(b->data);
		b->data = NULL;
		UNLOCK(&b->mutex);
		return;
	}
	struct ctBucket* prev = b;
	struct ctBucket* item = prev->next;
	while (item != NULL) {
		if (keyEqual(key, &item->key) == 0) {
			prev->next = item->next;
			if (b->data != NULL && ct->freefn != NULL)
				ct->freefn(b->data);
			BUCKET_FREE(item);
		} else {
			prev = item;
		}
		item = prev->next;
	}
	UNLOCK(&b->mutex);
}

// This function will lock and scan the entire hash table and should be used for
// debug and test only.
struct ctStats const* ctStats(struct ct* ct, struct timespec* now)
{
	ctCounter active = 0;
	uint64_t nowNanos = toNanos(now);
	unsigned i;
	LOCK(&ct->mutex);
	struct ctBucket* b = ct->bucket;
	for (i = 0; i < ct->stats.size; i++, b++) {
		LOCK(&b->mutex);
		active += bucketGC(ct, b, nowNanos);
		UNLOCK(&b->mutex);
	}
	ct->stats.active = active;
	UNLOCK(&ct->mutex);
	return &ct->stats;
}
void ctDestroy(struct ct* ct)
{
	unsigned i;
	LOCK(&ct->mutex);
	struct ctBucket* b = ct->bucket;
	for (i = 0; i < ct->stats.size; i++, b++) {
		LOCK(&b->mutex);
		bucketGC(ct, b, UINT64_MAX);
		UNLOCK(&b->mutex);
		MUTEX_DESTROY(&b->mutex);
	}
	free(ct->bucket);
	UNLOCK(&ct->mutex);
	MUTEX_DESTROY(&ct->mutex);
	free(ct);
}
