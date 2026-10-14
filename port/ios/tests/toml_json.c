/* A TOML file as JSON on standard output, parsed by the parser the app uses (port/third_party/tomlc17), for
   tools/profile_check.py: the check then reads a profile exactly as the app will. Exit 1 with the parser's
   message when the file does not parse. Dates and times, which no profile has, are refused. */
#include "tomlc17.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_string(const char *text)
{
	putchar('"');
	for (; *text; text++)
	{
		unsigned char character = (unsigned char)*text;

		if (character == '"' || character == '\\')
			printf("\\%c", character);
		else if (character < 0x20)
			printf("\\u%04x", character);
		else
			putchar(character);
	}
	putchar('"');
}

static int print_datum(toml_datum_t datum)
{
	int index, ok = 1;

	switch (datum.type)
	{
	case TOML_STRING:
		print_string(datum.u.s);
		break;
	case TOML_INT64:
		printf("%lld", (long long)datum.u.int64);
		break;
	case TOML_FP64:
		/* (JSON has no NaN or infinity: as strings, which no setting accepts) */
		if (datum.u.fp64 != datum.u.fp64 || datum.u.fp64 - datum.u.fp64 != 0.0)
			printf("\"not a finite number\"");
		else
			printf("%.17g%s", datum.u.fp64, datum.u.fp64 == (double)(long long)datum.u.fp64 ? ".0" : "");
		break;
	case TOML_BOOLEAN:
		printf(datum.u.boolean ? "true" : "false");
		break;
	case TOML_ARRAY:
		putchar('[');
		for (index = 0; index < datum.u.arr.size; index++)
		{
			if (index)
				putchar(',');
			ok &= print_datum(datum.u.arr.elem[index]);
		}
		putchar(']');
		break;
	case TOML_TABLE:
		putchar('{');
		for (index = 0; index < datum.u.tab.size; index++)
		{
			if (index)
				putchar(',');
			print_string(datum.u.tab.key[index]);
			putchar(':');
			ok &= print_datum(datum.u.tab.value[index]);
		}
		putchar('}');
		break;
	default:
		printf("null");
		ok = 0;
		break;
	}
	return ok;
}

int main(int argc, char **argv)
{
	FILE *file = argc == 2 ? fopen(argv[1], "rb") : NULL;
	static char text[1 << 20];
	size_t size;
	toml_result_t result;
	int ok;

	if (!file)
	{
		fprintf(stderr, "usage: toml-json FILE.toml\n");
		return 2;
	}
	size = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	result = toml_parse(text, (int)size);
	if (!result.ok)
	{
		fprintf(stderr, "%s\n", result.errmsg);
		return 1;
	}
	ok = print_datum(result.toptab);
	putchar('\n');
	toml_free(result);
	if (!ok)
		fprintf(stderr, "a date or time, which no profile has\n");
	return ok ? 0 : 1;
}
