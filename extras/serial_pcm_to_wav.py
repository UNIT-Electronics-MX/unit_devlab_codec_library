#!/usr/bin/env python3
"""Capture framed DevLab ES8311 serial PCM and save a mono WAV file."""

import argparse
import binascii
import math
import struct
import sys
import time
import wave

MAGIC = b"DLM1"
HEADER = struct.Struct("<4sIHH")
SAMPLE_RATE = 16000
FRAMES_PER_PACKET = 256
PACKET_BYTES = HEADER.size + FRAMES_PER_PACKET * 2


class PacketDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.bad_packets = 0

    def feed(self, chunk):
        self.buffer.extend(chunk)
        packets = []
        while True:
            start = self.buffer.find(MAGIC)
            if start < 0:
                del self.buffer[:-3]
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < HEADER.size:
                break

            _, sequence, frames, expected_crc = HEADER.unpack_from(self.buffer)
            if frames != FRAMES_PER_PACKET:
                self.bad_packets += 1
                del self.buffer[0]
                continue
            if len(self.buffer) < PACKET_BYTES:
                break

            payload = bytes(self.buffer[HEADER.size:PACKET_BYTES])
            if binascii.crc_hqx(payload, 0xFFFF) != expected_crc:
                self.bad_packets += 1
                del self.buffer[0]
                continue
            del self.buffer[:PACKET_BYTES]
            packets.append((sequence, payload))
        return packets


def apply_gain(payload, gain):
    if gain == 1.0:
        return payload
    samples = struct.unpack("<256h", payload)
    amplified = [max(-32768, min(32767, round(sample * gain))) for sample in samples]
    return struct.pack("<256h", *amplified)


def record(port_name, output_name, baud, seconds, gain=1.0):
    import serial
    target_frames = int(seconds * SAMPLE_RATE)
    frames_written = 0
    missing_packets = 0
    resets = 0
    signal_peak = 0
    signal_sum_squares = 0
    signal_frames = 0
    previous_sequence = None
    decoder = PacketDecoder()
    last_packet_at = time.monotonic()

    with serial.Serial(port_name, baudrate=baud, timeout=0.2) as source:
        with wave.open(output_name, "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(SAMPLE_RATE)

            while frames_written < target_frames:
                chunk = source.read(source.in_waiting or 1)
                for sequence, payload in decoder.feed(chunk):
                    last_packet_at = time.monotonic()
                    if previous_sequence is not None:
                        gap = (sequence - previous_sequence - 1) & 0xFFFFFFFF
                        if 0 < gap <= 250:
                            missing_packets += gap
                            silent_frames = min(
                                gap * FRAMES_PER_PACKET,
                                target_frames - frames_written,
                            )
                            output.writeframesraw(b"\x00\x00" * silent_frames)
                            frames_written += silent_frames
                        elif gap > 250:
                            resets += 1
                    previous_sequence = sequence

                    frames = min(FRAMES_PER_PACKET, target_frames - frames_written)
                    if frames:
                        samples = struct.unpack(f"<{frames}h", payload[: frames * 2])
                        signal_peak = max(signal_peak, max(abs(sample) for sample in samples))
                        signal_sum_squares += sum(sample * sample for sample in samples)
                        signal_frames += frames
                        output.writeframesraw(apply_gain(payload, gain)[: frames * 2])
                        frames_written += frames
                    if frames_written >= target_frames:
                        break

                if time.monotonic() - last_packet_at > 10:
                    raise TimeoutError("No valid PCM packets for 10 seconds")

    print(f"Saved {frames_written / SAMPLE_RATE:.2f} s to {output_name}")
    print(
        f"Missing packets: {missing_packets}; invalid packets: "
        f"{decoder.bad_packets}; stream resets: {resets}"
    )
    if signal_frames:
        signal_rms = math.sqrt(signal_sum_squares / signal_frames)
        peak_dbfs = 20 * math.log10(signal_peak / 32768) if signal_peak else float("-inf")
        rms_dbfs = 20 * math.log10(signal_rms / 32768) if signal_rms else float("-inf")
        print(
            f"Input level before digital gain: peak {signal_peak} "
            f"({peak_dbfs:.1f} dBFS), RMS {signal_rms:.1f} ({rms_dbfs:.1f} dBFS)"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port, such as /dev/ttyACM0")
    parser.add_argument("output", nargs="?", default="es8311_microphone.wav")
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--gain", type=float, default=1.0,
                        help="Digital PCM gain applied while saving WAV (default: 1.0)")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")
    if not math.isfinite(args.gain) or args.gain <= 0:
        parser.error("--gain must be a positive finite number")
    try:
        record(args.port, args.output, args.baud, args.seconds, args.gain)
    except (ImportError, TimeoutError, OSError) as exc:
        print(f"Capture failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
