# reVC GameCube release builder.
#
#   docker build -t revc .
#   docker run --rm -v /path/to/assets:/assets:ro -v "$PWD/out":/out revc
#
# /assets is the repo's assets/ layout: GTAVC/ (your Vice City install) and
# gamefiles/ (reVC's TEXT, neo, models, data). Mount real folders — a symlink
# pointing outside the mount does not resolve inside the container; mount it
# on its own instead (-v ~/GTAVC:/assets/GTAVC:ro). /out receives
#   reVC-GameCube.iso   GameCube mini-DVD, MEM1 + ARAM
# Optional: /assets/movies with opening.ogv + titles.ogv skips the FMV encode.
# Append cube to the run command to build only the DOL.
FROM devkitpro/devkitppc:latest

# xorriso builds the ISO; ffmpeg + sox convert the audio (sox writes Vorbis);
# libogg/libvorbis are for libtheora's encoder_example, which encode_fmv.py
# uses to turn the PC movies into the Theora stream the console decodes.
RUN apt-get update && apt-get install -y --no-install-recommends \
        xorriso sox libsox-fmt-all libogg-dev libvorbis-dev \
        pkg-config curl xz-utils build-essential \
    && rm -rf /var/lib/apt/lists/*

# FFmpeg 8.1, not Debian's 5.1: 5.1's Ogg stream copy drops Theora's empty
# duplicate-frame packets and re-stamps the next frame (the logo reel's fade
# showed a frame 25 frames early), which encode_fmv.py's SSIM floor rejects.
RUN arch=$(dpkg --print-architecture | sed 's/^amd64$/linux64/;s/^arm64$/linuxarm64/') \
    && curl -fsSL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-${arch}-gpl-8.1.tar.xz" \
        | tar xJ -C /opt \
    && ln -s /opt/ffmpeg-n8.1-latest-${arch}-gpl-8.1/bin/ffmpeg /usr/local/bin/ffmpeg \
    && ln -s /opt/ffmpeg-n8.1-latest-${arch}-gpl-8.1/bin/ffprobe /usr/local/bin/ffprobe

RUN curl -fsSL https://downloads.xiph.org/releases/theora/libtheora-1.2.0.tar.xz \
        | tar xJ -C /tmp \
    && cd /tmp/libtheora-1.2.0 \
    && ./configure --disable-shared --disable-doc --disable-spec --disable-oggtest \
                   --disable-vorbistest --disable-sdltest \
    && make -j"$(nproc)" -C lib \
    && make -j"$(nproc)" -C examples encoder_example \
    && install -m 755 examples/encoder_example /usr/local/bin/ \
    && rm -rf /tmp/libtheora-1.2.0

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITPPC=/opt/devkitpro/devkitPPC \
    THEORA_ENCODER_EXAMPLE=/usr/local/bin/encoder_example

COPY . /src
WORKDIR /src
ENTRYPOINT ["python3", "build.py", "--game", "/assets/GTAVC", "--gamefiles", "/assets/gamefiles", \
            "--movies", "/assets/movies", "--out", "/out"]
CMD ["iso"]
