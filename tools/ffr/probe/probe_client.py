"""Client for ffr_probe (development only): guest memory reads over `adb forward tcp:47123 tcp:47123`."""
import socket
import struct


class Probe:
    def __init__(self, host='127.0.0.1', port=47123):
        self.s = socket.create_connection((host, port), timeout=30)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def _recv(self, n):
        out = bytearray()
        while len(out) < n:
            c = self.s.recv(n - len(out))
            if not c:
                raise ConnectionError('probe closed')
            out += c
        return bytes(out)

    def info(self):
        self.s.sendall(struct.pack('<BQI', 2, 0, 0))
        return dict(zip(('main_base', 'main_size', 'heap_begin', 'heap_end', 'tick'), struct.unpack('<5Q', self._recv(40))))

    def read(self, addr, n):
        """bytes, or None when the range is not mapped"""
        if n <= 0:
            return b''
        self.s.sendall(struct.pack('<BQI', 1, addr, n))
        if self._recv(1)[0] != 1:
            return None
        return self._recv(n)

    def read_chunked(self, addr, n, chunk=1 << 20):
        """bytes with unmapped chunks zero-filled"""
        out = bytearray()
        for o in range(0, n, chunk):
            k = min(chunk, n - o)
            b = self.read(addr + o, k)
            out += b if b is not None else bytes(k)
        return bytes(out)

    def read_many(self, reqs, batch=1024):
        """[(addr, n)] -> [bytes or None], pipelined"""
        out = []
        for i in range(0, len(reqs), batch):
            part = reqs[i:i + batch]
            self.s.sendall(b''.join(struct.pack('<BQI', 1, a, n) for a, n in part))
            for a, n in part:
                out.append(self._recv(n) if self._recv(1)[0] == 1 else None)
        return out

    def mapped(self, addr, n):
        self.s.sendall(struct.pack('<BQI', 3, addr, n))
        return self._recv(1)[0] == 1

    def u64(self, a):
        b = self.read(a, 8)
        return struct.unpack('<Q', b)[0] if b else None

    def u32(self, a):
        b = self.read(a, 4)
        return struct.unpack('<I', b)[0] if b else None

    def i32(self, a):
        b = self.read(a, 4)
        return struct.unpack('<i', b)[0] if b else None
