#!/usr/bin/env python3
"""Encode one GameCube opening movie as Ogg Theora + Vorbis.

Runtime reads one standard .ogv sequentially: Theora 640x480/25/4:2:0 and
Vorbis stereo/44.1kHz. FFmpeg's libtheora is used when available (deterministic
q8/q4 output that passes the SSIM floor); Xiph's encoder_example is the
fallback for FFmpeg builds without libtheora.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave


MIN_SSIM = 0.985
VIDEO_QUALITY = 8
AUDIO_QUALITY = 4
KEYFRAME_FREQUENCY = 125
OGG_PAGE_BYTES = 8192
OGG_PAGE_DURATION_US = 40_000


def run(command, **kwargs):
    return subprocess.run(command, check=True, **kwargs)


def probe(ffprobe, path):
    result = run([
        ffprobe, "-v", "error", "-show_streams", "-show_format",
        "-of", "json", path,
    ], stdout=subprocess.PIPE, text=True)
    return json.loads(result.stdout)


def stream(info, codec_type):
    return next((item for item in info["streams"]
                 if item.get("codec_type") == codec_type), None)


def ratio(value):
    numerator, denominator = value.split("/", 1)
    denominator = float(denominator)
    return float(numerator) / denominator if denominator else 0.0


def find_encoder(explicit):
    candidates = (explicit, os.environ.get("THEORA_ENCODER_EXAMPLE"),
                  shutil.which("encoder_example"))
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return os.path.abspath(candidate)
    return None


def ffmpeg_has_libtheora(ffmpeg):
    """True when the host FFmpeg is built with libtheora enrollment enabled."""
    result = subprocess.run(
        [ffmpeg, "-hide_banner", "-encoders"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return "libtheora" in result.stdout


def write_wave(raw_path, wav_path):
    # encoder_example's intentionally tiny RIFF reader cannot skip FFmpeg's
    # LIST metadata chunk. stdlib wave writes the minimal fmt+data form.
    with open(raw_path, "rb") as source, wave.open(wav_path, "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(44100)
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            output.writeframesraw(chunk)


def validate(args, source, encoded):
    original = probe(args.ffprobe, source)
    result = probe(args.ffprobe, encoded)
    video = stream(result, "video")
    audio = stream(result, "audio")
    if (video is None or video.get("codec_name") != "theora" or
            video.get("width") != 640 or video.get("height") != 480 or
            video.get("pix_fmt") != "yuv420p" or
            abs(ratio(video.get("r_frame_rate", "0/1")) - 25.0) > 0.01):
        sys.exit("encoded FMV is not 640x480 yuv420p Theora at 25fps")
    if (audio is None or audio.get("codec_name") != "vorbis" or
            int(audio.get("sample_rate", 0)) != 44100 or
            int(audio.get("channels", 0)) != 2):
        sys.exit("encoded FMV is not stereo 44.1kHz Vorbis")

    original_duration = float(original["format"]["duration"])
    encoded_duration = float(result["format"]["duration"])
    if abs(original_duration - encoded_duration) > 0.25:
        sys.exit("encoded FMV duration changed by more than 250ms")
    if os.path.getsize(encoded) >= os.path.getsize(source):
        sys.exit("encoded FMV is not smaller than its source")

    quality = subprocess.run([
        args.ffmpeg, "-v", "info", "-i", encoded, "-i", source,
        "-filter_complex",
        "[0:v]fps=25,setpts=PTS-STARTPTS[enc];"
        "[1:v]fps=25,setpts=PTS-STARTPTS[src];[enc][src]ssim",
        "-an", "-f", "null", "-",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if quality.returncode != 0:
        sys.stderr.write(quality.stderr)
        sys.exit(quality.returncode)
    matches = re.findall(r"All:([0-9.]+)", quality.stderr)
    if not matches or float(matches[-1]) < MIN_SSIM:
        sys.exit("encoded FMV failed SSIM %.3f quality floor" % MIN_SSIM)
    return matches[-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="original GTAVC MPEG movie")
    parser.add_argument("output", help="output .ogv stream")
    parser.add_argument("--encoder-example",
                        help="Xiph libtheora encoder_example "
                             "(used only when FFmpeg lacks libtheora)")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    parser.add_argument("--ffprobe", default=shutil.which("ffprobe") or "ffprobe")
    args = parser.parse_args()

    source = os.path.abspath(args.source)
    output = os.path.abspath(args.output)
    encoder = find_encoder(args.encoder_example)
    # Prefer FFmpeg's native libtheora encoder whenever it is built in: its output
    # is deterministic and passes the SSIM floor every run. libtheora 1.2.0's
    # own encoder_example mangles the Theora keyframe granule flags, so its
    # stream decodes with missing frames and dips below the quality floor
    # intermittently. Any encoder_example supplied on the command line, via
    # THEORA_ENCODER_EXAMPLE or PATH is therefore used only as a fallback for
    # FFmpeg builds without libtheora.
    use_ffmpeg = ffmpeg_has_libtheora(args.ffmpeg)
    if not use_ffmpeg and encoder is None:
        sys.exit("no available Theora encoder: host FFmpeg lacks libtheora and "
                 "Xiph encoder_example was not found. Build libtheora 1.2.0, "
                 "then pass --encoder-example PATH or set "
                 "THEORA_ENCODER_EXAMPLE.")
    if not os.path.isfile(source):
        sys.exit("opening movie not found: " + source)
    if source == output:
        sys.exit("source and output must be different files")
    if not output.lower().endswith(".ogv"):
        sys.exit("GameCube FMV output must use .ogv")

    os.makedirs(os.path.dirname(output), exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".fmv-", suffix=".ogv",
                                     dir=os.path.dirname(output))
    os.close(fd)
    try:
        with tempfile.TemporaryDirectory(prefix="revc-theora-") as work:
            native_ogg = os.path.join(work, "native.ogv")
            source_info = probe(args.ffprobe, source)
            if use_ffmpeg:
                command = [args.ffmpeg, "-v", "error", "-y", "-i", source]
                if stream(source_info, "audio") is None:
                    # The original VC PSS logo reels carry video only. The
                    # console player requires one clocked stereo stream, so mux
                    # silence at the mandatory rate instead of weakening the
                    # runtime format or inventing lower-quality audio.
                    command += ["-f", "lavfi", "-i",
                                "anullsrc=r=44100:cl=stereo",
                                "-map", "1:a:0", "-t",
                                source_info["format"]["duration"]]
                    command += ["-map", "0:v:0", "-vf", "fps=25,format=yuv420p",
                                "-c:v", "libtheora", "-q:v",
                                str(VIDEO_QUALITY), "-g",
                                str(KEYFRAME_FREQUENCY), "-c:a", "libvorbis",
                                "-q:a", str(AUDIO_QUALITY), "-ar", "44100",
                                "-ac", "2"]
                else:
                    command += ["-map", "0:v:0", "-map", "0:a:0",
                                "-vf", "fps=25,format=yuv420p",
                                "-c:v", "libtheora", "-q:v",
                                str(VIDEO_QUALITY), "-g",
                                str(KEYFRAME_FREQUENCY), "-c:a", "libvorbis",
                                "-q:a", str(AUDIO_QUALITY), "-ar", "44100",
                                "-ac", "2"]
                command += ["-f", "ogv", native_ogg]
                run(command)
            else:
                y4m = os.path.join(work, "video.y4m")
                raw = os.path.join(work, "audio.s16le")
                wav = os.path.join(work, "audio.wav")
                audio_input = "0:a:0"
                command = [args.ffmpeg, "-v", "error", "-y", "-i", source]
                if stream(source_info, "audio") is None:
                    command += ["-f", "lavfi", "-i",
                                "anullsrc=r=44100:cl=stereo"]
                    audio_input = "1:a:0"
                command += [
                    "-map", "0:v:0", "-vf", "fps=25,format=yuv420p",
                    "-f", "yuv4mpegpipe", y4m,
                    "-map", audio_input, "-c:a", "pcm_s16le", "-ar", "44100",
                    "-ac", "2",
                ]
                if audio_input == "1:a:0":
                    command += ["-t", source_info["format"]["duration"]]
                command += ["-f", "s16le", raw]
                run(command)
                write_wave(raw, wav)
                run([
                    encoder, "-q", "-v", str(VIDEO_QUALITY),
                    "-a", str(AUDIO_QUALITY), "-k", str(KEYFRAME_FREQUENCY),
                    "-o", native_ogg, wav, y4m,
                ])
            # The capture encodes in ~320ms pages and video pages larger than
            # the console's 32KiB DVD cache. Stream-copy remuxing keeps every
            # Theora/Vorbis packet bit-exact, while the 8KiB/40ms pages keep
            # synchronous DVD reads from blocking a video or audio deadline.
            run([
                args.ffmpeg, "-v", "error", "-y", "-i", native_ogg,
                "-map", "0:v:0", "-map", "0:a:0", "-c", "copy",
                "-oggpagesize", str(OGG_PAGE_BYTES),
                "-page_duration", str(OGG_PAGE_DURATION_US), temporary,
            ])

        score = validate(args, source, temporary)
        os.replace(temporary, output)
        temporary = None
        os.chmod(output, 0o644)
        print("%s: %d -> %d bytes, Theora q%d + Vorbis q%d, SSIM %s" % (
            os.path.basename(output), os.path.getsize(source),
            os.path.getsize(output), VIDEO_QUALITY, AUDIO_QUALITY, score))
        return 0
    finally:
        if temporary and os.path.exists(temporary):
            os.remove(temporary)


if __name__ == "__main__":
    sys.exit(main())
