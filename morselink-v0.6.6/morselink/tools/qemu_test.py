#!/usr/bin/env python3
"""
MorseLink — end-to-end test in Espressif's ESP32 emulator (QEMU).

Boots the real firmware image, drives it through the serial console (key
injection, simulated traffic) and checks the log for the expected behaviour.

QEMU has no AXP192, motors or SX1276, so PMIC errors and "no LoRa radio found"
are expected. The test turns the loopback network on: a pretend partner
running its own copy of the link protocol answers instead of the radio, so
the whole stack above the radio driver runs for real (UI state machine,
decoder, calibration, NVS, the protocol engine with its retries, receipts
and keepalives).

    . $IDF_PATH/export.sh
    idf.py build
    python tools/qemu_test.py [--keep-log]

QEMU needs `python $IDF_PATH/tools/idf_tools.py install qemu-xtensa`; on
Linux it also needs libslirp0. About 20 minutes.
"""
import argparse
import base64
import os
import re
import socket
import subprocess
import sys
import time

PORT = 5556
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")

# A minimal eFuse image for the emulator (chip revision 3, no secure boot).
ESP32_EFUSE = bytes.fromhex(
    "00000000000000000000000000800000000000000000100000000000000000000000000000000000"
    + "00" * 80
    + "00" * 80
    + "00000000"
)


