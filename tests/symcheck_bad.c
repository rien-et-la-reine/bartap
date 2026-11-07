/* Deliberately breaks R34, to prove check_symbols.py catches a forbidden call. Never linked. */
#include <string.h>

size_t symcheck_bad(const char *s)
{
    return strlen(s);
}
