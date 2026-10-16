#!/usr/bin/env python3
"""Cast audio from this PC to the RT-Spark Hi-Fi UPnP/DLNA renderer (ESP32-S3).

    python spark_cast.py discover
    python spark_cast.py play song.wav [--ip 192.168.1.44] [--volume 80]
    python spark_cast.py live [--ip 192.168.1.44]        # everything the PC is playing

The PC runs a small HTTP server and tells the renderer over UPnP (SOAP) to fetch the stream from it.
The renderer decodes uncompressed 16-bit WAV only, so files in other formats are converted with ffmpeg
when it is installed. Live mode needs `pip install pyaudiowpatch numpy` (Windows WASAPI loopback).
"""

import argparse
import http.server
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import wave
import xml.etree.ElementTree as ET
from urllib.parse import urljoin
from xml.sax.saxutils import escape

SSDP_ADDR = ("239.255.255.250", 1900)
RENDERER_ST = "urn:schemas-upnp-org:device:MediaRenderer:1"
AVT = "urn:schemas-upnp-org:service:AVTransport:1"
RCS = "urn:schemas-upnp-org:service:RenderingControl:1"
UNKNOWN_SIZE = 0x7FFFFFFF


# ----------------------------------------------------------------------------- discovery

def local_ip_towards(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def local_addresses():
    """IPv4 addresses of this PC. A multicast search sent without choosing one goes out of whichever
    adapter Windows prefers (often a VPN), so each address is tried."""
    addrs = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except socket.gaierror:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("192.168.1.1", 9))
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return sorted(a for a in addrs if not a.startswith("127."))


def discover(timeout=3.0):
    """Returns a list of (location_url, server_ip) for every MediaRenderer that answers."""
    msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
           f"MX: 2\r\nST: {RENDERER_ST}\r\n\r\n").encode()
    socks = []
    for ip in local_addresses():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
            s.bind((ip, 0))
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
            s.settimeout(0.2)
            socks.append(s)
        except OSError:
            continue
    found = {}
    for s in socks:
        for _ in range(2):
            try:
                s.sendto(msg, SSDP_ADDR)
            except OSError:
                pass
    end = time.time() + timeout
    while time.time() < end:
        for s in socks:
            try:
                data, addr = s.recvfrom(4096)
            except (socket.timeout, OSError):
                continue
            for line in data.decode(errors="replace").split("\r\n"):
                if line.lower().startswith("location:"):
                    found[line.split(":", 1)[1].strip()] = addr[0]
    for s in socks:
        s.close()
    return list(found.items())


class Renderer:
    def __init__(self, location):
        self.location = location
        root = ET.fromstring(urllib.request.urlopen(location, timeout=5).read())
        ns = {"u": "urn:schemas-upnp-org:device-1-0"}
        dev = root.find("u:device", ns)
        self.name = dev.findtext("u:friendlyName", default="?", namespaces=ns)
        self.control = {}
        for svc in dev.iter("{urn:schemas-upnp-org:device-1-0}service"):
            stype = svc.findtext("u:serviceType", namespaces=ns)
            self.control[stype] = urljoin(location, svc.findtext("u:controlURL", namespaces=ns))
        if AVT not in self.control:
            raise RuntimeError("device has no AVTransport service")
        self.host = location.split("//", 1)[1].split(":", 1)[0].split("/", 1)[0]

    def call(self, service, action, args=""):
        body = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                f'<u:{action} xmlns:u="{service}"><InstanceID>0</InstanceID>{args}</u:{action}>'
                '</s:Body></s:Envelope>').encode()
        req = urllib.request.Request(self.control[service], body, {
            "Content-Type": 'text/xml; charset="utf-8"', "SOAPACTION": f'"{service}#{action}"'})
        return urllib.request.urlopen(req, timeout=8).read().decode(errors="replace")

    def set_uri(self, url, title, artist):
        didl = ('<DIDL-Lite xmlns:dc="http://purl.org/dc/elements/1.1/" '
                'xmlns:upnp="urn:schemas-upnp-org:metadata-1-0/upnp/" '
                'xmlns="urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/"><item id="0" parentID="0" restricted="1">'
                f'<dc:title>{escape(title)}</dc:title><upnp:artist>{escape(artist)}</upnp:artist>'
                f'<upnp:class>object.item.audioItem.musicTrack</upnp:class></item></DIDL-Lite>')
        self.call(AVT, "SetAVTransportURI",
                  f"<CurrentURI>{escape(url)}</CurrentURI><CurrentURIMetaData>{escape(didl)}</CurrentURIMetaData>")

    def play(self):
        self.call(AVT, "Play", "<Speed>1</Speed>")

    def stop(self):
        self.call(AVT, "Stop")

    def set_volume(self, percent):
        if RCS in self.control:
            self.call(RCS, "SetVolume", f"<Channel>Master</Channel><DesiredVolume>{percent}</DesiredVolume>")

    def state(self):
        x = self.call(AVT, "GetTransportInfo")
        a = x.find("<CurrentTransportState>")
        return x[a + 23:x.find("</CurrentTransportState>")] if a >= 0 else "?"

    def position(self):
        x = self.call(AVT, "GetPositionInfo")
        a = x.find("<RelTime>")
        return x[a + 9:x.find("</RelTime>")] if a >= 0 else "?"


