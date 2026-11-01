# Third-party notices

unVBN bundles the following third-party software.

- **macOS**: ffmpeg and libvpx, built by `scripts/build-ffmpeg.sh` from the unmodified upstream source
  tarballs (SHA-256 pinned in the script). Sections below.
- **Windows**: libvpx (BSD-3-Clause, VP9 encoder) and libwebm (BSD-3-Clause, WebM muxer), unmodified
  releases built by vcpkg (`windows/vcpkg.json`) and linked statically into `unvbn.exe` and
  `unvbn-export.exe`. No ffmpeg. Their licence texts are installed as
  `C:\Program Files\unVBN\THIRD_PARTY_NOTICES.txt`, generated at build time from vcpkg's copyright files.

## ffmpeg 7.1.5

- License: LGPL-2.1-or-later (built without `--enable-gpl` and without `--enable-nonfree`)
- Source: https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
  (SHA-256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`)
- Project: https://ffmpeg.org/. The LGPL 2.1 text ships in `unVBN.app/Contents/Resources/licenses/LGPL-2.1.txt`
  (also `licenses/LGPL-2.1.txt` in this repo).
- Used only to encode WebM VP9 with alpha from raw RGBA frames.
- Shipped unmodified, as a separate executable at `unVBN.app/Contents/Helpers/ffmpeg`
  (universal arm64 + x86_64, libvpx statically linked). You may replace it with your own
  build of ffmpeg; unVBN only invokes it as a subprocess.

Configure flags (common to both architectures; the per-arch `--cc`, `-I/-L` paths and, for x86_64,
`--enable-cross-compile --arch=x86_64 --target-os=darwin --disable-x86asm` are added by the script;
`MINVER` is 14.0):

```
--cc='clang -arch <arch> -mmacosx-version-min=14.0' --pkg-config=false
--disable-everything --disable-autodetect --disable-doc --disable-ffplay --disable-ffprobe
--disable-network --disable-debug --disable-avdevice --disable-postproc --enable-small
--enable-libvpx --enable-encoder=libvpx_vp9 --enable-muxer=webm --enable-demuxer=rawvideo
--enable-decoder=rawvideo --enable-bsf=vp9_superframe --enable-protocol=pipe --enable-protocol=fd
--enable-protocol=file --enable-filter=buffer --enable-filter=buffersink --enable-filter=format
--enable-filter=scale --enable-filter=null --enable-filter=nullsink --enable-filter=copy
--enable-swscale
--extra-cflags='-I<libvpx>/include -mmacosx-version-min=14.0'
--extra-ldflags='-L<libvpx>/lib -arch <arch> -mmacosx-version-min=14.0'
--extra-libs='-lm -lpthread'
```

## libvpx 1.15.2

- License: BSD-3-Clause (with the WebM patent grant below)
- Source: https://github.com/webmproject/libvpx/archive/refs/tags/v1.15.2.tar.gz
  (SHA-256 `26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa`)
- Project: https://www.webmproject.org/code/
- Built as a static library (VP9 encoder only) and linked into the ffmpeg executable.

### libvpx LICENSE

```
Copyright (c) 2010, The WebM Project authors. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in
    the documentation and/or other materials provided with the
    distribution.

  * Neither the name of Google, nor the WebM Project, nor the names
    of its contributors may be used to endorse or promote products
    derived from this software without specific prior written
    permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

```

### libvpx PATENTS

```
Additional IP Rights Grant (Patents)
------------------------------------

"These implementations" means the copyrightable works that implement the WebM
codecs distributed by Google as part of the WebM Project.

Google hereby grants to you a perpetual, worldwide, non-exclusive, no-charge,
royalty-free, irrevocable (except as stated in this section) patent license to
make, have made, use, offer to sell, sell, import, transfer, and otherwise
run, modify and propagate the contents of these implementations of WebM, where
such license applies only to those patent claims, both currently owned by
Google and acquired in the future, licensable by Google that are necessarily
infringed by these implementations of WebM. This grant does not include claims
that would be infringed only as a consequence of further modification of these
implementations. If you or your agent or exclusive licensee institute or order
or agree to the institution of patent litigation or any other patent
enforcement activity against any entity (including a cross-claim or
counterclaim in a lawsuit) alleging that any of these implementations of WebM
or any code incorporated within any of these implementations of WebM
constitute direct or contributory patent infringement, or inducement of
patent infringement, then any patent rights granted to you under this License
for these implementations of WebM shall terminate as of the date such
litigation is filed.
```
