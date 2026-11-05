"""Integration tests: the logger on a pseudo-terminal fed by fake_node, its TCP and
UDP interfaces, its signals, and the course's coder exercise. Run `make` first."""
import os
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD = os.path.join(ROOT, "build")
TCP, UDP = 15600, 15601


def ask_tcp(cmd, port=TCP):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(cmd.encode() + b"\n")
        return s.recv(4096).decode()


def ask_udp(cmd, port=UDP):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        s.sendto(cmd.encode(), ("127.0.0.1", port))
        return s.recv(4096).decode()


def zombies(ppid):
    out = subprocess.run(["ps", "-o", "pid=,stat=", "--ppid", str(ppid)], capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if "Z" in l.split()[1]]


class Logger(unittest.TestCase):
    def start(self, n=200, period=10, err=5):
        self.dir = tempfile.mkdtemp()
        self.node = subprocess.Popen([os.path.join(BUILD, "fake_node"), "-n", str(n), "-t", str(period), "-e", str(err)],
                                     stdout=subprocess.PIPE, text=True)
        self.pty = self.node.stdout.readline().strip()
        self.log = subprocess.Popen([os.path.join(BUILD, "logger"), "-d", self.pty, "-o", self.dir, "-p", str(TCP), "-u", str(UDP)],
                                    stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

    def finish(self):
        summary = self.node.stdout.read()
        self.node.wait()
        self.log.send_signal(signal.SIGTERM)
        err = self.log.communicate(timeout=5)[1]
        return summary, err

    def csv_lines(self):
        files = [f for f in os.listdir(self.dir) if f.endswith(".csv")]
        rows = []
        for f in files:
            with open(os.path.join(self.dir, f)) as fh:
                rows += [l for l in fh.read().splitlines() if l and not l.startswith("time,")]
        return files, rows

    def test_every_good_frame_logged_every_bad_one_rejected(self):
        self.start(n=300, err=10)
        time.sleep(1.5)
        last = ask_tcp("LAST")
        self.assertRegex(last, r"^\d{4}-\d\d-\d\dT[\d:]+,\d+,\d+,\d+\.\d,(\d+\.\d)?,\d+\n$")
        summary, err = self.finish()
        good = int(summary.split()[3]); bad = int(summary.split()[5]); noise = int(summary.split()[7])
        files, rows = self.csv_lines()
        self.assertEqual(len(rows), good)                       # every good frame, once, in a file
        self.assertIn(f"good {good} ", err)
        self.assertIn(f"bad_checksum {bad} ", err)              # every corrupted frame caught
        self.assertIn(f"no_start {noise} ", err)                # the noise lines
        self.assertIn("writer exit status 0", err)
        seqs = [int(r.split(",")[1]) for r in rows]
        self.assertEqual(seqs, sorted(seqs))                    # in order

    def test_tcp_stream_stats_and_udp(self):
        self.start(n=150, period=20)
        with socket.create_connection(("127.0.0.1", TCP), timeout=3) as s:
            s.sendall(b"STREAM\n")
            data = b""
            while data.count(b"\n") < 10:
                data += s.recv(4096)
        self.assertGreaterEqual(data.count(b"\n"), 10)
        self.assertIn("lines", ask_tcp("STATS"))
        self.assertRegex(ask_udp("LAST"), r"^\d{4}-")
        self.assertIn("good", ask_udp("STATS"))
        self.assertTrue(ask_udp("HELLO").startswith("ERR"))
        self.assertTrue(ask_tcp("HELLO").startswith("ERR"))
        self.finish()

    def test_sighup_reopens_the_file_for_rotation(self):
        self.start(n=200, period=10)
        time.sleep(0.8)
        files, _ = self.csv_lines()
        os.rename(os.path.join(self.dir, files[0]), os.path.join(self.dir, "rotated.old"))
        self.log.send_signal(signal.SIGHUP)
        time.sleep(0.6)
        self.finish()
        after = os.listdir(self.dir)
        self.assertIn("rotated.old", after)
        self.assertIn(files[0], after)                          # a fresh file with the same date
        with open(os.path.join(self.dir, files[0])) as f:
            self.assertTrue(f.readline().startswith("time,"))   # with its own header

    def test_killed_writer_is_reaped_and_replaced(self):
        self.start(n=200, period=10)
        time.sleep(0.5)
        children = subprocess.run(["pgrep", "-P", str(self.log.pid)], capture_output=True, text=True).stdout.split()
        self.assertEqual(len(children), 1)
        os.kill(int(children[0]), signal.SIGKILL)
        time.sleep(0.5)
        self.assertEqual(zombies(self.log.pid), [])             # waitpid() reaped it
        new = subprocess.run(["pgrep", "-P", str(self.log.pid)], capture_output=True, text=True).stdout.split()
        self.assertEqual(len(new), 1)
        self.assertNotEqual(new, children)
        _, err = self.finish()
        self.assertIn("child_restarts 1", err)

    def test_sigint_shuts_down_cleanly(self):
        self.start(n=500, period=10)
        time.sleep(0.5)
        self.log.send_signal(signal.SIGINT)
        err = self.log.communicate(timeout=5)[1]
        self.node.kill(); self.node.wait()
        self.assertEqual(self.log.returncode, 0)
        self.assertIn("writer exit status 0", err)

    def test_reads_from_stdin_too(self):
        d = tempfile.mkdtemp()
        frames = "$SN,42,123456,T=61,D=2001,L=512*63\n$SN,43,123656,T=61,D=0,L=512*53\ngarbage\n"
        r = subprocess.run([os.path.join(BUILD, "logger"), "-d", "-", "-o", d, "-p", str(TCP + 10), "-u", str(UDP + 10)],
                           input=frames, capture_output=True, text=True, timeout=5)
        self.assertEqual(r.returncode, 0, r.stderr)
        rows = open(os.path.join(d, os.listdir(d)[0])).read().splitlines()
        self.assertEqual(len(rows), 3)                          # header + 2 frames
        self.assertTrue(rows[1].endswith(",42,123456,29.8,34.3,50"))
        self.assertTrue(rows[2].endswith(",43,123656,29.8,,50"))  # no echo: empty distance


class Coder(unittest.TestCase):
    def run_coder(self, home, flag, answers):
        return subprocess.run([os.path.join(BUILD, "coder"), flag], input=answers, capture_output=True, text=True,
                              env={**os.environ, "HOME": home}, timeout=10)

    def test_code_then_decode_restores_the_file(self):
        home = tempfile.mkdtemp()
        os.mkdir(os.path.join(home, "a_directory"))
        original = bytes(range(256)) * 300 + "Árvíztűrő tükörfúrógép\n".encode()
        with open(os.path.join(home, "notes.txt"), "wb") as f:
            f.write(original)
        r = self.run_coder(home, "-c", "missing.txt\na_directory\nnotes.txt\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("notes.txt", r.stdout)
        self.assertNotIn("  a_directory", r.stdout)             # only regular files are listed
        self.assertEqual(r.stdout.count("try again"), 2)        # asked again twice
        coded = open(os.path.join(home, "notes.txt"), "rb").read()
        self.assertEqual(len(coded), 2 * len(original))
        self.assertTrue(all(chr(c).isalpha() and c < 128 for c in coded[1::2]))
        self.assertEqual(coded[2 * 0xC1], 0x43)                 # 1100 0001 -> 0100 0011
        r = self.run_coder(home, "-d", "notes.txt\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(open(os.path.join(home, "notes.txt"), "rb").read(), original)
        self.assertEqual([f for f in os.listdir(home) if f.startswith(".")], [])   # no temporary file left

    def test_wrong_arguments(self):
        for args in ([], ["-x"], ["-c", "extra"]):
            r = subprocess.run([os.path.join(BUILD, "coder")] + args, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("usage", r.stderr)


if __name__ == "__main__":
    unittest.main()
