/**
 * @file upnp_renderer.cpp
 * @brief Minimal UPnP/DLNA MediaRenderer so phone/PC control points (AirMusic, BubbleUPnP, VLC, ...)
 *        can discover the board and cast to it.
 *
 * Audio is pulled over HTTP from the URL the control point hands us. Only uncompressed 16-bit PCM WAV
 * is decoded (mono/stereo, any rate from 8 to 96 kHz, nearest-sample resampled to 44.1 kHz).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <esp_timer.h>
#include "upnp_renderer.h"
#include "wifi_protocol.h"

#define UPNP_PORT       8080
#define SSDP_PORT       1900
#define DEVICE_UUID     "uuid:7a1c4b2e-0d3f-4e5a-9b6c-52545370726b"
#define SUB_SID         "uuid:7a1c4b2e-0d3f-4e5a-9b6c-52545370726c"
#define FRIENDLY_NAME   "RT-Spark Hi-Fi"

#define SVC_AVT         "urn:schemas-upnp-org:service:AVTransport:1"
#define SVC_RCS         "urn:schemas-upnp-org:service:RenderingControl:1"
#define SVC_CM          "urn:schemas-upnp-org:service:ConnectionManager:1"

enum { TS_STOPPED = 0, TS_PLAYING, TS_PAUSED };

static WebServer s_http(UPNP_PORT);
static WiFiUDP s_ssdp;
static bool s_routes_ready = false;
static bool s_http_started = false;
static uint32_t s_last_alive_ms = 0;

static String s_uri;
static String s_title;
static String s_artist;
static volatile uint8_t s_state = TS_STOPPED;
static volatile bool s_stop_req = false;
static volatile bool s_busy = false;
static volatile uint8_t s_volume = 80;
static volatile bool s_mute = false;
static volatile uint32_t s_pos_frames = 0;
static volatile uint32_t s_dur_ms = 0;
static TaskHandle_t s_task = nullptr;

static const char *const NT_LIST[] = {
    "upnp:rootdevice",
    DEVICE_UUID,
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    SVC_AVT,
    SVC_RCS,
    SVC_CM,
};
#define NT_COUNT (sizeof(NT_LIST) / sizeof(NT_LIST[0]))

static IPAddress local_ip(void)
{
    if (WiFi.getMode() == WIFI_STA && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP();
    }
    return WiFi.softAPIP();
}

/* ---------------------------------------------------------------- SSDP */

static String usn_for(const char *nt)
{
    if (strcmp(nt, DEVICE_UUID) == 0) {
        return String(DEVICE_UUID);
    }
    return String(DEVICE_UUID) + "::" + nt;
}

static String location_url(void)
{
    return "http://" + local_ip().toString() + ":" + String(UPNP_PORT) + "/description.xml";
}

static void ssdp_send_alive(void)
{
    String loc = location_url();
    for (size_t i = 0; i < NT_COUNT; i++) {
        String m = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\n";
        m += "LOCATION: " + loc + "\r\n";
        m += "NT: " + String(NT_LIST[i]) + "\r\nNTS: ssdp:alive\r\n";
        m += "SERVER: ESP32/1.0 UPnP/1.0 RTSpark/1.0\r\n";
        m += "USN: " + usn_for(NT_LIST[i]) + "\r\n\r\n";
        s_ssdp.beginPacket(IPAddress(239, 255, 255, 250), SSDP_PORT);
        s_ssdp.write((const uint8_t *)m.c_str(), m.length());
        s_ssdp.endPacket();
    }
}

static void ssdp_reply(IPAddress ip, uint16_t port, const char *st)
{
    String m = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n";
    m += "LOCATION: " + location_url() + "\r\n";
    m += "SERVER: ESP32/1.0 UPnP/1.0 RTSpark/1.0\r\n";
    m += "ST: " + String(st) + "\r\n";
    m += "USN: " + usn_for(st) + "\r\n\r\n";
    s_ssdp.beginPacket(ip, port);
    s_ssdp.write((const uint8_t *)m.c_str(), m.length());
    s_ssdp.endPacket();
}