# ----------------------------------------------------------------------------- audio sources

def find_ffmpeg():
    """ffmpeg on the PATH, or the copy bundled with `pip install imageio-ffmpeg`."""
    try:
        subprocess.run(["ffmpeg", "-version"], capture_output=True, check=True)
        return "ffmpeg"
    except (OSError, subprocess.CalledProcessError):
        pass
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        return None


CODECS = {  # name -> (ffmpeg output args, Content-Type)
    "mp3": (["-c:a", "libmp3lame", "-b:a", "{kbps}k", "-f", "mp3"], "audio/mpeg"),
    "aac": (["-c:a", "aac", "-b:a", "{kbps}k", "-f", "adts"], "audio/aac"),
}


def wav_header(rate, channels, data_size):
    block = channels * 2
    riff = UNKNOWN_SIZE if data_size == UNKNOWN_SIZE else 36 + data_size
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", riff, b"WAVE", b"fmt ", 16, 1, channels, rate,
                       rate * block, block, 16, b"data", data_size)


class FileSource:
    """A WAV file sent as it is when it is 16-bit PCM, anything else decoded by ffmpeg to 44.1 kHz."""

    def __init__(self, path, codec="wav", kbps=192):
        self.path = path
        self.codec = codec
        self.kbps = kbps
        self.content_type = CODECS[codec][1] if codec in CODECS else "audio/wav"
        self.proc = None
        self.title = path.replace("\\", "/").rsplit("/", 1)[-1]
        self.wav = None
        self.ffmpeg = find_ffmpeg()
        if codec in CODECS:
            if not self.ffmpeg:
                raise SystemExit("MP3/AAC output needs ffmpeg:  pip install imageio-ffmpeg")
            return
        try:
            w = wave.open(path, "rb")
            if w.getsampwidth() == 2 and w.getnchannels() in (1, 2) and 8000 <= w.getframerate() <= 96000:
                self.wav = w
            else:
                w.close()
        except (wave.Error, EOFError):
            pass
        if self.wav is None and not self.ffmpeg:
            raise SystemExit("This file is not 16-bit PCM WAV and ffmpeg was not found to convert it.\n"
                             "Install it with:  pip install imageio-ffmpeg")

    def stream(self):
        if self.codec in CODECS:
            args = [a.format(kbps=self.kbps) for a in CODECS[self.codec][0]]
            self.proc = subprocess.Popen([self.ffmpeg, "-v", "error", "-i", self.path, "-ac", "2",
                                          "-ar", "44100"] + args + ["-"], stdout=subprocess.PIPE)
            try:
                while True:
                    block = self.proc.stdout.read(4096)
                    if not block:
                        return
                    yield block
            finally:
                self.proc.kill()
        elif self.wav is not None:
            self.wav.rewind()
            size = self.wav.getnframes() * self.wav.getnchannels() * 2
            yield wav_header(self.wav.getframerate(), self.wav.getnchannels(), size)
            while True:
                block = self.wav.readframes(8192)
                if not block:
                    return
                yield block
        else:
            yield wav_header(44100, 2, UNKNOWN_SIZE)
            self.proc = subprocess.Popen(
                [self.ffmpeg, "-v", "error", "-i", self.path, "-f", "s16le", "-ac", "2", "-ar", "44100", "-"],
                stdout=subprocess.PIPE)
            try:
                while True:
                    block = self.proc.stdout.read(16384)
                    if not block:
                        return
                    yield block
            finally:
                self.proc.kill()


