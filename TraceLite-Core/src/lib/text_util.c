#include "text_util.h"

#include <errno.h>
#include <string.h>

int tl_copy_text(char *dst, size_t dst_size, const char *src)
{
    size_t length;

    if (dst == NULL || dst_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (src == NULL) {
        src = "";
    }

    length = strlen(src);
    if (length >= dst_size) {
        length = dst_size - 1;
    }

    memcpy(dst, src, length);
    dst[length] = '\0';
    return 0;
}
