#include <stdlib.h>
#include <util/bmem.h>

#define ALIGNMENT 32

void *bmalloc(size_t size)
{
#ifdef _WIN32
	return _aligned_malloc(size, ALIGNMENT);
#else
	// TODO: align
	return malloc(size);
#endif
}

void *brealloc(void *ptr, size_t size)
{
#ifdef _WIN32
	return _aligned_realloc(ptr, size, ALIGNMENT);
#else
	return realloc(ptr, size);
#endif
}

void bfree(void *ptr)
{
#ifdef _WIN32
	_aligned_free(ptr);
#else
	free(ptr);
#endif
}