class Device:
    """The emulated board: QEMU with its UART on a local TCP port."""

    def __init__(self, log_path):
        self.buf = ""   # everything received so far
        self.pos = 0    # where the next expect() starts looking
        self.log = open(log_path, "w")

    def start(self):
        flash = os.path.join(BUILD, "qemu_flash.bin")
        subprocess.check_call(
            [sys.executable, "-m", "esptool", "--chip=esp32", "merge-bin", f"--output={flash}",
             "--pad-to-size=4MB", "@flash_args"],
            cwd=BUILD, stdout=subprocess.DEVNULL)
        efuse = os.path.join(BUILD, "qemu_efuse.bin")
        with open(efuse, "wb") as f:
            f.write(ESP32_EFUSE)
        # Note: no strap_mode global here: in this QEMU build it boots the
        # ROM's download mode instead of the firmware.
        args = ["qemu-system-xtensa", "-M", "esp32", "-m", "4M",
                "-drive", f"file={flash},if=mtd,format=raw",
                "-drive", f"file={efuse},if=none,format=raw,id=efuse",
                "-global", "driver=nvram.esp32.efuse,property=drive,value=efuse",
                "-global", "driver=timer.esp32.timg,property=wdt_disable,value=true",
                "-nic", "user,model=open_eth", "-nographic",
                "-serial", f"tcp::{PORT},server,nowait", "-monitor", "none"]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                self.sock = socket.create_connection(("127.0.0.1", PORT))
                break
            except OSError:
                time.sleep(0.1)
        self.sock.settimeout(0.1)

    def pump(self, seconds):
        """Collect output for `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                data = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not data:
                break
            text = data.decode("utf-8", "replace")
            self.buf += text
            self.log.write(text)
            self.log.flush()

    def send(self, line):
        """Type a console command."""
        self.log.write(f"\n>>> {line}\n")
        self.sock.sendall((line + "\r\n").encode())

    def expect(self, pattern, timeout, what=None):
        """Wait for a regex to appear after the last match position."""
        end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < end:
            m = rx.search(self.buf, self.pos)
            if m:
                self.pos = m.end()
                return m
            self.pump(0.2)
        raise AssertionError(f"timed out waiting for {what or pattern!r}")

    def expect_all(self, patterns, timeout, what=None):
        """Wait until every regex has appeared after the last match position,
        in any order: lines from different tasks (the UI on one core, the
        radio task on the other) can come out either way round."""
        end = time.time() + timeout
        rxs = [re.compile(p) for p in patterns]
        found = [None] * len(rxs)
        while time.time() < end:
            found = [rx.search(self.buf, self.pos) for rx in rxs]
            if all(found):
                self.pos = max(m.end() for m in found)
                return found
            self.pump(0.2)
        missing = [p for p, m in zip(patterns, found) if not m]
        raise AssertionError(f"timed out waiting for {what or missing!r}")

    def stop(self):
        try:
            self.sock.close()
        finally:
            self.proc.kill()
            self.log.close()


def command(dev, text, echo):
    """Hold 3 s (command mode), key `text`, hold 2 s; wait for the echo."""
    dev.send("key C")
    dev.expect(r"command mode", 10)
    dev.send(f"text -s {text}")
    dev.expect(rf'echoing "{echo}"', 20, f"echo of {text}")


def run(dev):
    results = []

    def step(name, fn):
        t0 = time.time()
        try:
            fn()
            results.append((name, True, ""))
            print(f"  ok    {name}  ({time.time() - t0:.1f}s)")
        except AssertionError as e:
            results.append((name, False, str(e)))
            print(f"  FAIL  {name}: {e}")

    dev.start()

    def boot():
        dev.expect(r"MorseLink [0-9.]+ starting", 20, "boot banner")
        dev.expect(r"no LoRa radio found", 15, "radio probe (none in the emulator)")
        dev.expect(r"first use: keying calibration needed", 15)
        dev.expect(r"key PARIS", 10)
    step("boots; no radio in the emulator; first-use calibration", boot)

    def set_id():
        dev.pump(2)
        dev.send("id r2d")
        dev.expect(r"device ID set to R2D", 5)
    step("console sets the device ID", set_id)

    def loopback():
        dev.send("loopback on")
        dev.expect(r"loopback on", 5)
        dev.expect(r"pretend partner is now TST", 5)
    step("loopback network on (pretend partner)", loopback)

    def bad_paris():
        dev.send("key -u 100 .-- .- .-. .. ...")   # wrong first letter (W): 13 elements
        dev.expect(r"calibration not accepted", 20)
    step("rejects a wrong calibration word", bad_paris)

    def paris():
        dev.pump(9)  # let the re-prompt play (error, attention, PARIS)
        dev.send("key -u 100 .--. .- .-. .. ...")
        m = dev.expect(r"calibrated: dot (\d+) ms .*?dash (\d+) ms", 25, "calibrated")
        dot, dash = int(m.group(1)), int(m.group(2))
        assert 85 <= dot <= 125 and 260 <= dash <= 340, f"dot {dot} dash {dash}"
    step("PARIS calibration accepted", paris)

    def speed():
        dev.pump(2)
        dev.send("speed 9")
        dev.expect(r"reading speed Q9", 5)
    step("reading speed via console", speed)

    def no_recipient():
        dev.send("text -s HI")
        dev.expect(r'draft: "HI"', 15)
        dev.expect(r"no recipient selected: draft kept", 10)
    step("send with no recipient: error, draft kept", no_recipient)

    def connect():
        dev.pump(1)
        command(dev, "SK7Q", "K7Q")
        dev.expect_all([r"asking K7Q to connect", r"tx LINK_REQ R2D->K7Q",
                        r"rx LINK_REPLY K7Q->R2D #\d+ RECEIVED", r"rx LINK_REPLY K7Q->R2D #\d+ ACCEPT",
                        r"tx PAIR_CONFIRM R2D->K7Q", r"recipient: K7Q \(new contact\)", r"K7Q is contact 1"], 25)
    step("S K7Q: request, RECEIVED, ACCEPT, confirm; new contact 1", connect)

    def accept_request():
        dev.pump(5)
        dev.send("sim req XYZ")
        dev.expect_all([r"rx LINK_REQ XYZ->R2D", r"tx LINK_REPLY R2D->XYZ #\d+ RECEIVED", r"XYZ asks to connect"], 10)
        dev.pump(8)  # the alert plays: attention, XYZ, decision pending
        command(dev, "A", "XYZ")
        dev.expect_all([r"accepting XYZ", r"tx LINK_REPLY R2D->XYZ #\d+ ACCEPT", r"rx PAIR_CONFIRM XYZ->R2D",
                        r"recipient: XYZ \(new contact\)", r"tx LINK_REPLY R2D->K7Q #\d+ ENDED"], 25)
    step("incoming request: A echoes the ID, accepts; old partner told it ended", accept_request)

    def decline_request():
        dev.pump(4)
        dev.send("sim req WXY")
        dev.expect(r"WXY asks to connect", 10)
        dev.pump(8)
        command(dev, "N", "WXY")
        dev.expect_all([r"declined and blocked WXY", r"tx LINK_REPLY R2D->WXY #\d+ DECLINE"], 20)
        dev.pump(1)
        dev.send("status")
        dev.expect(r"blocked: WXY", 10)
    step("N declines a stranger's request and blocks them", decline_request)

    def reconnect():
        dev.pump(3)
        command(dev, "S1", "1")     # S echoes its argument as keyed (spec §8)
        dev.expect_all([r"asking K7Q to connect", r"recipient: K7Q\r?\n"], 25, "connected (existing contact)")
    step("S 1 reconnects to contact 1", reconnect)

    def send_receive():
        dev.pump(4)
        dev.send("key S")          # just the send hold: the draft HI kept since the first step
        dev.expect_all([r'sending to K7Q: "HI"', r'tx MSG R2D->K7Q #\d+ "HI"', r"rx RECEIPT K7Q->R2D",
                        r"delivered to K7Q"], 40)
        # Just connected and talking: the echo plays without attention + ID.
        dev.expect(r'message from K7Q: "HI" \(active conversation', 10)
        dev.expect(r'play: "HI"', 20, "the echo played bare")
    step("send the kept draft: announce, MSG, silent receipt, OK; echo arrives, plays bare", send_receive)

    def busy():
        dev.pump(8)
        dev.send("sim req VWX")
        dev.expect(r"tx LINK_REPLY R2D->VWX #\d+ BUSY", 10)
        dev.pump(2)
        dev.send("status")
        dev.expect(r"requests waiting for you: none", 10)
    step("a request during a conversation gets BUSY and waits unplayed", busy)

    def reconnect_after_busy():
        command(dev, "S1", "1")
        dev.expect(r"recipient: K7Q\r?\n", 25)
    step("back to K7Q (the pretend partner had become VWX)", reconnect_after_busy)

    def error_prosign():
        dev.pump(8)                # let the alerts play
        dev.send("text HELLO")
        dev.expect(r'draft: "HELLO"', 20)
        dev.send("key ........")
        dev.expect(r'draft: "HELL" \(deleted one\)', 10)
    step("error prosign deletes the last character", error_prosign)

    def readback():
        dev.pump(2)
        command(dev, "E", "E")
        dev.pump(3)
        dev.send("status")
        dev.expect(r'draft: "HELL"', 10)
    step("E reads back the draft (draft survives command mode)", readback)

    def cancel():
        dev.pump(4)
        dev.send("key X")
        dev.expect(r"cancelled", 12)
        dev.send("status")
        dev.expect(r'draft: ""', 5)
    step("5 s hold cancels everything", cancel)

    def no_receipt():
        dev.pump(2)
        dev.send("sim deaf on")
        dev.expect(r"pretend partner is out of range", 5)
        dev.send("text -s GONE")
        dev.expect(r'sending to K7Q: "GONE"', 30)
        dev.expect_all([r"K7Q did not confirm the message", r'not delivered: back in the draft: "GONE"',
                        r"connection to K7Q lost \(a message got no receipt\)"], 25)
        dev.send("status")
        dev.expect(r"recipient: K7Q \(LOST\)", 10)
        dev.expect(r'draft: "GONE"', 10)
        dev.send("text -s X")      # sending while lost: refused at once, draft kept
        dev.expect(r"K7Q is out of range: draft kept", 20)
        dev.send("sim deaf off")
        dev.expect(r"connection to K7Q restored", 45)
        dev.pump(2)
        dev.send("key X")          # clear the draft
        dev.expect(r"cancelled", 12)
    step("no receipt: error, draft restored, link lost; then restored", no_receipt)

    def silent_partner():
        # The partner switched off or out of range while nothing is being
        # sent: only the missing keepalives tell. (Found 13 Oct: on the
        # boards this never played while loopback was left on.)
        dev.pump(8)                # let the "restored" alert play first
        dev.send("sim deaf on")
        dev.expect(r"pretend partner is out of range", 5)
        dev.expect(r"connection to K7Q lost", 130, "lost after 90 s of silence")
        dev.expect(r'play: \[error\] "K7Q"', 30, "error + K7Q played")
        dev.send("sim deaf off")
        dev.expect(r"connection to K7Q restored", 60)
        dev.expect(r'play: \[ok\] "K7Q"', 30, "OK + K7Q played")
    step("silent partner: lost after 90 s (error + ID), back again (OK + ID)", silent_partner)

    def fail_then_silence():
        # The bug of 15 Oct (mall session): a message fails, then the partner
        # stays silent. Up to v0.5.1 the link went down silently at the
        # failure and nothing was reported ever after. Now: one loss, with
        # its reason, at the failure; nothing more however long the silence;
        # the path says lost; and back when the partner is.
        dev.pump(8)
        dev.send("sim deaf on")
        dev.expect(r"pretend partner is out of range", 5)
        dev.send("text -s ANYONE")
        dev.expect(r'sending to K7Q: "ANYONE"', 30)
        dev.expect(r"connection to K7Q lost \(a message got no receipt\)", 25, "lost: no-receipt")
        start = dev.pos
        dev.pump(100)                # more than the lost time, in silence
        later = dev.buf[start:]
        assert "connection to K7Q lost" not in later, "a second 'lost' for the same loss"
        dev.send("status")
        dev.expect(r"recipient: K7Q \(LOST\)", 10)
        dev.expect(r"paths: LoRa lost \(heard \d+ s ago, they heard us \d+ s ago\) \| relay off \| lost: no-receipt", 10)
        dev.send("sim deaf off")
        dev.expect(r"connection to K7Q restored", 60)
        dev.pump(2)
        dev.send("status")
        dev.expect(r"paths: LoRa connected \(heard \d+ s ago, they heard us \d+ s ago\) \| relay off", 10)
        dev.send("key X")            # clear the draft (ANYONE is back in it)
        dev.expect(r"cancelled", 12)
    step("the 15 Oct bug: a failed message then 100 s of silence: one 'lost' (no-receipt), paths lost; back", fail_then_silence)

    def range_level():
        # G 1-4 = SF9-SF12 (agreed 15 Oct); G alone plays the level.
        dev.pump(8)
        command(dev, "G4", "4")
        dev.expect_all([r"range level 4 = SF12. Set the SAME level on K7Q",
                        r"spreading factor SF12: keepalive every 60 s, lost after 180 s"], 20)
        dev.pump(4)
        command(dev, "G", "G")
        dev.expect(r"range level 4 \(SF12\)", 15)
        dev.pump(3)
        dev.send("radio sf 9")
        dev.expect_all([r"SF9 \(range level 1\)", r"spreading factor SF9: keepalive every 30 s, lost after 90 s"], 10)
        dev.pump(2)
        dev.send("status")
        dev.expect(r"recipient: K7Q \(connected\)", 10, "the link stayed up (both on the same setting)")
    step("G 4 sets SF12 (timing stretched), G alone tells the level, radio sf 9 goes back; the link stays up",
         range_level)

    def sos_general():
        dev.pump(6)
        dev.send("text SOS")
        dev.expect(r"SOS keyed", 15)
        dev.expect(r"SOS armed \(ALL\)", 8)
        dev.expect(r"tx SOS R2D->ALL", 8)
        dev.expect(r"SOS acknowledged by K7Q", 10)
    step("SOS as letters: 3 s confirm, armed, sent, acknowledged", sos_general)

    def off_refused_during_sos():
        dev.pump(2)
        command(dev, "F", "OFF")
        dev.expect(r"your SOS is active: cancel it first", 20)
    step("F is refused while our own SOS is active", off_refused_during_sos)

    def sos_cancel():
        dev.pump(2)
        dev.send("key X")
        dev.expect_all([r"SOS cancelled", r"tx SOS_CANCEL R2D->ALL"], 15)
    step("5 s hold cancels the active SOS (SOS_CANCEL sent)", sos_cancel)

    def sos_targeted_prosign():
        dev.pump(2)
        dev.send("key ...---... / -.- --... --.-")
        dev.expect(r"SOS keyed", 15)
        dev.expect(r'SOS target so far: "K7Q"', 15)
        dev.expect(r"SOS armed \(K7Q\)", 10)
        dev.expect(r"tx SOS R2D->K7Q", 15)
        dev.send("key X")
        dev.expect(r"SOS cancelled", 12)
    step("SOS prosign with a target ID", sos_targeted_prosign)

    def sos_abort():
        dev.pump(2)
        dev.send("text SOS")
        dev.expect(r"SOS armed", 15)
        dev.send("key .")
        dev.expect(r"SOS aborted before sending", 5)
    step("press during the armed buzz aborts the SOS", sos_abort)

    def sos_receive():
        dev.pump(3)
        dev.send("sos ABC")
        dev.expect(r"SOS received from ABC", 5)
        dev.pump(1)
        dev.send("key .")
        dev.expect_all([r"SOS from ABC acknowledged; they are now your recipient", r"tx SOS_ACK R2D->ABC"], 8)
    step("incoming SOS alarm, tap acknowledges and links", sos_receive)

    def sos_unblocks():
        dev.pump(3)
        dev.send("sos WXY")        # WXY was blocked by the N step
        dev.expect(r"SOS received from WXY", 5)
        dev.pump(1)
        dev.send("key .")
        dev.expect_all([r"WXY was blocked: unblocked", r"SOS from WXY acknowledged"], 8)
        dev.pump(1)
        dev.send("status")
        dev.expect(r"blocked: none", 10)
    step("SOS from a blocked device still alarms; acknowledging unblocks it", sos_unblocks)

    def two_alarms():
        dev.pump(3)
        dev.send("sos BCD")
        dev.expect(r"SOS received from BCD", 5)
        dev.send("sos CDE")
        dev.expect(r"SOS received from CDE; will alarm next", 5)
        dev.pump(1)
        dev.send("key .")
        dev.expect(r"SOS from BCD acknowledged", 5)
        dev.expect(r"SOS received from CDE: tap", 10)
        dev.send("key .")
        dev.expect(r"SOS from CDE acknowledged", 5)
        dev.send("sos BCD cancel")
        dev.expect(r"BCD cancelled their SOS", 5)
    step("a second SOS waits for the first to be acknowledged", two_alarms)

    def replay_and_list():
        dev.pump(3)
        dev.send("rx K7Q MEET AT SIX")
        dev.expect(r'message from K7Q: "MEET AT SIX"', 5)
        dev.pump(1)
        command(dev, "L", "L")
        dev.pump(1)
        dev.send("status")
        dev.expect(r"contacts: 1=K7Q 2=XYZ", 10)
    step("incoming message queued; L lists contacts", replay_and_list)

    def tap_to_stop():
        dev.pump(8)
        dev.send("verbose on")
        dev.send("rx K7Q THIS IS A LONG MESSAGE TO INTERRUPT")
        dev.expect(r"message from K7Q", 5)
        # alerts play in order: an earlier queued message may play first.
        # K7Q's MEET AT SIX just arrived, so this one continues the
        # conversation: no attention + ID.
        dev.expect(r'play: "THIS IS', 40, "alert playback")
        dev.pump(1.5)
        dev.send("key .")
        dev.pump(3)
        dev.send("status")
        dev.expect(r"state: idle", 5)
        dev.expect(r'draft: ""', 5)
    step("a tap that only stops playback is not keyed", tap_to_stop)

    def sos_during_playback():
        dev.send("rx K7Q ANOTHER LONG MESSAGE FOR TESTING")
        dev.expect(r'play: "ANOTHER', 20, "alert playback")
        dev.pump(1)
        dev.send("key ...---...")
        dev.expect(r"SOS keyed", 10)
        dev.send("key X")
        dev.expect(r"cancelled", 12)
        dev.send("verbose off")
    step("SOS keyed over a playing message is recognised", sos_during_playback)

    def conversation():
        dev.pump(3)
        dev.send("verbose on")
        dev.send("rx XYZ HELLO")   # someone else, mid-conversation with K7Q: announced
        dev.expect(r'message from XYZ: "HELLO"\r?\n', 5)
        dev.expect(r'play: \[attention\] "XYZ" "HELLO"', 30, "announced in full")
        dev.pump(1)
        dev.send("rx XYZ AGAIN")   # XYZ again a moment later: bare
        dev.expect(r'message from XYZ: "AGAIN" \(active conversation', 5)
        dev.expect(r'play: "AGAIN"', 30, "played bare")
        dev.pump(1)
        dev.send("rx K7Q BACK")    # K7Q after XYZ: announced again
        dev.expect(r'message from K7Q: "BACK"\r?\n', 5)
        dev.expect(r'play: \[attention\] "K7Q" "BACK"', 30, "announced in full")
        dev.pump(1)
        dev.send("status")
        dev.expect(r"conversation: K7Q, last exchange \d+ s ago: their messages play without attention", 10)
        dev.send("verbose off")
    step("5-minute rule: another sender is announced, then continues bare; switching back announces", conversation)

    def unknown_command():
        dev.pump(12)
        dev.send("key C")
        dev.expect(r"command mode", 10)
        dev.send("text -s Q0")      # Q needs 1-9
        dev.expect(r'command "Q0" not understood', 15)
        dev.pump(2)
        dev.send("key C")           # (v0.6.4) the 3 s hold again leaves command mode
        dev.expect(r"left command mode \(3 s hold\)", 10)
    step("invalid command argument rejected; the 3 s hold again leaves command mode", unknown_command)

    # Discovery (D). The pretend partner is CDE now (it follows our link,
    # which the SOS steps moved). It answers our sweep heard strongly (near);
    # two more pretend devices answer further off: M4X (mid) and F9Z (far).

    def discover_list():
        dev.pump(3)
        dev.send("verbose on")
        command(dev, "D", "D")
        dev.expect_all([r"discovering: who is around", r"tx DISCOVER R2D->ALL"], 15)
        dev.expect(r"discovery: 3 found", 10)
        dev.expect_all([r"1\. CDE near: path loss 62 dB", r"2\. M4X mid: path loss 100 dB",
                        r"3\. F9Z far: path loss 130 dB"], 5)
        dev.expect(r'play: "CDE N"', 5, "the first entry: CDE, near")
    step("D: everyone answers in a slot; the list plays near, mid, far", discover_list)

    def discover_invite():
        dev.send("key ..")          # double tap: the next one
        dev.expect(r'play: "M4X M"', 15, "the second entry")
        dev.send("key .")           # tap: invite it
        dev.expect(r'discovery: inviting M4X: echoing "M4X"', 10)
        dev.expect_all([r"asking M4X to connect", r"tx LINK_REQ R2D->M4X", r"recipient: M4X \(new contact\)",
                        r"M4X is contact 3"], 30)
    step("D: double tap skips, tap echoes the ID and invites (as S)", discover_invite)

    def discover_stop():
        dev.pump(8)                 # OK + 3 plays
        command(dev, "D", "D")      # M4X is the pretend partner now: near, listed once
        dev.expect(r"discovery: 2 found", 15)
        dev.expect(r'play: "M4X N"', 5)
        dev.send("key -")           # not an answer here
        dev.expect(r'play: \[error\] "M4X N"', 10, "error, then the same entry again")
        dev.send("key S")           # 2 s hold: stop
        dev.expect(r"discovery: list closed \(stopped with a hold\)", 10)
    step("D: a wrong key replays the entry; a 2 s hold stops the list", discover_stop)

    def discover_silent():
        dev.pump(3)
        command(dev, "Z", "Z")
        dev.expect(r"silent mode on", 10)
        dev.pump(3)
        dev.send("sim disc")
        m1 = dev.expect(r"rx DISCOVER M4X->ALL", 10)
        m2 = dev.expect(r"pretend partner M4X's sweep found nobody", 10)
        assert "tx PRESENCE" not in dev.buf[m1.end():m2.start()], "answered in silent mode"
        dev.send("status")
        dev.expect(r"discovery: silent mode: not answering; sweeps answered 0, left unanswered 1", 10)
        command(dev, "Z", "Z")
        dev.expect(r"silent mode off", 10)
        dev.pump(3)
        dev.send("sim disc")
        dev.expect_all([r"tx PRESENCE R2D->M4X",
                        r"pretend partner M4X's sweep found R2D, near \(path loss 62 dB\)"], 15)
    step("Z: silent mode leaves other people's sweeps unanswered; off again, answered", discover_silent)

    def discover_nobody():
        dev.pump(3)
        dev.send("sim deaf on")
        dev.expect(r"out of range", 5)
        command(dev, "D", "D")
        dev.expect(r"discovery: 0 found", 15)
        dev.expect(r'play: \[error\] "0"', 5, "error + 0")
        dev.send("sim deaf off")
        dev.expect(r"back in range", 5)
        dev.send("verbose off")
    step("D with nobody in range: error + 0", discover_nobody)

    # The phone (BLE). The emulator has no Bluetooth (starting it there stops
    # the emulated chip), so the console's pretend phone stands in for it:
    # every line for the phone shows up as "ble: phone << ...", and `phone
    # send ...` types a line as if the phone had written it. The partner is
    # M4X (invited from D); the contacts are K7Q, XYZ and M4X.

    def packet(ptype, src, dst, pid, payload=b"", flags=0):
        """A packet as the radio (or MQTT) carries it (proto.h)."""
        def addr(id3):
            v = 0
            for ch in id3:
                v = v * 31 + "23456789ABCDEFGHJKMNPQRSTUVWXYZ".index(ch)
            return v
        return (bytes([(1 << 4) | ptype, flags]) + addr(src).to_bytes(3, "big") + addr(dst).to_bytes(3, "big")
                + pid.to_bytes(2, "big") + payload)

    def phone_hello():
        dev.pump(2)
        dev.send("phone")
        dev.expect(r"phone: BLE off \(O pairs a phone", 10)
        dev.send("phone sim on")
        dev.expect(r"ble: phone << HI R2D 4 [0-9.]+\r?\n", 5, "HI (protocol 4)")
        dev.expect(r"ble: phone << ST M4X connected \? 0 connected (checking|off)\r?\n", 5,
                   "ST, LoRa connected, the relay checking (or still off: the phone just came)")
        dev.expect(r"ble: phone << RD 9 125 22\r?\n", 5, "RD: SF9, 125 kHz, 22 dBm")
        dev.expect(r"ble: phone << DR\r?\n", 5, "DR (no draft)")
        dev.expect(r"ble: phone << SP 1 0 (\d+ ){7}(healthy|marginal) 9 120 10 8 0 1\r?\n", 5,
                   "SP: calibrated, the timings, Q9 (120 ms), strength I8, no power chip (0 mV), the sweep")
        dev.send("phone send HI")
        dev.expect_all([r"ble: phone >> HI", r"ble: phone << HI R2D", r"ble: phone << ST M4X"], 5)
    step("phone (pretend): HI, status and the empty draft; HI from the phone repeats them", phone_hello)

    def phone_live_draft():
        dev.pump(1)
        dev.send("text OK")
        dev.expect(r"ble: phone << DR O\r?\n", 15, "DR O")
        dev.expect(r"ble: phone << DR OK\r?\n", 15, "DR OK")
    step("phone: the draft follows the keying letter by letter (live compose)", phone_live_draft)

    def phone_learning_view():
        # The learning view (protocol 4, v0.6.2): every press as KY (the
        # letter so far, its length, the boundary), the closed letter as KC
        # before its DR, the error prosign as KC error; E's echo and the
        # draft's read-back as PL lines, each ended by PL end; I with no power
        # chip (the emulator has none): error, nothing stored.
        dev.pump(2)
        dev.send("key -u 100 .-")
        dev.expect_all([r"ble: phone << KY key \. \d+ \d+\r?\n", r"ble: phone << KY key \.- \d+ \d+\r?\n",
                        r"ble: phone << KC char \.- A\r?\n", r"ble: phone << DR OK ?A\r?\n"], 15)
        # (no KY alone here: the KC already told the phone the letter closed)
        dev.send("key -u 100 ........")
        dev.expect_all([r"ble: phone << KY key \.{8} \d+ \d+\r?\n", r"ble: phone << KC error \.{8}\r?\n",
                        r"ble: phone << DR OK ?\r?\n"], 20, "the error prosign took the A back")
        dev.pump(2)
        mark = dev.pos
        command(dev, "E", "E")   # (the echo's PL line goes just before the "echoing" log line command() waits for)
        assert re.search(r"ble: phone << PL 0 0 120 10 E\r?\n", dev.buf[mark:]), "the echo as PL"
        dev.expect(r"ble: phone << PL end\r?\n", 10, "the echo's end")
        dev.expect(r"ble: phone << PL 0 0 120 10 OK\r?\n", 10, "the draft read back")
        dev.expect(r"ble: phone << PL end\r?\n", 10, "... and its end")
        dev.pump(2)
        # I (1-9 since v0.6.4): below I8 the drive changes, which needs no power chip
        command(dev, "I3", "3")
        dev.expect(r"vibration strength I3: drive 59 %, motors at 0 mV", 10)
        dev.pump(3)
        dev.send("haptic strength 2")
        dev.expect(r"strength: I2 \(drive 52 %\), motor supply \(DCDC1\) 0 mV", 5)
        dev.send("haptic strength 8")
        dev.expect(r"strength: I8 \(drive 100 %\), motor supply \(DCDC1\) 0 mV", 5)
        # H (v0.6.3): dashes on M2 only, a sample A in the new style, SP says 2; H1 back to the sweep
        dev.pump(2)
        dev.send("verbose on")
        dev.expect(r"verbose log on", 5)
        command(dev, "H2", "2")
        dev.expect_all([r"dashes H2: on M2 \(forearm\) only", r'play: \[ok\] "A"',
                        r"ble: phone << SP 1 0 (\d+ ){7}\w+ 9 120 10 8 0 2\r?\n"], 15)
        dev.pump(3)
        dev.send("status")
        dev.expect(r"dashes: H2 \(M2 only, dots on M1\)", 5)
        dev.send("haptic dash 1")
        dev.expect(r"dashes: H1 \(sweep M1 -> M2\)", 5)
        dev.send("verbose off")
        dev.expect(r"verbose log off", 5)
    step("phone: the learning view: KY / KC per press and letter, PL for what plays; I without a power chip",
         phone_learning_view)

    def phone_calibration_view():
        # C with the phone on: SP says "calibrating", the prompt as PL
        # (PARIS), every press of the attempt as KY cal, then CB ok and KY.
        dev.pump(3)
        command(dev, "C", "C")
        dev.expect_all([r"ble: phone << PL 0 \d+ 120 10 PARIS\r?\n",
                        r"ble: phone << SP 1 1 (\d+ ){7}\w+ 9 120 10 8 0 [12]\r?\n"], 15)
        dev.pump(9)                 # let the prompt play (attention, PARIS)
        dev.send("key -u 100 .--. .- .-. .. ...")
        dev.expect_all([r"ble: phone << KY cal \. \d+ \d+\r?\n", r"ble: phone << KY cal \.--\.\.-\.-\.{6} \d+ \d+\r?\n",
                        r"ble: phone << CB ok\r?\n", r"calibrated: dot"], 25)
        dev.expect_all([r"ble: phone << KY\r?\n", r"ble: phone << SP 1 0 "], 10, "KY alone; SP no longer calibrating")
        dev.send("status")
        dev.expect(r'draft: "OK ?"', 10, "the draft survived the recalibration")
    step("phone: recalibration as the phone sees it: SP calibrating, PL PARIS, KY cal per press, CB ok",
         phone_calibration_view)

    def phone_command_mode():
        # The app shows command mode in its keying box (agreed 15 Oct): CM
        # with what is keyed, CE when it ends; the draft stays.
        dev.pump(2)
        dev.send("key C")
        dev.expect_all([r"command mode \(draft kept\)", r"ble: phone << CM\r?\n"], 10)
        dev.send("text -s Q9")       # (the speed set earlier: nothing changes)
        dev.expect_all([r"ble: phone << CM Q\r?\n", r"ble: phone << CM Q9\r?\n", r"ble: phone << CE done Q9\r?\n",
                        r"reading speed Q9"], 25)
        dev.pump(3)
        dev.send("key C")
        dev.expect(r"ble: phone << CM\r?\n", 10)
        dev.send("text -s Q0")       # not understood: still in command mode
        dev.expect_all([r"ble: phone << CE error Q0\r?\n", r"ble: phone << CM\r?\n"], 25)
        dev.pump(32)                 # 30 s untouched: command mode ends
        dev.expect(r"ble: phone << CE timeout", 10)
        dev.send("status")
        dev.expect(r'draft: "OK ?"', 10, "the draft survived")   # (the pause after it added a word space)
        # (v0.6.4) the 3 s hold in command mode leaves it, the draft kept (CE left)
        dev.pump(1)
        dev.send("key C")
        dev.expect(r"ble: phone << CM\r?\n", 10)
        dev.send("key C")
        dev.expect_all([r"ble: phone << CE left\r?\n", r"left command mode \(3 s hold\); draft kept"], 10)
        # K keeps the draft past 60 s untouched
        dev.pump(2)
        command(dev, "K", "K")
        dev.expect(r'draft "OK ?" kept: no time limit', 10)
        dev.pump(65)
        rest = dev.buf[dev.pos:]
        assert "discarded after" not in rest, "a kept draft was discarded"
        dev.send("status")
        dev.expect_all([r'draft: "OK ?"', r"draft kept \(K\): yes"], 10, "the kept draft after 65 s")
    step("phone: command mode as CM lines, CE done / error / timeout / left (3 s); the draft kept; K keeps it past 60 s",
         phone_command_mode)

    def phone_message_out():
        dev.pump(1)
        dev.send("key S")
        found = dev.expect_all([r"ble: phone << TX (\d+) M4X OK\r?\n", r'tx MSG R2D->M4X #\d+ "OK"',
                                r"net: phone << MSG R2D->M4X #\d+ \(topic M4X\)",
                                r"ble: phone << RL M4X [A-Za-z0-9+/]+=*\r?\n"], 30)
        n = found[0].group(1)
        dev.expect(rf"ble: phone << OK {n}\r?\n", 20, f"OK {n}")
        dev.expect(r"ble: phone << RX M4X OK\r?\n", 20, "the echo, as RX")
    step("phone: a sent message as TX, a copy relayed (RL), then OK; the echo as RX", phone_message_out)

    def phone_message_failed():
        dev.pump(3)
        dev.send("sim deaf on")
        dev.expect(r"pretend partner is out of range", 5)
        dev.send("text -s LOST")
        m = dev.expect(r"ble: phone << TX (\d+) M4X LOST\r?\n", 30, "TX")
        n = m.group(1)
        dev.expect(rf"ble: phone << NO {n}\r?\n", 30, f"NO {n}")
        dev.expect_all([r"ble: phone << ST M4X lost", r"ble: phone << DR LOST\r?\n",
                        r"ble: phone << EV LOST M4X no-receipt"], 5,
                       "ST says lost, an EV says why; the text is back in the draft")
        dev.send("sim deaf off")
        dev.expect_all([r"connection to M4X restored", r"ble: phone << EV RESTORED M4X",
                        r"ble: phone << ST M4X connected"], 45)
        dev.pump(2)
        dev.send("key X")           # clear the draft
        dev.expect(r"cancelled", 12)
    step("phone: an undelivered message as NO; ST and the draft follow, then RESTORED", phone_message_failed)

    def phone_relay_in():
        dev.pump(3)
        line = "phone send RL " + base64.b64encode(packet(7, "K7Q", "R2D", 0xBEEF, b"VIA PHONE")).decode()
        dev.send(line)
        dev.expect_all([r"net: phone >> MSG K7Q->R2D #48879", r'message from K7Q: "VIA PHONE"',
                        r"tx RECEIPT R2D->K7Q", r"net: phone << RECEIPT R2D->K7Q #\d+ \(topic K7Q\)",
                        r"ble: phone << RX K7Q VIA PHONE"], 15)
        dev.send(line)              # the same packet again (as if LoRa had brought it too)
        dev.expect(r"net: phone >> MSG K7Q->R2D #48879", 10)
        dev.pump(3)
        assert 'message from K7Q: "VIA PHONE"' not in dev.buf[dev.pos:], "the second copy was taken as new"
    step("phone: a packet relayed in (RL) is received like a radio one, once; its receipt goes both ways",
         phone_relay_in)

    def phone_sos_out():
        dev.pump(10)                # the alerts (OK, the echo, the message) play first
        dev.send("text SOS")
        dev.expect(r"SOS armed \(ALL\)", 20)
        dev.expect_all([r"tx SOS R2D->ALL", r"ble: phone << EV SOS_SENT ALL",
                        r"net: phone << SOS R2D->ALL #\d+ \(topic K7Q\)",
                        r"net: phone << SOS R2D->ALL #\d+ \(topic XYZ\)",
                        r"net: phone << SOS R2D->ALL #\d+ \(topic M4X\)"], 15)
        dev.expect(r"ble: phone << EV SOS_ACK M4X", 15)
        dev.send("key X")
        dev.expect_all([r"SOS cancelled", r"ble: phone << EV SOS_STOPPED ALL",
                        r"net: phone << SOS_CANCEL R2D->ALL #\d+ \(topic K7Q\)"], 15)
    step("phone: a general SOS also goes to every contact's topic; its ack and cancel reach the phone", phone_sos_out)

    def phone_sos_in():
        dev.pump(4)
        dev.send("sos BCD")
        dev.expect_all([r"SOS received from BCD", r"ble: phone << EV SOS BCD"], 5)
        dev.pump(1)
        dev.send("sos BCD cancel")
        dev.expect_all([r"BCD cancelled their SOS", r"ble: phone << EV SOS_CANCEL BCD"], 5)
    step("phone: someone else's SOS and its cancellation reach the phone", phone_sos_in)

    def phone_gone():
        dev.pump(6)
        dev.send("phone sim off")
        dev.expect(r"ui: phone gone", 5)
        dev.send("phone send HI")
        dev.expect(r"phone sim is off: line ignored", 5)
        dev.send("status")
        dev.expect(r"phone: BLE off", 10)
        m = dev.expect(r"packets through the phone: out (\d+), in (\d+)", 5)
        assert int(m.group(1)) >= 5 and int(m.group(2)) == 2, m.group(0)
    step("phone: gone again; status counts the packets that went through it", phone_gone)

    # Test mode (docs/TEST_PLAN.md). Started from the console with loopback
    # on, so the pretend partner M4X answers the L copies over "LoRa"; the
    # pretend phone gets the R copies, and the test answers one of them as
    # the other board would, with a receipt through the phones.

    def test_refused():
        dev.pump(2)
        dev.send("phone sim on")
        dev.expect(r"ble: phone << HI R2D", 5)
        dev.send("phone send TEST START test=dynamic paths=both role=rover session=S1 seed=5")
        dev.expect(r"T,0,\d+,REFUSED,reason=loopback", 10, "a field test from the phone needs the radio")
    step("test mode: the phone can't start a test while loopback is on", test_refused)

    test_pos = {}

    def test_mode():
        dev.pump(1)
        test_pos["start"] = len(dev.buf)
        dev.send("test start dynamic both rover")
        dev.expect_all([r"T,1,\d+,START,session=console,role=rover,test=dynamic,paths=both,seed=1,tx_dbm=\d+,"
                        r"sf=9,fw=[0-9.]+,own=R2D,peer=M4X,lb=1", r"ui: test mode on \(dynamic\)"], 10)
        m = dev.expect(r"T,\d+,\d+,GEN,seq=1,size=(\d+),lora=queued,relay_id=(\d+)", 15, "the first message")
        rid = int(m.group(2))
        dev.expect_all([r"T,\d+,\d+,TX,type=MSG,dst=M4X,id=\d+,copy=1,bytes=\d+,air_ms=\d+,.*,seq=1,path=L",
                        rf"T,\d+,\d+,ROUT,type=MSG,dst=M4X,id={rid},bytes=\d+",
                        r"ble: phone << RL M4X [A-Za-z0-9+/]+=*\r?\n",
                        r"T,\d+,\d+,RX,type=RECEIPT,src=M4X,dst=R2D,id=\d+,bytes=\d+,rssi=-?\d+,snr=-?[0-9.]+,dup=0",
                        r"T,\d+,\d+,ACK,path=L,seq=1,id=\d+,size=\d+,rtt_ms=\d+,rtt_gen_ms=\d+,tries=1"], 15)
        # The other board's answer to the R copy: a receipt through the phones.
        rcpt = packet(8, "M4X", "R2D", 0x4321, rid.to_bytes(2, "big"), flags=0x08)
        dev.send("phone send RL " + base64.b64encode(rcpt).decode())
        dev.expect_all([r"T,\d+,\d+,RIN,type=RECEIPT,src=M4X,id=17185,bytes=12,dup=0",
                        rf"T,\d+,\d+,ACK,path=R,seq=1,id={rid},size=\d+,rtt_ms=\d+"], 10)
        # The echo of the L copy arrives as a message: test mode's, not played.
        dev.expect(r"T,\d+,\d+,RX,type=MSG,src=M4X,dst=R2D,.*,seq=1,path=L", 10)
    step("test mode: a message as an L copy (LoRa, its receipt) and an R copy (phones, its receipt)", test_mode)

    def test_mode_lost():
        # The 15 Oct mall session: during a test, LoRa went and nothing said
        # so. Now an L copy without a receipt brings a LINK line with the
        # reason and PATH lines, and the phone's status says lost (its
        # partner line changes); the L copies are then skipped as lost.
        dev.pump(1)
        dev.send("sim deaf on")
        dev.expect(r"pretend partner is out of range", 5)
        dev.expect(r"T,\d+,\d+,FAIL,path=L,seq=\d+,id=\d+,size=\d+,tries=\d+,why=no-receipt", 60, "an L copy fails")
        dev.expect_all([r"T,\d+,\d+,LINK,state=lost,why=no-receipt,heard_ago_ms=\d+",
                        r"T,\d+,\d+,PATH,path=L,state=(they-cant-hear|lost),heard_s=-?\d+,they_s=-?\d+",
                        r"ble: phone << ST M4X lost"], 10)
        dev.expect(r"T,\d+,\d+,GEN,seq=\d+,size=\d+,lora_skip=lost", 30, "the next L copy skipped as lost")
        dev.send("sim deaf off")
        dev.expect_all([r"T,\d+,\d+,LINK,state=restored", r"T,\d+,\d+,PATH,path=L,state=connected",
                        r"ble: phone << ST M4X connected"], 60)
    step("test mode: losing LoRa is a LINK line with its reason, PATH lines, ST lost; back again", test_mode_lost)

    def test_mode_hold():
        dev.pump(2)
        dev.send("key X")           # the 5 s hold: the only key test mode listens to
        dev.expect(r"5 s hold: stopping the test", 10)
        dev.expect_all([r"ui: test mode off", r"T,\d+,\d+,STOP,reason=hold,gen=\d+,ack_l=\d+,fail_l=\d+,skip_l=\d+,"
                        r"ack_r=\d+,fail_r=\d+,skip_r=\d+"], 45)
        played = re.findall(r"play: ", dev.buf[test_pos["start"]:dev.pos])
        blocked = re.findall(r"test mode: nothing plays", dev.buf[test_pos["start"]:dev.pos])
        assert not played, f"{len(played)} playbacks during the test"
        dev.pump(3)
        dev.send("status")
        dev.expect(r"MorseLink R2D \| state: idle", 10)
        dev.send("phone sim off")
        dev.expect(r"ui: phone gone", 5)
        print(f"        (attempts to play during the test, all stopped: {len(blocked)})")
    step("test mode: nothing plays; a 5 s hold stops it (STOP with the totals); normal use after", test_mode_hold)

    def rundown_m():
        # The motor night (TEST_PLAN §14): PARIS loops on the motors, the
        # run-down log keeps each motor's on-time per minute, and `test dump`
        # starts with the run's details. Stopped after the first sample.
        dev.pump(2)
        dev.send("test start rundown-m")
        dev.expect_all([r"T,1,\d+,START,session=console,role=rover,test=rundown-m,paths=both,seed=1,tx_dbm=\d+,"
                        r"sf=9,fw=[0-9.]+,own=R2D,peer=M4X,lb=1",
                        r"ui: test mode on \(rundown-m\): PARIS on the motors"], 10)
        dev.pump(12)                # the first sample is taken 5 s in; PARIS keeps playing
        dev.send("test stop")
        dev.expect_all([r"ui: test mode off", r"T,\d+,\d+,STOP,reason=console,gen=0,"], 45)
        dev.pump(2)
        dev.send("test dump")
        dev.expect(r"# morselink-rundown board=R2D peer=M4X test=rundown-m paths=both role=rover session=console "
                   r"fw=[0-9.]+ tx_dbm=\d+ sf=9 speed=\d unit_ms=\d+ m1_pct=100 m2_pct=100 motor_log=1 end=console "
                   r"samples=1\r?\n", 10)
        dev.expect(r"# one sample a minute; m1_ms/m2_ms = motor on-time in that minute; ended: console\r?\n", 5)
        dev.expect(r"minute,mv,charging,m1_ms,m2_ms\r?\n", 5)
        m = dev.expect(r"0,\d+,[01],(\d+),(\d+)\r?\n", 5, "the first sample")   # the row right after the column line
        m1, m2 = int(m.group(1)), int(m.group(2))
        print(f"        (first minute's sample: M1 on {m1} ms, M2 on {m2} ms in the first 5 s)")
        assert 0 < m1 <= 5000 and m1 % 250 == 0 and m2 % 250 == 0, f"m1_ms {m1}, m2_ms {m2}"
    step("test mode: rundown-m plays PARIS; the dump has the header and each motor's on-time", rundown_m)

    def rundown_l():
        # The LoRa-only run-down (agreed 15 Oct): LoRa test traffic, no relay
        # copies (Bluetooth is off for the run), logged like the others.
        dev.pump(2)
        dev.send("test start rundown-l both")
        dev.expect_all([r"T,1,\d+,START,session=console,role=rover,test=rundown-l,paths=lora,seed=1,tx_dbm=\d+,sf=9,",
                        r"ui: test mode on \(rundown-l\)"], 10)
        dev.expect(r"T,\d+,\d+,GEN,seq=1,size=\d+,lora=queued,relay=off", 15, "a LoRa copy only")
        dev.expect(r"T,\d+,\d+,ACK,path=L,seq=1,", 15)
        dev.send("test stop")
        dev.expect_all([r"ui: test mode off", r"T,\d+,\d+,STOP,reason=console,"], 45)
        dev.pump(2)
        dev.send("test dump")
        dev.expect(r"# morselink-rundown board=R2D peer=M4X test=rundown-l paths=lora role=rover session=console "
                   r"fw=[0-9.]+ tx_dbm=\d+ sf=9 ", 10)
    step("test mode: rundown-l sends LoRa copies only; its dump says so", rundown_l)

    def point_test():
        # The point test (v0.6.1): a 2-step plan with 2 messages a step (n=2:
        # 55 s at SF9, 70 s at SF10). The pretend partner follows the SF
        # (loopback switches both engines). Its settings apply for the test
        # only (RD lines to the pretend phone show them, then the board's own
        # again), it keeps sending while the link is lost, and `radio sf` is
        # refused meanwhile.
        dev.pump(2)
        dev.send("phone sim on")
        dev.expect(r"ble: phone << HI R2D 4", 5)
        dev.send("test start test=point plan=9:17,10:22 n=2 role=rover session=console seed=1")
        dev.expect_all([r"T,1,\d+,START,session=console,role=rover,test=point,paths=lora,seed=1,tx_dbm=22,sf=9,"
                        r"fw=[0-9.]+,own=R2D,peer=M4X,lb=1,plan=9:17/10:22,n=2,size=25",
                        r"T,2,\d+,STEP,i=1,sf=9,dbm=17,n=2,step_ms=55000", r"ui: test mode on \(point\)",
                        r"net: transmit power 17 dBm", r"ble: phone << RD 9 125 17"], 10)
        dev.send("radio sf 11")
        dev.expect(r"not during a test", 5)
        dev.expect(r"T,\d+,\d+,GEN,seq=1,size=25,step=1,lora=queued,relay=off", 15)
        dev.expect(r"T,\d+,\d+,ACK,path=L,seq=1,", 15)
        dev.expect(r"T,\d+,\d+,GEN,seq=2,size=25,step=1,lora=queued,relay=off", 20)
        dev.expect_all([r"T,\d+,\d+,STEP,i=2,sf=10,dbm=22,n=2,step_ms=70000", r"net: spreading factor SF10",
                        r"ble: phone << RD 10 125 22"], 45, "step 2 at 55 s: SF10, 22 dBm")
        dev.send("sim deaf on")      # out of range: the step's messages still go out
        dev.expect(r"T,\d+,\d+,GEN,seq=3,size=25,step=2,lora=queued", 15)
        dev.expect(r"T,\d+,\d+,FAIL,path=L,seq=3,.*why=no-receipt", 30)
        dev.expect(r"T,\d+,\d+,LINK,state=lost,why=no-receipt", 5)
        dev.expect(r"T,\d+,\d+,GEN,seq=4,size=25,step=2,lora=queued,relay=off", 20, "sent although the link is lost")
        dev.send("sim deaf off")
        dev.expect(r"T,\d+,\d+,ACK,path=L,seq=4,", 25, "delivered at the edge")
        dev.expect_all([r"T,\d+,\d+,STEP,i=0,sf=9,dbm=22,restored=1",
                        r"T,\d+,\d+,STOP,reason=done,gen=4,ack_l=3,fail_l=1,skip_l=0,", r"ble: phone << RD 9 125 22",
                        r"ui: test mode off"], 60, "the plan over: the board's own setting back, done")
        dev.send("phone sim off")
        dev.expect(r"ui: phone gone", 5)
    step("point test: a 2-step plan, settings for the test only, sending while lost, done", point_test)

    def bluetooth_w():
        # W (v0.6.1): Bluetooth off tells the phone first (EV BLE_OFF), OK +
        # OFF; with no phone paired (the emulator has none), W again can't
        # turn it on: error. The emulator has no Bluetooth: the pretend phone
        # stands in (it goes, as a real one would). The play log is off after
        # the earlier steps (`verbose off`): on for this step, off again after.
        dev.pump(2)
        dev.send("verbose on")
        dev.expect(r"verbose log on", 5)
        dev.send("phone sim on")
        dev.expect(r"ble: phone << HI R2D 4", 5)
        command(dev, "W", "W")
        dev.expect_all([r"ble: phone << EV BLE_OFF R2D", r"Bluetooth off \(W\)", r'play: \[ok\] "OFF"',
                        r"ui: phone gone"], 15)
        dev.pump(3)
        command(dev, "W", "W")
        dev.expect_all([r"Bluetooth: no phone is paired", r"play: \[error\]"], 15, "W with no phone paired: error")
        dev.pump(2)
        dev.send("phone on")
        dev.expect(r"no phone is paired: O \(or `phone pair`\) pairs one", 5)
        dev.send("verbose off")
        dev.expect(r"verbose log off", 5)
    step("W: Bluetooth off (EV BLE_OFF, OK + OFF); on again needs a paired phone", bluetooth_w)

    def bench_from_phone():
        # The meter session started from the phone (BENCH POWER, v0.6.1): BN
        # lines with the total, each step as it starts (1, then the part 1b
        # after its short buzz), refused while running, stopped by BENCH STOP.
        dev.pump(2)
        dev.send("phone sim on")
        dev.expect(r"ble: phone << HI R2D 4", 5)
        dev.send("phone send BENCH POWER")
        dev.expect_all([r"ble: phone >> BENCH POWER", r"bench power \(started from the phone\)",
                        r"ble: phone << BN start \d+ 22", r"bench power: USB out; step 1 in 5 s"], 10)
        dev.send("phone send BENCH POWER")
        dev.expect(r"ble: phone << BN refused running", 5)
        dev.expect_all([r"ble: phone << BN step 1 60", r"bench power step 1: processor only"], 15)
        dev.expect_all([r"ble: phone << BN step 1b 45", r"bench power step 1b: \+ keying"], 70, "1b after 60 s")
        dev.send("phone send BENCH STOP")
        dev.expect_all([r"ble: phone << BN end app", r"bench power stopped \(app\)"], 10)
        dev.send("phone sim off")
        dev.expect(r"ui: phone gone", 5)
    step("bench power from the phone: BN start / step / refused / end", bench_from_phone)

    def bench_tests():
        # bench motor plays the phyphox pattern (stopped early); bench power
        # prints its plan (the emulator has no power chip: "USB out" at once)
        # and runs step 1, then `bench stop` puts everything back.
        dev.pump(2)
        dev.send("bench motor m1")
        dev.expect(r"bench motor: M1 plays the pattern for about \d+ s", 10)
        dev.pump(3)
        dev.send("bench stop")
        dev.pump(2)
        # (v0.6.6) the sweep: block I1 starts; bench stop ends it and the strength set comes back
        dev.send("bench motor m1 all")
        dev.expect_all([r"bench motor: M1 sweeps I1-I9", r"sweep block I1 of 9: drive 45 %"], 10)
        dev.pump(3)
        dev.send("bench stop")
        dev.expect(r"the sweep stopped at I1; the strength set \(I8\) is back", 10)
        dev.pump(2)
        dev.send("bench power")
        dev.expect_all([r"bench power: the board steps through these states", r"1 +processor only",
                        r"1b +\+ keying", r"3a +\+ Bluetooth advertising, not connectable", r"6 +switched off",
                        r"about \d+ s in all"], 10)
        dev.expect(r"bench power: USB out; step 1 in 5 s", 10)
        dev.expect(r"bench power step 1: processor only", 15)
        dev.send("bench stop")
        dev.expect(r"bench power stopped", 10)
        dev.pump(3)
        dev.send("status")
        dev.expect(r"MorseLink R2D \| state: idle", 10, "normal use again")
    step("bench motor plays (and stops); bench power prints its plan, runs step 1, stops on request", bench_tests)

    def stack_headroom():
        dev.pump(2)
        dev.send("status")
        m = dev.expect(r"least free stack ever, ui (\d+) B, net (\d+) B", 10)
        ui_free, net_free = int(m.group(1)), int(m.group(2))
        print(f"        (free stack: ui {ui_free} B, net {net_free} B)")
        assert ui_free >= 1024 and net_free >= 1024, f"ui {ui_free} B, net {net_free} B"
    step("UI and net tasks kept at least 1 KB of stack free", stack_headroom)

    def reboot_persists():
        dev.pump(2)
        dev.send("radio sf 11")      # the range level is stored too (settings v3)
        dev.expect(r"SF11 \(range level 3\)", 10)
        dev.pump(1)
        dev.send("restart")
        dev.expect(r"MorseLink [0-9.]+ starting", 20)
        # Loopback was on: it must not survive (left on, the pretend partner
        # would hide a real partner going away). M4X was invited from D.
        dev.expect_all([r"MorseLink R2D, recipient M4X, 3 contact\(s\)\r?\n", r"net: loopback off"], 15)
        dev.expect(r"calibration loaded: dot", 10)
        dev.pump(1)
        dev.send("status")
        dev.expect(r"radio: NOT FOUND", 10)      # the emulator has no radio, and loopback is off
        dev.pump(1)
        command(dev, "G", "G")
        dev.expect(r"range level 3 \(SF11\)", 15, "the range level survived")
        dev.pump(3)
        dev.send("radio sf 9")
        dev.expect(r"SF9 \(range level 1\)", 10)
    step("ID, contacts, recipient, calibration and the range level survive a reboot; loopback doesn't",
         reboot_persists)

    def off_aborted():
        dev.pump(3)
        command(dev, "F", "OFF")
        dev.send("key .")          # a press during the echo
        dev.expect(r"command aborted", 10)
        dev.pump(2)
        dev.send("status")
        dev.expect(r"MorseLink R2D \| state:", 10)   # still running
    step("F echoes OFF; a press during the echo aborts it", off_aborted)

    # Everything up to here must be free of crashes. (The emulator can't do
    # a deep sleep, so the switch-off step below ends in an emulator-only
    # panic: it is checked separately.)
    checked = dev.buf

    def switch_off():
        dev.pump(3)
        dev.send("off")
        dev.expect(r"power: switching off", 10)
        dev.expect(r"off: hold the key 2 s to switch on", 20)   # (QEMU's key pin reads "down": 10 s wait)
        # QEMU can't sleep: it panics inside ESP-IDF's sleep code and restarts.
        # That restart stands in for "power arrived": the switch is off, so
        # the device must go straight back to sleep instead of starting.
        dev.expect(r"MorseLink [0-9.]+ starting", 40, "restart after the emulated sleep")
        dev.expect(r"switched off \(F\), so staying off although power arrived", 10)
        dev.pump(3)
        rest = dev.buf[dev.pos:]
        assert "MorseLink R2D, recipient" not in rest, "the UI started although switched off"
    step("off (as F): switch remembered; power arriving keeps it off", switch_off)

    crashes = re.findall(r"Guru Meditation|abort\(\) was called|Stack overflow|Task watchdog got triggered|"
                         r"assert failed|CORRUPT HEAP|queue full", checked)
    results.append(("no crashes / watchdogs / lost events", not crashes, ", ".join(crashes)))
    print(f"  {'ok  ' if not crashes else 'FAIL'}  no crashes / watchdogs / lost events")
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep-log", action="store_true")
    a = ap.parse_args()
    log_path = os.path.join(BUILD, "qemu_test.log")
    dev = Device(log_path)
    try:
        results = run(dev)
    finally:
        dev.stop()
    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed  (log: {log_path})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
