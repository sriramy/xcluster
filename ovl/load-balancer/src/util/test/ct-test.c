// gcc -o /tmp/$USER/ct-test -I. -I.. test/ct-test.c conntrack.c hash.c -lrt && /tmp/$USER/ct-test


#include <netinet/in.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "conntrack.h"

static long nAllocatedBuckets = 0;
static void* BUCKET_ALLOC(void) {
	nAllocatedBuckets++;
	return calloc(1,sizeof_bucket);
}
static void BUCKET_FREE(void* b) {
	nAllocatedBuckets--;
	free(b);
}



#define Dx(x) x
#define D(x)

void testConntrack(struct ctStats* stats);

int
main(int argc, char* argv[])
{
	struct ctStats stats = {0};
	testConntrack(&stats);
	printf(
		"Test OK. inserts=%u lookups=%u collisions=%u\n",
		stats.inserts, stats.lookups, stats.collisions);
	return 0;
}

static long nFreeData = 0;
static uint64_t expectedFreeData = 0;
static void freeData(void* data) {
	D(printf("Free data; %lu\n", (uint64_t)data));
	nFreeData++;
	if (expectedFreeData != 0) {
		if ((uint64_t)data != expectedFreeData)
			printf(
				"Free data = %lu, expected = %lu\n",
				(uint64_t)data, expectedFreeData);
		assert((uint64_t)data == expectedFreeData);
	}
}

static void* collectStats(
	struct ctStats* accumulatedStats, struct ctStats const* stats)
{
	accumulatedStats->collisions += stats->collisions;
	accumulatedStats->inserts += stats->inserts;
	accumulatedStats->lookups += stats->lookups;
}

void testConntrack(struct ctStats* accumulatedStats)
{
	struct ct* ct = ctCreate(1, 99, freeData, NULL, BUCKET_ALLOC, BUCKET_FREE);
	struct timespec now = {0,0};
	struct ctKey key = {IN6ADDR_ANY_INIT,IN6ADDR_ANY_INIT,0ull};
	void* data;
	int rc;

	// Insert an empty key
	data = ctLookup(ct, &now, &key);
	assert(data == NULL);
	rc = ctInsert(ct, &now, &key, (void*)1001);
	assert(rc == 0);
	assert(nAllocatedBuckets == 0);
	data = ctLookup(ct, &now, &key);
	assert(data == (void*)1001);
	assert(ctStats(ct, &now)->active == 1);
	assert(nFreeData == 0);

	// Insert the same key again.
	nFreeData = 0;
	rc = ctInsert(ct, &now, &key, (void*)1002);
	assert(rc == 1);
	assert(nAllocatedBuckets == 0);
	assert(nFreeData == 0);
	data = ctLookup(ct, &now, &key);
	assert(data == (void*)1001);
	assert(ctStats(ct, &now)->active == 1);
	
	// The existing item should expire
	nFreeData = 0;
	expectedFreeData = 1001;
	now.tv_nsec += 100;
	rc = ctInsert(ct, &now, &key, (void*)1003);
	assert(rc == 0);
	assert(nFreeData == 1);
	assert(nAllocatedBuckets == 0);
	assert(ctStats(ct, &now)->active == 1);
	expectedFreeData = 0;

	// Cause a collision
	nFreeData = 0;
	key.id++;
	rc = ctInsert(ct, &now, &key, (void*)1004);
	assert(rc == 0);
	assert(nFreeData == 0);
	assert(nAllocatedBuckets == 1);
	assert(ctStats(ct, &now)->active == 2);
	assert(ctStats(ct, &now)->collisions == 1);

	// Insert a new item after some time
	nFreeData = 0;
	key.id++;
	now.tv_nsec += 50;
	rc = ctInsert(ct, &now, &key, (void*)1005);
	assert(rc == 0);
	assert(nFreeData == 0);
	assert(nAllocatedBuckets == 2);
	assert(ctStats(ct, &now)->active == 3);
	assert(ctStats(ct, &now)->collisions == 2);

	// Let the first 2 items expire then lookup the remaining
	nFreeData = 0;
	now.tv_nsec += 50;
	data = ctLookup(ct, &now, &key);
	assert(data == (void*)1005);
	assert(nAllocatedBuckets == 1);
	assert(nFreeData == 2);
	assert(ctStats(ct, &now)->active == 1);
	assert(ctStats(ct, &now)->collisions == 2);

	// The main bucket should be free. Insert and check nAllocatedBuckets
	nFreeData = 0;
	key.id++;
	rc = ctInsert(ct, &now, &key, (void*)1006);
	assert(rc == 0);
	data = ctLookup(ct, &now, &key);
	assert(data == (void*)1006);
	assert(nAllocatedBuckets == 1);
	assert(nFreeData == 0);
	assert(ctStats(ct, &now)->active == 2);
	assert(ctStats(ct, &now)->collisions == 3);

	// Remove the item in the "main" bucket
	nFreeData = 0;
	expectedFreeData = 1006;
	ctRemove(ct, &now, &key);
	assert(nFreeData == 1);
	assert(nAllocatedBuckets == 1);	
	data = ctLookup(ct, &now, &key);
	assert(data == NULL);
	assert(ctStats(ct, &now)->active == 1);
	assert(ctStats(ct, &now)->collisions == 3);

	// Destroy the table. Remaining items shall be freed
	nFreeData = 0;
	expectedFreeData = 0;
	collectStats(accumulatedStats, ctStats(ct, &now));
	ctDestroy(ct);
	assert(nFreeData == 1);
	assert(nAllocatedBuckets == 0);	

	// Test with a larger table
	ct = ctCreate(1000, 1000, freeData, NULL, BUCKET_ALLOC, BUCKET_FREE);
	now.tv_nsec = 0;
	key.id = 0;
	nFreeData = 0;
	for (int i = 0; i < 1000; i++) {
		rc = ctInsert(ct, &now, &key, (void*)(key.id+1)); /* Don't use NULL! */
		assert(rc == 0);
		now.tv_nsec++;
		key.id++;
	}
	assert(nFreeData == 0);
	D(printf("Now = %lu, Active = %u\n",now.tv_nsec,ctStats(ct,&now)->active));
	assert(ctStats(ct, &now)->active == 1000);
	D(printf(
		  "allocated=%ld, collisions=%u\n",
		  nAllocatedBuckets, ctStats(ct, &now)->collisions));
	// NOTE; nAllocatedBuckets will change if a better hash function is used!!
	// (and BTW 766 is quite lousy)
	assert(nAllocatedBuckets == 766);
	assert(ctStats(ct, &now)->collisions == nAllocatedBuckets);
	now.tv_nsec += 500;
	D(printf("Now = %lu, Active = %u\n",now.tv_nsec,ctStats(ct,&now)->active));
	assert(ctStats(ct, &now)->active == 500);
	assert(nFreeData == 500);
	D(printf("allocated=%ld\n", nAllocatedBuckets));
	assert(nAllocatedBuckets == 384);
	collectStats(accumulatedStats, ctStats(ct, &now));
	ctDestroy(ct);
	assert(nFreeData == 1000);
}
