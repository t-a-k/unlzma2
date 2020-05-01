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

  while ((optc = getopt(argc, argv, "b:v")) >= 0)
    switch (optc)
      {
      case 'b':
	outbufsize = str_to_size(optarg);
	break;
      case 'v':
	verbosity++;
	break;
      default:
	errx(2, "usage: %s [-v] [-b OUTPUT-BUFFER-SIZE] [FILE]", argv[0]);
	__builtin_unreachable();
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

  size_t insize = inbufsize;
  size_t outsize = outbufsize;
  size_t saved_insize = insize;

  enum uncompress_status status = uncompress_lzma2(buf, &insize,
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
		 buf, saved_insize, insize, outbuf, outbufsize, outsize,
		 (int) status, s);
    }

  for (size_t offset = 0; offset < outsize; )
    {
      ssize_t nwritten
	= write(STDOUT_FILENO, outbuf + offset, outsize - offset);
      if (nwritten < 0)
	err(1, "(standard output)");
      offset += nwritten;
    }

  return status != UNCOMPRESS_OK;
}