static void ssdp_poll(void)
{
    int n = s_ssdp.parsePacket();
    if (n <= 0) {
        return;
    }
    char buf[600];
    int len = s_ssdp.read(buf, sizeof(buf) - 1);
    if (len <= 0) {
        return;
    }
    buf[len] = 0;
    if (strncmp(buf, "M-SEARCH", 8) != 0) {
        return;
    }

    String req(buf);
    String low = req;
    low.toLowerCase();
    int p = low.indexOf("\r\nst:");
    if (p < 0) {
        return;
    }
    int e = req.indexOf("\r\n", p + 2);
    String st = req.substring(p + 5, e < 0 ? req.length() : e);
    st.trim();

    IPAddress rip = s_ssdp.remoteIP();
    uint16_t rport = s_ssdp.remotePort();
    bool all = st.equalsIgnoreCase("ssdp:all");
    for (size_t i = 0; i < NT_COUNT; i++) {
        if (all || st.equalsIgnoreCase(NT_LIST[i])) {
            ssdp_reply(rip, rport, NT_LIST[i]);
        }
    }
}

/* ---------------------------------------------------------------- XML helpers */

static String xml_escape(const String &s)
{
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

static String xml_unescape(String s)
{
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&amp;", "&");
    return s;
}

static String tag_value(const String &body, const String &tag)
{
    int a = body.indexOf("<" + tag + ">");
    if (a < 0) {
        return String();
    }
    a += tag.length() + 2;
    int b = body.indexOf("</" + tag + ">", a);
    if (b < 0) {
        return String();
    }
    return body.substring(a, b);
}

static String fmt_time(uint32_t ms)
{
    uint32_t s = ms / 1000U;
    char b[16];
    snprintf(b, sizeof(b), "%u:%02u:%02u", (unsigned)(s / 3600U), (unsigned)((s / 60U) % 60U), (unsigned)(s % 60U));
    return String(b);
}

/* ---------------------------------------------------------------- Device / service descriptions */

static String build_description(void)
{
    String x = "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\">";
    x += "<specVersion><major>1</major><minor>0</minor></specVersion><device>";
    x += "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>";
    x += "<friendlyName>" FRIENDLY_NAME "</friendlyName>";
    x += "<manufacturer>BCA182</manufacturer><modelName>RT-Spark Wi-Fi Audio</modelName>";
    x += "<modelNumber>1</modelNumber><UDN>" DEVICE_UUID "</UDN><serviceList>";
    x += "<service><serviceType>" SVC_AVT "</serviceType><serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
         "<SCPDURL>/AVTransport/scpd.xml</SCPDURL><controlURL>/AVTransport/control</controlURL>"
         "<eventSubURL>/AVTransport/event</eventSubURL></service>";
    x += "<service><serviceType>" SVC_RCS "</serviceType><serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
         "<SCPDURL>/RenderingControl/scpd.xml</SCPDURL><controlURL>/RenderingControl/control</controlURL>"
         "<eventSubURL>/RenderingControl/event</eventSubURL></service>";
    x += "<service><serviceType>" SVC_CM "</serviceType><serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
         "<SCPDURL>/ConnectionManager/scpd.xml</SCPDURL><controlURL>/ConnectionManager/control</controlURL>"
         "<eventSubURL>/ConnectionManager/event</eventSubURL></service>";
    x += "</serviceList></device></root>";
    return x;
}

/* Action spec: "Name|i ArgName RelatedVar|o ArgName RelatedVar" ; variable spec: "Name type" */
static const char *const AVT_ACTIONS[] = {
    "SetAVTransportURI|i InstanceID A_ARG_TYPE_InstanceID|i CurrentURI AVTransportURI|i CurrentURIMetaData AVTransportURIMetaData",
    "Play|i InstanceID A_ARG_TYPE_InstanceID|i Speed TransportPlaySpeed",
    "Pause|i InstanceID A_ARG_TYPE_InstanceID",
    "Stop|i InstanceID A_ARG_TYPE_InstanceID",
    "Next|i InstanceID A_ARG_TYPE_InstanceID",
    "Previous|i InstanceID A_ARG_TYPE_InstanceID",
    "Seek|i InstanceID A_ARG_TYPE_InstanceID|i Unit A_ARG_TYPE_SeekMode|i Target A_ARG_TYPE_SeekTarget",
    "GetTransportInfo|i InstanceID A_ARG_TYPE_InstanceID|o CurrentTransportState TransportState|o CurrentTransportStatus TransportStatus|o CurrentSpeed TransportPlaySpeed",
    "GetPositionInfo|i InstanceID A_ARG_TYPE_InstanceID|o Track CurrentTrack|o TrackDuration CurrentTrackDuration|o TrackMetaData CurrentTrackMetaData|o TrackURI CurrentTrackURI|o RelTime RelativeTimePosition|o AbsTime AbsoluteTimePosition|o RelCount RelativeCounterPosition|o AbsCount AbsoluteCounterPosition",
    "GetMediaInfo|i InstanceID A_ARG_TYPE_InstanceID|o NrTracks NumberOfTracks|o MediaDuration CurrentMediaDuration|o CurrentURI AVTransportURI|o CurrentURIMetaData AVTransportURIMetaData|o NextURI NextAVTransportURI|o NextURIMetaData NextAVTransportURIMetaData|o PlayMedium PlaybackStorageMedium|o RecordMedium RecordStorageMedium|o WriteStatus RecordMediumWriteStatus",
};
static const char *const AVT_VARS[] = {
    "A_ARG_TYPE_InstanceID ui4", "A_ARG_TYPE_SeekMode string", "A_ARG_TYPE_SeekTarget string",
    "AVTransportURI string", "AVTransportURIMetaData string", "NextAVTransportURI string",
    "NextAVTransportURIMetaData string", "TransportState string", "TransportStatus string",
    "TransportPlaySpeed string", "CurrentTrack ui4", "CurrentTrackDuration string",
    "CurrentTrackMetaData string", "CurrentTrackURI string", "RelativeTimePosition string",
    "AbsoluteTimePosition string", "RelativeCounterPosition i4", "AbsoluteCounterPosition i4",
    "NumberOfTracks ui4", "CurrentMediaDuration string", "PlaybackStorageMedium string",
    "RecordStorageMedium string", "RecordMediumWriteStatus string",
};

static const char *const RCS_ACTIONS[] = {
    "GetVolume|i InstanceID A_ARG_TYPE_InstanceID|i Channel A_ARG_TYPE_Channel|o CurrentVolume Volume",
    "SetVolume|i InstanceID A_ARG_TYPE_InstanceID|i Channel A_ARG_TYPE_Channel|i DesiredVolume Volume",
    "GetMute|i InstanceID A_ARG_TYPE_InstanceID|i Channel A_ARG_TYPE_Channel|o CurrentMute Mute",
    "SetMute|i InstanceID A_ARG_TYPE_InstanceID|i Channel A_ARG_TYPE_Channel|i DesiredMute Mute",
};
static const char *const RCS_VARS[] = {
    "A_ARG_TYPE_InstanceID ui4", "A_ARG_TYPE_Channel string", "Volume ui2", "Mute boolean",
};

static const char *const CM_ACTIONS[] = {
    "GetProtocolInfo|o Source SourceProtocolInfo|o Sink SinkProtocolInfo",
    "GetCurrentConnectionIDs|o ConnectionIDs CurrentConnectionIDs",
    "GetCurrentConnectionInfo|i ConnectionID A_ARG_TYPE_ConnectionID|o RcsID A_ARG_TYPE_RcsID|o AVTransportID A_ARG_TYPE_AVTransportID|o ProtocolInfo A_ARG_TYPE_ProtocolInfo|o PeerConnectionManager A_ARG_TYPE_ConnectionManager|o PeerConnectionID A_ARG_TYPE_ConnectionID|o Direction A_ARG_TYPE_Direction|o Status A_ARG_TYPE_ConnectionStatus",
};
static const char *const CM_VARS[] = {
    "SourceProtocolInfo string", "SinkProtocolInfo string", "CurrentConnectionIDs string",
    "A_ARG_TYPE_ConnectionStatus string", "A_ARG_TYPE_ConnectionManager string",
    "A_ARG_TYPE_Direction string", "A_ARG_TYPE_ProtocolInfo string", "A_ARG_TYPE_ConnectionID i4",
    "A_ARG_TYPE_AVTransportID i4", "A_ARG_TYPE_RcsID i4",
};

static String build_scpd(const char *const *actions, size_t na, const char *const *vars, size_t nv)
{
    String x = "<?xml version=\"1.0\"?><scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">"
               "<specVersion><major>1</major><minor>0</minor></specVersion><actionList>";
    for (size_t i = 0; i < na; i++) {
        String spec(actions[i]);
        int start = 0;
        int bar = spec.indexOf('|');
        x += "<action><name>" + spec.substring(0, bar < 0 ? spec.length() : bar) + "</name><argumentList>";
        while (bar >= 0) {
            start = bar + 1;
            bar = spec.indexOf('|', start);
            String tok = spec.substring(start, bar < 0 ? spec.length() : bar);
            int sp = tok.indexOf(' ', 2);
            x += "<argument><name>" + tok.substring(2, sp) + "</name><direction>";
            x += (tok[0] == 'i') ? "in" : "out";
            x += "</direction><relatedStateVariable>" + tok.substring(sp + 1) + "</relatedStateVariable></argument>";
        }
        x += "</argumentList></action>";
    }
    x += "</actionList><serviceStateTable>";
    for (size_t i = 0; i < nv; i++) {
        String v(vars[i]);
        int sp = v.indexOf(' ');
        String name = v.substring(0, sp);
        x += "<stateVariable sendEvents=\"no\"><name>" + name + "</name><dataType>" + v.substring(sp + 1) + "</dataType>";
        if (name == "Volume") {
            x += "<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</step></allowedValueRange>";
        }
        x += "</stateVariable>";
    }
    x += "</serviceStateTable></scpd>";
    return x;
}

/* ---------------------------------------------------------------- Streaming */

static void set_state(uint8_t ts)
{
    s_state = ts;
    spark_set_stream_state(ts == TS_PLAYING ? STREAM_STATE_PLAYING :
                           ts == TS_PAUSED  ? STREAM_STATE_PAUSED  : STREAM_STATE_STOPPED);
}

static void push_meta(const char *format)
{
    wifi_track_meta_t m = {};
    strncpy(m.title, s_title.c_str(), sizeof(m.title) - 1);
    strncpy(m.artist, s_artist.c_str(), sizeof(m.artist) - 1);
    strncpy(m.album, "DLNA Cast", sizeof(m.album) - 1);
    strncpy(m.format, format, sizeof(m.format) - 1);
    m.duration_ms = s_dur_ms;
    send_spark_packet(WIFI_MSG_TRACK_META, (const uint8_t *)&m, sizeof(m));
}

static bool read_exact(WiFiClient *c, uint8_t *dst, size_t n)
{
    uint32_t t = millis();
    size_t got = 0;
    while (got < n) {
        if (s_stop_req) {
            return false;
        }
        int a = c->available();
        if (a > 0) {
            size_t want = (size_t)a < (n - got) ? (size_t)a : (n - got);
            int r = c->read(dst + got, want);
            if (r > 0) {
                got += (size_t)r;
                t = millis();
            }
        } else {
            if (!c->connected() || millis() - t > 5000) {
                return false;
            }
            delay(1);
        }
    }
    return true;
}

static bool skip_bytes(WiFiClient *c, uint32_t n)
{
    uint8_t tmp[64];
    while (n > 0) {
        size_t k = n < sizeof(tmp) ? n : sizeof(tmp);
        if (!read_exact(c, tmp, k)) {
            return false;
        }
        n -= (uint32_t)k;
    }
    return true;
}

static void stream_once(void)
{
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(s_uri)) {
        Serial.println("[UPnP] Bad stream URL");
        return;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[UPnP] Stream GET failed: %d\n", code);
        http.end();
        return;
    }
    WiFiClient *c = http.getStreamPtr();

    uint8_t hdr[12];
    if (!read_exact(c, hdr, 12) || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        Serial.println("[UPnP] Unsupported stream: only PCM WAV is decoded (MP3/AAC/FLAC not yet supported)");
        http.end();
        return;
    }

    uint16_t ch = 0, bits = 0, tag = 0;
    uint32_t rate = 0, data_size = 0;
    bool have_fmt = false, have_data = false;
    while (!have_data) {
        uint8_t ck[8];
        if (!read_exact(c, ck, 8)) {
            break;
        }
        uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16) {
            uint8_t f[16];
            if (!read_exact(c, f, 16)) {
                break;
            }
            tag = f[0] | (f[1] << 8);
            ch = f[2] | (f[3] << 8);
            rate = f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24);
            bits = f[14] | (f[15] << 8);
            have_fmt = true;
            if (!skip_bytes(c, (sz - 16) + (sz & 1U))) {
                break;
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            data_size = sz;
            have_data = true;
        } else if (!skip_bytes(c, sz + (sz & 1U))) {
            break;
        }
    }

    if (!have_fmt || !have_data || (tag != 1 && tag != 0xFFFE) || bits != 16 ||
        (ch != 1 && ch != 2) || rate < 8000 || rate > 96000) {
        Serial.printf("[UPnP] Unsupported WAV (tag %u, %u ch, %u-bit, %u Hz)\n", tag, ch, bits, (unsigned)rate);
        http.end();
        return;
    }

    uint32_t remaining = (data_size == 0 || data_size >= 0x7FFFFFFFU) ? 0xFFFFFFFFU : data_size;
    s_dur_ms = (remaining == 0xFFFFFFFFU) ? 0 : (uint32_t)(((uint64_t)remaining * 1000ULL) / ((uint64_t)rate * ch * 2U));
    s_pos_frames = 0;

    char fmt[16];
    snprintf(fmt, sizeof(fmt), "WAV %uHz %uch", (unsigned)rate, ch);
    push_meta(fmt);
    Serial.printf("[UPnP] Streaming '%s' (%s)\n", s_title.c_str(), fmt);

    const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 44100U);
    const size_t fb = ch * 2U;
    uint8_t inbuf[1024];
    size_t in_len = 0, in_pos = 0;
    uint32_t phase = 0x10000U;
    int16_t cl = 0, cr = 0;
    int16_t out[AUDIO_CHUNK_SAMPLES * 2];
    const int64_t chunk_us = (int64_t)AUDIO_CHUNK_SAMPLES * 1000000LL / 44100LL;
    int64_t t_next = esp_timer_get_time();
    int sent = 0;
    bool ended = false;

    while (!s_stop_req && !ended) {
        if (s_state == TS_PAUSED) {
            delay(20);
            t_next = esp_timer_get_time();
            sent = 0;
            continue;
        }

        const uint32_t gain = s_mute ? 0U : s_volume;
        for (size_t i = 0; i < AUDIO_CHUNK_SAMPLES && !ended; i++) {
            while (phase >= 0x10000U) {
                if (in_pos + fb > in_len) {
                    size_t left = in_len - in_pos;
                    memmove(inbuf, inbuf + in_pos, left);
                    in_len = left;
                    in_pos = 0;
                    uint32_t t = millis();
                    while (in_len < fb && !ended) {
                        int a = c->available();
                        if (s_stop_req) {
                            ended = true;
                        } else if (a > 0) {
                            size_t want = (size_t)a < (sizeof(inbuf) - in_len) ? (size_t)a : (sizeof(inbuf) - in_len);
                            if (want > remaining) {
                                want = remaining;
                            }
                            if (want == 0) {
                                ended = true;
                            } else {
                                int r = c->read(inbuf + in_len, want);
                                if (r > 0) {
                                    in_len += (size_t)r;
                                    remaining -= (uint32_t)r;
                                    t = millis();
                                }
                            }
                        } else if (!c->connected() || millis() - t > 5000) {
                            ended = true;
                        } else {
                            delay(1);
                        }
                    }
                    if (ended) {
                        break;
                    }
                }
                cl = (int16_t)(inbuf[in_pos] | (inbuf[in_pos + 1] << 8));
                cr = (ch == 2) ? (int16_t)(inbuf[in_pos + 2] | (inbuf[in_pos + 3] << 8)) : cl;
                in_pos += fb;
                phase -= 0x10000U;
            }
            if (ended) {
                break;
            }
            out[2 * i] = (int16_t)(((int32_t)cl * (int32_t)gain) / 100);
            out[2 * i + 1] = (int16_t)(((int32_t)cr * (int32_t)gain) / 100);
            phase += step;
        }
        if (ended) {
            break;
        }

        send_spark_packet(WIFI_MSG_AUDIO_PCM, (const uint8_t *)out, sizeof(out));
        s_pos_frames += AUDIO_CHUNK_SAMPLES;

        if (++sent > 4) {
            t_next += chunk_us;
            int64_t wait = t_next - esp_timer_get_time();
            if (wait > 1000) {
                delay((uint32_t)(wait / 1000));
            } else if (wait < -50000) {
                t_next = esp_timer_get_time();
            }
        }
    }

    http.end();
    Serial.println("[UPnP] Stream finished");
}

