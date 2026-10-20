/* One request through posix_https_request (port/linux/src/posix_update.c), for tools/test_delta.py: prints the
   status, the error and the body.
   Usage: delta_transport_check URL [FORM [RESPONSE_SIZE [TIMEOUT_MILLISECONDS]]] ("-" for no form) */
#include "posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int count, char **arguments)
{
	const char *form = count > 2 && strcmp(arguments[2], "-") ? arguments[2] : NULL;
	int response_size = count > 3 ? atoi(arguments[3]) : 65536;
	int timeout = count > 4 ? atoi(arguments[4]) : 5000;
	char *response = malloc((size_t)response_size);
	char error[256];
	int status;

	if (count < 2 || !response)
		return 2;
	status = posix_https_request(arguments[1], form, form ? (int)strlen(form) : 0, response, response_size, timeout,
		error, sizeof(error));
	printf("status=%d\nerror=%s\nbody=%s\n", status, error, response);
	return 0;
}
