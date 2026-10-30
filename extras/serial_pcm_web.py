#!/usr/bin/env python3
"""Serve ES8311 serial PCM to a browser on this computer or local network."""

import argparse
import json
import queue
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from serial_pcm_to_wav import FRAMES_PER_PACKET, PacketDecoder


SILENCE = bytes(FRAMES_PER_PACKET * 2)
MAX_QUEUED_PACKETS = 16  # Keep browser latency below about 250 ms.

PAGE = b"""<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DevLab ES8311 microphone</title>
<style>
  body { font: 16px system-ui, sans-serif; max-width: 36rem; margin: 3rem auto;
         padding: 0 1rem; color: #18212b; background: #f6f8fa; }
  main { background: white; border-radius: 1rem; padding: 1.5rem;
         box-shadow: 0 2px 18px #0001; }
  button { font: inherit; padding: .7rem 1rem; margin-right: .5rem; cursor: pointer; }
  label { display: block; margin-top: 1.5rem; }
  input { width: 100%; }
  #status { min-height: 1.5rem; font-weight: 600; }
  small { color: #56616e; }
</style>
<main>
  <h1>ES8311 microphone</h1>
  <p>Live 16 kHz mono audio from the ESP32 serial connection.</p>
  <p><button id="start">Start listening</button><button id="stop" disabled>Stop</button></p>
  <p id="status">Ready</p>
  <label for="gain">Playback gain: <output id="gainValue">1.0</output>x</label>
  <input id="gain" type="range" min="0.5" max="16" step="0.5" value="1">
  <p><small id="health">Checking microphone...</small></p>
  <small>Use headphones if the microphone is near the speakers.</small>
</main>
<script>
const startButton = document.getElementById('start');
const stopButton = document.getElementById('stop');
const statusText = document.getElementById('status');
const gainSlider = document.getElementById('gain');
const gainValue = document.getElementById('gainValue');
let context, gainNode, controller, nextTime = 0, active = false;
let pending = new Uint8Array(0);

gainSlider.oninput = () => {
  gainValue.value = Number(gainSlider.value).toFixed(1);
  if (gainNode) gainNode.gain.value = Number(gainSlider.value);
};

function playBlock(bytes) {
  if (nextTime > context.currentTime + 0.5) return;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const buffer = context.createBuffer(1, 256, 16000);
  const samples = buffer.getChannelData(0);
  for (let i = 0; i < 256; i++) samples[i] = view.getInt16(i * 2, true) / 32768;
  const source = context.createBufferSource();
  source.buffer = buffer;
  source.connect(gainNode);
  source.onended = () => source.disconnect();
  nextTime = Math.max(nextTime, context.currentTime + 0.12);
  source.start(nextTime);
  nextTime += buffer.duration;
}

function consume(chunk) {
  const joined = new Uint8Array(pending.length + chunk.length);
  joined.set(pending);
  joined.set(chunk, pending.length);
  let offset = 0;
  while (joined.length - offset >= 512) {
    playBlock(joined.subarray(offset, offset + 512));
    offset += 512;
  }
  pending = joined.slice(offset);
}

async function stop(message = 'Stopped') {
  active = false;
  if (controller) controller.abort();
  controller = null;
  if (context) await context.close();
  context = null;
  gainNode = null;
  pending = new Uint8Array(0);
  startButton.disabled = false;
  stopButton.disabled = true;
  statusText.textContent = message;
}

startButton.onclick = async () => {
  startButton.disabled = true;
  stopButton.disabled = false;
  active = true;
  pending = new Uint8Array(0);
  nextTime = 0;
  let finalMessage = 'Stopped';
  try {
    context = new AudioContext();
    gainNode = context.createGain();
    gainNode.gain.value = Number(gainSlider.value);
    const highPass = context.createBiquadFilter();
    highPass.type = 'highpass';
    highPass.frequency.value = 100;
    gainNode.connect(highPass).connect(context.destination);
    await context.resume();
    controller = new AbortController();
    statusText.textContent = 'Connecting...';
    const response = await fetch('/pcm', { signal: controller.signal, cache: 'no-store' });
    if (!response.ok || !response.body) throw new Error('Audio stream unavailable');
    statusText.textContent = 'Listening';
    const reader = response.body.getReader();
    while (active) {
      const { value, done } = await reader.read();
      if (done) break;
      consume(value);
    }
    if (active) throw new Error('Audio stream ended');
  } catch (error) {
    if (active) finalMessage = error.message;
  } finally {
    if (active) await stop(finalMessage);
  }
};
stopButton.onclick = () => stop();

async function updateHealth() {
  try {
    const response = await fetch('/health', { cache: 'no-store' });
    const data = await response.json();
    const age = data.last_audio_age_ms === null ? 'waiting' :
      (data.last_audio_age_ms / 1000).toFixed(1) + ' s ago';
    document.getElementById('health').textContent =
      `Serial packets: ${data.packets}; missing: ${data.missing}; ` +
      `invalid: ${data.invalid}; last audio: ${age}` +
      (data.error ? `; serial error: ${data.error}` : '');
  } catch (_) {
    document.getElementById('health').textContent = 'Server unavailable';
  }
}
updateHealth();
setInterval(updateHealth, 2000);
</script>
</html>
"""


