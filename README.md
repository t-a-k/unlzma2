# unlzma2
Tiny decompression library for LZMA2.

LZMA2 is a simple container format for LZMA compressed data
internally used in [XZ Utils](http://tukaani.org/xz/) and
[7-Zip](http://7-zip.org).

This decompressor is specialized for buffer-to-buffer decompression,
analogous to [zlib](http://zlib.net)'s `uncompress2`, and intended
to be small in code size (and reasonably fast, hopefully).
I hope that it can be used for decompressing constant data in
an embedded system, for example.

This decompressor requires ~29KiB for working memory in addition to
compressed and decompressed data themselves.  Current implementation
allocates them on the stack.
Note that the LZMA "dictionary size" will not affect memory usage of
buffer-to-buffer decompression.

This decompressor will check the sanity of compressed data as much
as possible, but cannot check the integrity of uncompressed data
(because LZMA2 format itself does not have any integrity checks
such as CRC).
Use appropriate integrity checks on top of the decompressor
if necessary.

## Copyright and License

Copyright 2020 TAKAI Kousuke

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
