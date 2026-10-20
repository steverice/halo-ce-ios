/*
Writing a screenshot as a PNG, with no library: the guest has only zlib's
inflate. Rows are filtered "up", then compressed with LZ77 matches under
deflate's fixed Huffman codes, which shrinks flat and repeated areas to a few
percent of a BMP and loses nothing. png_write_bgra drops the alpha channel,
which every screenshot forces opaque. tools/mac_run.py reads the result.
Include it in one source file: the functions are static.
*/
#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct png_bits
{
	unsigned char *data;
	size_t size, capacity;
	uint32_t buffer;
	int count;
	int failed;
};

static void png_put_byte(struct png_bits *bits, unsigned char value)
{
	if (bits->size == bits->capacity)
	{
		size_t capacity = bits->capacity ? bits->capacity * 2 : 65536;
		unsigned char *grown = realloc(bits->data, capacity);

		if (!grown)
		{
			bits->failed = 1;
			return;
		}
		bits->data = grown;
		bits->capacity = capacity;
	}
	bits->data[bits->size++] = value;
}

/* least significant bit first, as deflate packs everything but a Huffman code */
static void png_put_bits(struct png_bits *bits, uint32_t value, int count)
{
	bits->buffer |= value << bits->count;
	bits->count += count;
	while (bits->count >= 8)
	{
		png_put_byte(bits, (unsigned char)bits->buffer);
		bits->buffer >>= 8;
		bits->count -= 8;
	}
}

/* a Huffman code goes in most significant bit first */
static void png_put_code(struct png_bits *bits, uint32_t code, int length)
{
	uint32_t reversed = 0;
	int index;

	for (index = 0; index < length; index++)
		reversed |= ((code >> index) & 1u) << (length - 1 - index);
	png_put_bits(bits, reversed, length);
}

static void png_put_symbol(struct png_bits *bits, int symbol)
{
	if (symbol < 144)
		png_put_code(bits, 0x30u + (uint32_t)symbol, 8);
	else if (symbol < 256)
		png_put_code(bits, 0x190u + (uint32_t)(symbol - 144), 9);
	else if (symbol < 280)
		png_put_code(bits, (uint32_t)(symbol - 256), 7);
	else
		png_put_code(bits, 0xc0u + (uint32_t)(symbol - 280), 8);
}

static const unsigned short png_length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char png_length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short png_distance_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
	4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char png_distance_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void png_put_match(struct png_bits *bits, unsigned length, unsigned distance)
{
	int code = 28, index = 29;

	while (length < png_length_base[code])
		code--;
	png_put_symbol(bits, 257 + code);
	png_put_bits(bits, length - png_length_base[code], png_length_extra[code]);
	while (distance < png_distance_base[index])
		index--;
	png_put_code(bits, (uint32_t)index, 5);
	png_put_bits(bits, distance - png_distance_base[index], png_distance_extra[index]);
}

/* a zlib stream of data, in one fixed-Huffman block; the number of bytes at bits->data */
static int png_deflate(struct png_bits *bits, const unsigned char *data, size_t size)
{
	enum { hash_bits = 15, window = 32768 };
	size_t *head = malloc(sizeof(size_t) * (1u << hash_bits));
	size_t position = 0;
	uint32_t a = 1, b = 0;
	size_t index;

	if (!head)
		return 0;
	for (index = 0; index < (1u << hash_bits); index++)
		head[index] = (size_t)-1;
	png_put_byte(bits, 0x78);
	png_put_byte(bits, 0x01);
	png_put_bits(bits, 1, 1); /* the last block */
	png_put_bits(bits, 1, 2); /* fixed Huffman codes */
	while (position < size)
	{
		unsigned length = 0, distance = 0;

		if (position + 3 <= size)
		{
			uint32_t hash = (((uint32_t)data[position] << 10) ^ ((uint32_t)data[position + 1] << 5) ^
				data[position + 2]) * 2654435761u >> (32 - hash_bits);
			size_t candidate = head[hash];

			head[hash] = position;
			if (candidate != (size_t)-1 && position - candidate <= window)
			{
				size_t limit = size - position < 258 ? size - position : 258;

				while (length < limit && data[candidate + length] == data[position + length])
					length++;
				distance = (unsigned)(position - candidate);
			}
		}
		if (length >= 3)
		{
			png_put_match(bits, length, distance);
			for (index = 1; index < length; index++)
				if (position + index + 3 <= size)
				{
					const unsigned char *at = data + position + index;
					uint32_t hash = (((uint32_t)at[0] << 10) ^ ((uint32_t)at[1] << 5) ^ at[2]) *
						2654435761u >> (32 - hash_bits);

					head[hash] = position + index;
				}
			position += length;
		}
		else
			png_put_symbol(bits, data[position++]);
	}
	png_put_symbol(bits, 256);
	if (bits->count)
		png_put_bits(bits, 0, 8 - bits->count);
	for (index = 0; index < size; index++)
	{
		a = (a + data[index]) % 65521u;
		b = (b + a) % 65521u;
	}
	png_put_byte(bits, (unsigned char)(b >> 8));
	png_put_byte(bits, (unsigned char)b);
	png_put_byte(bits, (unsigned char)(a >> 8));
	png_put_byte(bits, (unsigned char)a);
	free(head);
	return !bits->failed;
}

