# About

A video player for the 32Blit. Supports playing AVI files with MJPEG video and MP3 or raw audio. To convert with ffmpeg:

```
ffmpeg -i [input file] -vcodec mjpeg -q:v 2 -pix_fmt yuvj420p -vf scale=w=320:h=240:force_original_aspect_ratio=decrease,fps=fps=25 -acodec libmp3lame -ar 22050 -ac 1 output.avi

```
For raw audio (not recommended due to SD card read speed) use `-acodec pcm_s16le`, you can also adjust the quality by changing the `-q:v 2` (lower is better). 

A lot of code is shared with [the music player](https://github.com/Daft-Freak/32blit-music-player)

# Building

```
mkdir build
cd build
cmake -D32BLIT_PATH=path/to/32blit-beta ..
make
```

(See 32Blit docs for more info)
