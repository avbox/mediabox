#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <libavbox/avbox.h>


#define TEST_ASSERT(expr) \
	do { if (!(expr)) { abort(); } } while(0)


void
test_queue()
{
	struct avbox_queue*const q = avbox_queue_new(100);
	TEST_ASSERT(q != NULL);

	avbox_queue_put(q, (void*)1);
	avbox_queue_put(q, (void*)2);
	avbox_queue_put(q, (void*)3);
	TEST_ASSERT(avbox_queue_count(q) == 3);

	TEST_ASSERT((intptr_t)avbox_queue_get(q) == 1);
	TEST_ASSERT((intptr_t)avbox_queue_get(q) == 2);
	TEST_ASSERT((intptr_t)avbox_queue_get(q) == 3);

	/* cleanup */
	avbox_queue_destroy(q);
}


int
main()
{
	test_queue();
	return 0;
}