static uint32_t png_crc(uint32_t crc, const unsigned char *data, size_t size)
{
	static uint32_t table[256];
	size_t index;

	if (!table[1])
		for (index = 0; index < 256; index++)
		{
			uint32_t value = (uint32_t)index;
			int bit;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xedb88320u ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
	crc = ~crc;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void png_put_chunk(FILE *file, const char *type, const unsigned char *data, size_t size)
{
	unsigned char head[8] = { (unsigned char)(size >> 24), (unsigned char)(size >> 16), (unsigned char)(size >> 8),
		(unsigned char)size, type[0], type[1], type[2], type[3] };
	uint32_t crc = png_crc(png_crc(0, head + 4, 4), data, size);
	unsigned char tail[4] = { (unsigned char)(crc >> 24), (unsigned char)(crc >> 16), (unsigned char)(crc >> 8),
		(unsigned char)crc };

	fwrite(head, 1, 8, file);
	if (size)
		fwrite(data, 1, size, file);
	fwrite(tail, 1, 4, file);
}

/* the width x height BGRA picture at pixels (rows from the top, stride bytes apart) as an RGB PNG at path;
nonzero when it was written */
static int png_write_bgra(const char *path, const unsigned char *pixels, unsigned long width,
	unsigned long height, unsigned long stride)
{
	static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	size_t row_size = (size_t)width * 3 + 1, size = row_size * height, written;
	unsigned char *raw, header[13];
	struct png_bits bits;
	unsigned long x, y;
	FILE *file;

	if (!width || !height || width > 0x7fffffffu / 4 || height > 0x7fffffffu / 4)
		return 0;
	raw = malloc(size);
	if (!raw)
		return 0;
	for (y = 0; y < height; y++)
	{
		unsigned char *out = raw + y * row_size;
		const unsigned char *row = pixels + y * stride, *above = y ? row - stride : NULL;

		*out++ = 2; /* up */
		for (x = 0; x < width; x++)
		{
			int channel;

			for (channel = 0; channel < 3; channel++)
				*out++ = (unsigned char)(row[x * 4 + 2 - channel] - (above ? above[x * 4 + 2 - channel] : 0));
		}
	}
	memset(&bits, 0, sizeof(bits));
	if (!png_deflate(&bits, raw, size))
	{
		free(raw);
		free(bits.data);
		return 0;
	}
	free(raw);
	file = fopen(path, "wb");
	if (!file)
	{
		free(bits.data);
		return 0;
	}
	header[0] = (unsigned char)(width >> 24); header[1] = (unsigned char)(width >> 16);
	header[2] = (unsigned char)(width >> 8); header[3] = (unsigned char)width;
	header[4] = (unsigned char)(height >> 24); header[5] = (unsigned char)(height >> 16);
	header[6] = (unsigned char)(height >> 8); header[7] = (unsigned char)height;
	header[8] = 8; /* bits a channel */
	header[9] = 2; /* RGB */
	header[10] = header[11] = header[12] = 0;
	fwrite(signature, 1, 8, file);
	png_put_chunk(file, "IHDR", header, 13);
	png_put_chunk(file, "IDAT", bits.data, bits.size);
	png_put_chunk(file, "IEND", NULL, 0);
	written = (size_t)ftell(file);
	free(bits.data);
	return fclose(file) == 0 && written > 0;
}

#endif
