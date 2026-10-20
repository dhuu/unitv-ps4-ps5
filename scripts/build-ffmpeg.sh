#!/bin/bash
# Compila o FFmpeg (bibliotecas estáticas) para o PS4 usando o OpenOrbis Toolchain.
# Baseado na receita do PacBrew (ps4-openorbis-portlibs/ffmpeg).
#
# Uso: OO_PS4_TOOLCHAIN=/caminho/PS4Toolchain ./scripts/build-ffmpeg.sh [pasta_saida]
# Requer: clang/lld 18, llvm-ar, nasm, make, curl.
set -euo pipefail

FFMPEG_VERSION="${FFMPEG_VERSION:-5.1.6}"
OUT="$(realpath -m "${1:-ffmpeg-ps4}")"
OO="${OO_PS4_TOOLCHAIN:?defina OO_PS4_TOOLCHAIN}"
LLVM_SUFFIX="${LLVM_SUFFIX:--18}"
WORK="$(mktemp -d)"

echo "FFmpeg $FFMPEG_VERSION -> $OUT"
if [ -n "${FFMPEG_SRC:-}" ]; then
  cp -r "$FFMPEG_SRC" "$WORK/ffmpeg-${FFMPEG_VERSION}"
else
  curl -sL "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" | tar xJ -C "$WORK"
fi
cd "$WORK/ffmpeg-${FFMPEG_VERSION}"

CFLAGS="-O3 --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -isysroot $OO -isystem $OO/include -D__PS4__"
LDFLAGS="--target=x86_64-pc-freebsd12-elf -fuse-ld=lld -nostdlib -Wl,-pie -Wl,--script,$OO/link.x -Wl,--eh-frame-hdr -L$OO/lib $OO/lib/crt1.o"

./configure --prefix="$OUT" \
  --enable-cross-compile --arch=x86_64 --target-os=freebsd \
  --cc="clang$LLVM_SUFFIX" --cxx="clang++$LLVM_SUFFIX" --ld="clang$LLVM_SUFFIX" \
  --ar="llvm-ar$LLVM_SUFFIX" --ranlib="llvm-ranlib$LLVM_SUFFIX" --nm="llvm-nm$LLVM_SUFFIX" --strip="llvm-strip$LLVM_SUFFIX" \
  --extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS" --extra-libs="-lc -lkernel" \
  --disable-shared --enable-static --enable-pic --enable-pthreads \
  --disable-programs --disable-doc --disable-debug --disable-autodetect \
  --disable-avdevice --disable-avfilter --disable-postproc --disable-network \
  --disable-everything \
  --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
  --enable-protocol=file \
  --enable-demuxer=mov,matroska,mpegts,hls,avi,flv,aac,mp3,ac3,eac3,h264,hevc,mpegps,mpegvideo,ogg,wav,asf \
  --enable-decoder=h264,hevc,mpeg2video,mpeg1video,mpeg4,msmpeg4v3,vc1,wmv3,vp8,vp9,av1 \
  --enable-decoder=aac,aac_latm,ac3,eac3,mp3,mp2,mp1,opus,vorbis,flac,dca,truehd,wmav2,pcm_s16le,pcm_s24le \
  --enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio,mpegvideo,mpeg4video,vc1,vp8,vp9,av1,opus,flac,dca \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata \
  ${EXTRA_CONFIGURE:-} \
  || { echo "==== configure falhou; final do ffbuild/config.log ===="; tail -n 80 ffbuild/config.log; exit 1; }

if [ -n "${CONFIGURE_ONLY:-}" ]; then echo "configure OK"; grep -E "HAVE_PTHREADS|HAVE_X86ASM|HAVE_SYSCTL |HAVE_CLOCK_GETTIME" config.h; exit 0; fi

# sysctl não existe no PS4 (mesmo ajuste do PacBrew)
sed -i 's|#define HAVE_SYSCTL 1|#define HAVE_SYSCTL 0|' config.h

# As threads do FFmpeg passam pelo willpro_pthread_create do app (src/player.cpp), que usa pilha de 1 MB.
# Aplicado só na compilação (depois do configure, para os testes acharem o pthread_create real).
sed -i 's|^CFLAGS=|CFLAGS=-Dpthread_create=willpro_pthread_create |' ffbuild/config.mak
grep -q "willpro_pthread_create" ffbuild/config.mak || { echo "falha ao ajustar CFLAGS"; exit 1; }

make -j"$(nproc)"
make install
echo "FFmpeg instalado em $OUT"
ls "$OUT/lib"
