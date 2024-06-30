#include <stdlib.h>
#include <util/bmem.h>

void *bmalloc(size_t size)
{
	return malloc(size);
}

void *brealloc(void *ptr, size_t size)
{
	return realloc(ptr, size);
}

void bfree(void *ptr)
{
	free(ptr);
}
