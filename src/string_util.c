#include <stdlib.h>
#include <string.h>
#include <ctype.h>


/**
 * strisdigit() -- Like isdigit() but works on strings.
 */
int
strisdigit(const char *str)
{
	while (*str != '\0') {
		if (!isdigit(*str++)) {
			return 0;
		}
	}
	return 1;
}

