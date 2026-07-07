"""
test_mesh_sender.py — sends test meshes to UE via MeshReceiverSubsystem.
Protocol: port 9001 (Python → UE)
  CMD_MESH   = 1  payload: name + vertices + triangles
  CMD_UPDATE = 2  payload: empty (triggers the flip/install)
"""
import socket, struct, time

HOST, INBOUND_PORT = '127.0.0.1', 9001
CMD_STRING, CMD_MESH, CMD_UPDATE = 0, 1, 2

def recv_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk: raise ConnectionError("Socket closed")
        buf += chunk
    return buf

def send_message(sock, cmd, payload: bytes) -> int:
    sock.sendall(struct.pack('<ii', len(payload), cmd) + payload)
    return struct.unpack('<i', recv_exact(sock, 4))[0]

def pack_mesh(name: str, vertices: list, triangles: list) -> bytes:
    name_bytes = name.encode('utf-8')
    data  = struct.pack('<i', len(name_bytes)) + name_bytes
    data += struct.pack('<i', len(vertices))
    for v in vertices:
        data += struct.pack('<fff', float(v[0]), float(v[1]), float(v[2]))
    data += struct.pack('<i', len(triangles))
    for idx in triangles:
        data += struct.pack('<i', idx)
    return data

def make_cube(size=100.0):
    h = size / 2
    verts = [(-h,-h,-h),(h,-h,-h),(h,h,-h),(-h,h,-h),(-h,-h,h),(h,-h,h),(h,h,h),(-h,h,h)]
    tris  = [0,2,1,0,3,2, 4,5,6,4,6,7, 0,1,5,0,5,4, 1,2,6,1,6,5, 2,3,7,2,7,6, 3,0,4,3,4,7]
    return verts, tris

def make_triangle(size=100.0):
    return [(0,0,0),(size,0,0),(size/2,size,0)], [0,1,2]

def main():
    print(f"Connecting to {HOST}:{INBOUND_PORT} ...")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, INBOUND_PORT))
        print("Connected.")

        verts, tris = make_cube(100.0)
        status = send_message(s, CMD_MESH, pack_mesh("TestCube", verts, tris))
        print(f"  CMD_MESH 'TestCube' ({len(verts)} verts)  ack={status}")

        verts, tris = make_triangle(150.0)
        status = send_message(s, CMD_MESH, pack_mesh("TestTriangle", verts, tris))
        print(f"  CMD_MESH 'TestTriangle' ({len(verts)} verts)  ack={status}")

        status = send_message(s, CMD_UPDATE, b'')
        print(f"  CMD_UPDATE  ack={status}")
        print("Check UE viewport — TestCube and TestTriangle should appear.")

        time.sleep(2)
        verts, tris = make_cube(50.0)
        status = send_message(s, CMD_MESH, pack_mesh("TestCube", verts, tris))
        print(f"\n  CMD_MESH 'TestCube' (smaller, replaces previous)  ack={status}")
        status = send_message(s, CMD_UPDATE, b'')
        print(f"  CMD_UPDATE  ack={status}")
        print("TestCube should now be half the size.")

if __name__ == '__main__':
    main()
