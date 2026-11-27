# GDB remote-stub memory dumper: reads CLIC/intmatrix/clock state from a
# running esp-emu (--gdb PORT). Usage: gdb-dump.py PORT OUTFILE
import socket, sys, time

def rsp(sock, cmd):
    cs = sum(cmd.encode()) & 0xff
    sock.sendall(b'+')
    sock.sendall(f'${cmd}#{cs:02x}'.encode())
    buf = b''
    while True:
        buf += sock.recv(4096)
        if b'#' in buf and b'$' in buf:
            end = buf.index(b'#')
            if len(buf) >= end + 3:
                break
    payload = buf[buf.index(b'$')+1:buf.index(b'#')]
    sock.sendall(b'+')
    return payload

def read_mem(sock, addr, length):
    out = bytearray()
    while length > 0:
        chunk = min(length, 64)
        data = rsp(sock, f'm{addr:x},{chunk:x}')
        if data.startswith(b'E'):
            raise RuntimeError(f'read {addr:#x} -> {data!r}')
        out += bytes.fromhex(data.decode())
        addr += chunk
        length -= chunk
    return bytes(out)

def parse_args():
    return int(sys.argv[1]), sys.argv[2]

port, outpath = parse_args()
s = socket.create_connection(('127.0.0.1', port), timeout=10)
s.settimeout(10)
try:
    rsp(s, 'qSupported:')
except Exception:
    pass

regs = {}
regs['clic_intctl_0_63'] = read_mem(s, 0x10801000, 64*4)
regs['clic_cfg_intthresh'] = read_mem(s, 0x10800000, 0x40)
regs['intmatrix'] = read_mem(s, 0x20585000, 160*4)
regs['hpsys_clkrst_modem_conf'] = read_mem(s, 0x20587000+0x1e0, 8)
# S31 modem register bases (soc/esp32s31/include/modem/reg_base.h)
try:
    regs['rom_slots'] = read_mem(s, 0x2f07ff8c, 4)
except Exception as e:
    regs['rom_slots'] = b'ERR ' + str(e).encode()
try:
    regs['wifi_mac_int_ena_status'] = read_mem(s, 0x20104000, 0x240)
except Exception as e:
    regs['wifi_mac_int_ena_status'] = b'ERR ' + str(e).encode()
try:
    regs['modem_syscon_0x00_0x80'] = read_mem(s, 0x20100000+0x9C00, 0x80)
except Exception as e:
    regs['modem_syscon_0x00_0x80'] = b'ERR ' + str(e).encode()
try:
    regs['modem_lpcon_0x00_0x60'] = read_mem(s, 0x2010D000+0x2000, 0x60)
except Exception as e:
    regs['modem_lpcon_0x00_0x60'] = b'ERR ' + str(e).encode()
try:
    regs['modem_conf_u', ] = b''
except Exception:
    pass
regs.pop('modem_conf_u', None)

import struct

def fmt(name, base, data):
    lines = [f'== {name} @{base:#x}']
    for i in range(0, len(data), 4):
        w = struct.unpack('<I', data[i:i+4])[0]
        if w:
            lines.append(f'{base+i:08x}: {w:#010x}')
    return '\n'.join(lines) + '\n'

with open(outpath, 'w') as f:
    f.write(fmt('clic_intctl', 0x10801000, regs['clic_intctl_0_63']))
    f.write(fmt('intmatrix', 0x20585000, regs['intmatrix']))
    f.write(fmt('modem_syscon', 0x20109C00, regs['modem_syscon_0x00_0x80']))
    f.write(fmt('modem_lpcon', 0x2010F000, regs['modem_lpcon_0x00_0x60']))
    f.write(fmt('hpsys_modem_conf', 0x205871E0, regs['hpsys_clkrst_modem_conf']))
    f.write(fmt('wifi_mac', 0x20104000, regs['wifi_mac_int_ena_status']))
    gic = struct.unpack('<I', regs.get('rom_slots', b'\0'*8)[:4])[0] if 'rom_slots' in regs else 0
    try:
        slots = read_mem(s, 0x2f07ff84, 4)
        gic = struct.unpack('<I', slots)[0]
        f.write(f'g_ic_ptr = {gic:#x} g_scan_slot = {struct.unpack(chr(60)+chr(73), slots[4:8])[0]:#x}\n')
        if gic:
            f.write(fmt('g_ic_0x180_0x1c0', gic+0x180, read_mem(s, gic+0x180, 0x40)))
        # the blob's own g_ic global lives in our .bss (nm wifi.bin.elf: g_ic)
        BLOB_GIC = 0x2f006aa0
        f.write(fmt('scan_obj_0x1a0_0x220', BLOB_GIC+0x1a0, read_mem(s, BLOB_GIC+0x1a0, 0x80)))
    except Exception as e:
        f.write(f'g_ic read err {e}\n')
    # the blob scan object written by net80211_data_ptr_init into the g_scan
    # slot (nm: g_scan = 0x2f07ff8c; value = 0x2f006aa0 static obj in .bss)
    SCAN_OBJ = 0x2f003914
    f.write(fmt('gic568', SCAN_OBJ, read_mem(s, SCAN_OBJ, 0x8)))
print('dumped to', outpath)
s.close()
