/*
 * LZMA2 simplified decompressor
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

#ifndef UNCOMPRESS_LZMA2_H
#define UNCOMPRESS_LZMA2_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum uncompress_status
  {
    UNCOMPRESS_OK,
    UNCOMPRESS_NO_MEMORY,
    UNCOMPRESS_DATA_ERROR,
    UNCOMPRESS_INLIMIT,
    UNCOMPRESS_OUTLIMIT,
  };

extern enum uncompress_status uncompress_lzma2 (const void */* inbuf */,
						size_t */* insize_ptr */,
						void */* outbuf */,
						size_t */* outsize_ptr */);

#ifdef __cplusplus
}
#endif

#endif