class LiveSource:
    """Captures what the PC is playing (WASAPI loopback) and serves it as an endless stream."""

    MAX_BUFFER_SECONDS = 0.3

    def __init__(self, codec="wav", kbps=192):
        self.codec = codec
        self.kbps = kbps
        self.content_type = CODECS[codec][1] if codec in CODECS else "audio/wav"
        self.ffmpeg = find_ffmpeg()
        if codec in CODECS and not self.ffmpeg:
            raise SystemExit("MP3/AAC output needs ffmpeg:  pip install imageio-ffmpeg")
        import numpy as np
        import pyaudiowpatch as pyaudio
        self.np = np
        self.pa = pyaudio.PyAudio()
        info = self.pa.get_host_api_info_by_type(pyaudio.paWASAPI)
        dev = self.pa.get_device_info_by_index(info["defaultOutputDevice"])
        if not dev["isLoopbackDevice"]:
            for lb in self.pa.get_loopback_device_info_generator():
                if dev["name"] in lb["name"]:
                    dev = lb
                    break
        self.rate = int(dev["defaultSampleRate"])
        self.channels = int(dev["maxInputChannels"])
        if not 8000 <= self.rate <= 96000:
            raise SystemExit(f"The playback device runs at {self.rate} Hz; set it to 44100 or 48000 Hz "
                             "in Windows sound settings.")
        self.device_name = dev["name"]
        self.title = "PC Audio"
        self.buf = bytearray()
        self.lock = threading.Lock()
        self.peak = 0
        self.stream_in = self.pa.open(format=pyaudio.paInt16, channels=self.channels, rate=self.rate,
                                      frames_per_buffer=1024, input=True, input_device_index=dev["index"],
                                      stream_callback=self._capture)

    def _capture(self, data, frames, info, status):
        np = self.np
        a = np.frombuffer(data, dtype=np.int16).reshape(-1, self.channels)
        st = np.repeat(a, 2, axis=1) if self.channels == 1 else a[:, :2]
        if st.size:
            self.peak = max(self.peak, int(np.abs(st).max()))
        raw = st.tobytes()
        with self.lock:
            self.buf.extend(raw)
            limit = int(self.rate * self.MAX_BUFFER_SECONDS) * 4
            if len(self.buf) > limit:
                del self.buf[:len(self.buf) - limit]
        return (None, 0)

    def take_peak(self):
        p, self.peak = self.peak, 0
        return p

    def stream(self):
        """Real-time paced: WASAPI loopback delivers nothing while the PC is silent, so silence is
        inserted to keep the renderer's connection alive."""
        if self.codec in CODECS:
            yield from self._encoded_stream()
            return
        yield wav_header(self.rate, 2, UNKNOWN_SIZE)
        need = (self.rate // 50) * 4
        next_t = time.monotonic()
        while True:
            with self.lock:
                block = bytes(self.buf[:need])
                del self.buf[:len(block)]
            if len(block) < need:
                block += bytes(need - len(block))
            yield block
            next_t += 0.02
            delay = next_t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            elif delay < -0.5:
                next_t = time.monotonic()

    def _encoded_stream(self):
        args = [a.format(kbps=self.kbps) for a in CODECS[self.codec][0]]
        proc = subprocess.Popen([self.ffmpeg, "-v", "error", "-f", "s16le", "-ar", str(self.rate), "-ac", "2",
                                 "-i", "-", "-ar", "44100"] + args + ["-"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        stop = threading.Event()

        def feed():
            need = (self.rate // 50) * 4
            next_t = time.monotonic()
            try:
                while not stop.is_set():
                    with self.lock:
                        block = bytes(self.buf[:need])
                        del self.buf[:len(block)]
                    if len(block) < need:
                        block += bytes(need - len(block))
                    proc.stdin.write(block)
                    proc.stdin.flush()
                    next_t += 0.02
                    delay = next_t - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)
                    elif delay < -0.5:
                        next_t = time.monotonic()
            except (OSError, ValueError):
                pass

        threading.Thread(target=feed, daemon=True).start()
        try:
            while True:
                block = proc.stdout.read(1024)
                if not block:
                    return
                yield block
        finally:
            stop.set()
            proc.kill()

    def close(self):
        self.stream_in.stop_stream()
        self.stream_in.close()
        self.pa.terminate()


# ----------------------------------------------------------------------------- HTTP server

def make_server(source, port):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt, *args):
            print("  [http]", fmt % args)

        def _headers(self):
            self.send_response(200)
            self.send_header("Content-Type", getattr(source, "content_type", "audio/wav"))
            self.send_header("Accept-Ranges", "none")
            self.send_header("transferMode.dlna.org", "Streaming")
            self.end_headers()

        def do_HEAD(self):
            self._headers()

        def do_GET(self):
            self._headers()
            try:
                for block in source.stream():
                    self.wfile.write(block)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
                pass

    class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    return Server(("0.0.0.0", port), Handler)


# ----------------------------------------------------------------------------- commands

def find_renderer(ip):
    if ip:
        return Renderer(f"http://{ip}:8080/description.xml")
    print("Searching for UPnP renderers...")
    found = discover()
    renderers = []
    for loc, _ in found:
        try:
            renderers.append(Renderer(loc))
        except Exception:
            continue
    if not renderers:
        raise SystemExit("No renderer found. Check that the PC and the ESP32 are on the same Wi-Fi, "
                         "or pass --ip <address>.")
    for r in renderers:
        if "spark" in r.name.lower():
            return r
    return renderers[0]


def cmd_discover(_args):
    found = discover()
    if not found:
        print("No UPnP MediaRenderer answered.")
        return
    for loc, ip in found:
        try:
            print(f"{ip:15}  {Renderer(loc).name}   {loc}")
        except Exception as e:
            print(f"{ip:15}  (could not read description: {e})   {loc}")


def run(args, source, artist):
    r = find_renderer(args.ip)
    print(f"Renderer: {r.name} at {r.host}")
    server = make_server(source, args.port)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    ext = args.codec if args.codec in CODECS else "wav"
    url = f"http://{local_ip_towards(r.host)}:{args.port}/stream.{ext}"
    print("Serving", url)

    r.set_uri(url, source.title, artist)
    if args.volume is not None:
        r.set_volume(args.volume)
    r.play()
    print("Playing. Press Ctrl+C to stop.")

    seen_playing = False
    try:
        while True:
            time.sleep(1.0)
            state = r.state()
            seen_playing = seen_playing or state == "PLAYING"
            level = f"  level {source.take_peak() * 100 // 32768:3d}%" if isinstance(source, LiveSource) else ""
            print(f"  {state:16} {r.position()}{level}")
            if seen_playing and state == "STOPPED":
                print("Finished.")
                break
    except KeyboardInterrupt:
        print("\nStopping...")
    finally:
        try:
            r.stop()
        except Exception:
            pass
        server.shutdown()


def cmd_play(args):
    run(args, FileSource(args.file, args.codec, args.kbps), "PC")


def cmd_live(args):
    try:
        source = LiveSource(args.codec, args.kbps)
    except ImportError:
        raise SystemExit("Live mode needs:  pip install pyaudiowpatch numpy")
    print(f"Capturing '{source.device_name}' ({source.rate} Hz)")
    try:
        run(args, source, "System audio")
    finally:
        source.close()


def main():
    ap = argparse.ArgumentParser(description="Cast PC audio to the RT-Spark UPnP renderer")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("discover", help="list UPnP renderers on the network").set_defaults(fn=cmd_discover)
    for name, fn, help_text in (("play", cmd_play, "play an audio file"),
                                ("live", cmd_live, "stream everything the PC is playing")):
        p = sub.add_parser(name, help=help_text)
        if name == "play":
            p.add_argument("file")
        p.add_argument("--ip", help="renderer IP (skips discovery)")
        p.add_argument("--port", type=int, default=8099, help="local HTTP port (default 8099)")
        p.add_argument("--volume", type=int, help="renderer volume 0-100")
        p.add_argument("--codec", choices=["wav", "mp3", "aac"], default="wav",
                       help="stream format (mp3/aac need ffmpeg and use far less Wi-Fi bandwidth)")
        p.add_argument("--kbps", type=int, default=192, help="MP3/AAC bitrate (default 192)")
        p.set_defaults(fn=fn)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