static void stream_task(void *param)
{
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_busy = true;
        stream_once();
        set_state(TS_STOPPED);
        s_busy = false;
    }
}

static void start_stream(void)
{
    if (s_uri.length() == 0) {
        return;
    }
    if (s_busy) {
        s_stop_req = true;
        uint32_t t = millis();
        while (s_busy && millis() - t < 2000) {
            delay(10);
        }
    }
    s_stop_req = false;
    set_state(TS_PLAYING);
    xTaskNotifyGive(s_task);
}

static void stop_stream(void)
{
    s_stop_req = true;
    set_state(TS_STOPPED);
}

void upnp_button(uint8_t cmd)
{
    if (cmd == BTN_CMD_PLAY_PAUSE) {
        if (s_state == TS_PLAYING) {
            set_state(TS_PAUSED);
        } else if (s_state == TS_PAUSED) {
            set_state(TS_PLAYING);
        } else {
            start_stream();
        }
    } else if (cmd == BTN_CMD_VOL_UP || cmd == BTN_CMD_VOL_DOWN) {
        int v = (int)s_volume + (cmd == BTN_CMD_VOL_UP ? 5 : -5);
        s_volume = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
        spark_set_volume(s_volume);
    }
}

/* ---------------------------------------------------------------- SOAP */

