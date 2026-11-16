import socket, struct, os, sys, json
d, out = sys.argv[1], sys.argv[2]
path = os.path.join(d, "bmvr_xfer.sock")
try: os.unlink(path)
except FileNotFoundError: pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(path); srv.listen(1)
print("mock helper listening", flush=True)
cl, _ = srv.accept()
fd = os.memfd_create("fake")
hello = struct.pack("<8IQ", 0x31564d42, 2, 1920, 1080, 0x8058, 1, 1, 0, 4096)
cl.sendmsg([hello], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", fd))])
frames = []
while True:
    buf = b""
    while len(buf) < 48:
        c = cl.recv(48 - len(buf))
        if not c: break
        buf += c
    if len(buf) < 48: break
    magic, tick, w, h, flags, gl_name, name = struct.unpack("<6I24s", buf)
    assert magic == 0x31524642, hex(magic)
    frames.append(dict(tick=tick, w=w, h=h, flags=flags, gl_name=gl_name, name=name.split(b"\0")[0].decode()))
    if os.environ.get("DIE_AFTER") and len(frames) >= int(os.environ["DIE_AFTER"]): break   # vanish without acking
    cl.sendall(struct.pack("<II", 0x314b4341, 0))
json.dump(frames, open(out, "w"), indent=1)
os.unlink(path)