class AudioHub:
    def __init__(self):
        self.lock = threading.Lock()
        self.listeners = set()
        self.packets = 0
        self.missing = 0
        self.invalid = 0
        self.resets = 0
        self.previous_sequence = None
        self.last_audio_at = None
        self.error = None

    def subscribe(self):
        listener = queue.Queue(maxsize=MAX_QUEUED_PACKETS)
        with self.lock:
            self.listeners.add(listener)
        return listener

    def unsubscribe(self, listener):
        with self.lock:
            self.listeners.discard(listener)

    def publish(self, sequence, payload):
        with self.lock:
            if self.previous_sequence is not None:
                gap = (sequence - self.previous_sequence - 1) & 0xFFFFFFFF
                if 0 < gap <= 250:
                    self.missing += gap
                    for _ in range(min(gap, MAX_QUEUED_PACKETS)):
                        self._broadcast(SILENCE)
                elif gap > 250:
                    self.resets += 1
            self.previous_sequence = sequence
            self.packets += 1
            self.last_audio_at = time.monotonic()
            self._broadcast(payload)

    def _broadcast(self, payload):
        for listener in self.listeners:
            if listener.full():
                try:
                    listener.get_nowait()
                except queue.Empty:
                    pass
            listener.put_nowait(payload)

    def snapshot(self):
        with self.lock:
            age = None if self.last_audio_at is None else int(
                (time.monotonic() - self.last_audio_at) * 1000
            )
            return {
                'packets': self.packets,
                'missing': self.missing,
                'invalid': self.invalid,
                'resets': self.resets,
                'listeners': len(self.listeners),
                'last_audio_age_ms': age,
                'error': self.error,
            }


def read_serial(source, hub, stop_event):
    decoder = PacketDecoder()
    try:
        while not stop_event.is_set():
            chunk = source.read(source.in_waiting or 1)
            for sequence, payload in decoder.feed(chunk):
                hub.publish(sequence, payload)
            with hub.lock:
                hub.invalid = decoder.bad_packets
    except OSError as exc:
        with hub.lock:
            hub.error = str(exc)


class AudioHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def send_bytes(self, body, content_type):
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/':
            self.send_bytes(PAGE, 'text/html; charset=utf-8')
        elif self.path == '/health':
            body = json.dumps(self.server.hub.snapshot()).encode('utf-8')
            self.send_bytes(body, 'application/json; charset=utf-8')
        elif self.path == '/pcm':
            listener = self.server.hub.subscribe()
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Transfer-Encoding', 'chunked')
                self.end_headers()
                while True:
                    try:
                        payload = listener.get(timeout=0.25)
                    except queue.Empty:
                        payload = SILENCE
                    self.wfile.write(f'{len(payload):X}\r\n'.encode('ascii'))
                    self.wfile.write(payload + b'\r\n')
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                self.server.hub.unsubscribe(listener)
                self.close_connection = True
        else:
            self.send_error(404, 'Not found')

    def log_message(self, format_string, *args):
        if self.path != '/health':
            super().log_message(format_string, *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('serial_port', help='ESP32 port, such as /dev/ttyACM0')
    parser.add_argument('--baud', type=int, default=921600)
    parser.add_argument('--host', default='127.0.0.1',
                        help='HTTP bind address; use 0.0.0.0 for local network access')
    parser.add_argument('--port', type=int, default=8000, help='HTTP port')
    args = parser.parse_args()

    hub = AudioHub()
    stop_event = threading.Event()
    try:
        import serial
        with serial.Serial(args.serial_port, baudrate=args.baud, timeout=0.2) as source:
            reader = threading.Thread(target=read_serial,
                                      args=(source, hub, stop_event), daemon=True)
            reader.start()
            server = ThreadingHTTPServer((args.host, args.port), AudioHandler)
            server.hub = hub
            print(f'Open http://127.0.0.1:{args.port}/ on this computer')
            if args.host == '0.0.0.0':
                print(f'Other devices can use http://<this-computer-IP>:{args.port}/')
            print('Press Ctrl+C to stop')
            try:
                server.serve_forever(poll_interval=0.2)
            except KeyboardInterrupt:
                pass
            finally:
                stop_event.set()
                server.server_close()
                reader.join(timeout=1)
    except (ImportError, OSError) as exc:
        parser.exit(1, f'Could not start bridge: {exc}\n')


if __name__ == '__main__':
    main()