static void soap_reply(const char *svc, const String &action, const String &args)
{
    String b = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action +
               "Response xmlns:u=\"" + String(svc) + "\">" + args + "</u:" + action + "Response></s:Body></s:Envelope>";
    s_http.sendHeader("EXT", "");
    s_http.send(200, "text/xml; charset=\"utf-8\"", b);
}

static void soap_fault(int code, const char *desc)
{
    String b = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault>"
               "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
               "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" + String(code) +
               "</errorCode><errorDescription>" + String(desc) + "</errorDescription></UPnPError>"
               "</detail></s:Fault></s:Body></s:Envelope>";
    s_http.send(500, "text/xml; charset=\"utf-8\"", b);
}

static String action_name(const String &body)
{
    int p = body.indexOf("Body>");
    if (p < 0) {
        return String();
    }
    int a = body.indexOf('<', p + 5);
    if (a < 0) {
        return String();
    }
    int e = a + 1;
    while (e < (int)body.length() && body[e] != ' ' && body[e] != '>' && body[e] != '/') {
        e++;
    }
    String n = body.substring(a + 1, e);
    int colon = n.indexOf(':');
    return colon >= 0 ? n.substring(colon + 1) : n;
}

static const char *state_name(void)
{
    return s_state == TS_PLAYING ? "PLAYING" : (s_state == TS_PAUSED ? "PAUSED_PLAYBACK" : "STOPPED");
}

