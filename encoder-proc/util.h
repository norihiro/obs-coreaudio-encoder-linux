#pragma once

struct encoder_frame
{
	uint8_t *data[1];
	uint32_t linesize[1];
	uint32_t frames;
	int64_t pts;
};

inline void dstr_vcatf(struct dstr *dst, const char *format, va_list args)
{
	va_list args_cp;
	va_copy(args_cp, args);

	int len = vsnprintf(NULL, 0, format, args_cp);
	va_end(args_cp);

	if (len < 0)
		len = 4095;

	dstr_ensure_capacity(dst, dst->len + ((size_t)len) + 1);
	len = vsnprintf(dst->array + dst->len, ((size_t)len) + 1, format, args);

	if (!*dst->array) {
		dstr_free(dst);
		return;
	}

	dst->len += len < 0 ? strlen(dst->array + dst->len) : (size_t)len;
}

inline void dstr_vprintf(struct dstr *dst, const char *format, va_list args)
{
	va_list args_cp;
	va_copy(args_cp, args);

	int len = vsnprintf(NULL, 0, format, args_cp);
	va_end(args_cp);

	if (len < 0)
		len = 4095;

	dstr_ensure_capacity(dst, ((size_t)len) + 1);
	len = vsnprintf(dst->array, ((size_t)len) + 1, format, args);

	if (!*dst->array) {
		dstr_free(dst);
		return;
	}

	dst->len = len < 0 ? strlen(dst->array) : (size_t)len;
}

inline void dstr_printf(struct dstr *dst, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	dstr_vprintf(dst, format, args);
	va_end(args);
}
