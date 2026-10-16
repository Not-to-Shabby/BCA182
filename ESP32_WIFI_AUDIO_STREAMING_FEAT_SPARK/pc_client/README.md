# PC client: cast audio to the RT-Spark over UPnP

`spark_cast.py` finds the "RT-Spark Hi-Fi" renderer, starts a small HTTP server on the PC and tells the
renderer (UPnP AVTransport) to fetch the stream from it. The PC and the ESP32 must be on the same Wi-Fi.

    python spark_cast.py discover
    python spark_cast.py play song.wav --volume 80
    python spark_cast.py live                  # everything the PC is playing (Windows)

Add `--ip 192.168.1.44` to skip discovery. Ctrl+C stops playback.

- `play` sends 16-bit PCM WAV files unchanged. Other formats (MP3, FLAC, ...) need `ffmpeg` on the PATH.
- `live` needs `pip install pyaudiowpatch numpy`. It captures the default playback device with WASAPI
  loopback, so set that device to 44100 or 48000 Hz. Silence is sent while nothing is playing.
- Live audio arrives about a third of a second late because the ESP32 buffers it; do not use it for video.