static void handle_avt_control(void)
{
    String body = s_http.arg("plain");
    String act = action_name(body);
    Serial.printf("[UPnP] AVTransport %s\n", act.c_str());

    if (act == "SetAVTransportURI") {
        stop_stream();
        s_uri = xml_unescape(tag_value(body, "CurrentURI"));
        String meta = xml_unescape(tag_value(body, "CurrentURIMetaData"));
        s_title = xml_unescape(tag_value(meta, "dc:title"));
        s_artist = xml_unescape(tag_value(meta, "upnp:artist"));
        if (s_title.length() == 0) {
            int sl = s_uri.lastIndexOf('/');
            s_title = s_uri.substring(sl + 1);
            int q = s_title.indexOf('?');
            if (q >= 0) {
                s_title = s_title.substring(0, q);
            }
        }
        if (s_artist.length() == 0) {
            s_artist = "Wi-Fi Cast";
        }
        s_dur_ms = 0;
        s_pos_frames = 0;
        push_meta("DLNA");
        Serial.printf("[UPnP] URI: %s\n", s_uri.c_str());
        soap_reply(SVC_AVT, act, "");
    } else if (act == "Play") {
        if (s_state == TS_PAUSED) {
            set_state(TS_PLAYING);
        } else {
            start_stream();
        }
        soap_reply(SVC_AVT, act, "");
    } else if (act == "Pause") {
        if (s_state == TS_PLAYING) {
            set_state(TS_PAUSED);
        }
        soap_reply(SVC_AVT, act, "");
    } else if (act == "Stop") {
        stop_stream();
        soap_reply(SVC_AVT, act, "");
    } else if (act == "Seek" || act == "Next" || act == "Previous") {
        soap_reply(SVC_AVT, act, "");
    } else if (act == "GetTransportInfo") {
        soap_reply(SVC_AVT, act,
                   "<CurrentTransportState>" + String(state_name()) + "</CurrentTransportState>"
                   "<CurrentTransportStatus>OK</CurrentTransportStatus><CurrentSpeed>1</CurrentSpeed>");
    } else if (act == "GetPositionInfo") {
        uint32_t pos_ms = (uint32_t)(((uint64_t)s_pos_frames * 1000ULL) / 44100ULL);
        String pos = fmt_time(pos_ms);
        soap_reply(SVC_AVT, act,
                   "<Track>1</Track><TrackDuration>" + fmt_time(s_dur_ms) + "</TrackDuration>"
                   "<TrackMetaData></TrackMetaData><TrackURI>" + xml_escape(s_uri) + "</TrackURI>"
                   "<RelTime>" + pos + "</RelTime><AbsTime>" + pos + "</AbsTime>"
                   "<RelCount>0</RelCount><AbsCount>0</AbsCount>");
    } else if (act == "GetMediaInfo") {
        soap_reply(SVC_AVT, act,
                   "<NrTracks>1</NrTracks><MediaDuration>" + fmt_time(s_dur_ms) + "</MediaDuration>"
                   "<CurrentURI>" + xml_escape(s_uri) + "</CurrentURI><CurrentURIMetaData></CurrentURIMetaData>"
                   "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
                   "<PlayMedium>NETWORK</PlayMedium><RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
                   "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
    } else {
        soap_fault(401, "Invalid Action");
    }
}

static void handle_rcs_control(void)
{
    String body = s_http.arg("plain");
    String act = action_name(body);
    Serial.printf("[UPnP] RenderingControl %s\n", act.c_str());

    if (act == "GetVolume") {
        soap_reply(SVC_RCS, act, "<CurrentVolume>" + String((int)s_volume) + "</CurrentVolume>");
    } else if (act == "SetVolume") {
        int v = tag_value(body, "DesiredVolume").toInt();
        s_volume = (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
        spark_set_volume(s_volume);
        soap_reply(SVC_RCS, act, "");
    } else if (act == "GetMute") {
        soap_reply(SVC_RCS, act, "<CurrentMute>" + String(s_mute ? 1 : 0) + "</CurrentMute>");
    } else if (act == "SetMute") {
        String m = tag_value(body, "DesiredMute");
        s_mute = (m == "1" || m.equalsIgnoreCase("true"));
        soap_reply(SVC_RCS, act, "");
    } else {
        soap_fault(401, "Invalid Action");
    }
}

static void handle_cm_control(void)
{
    String body = s_http.arg("plain");
    String act = action_name(body);
    Serial.printf("[UPnP] ConnectionManager %s\n", act.c_str());

    if (act == "GetProtocolInfo") {
        soap_reply(SVC_CM, act,
                   "<Source></Source><Sink>http-get:*:audio/wav:*,http-get:*:audio/x-wav:*,"
                   "http-get:*:audio/L16:*</Sink>");
    } else if (act == "GetCurrentConnectionIDs") {
        soap_reply(SVC_CM, act, "<ConnectionIDs>0</ConnectionIDs>");
    } else if (act == "GetCurrentConnectionInfo") {
        soap_reply(SVC_CM, act,
                   "<RcsID>0</RcsID><AVTransportID>0</AVTransportID><ProtocolInfo></ProtocolInfo>"
                   "<PeerConnectionManager></PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
                   "<Direction>Input</Direction><Status>OK</Status>");
    } else {
        soap_fault(401, "Invalid Action");
    }
}

static void handle_event(void)
{
    s_http.sendHeader("SID", SUB_SID);
    s_http.sendHeader("TIMEOUT", "Second-1800");
    s_http.send(200);
}

static void setup_routes(void)
{
    s_http.on("/description.xml", HTTP_GET, []() {
        s_http.send(200, "text/xml; charset=\"utf-8\"", build_description());
    });
    s_http.on("/AVTransport/scpd.xml", HTTP_GET, []() {
        s_http.send(200, "text/xml; charset=\"utf-8\"",
                    build_scpd(AVT_ACTIONS, sizeof(AVT_ACTIONS) / sizeof(AVT_ACTIONS[0]),
                               AVT_VARS, sizeof(AVT_VARS) / sizeof(AVT_VARS[0])));
    });
    s_http.on("/RenderingControl/scpd.xml", HTTP_GET, []() {
        s_http.send(200, "text/xml; charset=\"utf-8\"",
                    build_scpd(RCS_ACTIONS, sizeof(RCS_ACTIONS) / sizeof(RCS_ACTIONS[0]),
                               RCS_VARS, sizeof(RCS_VARS) / sizeof(RCS_VARS[0])));
    });
    s_http.on("/ConnectionManager/scpd.xml", HTTP_GET, []() {
        s_http.send(200, "text/xml; charset=\"utf-8\"",
                    build_scpd(CM_ACTIONS, sizeof(CM_ACTIONS) / sizeof(CM_ACTIONS[0]),
                               CM_VARS, sizeof(CM_VARS) / sizeof(CM_VARS[0])));
    });
    s_http.on("/AVTransport/control", HTTP_POST, handle_avt_control);
    s_http.on("/RenderingControl/control", HTTP_POST, handle_rcs_control);
    s_http.on("/ConnectionManager/control", HTTP_POST, handle_cm_control);
    s_http.on("/AVTransport/event", HTTP_ANY, handle_event);
    s_http.on("/RenderingControl/event", HTTP_ANY, handle_event);
    s_http.on("/ConnectionManager/event", HTTP_ANY, handle_event);
}

void upnp_begin(void)
{
    if (s_task == nullptr) {
        xTaskCreatePinnedToCore(stream_task, "upnp_stream", 8192, nullptr, 4, &s_task, 0);
    }
    if (!s_routes_ready) {
        setup_routes();
        s_routes_ready = true;
    }
    if (s_http_started) {
        s_http.stop();
    }
    s_http.begin();
    s_http_started = true;

    s_ssdp.stop();
    s_ssdp.beginMulticast(IPAddress(239, 255, 255, 250), SSDP_PORT);
    ssdp_send_alive();
    s_last_alive_ms = millis();
    Serial.printf("[UPnP] MediaRenderer '%s' at %s\n", FRIENDLY_NAME, location_url().c_str());
}

void upnp_loop(void)
{
    if (!s_http_started) {
        return;
    }
    s_http.handleClient();
    ssdp_poll();
    if (millis() - s_last_alive_ms > 30000) {
        s_last_alive_ms = millis();
        ssdp_send_alive();
    }
}
