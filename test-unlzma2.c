/*
 * Test bench for LZMA2 simplified decompressor
 *
 * Copyright 2020 TAKAI Kousuke
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <err.h>
#include <stdarg.h>

#include "uncompress_lzma2.h"

int verbosity;

void __attribute__((format(printf, 1, 2)))
dbg_printf (const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

static size_t
str_to_size (const char *s)
{
  unsigned long ulval;
  size_t size;
  char *suffix;
  uint_fast32_t unit;

  errno = 0;
  ulval = strtoul(s, &suffix, 0);
  if (errno)
    err(2, "Invalid size `%s'", s);
  else if (suffix == s)
    errx(2, "Invalid number in `%s'", s);
  while (*(unsigned char *) suffix && isspace(*(unsigned char *) suffix))
    suffix++;
  unit = 1;
  if (!strcmp(suffix, "K"))
    unit = 1024;
  else if (!strcmp(suffix, "M"))
    unit = 1024 * 1024;
  else if (!strcmp(suffix, "G"))
    unit = 1024 * 1024 * 1024;
  else
    errx(2, "Unknown suffix `%s' in `%s'", suffix, s);
  if (__builtin_mul_overflow(ulval, unit, &size))
    errx(2, "Size argument `%s' overflow", s);
  return size;
}

static uint_fast32_t
crc32 (const void *const buf, size_t size)
{
  static uint_least32_t table[256];

  if (!table[1])
    {
      const uint_fast32_t poly = UINT32_C(0xEDB88320);

      for (unsigned int i = 0; i < 256; i++)
	{
	  uint_fast32_t crc = i;

	  for (unsigned int j = 0; j < 8; j++)
	    crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
	  table[i] = crc;
	}
    }

  uint_fast32_t crc = UINT32_C(0xFFFFFFFF);
  for (size_t i = 0; i < size; i++)
    crc = (crc >> 8) ^ table[(crc & 0xFF) ^ ((const uint8_t *) buf)[i]];
  return ~crc & UINT32_C(0xFFFFFFFF);
}

static uint_fast16_t
read_aligned_le16 (const void *const vp)
{
  const uint8_t *const p = __builtin_assume_aligned(vp, 2);
  return (uint_fast16_t) p[0] | (p[1] << 8);
}

static uint_fast32_t
read_aligned_le32 (const void *const vp)
{
  const uint8_t *const p = __builtin_assume_aligned(vp, 4);
  return (uint_fast32_t) p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

int
main (int argc, char *argv[])
{
  int optc;
  const char *filename;
  int fd;
  struct stat statbuf;
  char *buf;
  size_t inbufsize;
  char *outbuf;
  size_t outbufsize = 0;
  _Bool check_crc = 0;
  enum { FMT_AUTO, FMT_RAW, FMT_XZ, FMT_XZ_CRC32 } format = FMT_AUTO;

  while ((optc = getopt(argc, argv, "b:crvx")) >= 0)
    switch (optc)
      {
      case 'b':
	outbufsize = str_to_size(optarg);
	break;
      case 'c':
	check_crc = 1;
	break;
      case 'r':
	format = FMT_RAW;
	break;
      case 'v':
	verbosity++;
	break;
      case 'x':
	format = FMT_XZ;
	break;
      default:
	errx(2, "usage: %s [-v] [-r|-x] [-c] [-b OUTPUT-BUFFER-SIZE] [FILE]",
	     argv[0]);
	return 2;
      }

  if (optind >= argc)
    filename = "-";
  else if (optind + 1 == argc)
    filename = argv[optind];
  else
    errx(2, "Too many arguments");

  if (filename[0] == '-' && !filename[1])
    fd = STDIN_FILENO;
  else if ((fd = open(filename, O_RDONLY)) < 0)
    err(1, "%s", filename);

  if (fstat(fd, &statbuf) < 0)
    err(1, "fstat");
  if (S_ISREG(statbuf.st_mode) && statbuf.st_size != 0)
    {
      if ((inbufsize = statbuf.st_size) != statbuf.st_size)
	errx(1, "%s: File size too big", filename);
      buf = mmap(NULL, inbufsize, PROT_READ, MAP_PRIVATE | MAP_FILE, fd, 0);
      if (buf == MAP_FAILED)
	err(1, "mmap");
    }
  else
    {
      size_t buf_alloc = 1 << 20;

      for (buf = NULL, inbufsize = 0;;)
	{
	  ssize_t nread;

	  if (!(buf = realloc(buf, buf_alloc)))
	    errx(1, "Memory exhausted");

	  nread = read(fd, buf + inbufsize, buf_alloc - inbufsize);
	  if (nread < 0)
	    err(1, "%s", filename);
	  else if (nread == 0)
	    break;
	  else if ((inbufsize += nread) >= buf_alloc &&
		   __builtin_add_overflow(buf_alloc, (buf_alloc < (1U << 30) ?
						      buf_alloc :
						      (1U << 30)),
					  &buf_alloc))
	    errx(1, "%s: File size overflow", filename);
	}

      if (inbufsize == 0)
	errx(1, "%s: File is empty", filename);
      else if (inbufsize > (1U << 20))
	{
	  /* Hopefully return unused memory to the system */
	  void *newbuf = realloc(buf, inbufsize);
	  if (newbuf)
	    buf = newbuf;
	}
    }
  close(fd);

  /* By default the output buffer is allocated for 4 times larger than
     the input size (that is, compression ratio is assumed to be 25%). */
  if (!outbufsize && __builtin_mul_overflow(inbufsize, 4, &outbufsize))
    errx(1, "Output buffer size overflow (input size = %zu)", inbufsize);

  outbuf = mmap(NULL, outbufsize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (outbuf == MAP_FAILED)
    err(1, "anonymous mmap");

  char *inbuf = buf;
  size_t insize = inbufsize;

#define XZ_MAGIC1	(0xFD | ('7' << 8) | ('z' << 16) | ('X' << 24))
#define XZ_MAGIC2	('Z' | (0x00 << 8))
#define XZ_MAGIC3	('Y' | ('Z' << 8))

  if (format != FMT_RAW &&
      insize > (12 + 8) &&
      read_aligned_le32(inbuf) == XZ_MAGIC1 &&
      read_aligned_le16(&inbuf[4]) == XZ_MAGIC2 &&
      crc32(&inbuf[6], 2) == read_aligned_le32(&inbuf[8]))
    {
      /* Found XZ Stream Header. */

      if (format == FMT_AUTO)
	format = FMT_XZ;
      unsigned int const stream_flags = read_aligned_le16(&inbuf[6]);
      if (stream_flags & ~0x0F00)
	errx(1, "%s: Unsupported .xz file (Stream Flags = %#x)",
	     filename, stream_flags);
      unsigned int const checktype = (stream_flags >> 8) & 0xF;
      if (checktype == 0x1)
	format = FMT_XZ_CRC32;

      unsigned int const block_header_size = *(uint8_t *) &buf[12];

      if (block_header_size != 0 &&
	  (insize - 12 - 4) > (block_header_size * 4) &&
	  crc32(&inbuf[12], block_header_size * 4) == read_aligned_le32(&inbuf[12 + block_header_size * 4]))
	{
	  /* Found XZ Block Header. */
	  if ((*(uint8_t *) &inbuf[13] & 0x03) != 0x00)
	    errx(1, "%s: unsupported .xz file (%u filters)",
		 filename, (*(uint8_t *) &inbuf[13] & 0x03) + 1);

	  inbuf += 12 + block_header_size * 4 + 4;
	  insize -= 12 + block_header_size * 4 + 4;
	  if (verbosity > 0)
	    dbg_printf("Skipping .xz header, %u bytes",
		       12 + block_header_size * 4 + 4);

	  unsigned int const checksize
	    = checktype ? (4 << ((checktype - 1) / 3)) : 0;
	  if (insize > (8 + 12 + checksize) && (insize & 3) == 0 &&
	      read_aligned_le16(&inbuf[insize - 2]) == XZ_MAGIC3 &&
	      /* Footer Stream Flags */
	      (read_aligned_le16(&inbuf[insize - 4]) ==
	       read_aligned_le16(&buf[6])) &&
	      (crc32(&inbuf[insize - 8], 6) ==
	       read_aligned_le32(&inbuf[insize - 12])))
	    {
	      /* Found Stream Footer. */
	      uint_fast32_t const backward_size
		= read_aligned_le32(&inbuf[insize - 8]);
	      if (backward_size > 0 &&
		  backward_size < (insize / 4 - 4) &&
		  /* Index Indicator */
		  !inbuf[insize - 16 - backward_size * 4] &&
		  (crc32(&inbuf[insize - 16 - backward_size * 4],
			 backward_size * 4) ==
		   read_aligned_le32(&inbuf[insize - 16])))
		{
		  /* Found Index. */
		  if (*(uint8_t *) &inbuf[insize - 16 - backward_size * 4 + 1] != 0x01)
		    errx(1, "%s: unsupported .xz file (more than one blocks)",
			 filename);
		  size_t const stripsize = 16 + backward_size * 4 + checksize;
		  insize -= stripsize;
		  if (verbosity > 0)
		    dbg_printf("Stripping .xz footer, %zu bytes", stripsize);
		}
	    }
	}
    }
  else if (format == FMT_XZ)
    errx(1, "%s: Not a .xz file", filename);

  size_t outsize = outbufsize;
  size_t saved_insize = insize;

  enum uncompress_status status = uncompress_lzma2(inbuf, &insize,
						   outbuf, &outsize);

  if (verbosity > 0)
    {
      char *s;

      switch (status)
	{
	case UNCOMPRESS_OK:		s = "OK";		break;
	case UNCOMPRESS_NO_MEMORY:	s = "NO_MEMORY";	break;
	case UNCOMPRESS_DATA_ERROR:	s = "DATA_ERROR";	break;
	case UNCOMPRESS_INLIMIT:	s = "INLIMIT";		break;
	case UNCOMPRESS_OUTLIMIT:	s = "OUTLIMIT";		break;
	default:			s = "???";		break;
	}

      dbg_printf("uncompress_lzma2(%p, [%zu -> %zu], %p, [%zu -> %zu]) = %d (%s)",
		 inbuf, saved_insize, insize, outbuf, outbufsize, outsize,
		 (int) status, s);
    }

  /* Sanity check */
  if (insize > saved_insize)
    errx(3, "input buffer overrun (insize = %zu -> %zu)",
	 saved_insize, insize);

  for (size_t offset = 0; offset < outsize; )
    {
      ssize_t nwritten
	= write(STDOUT_FILENO, outbuf + offset, outsize - offset);
      if (nwritten < 0)
	err(1, "(standard output)");
      offset += nwritten;
    }

  if (status != UNCOMPRESS_OK)
    return 1;

  if (format == FMT_XZ_CRC32)
    {
      if (saved_insize - insize > 3)
	errx(1, "invalid block padding (%zu bytes)", saved_insize - insize);

      const uint_least32_t recorded_crc = read_aligned_le32(&inbuf[saved_insize]);
      const uint_least32_t computed_crc	= crc32(outbuf, outsize);
      if (recorded_crc != computed_crc)
	errx(1, "CRC32 mismatch (recorded %.8" PRIxLEAST32 ", computed %.8" PRIxLEAST32 ")",
	     recorded_crc, computed_crc);
      else if (verbosity)
	dbg_printf("CRC32 = %.8" PRIxLEAST32 ", OK", computed_crc);
    }
  else if (check_crc)
    errx(1, "%s: No 32-bit CRC", filename);

  return 0;
}
