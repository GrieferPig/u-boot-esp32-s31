#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack a standard fixed-position external-data FIT into the original P4 slots.
No IDF loader/app is installed. No flash device or network is accessed.
"""
import argparse, hashlib, json, os, struct, subprocess, tempfile
from pathlib import Path

def run(*cmd):
    return subprocess.check_output([str(x) for x in cmd], text=True).strip()

p = argparse.ArgumentParser()
for key in ('spl','uboot','linux','dtb','out'):
    p.add_argument(key,type=Path)
p.add_argument('--mkimage',default='mkimage')
a = p.parse_args()
out = a.out.resolve()
out.mkdir(parents=True,exist_ok=True)
fit_off, capacity = 0xf0000, 16 << 20
devices = [
    ('uboot', a.uboot, 'standalone', 'u-boot', 0x100000, 0x4ff00000, 0x34000),
    ('linux', a.linux, 'kernel', 'linux', 0x200000, 0x48000000, 0x400000),
    ('dtb', a.dtb, 'flat_dt', 'linux', 0x900000, 0x48600000, 0x10000)]
spl = a.spl.read_bytes()
if not (32 < len(spl) <= fit_off - 0x2000 and spl[:2] == b"\xe9\x01"
        and struct.unpack_from("<I", spl, 4)[0] == 0x4ff40000
        and struct.unpack_from("<H", spl, 12)[0] == 0x12):
    p.error("expected direct ROM -> P4 U-Boot single-segment SPL")
addr, n = struct.unpack_from("<II", spl, 24)
if addr != 0x4ff40000 or not 0 < n < 0x1c000 or n > len(spl) - 33:
    p.error("SPL segment lies outside its reserved SRAM or ESP image")
with tempfile.TemporaryDirectory(dir=out) as td:
    t = Path(td)
    nodes = []
    for name,path,kind,osname,off,dest,limit in devices:
        if not 64 <= path.stat().st_size <= limit:
            p.error(f'{name} outside board slot')
        (t / (name+'.bin')).symlink_to(path.resolve())
        entry = '' if name == 'dtb' else f'entry = <0x{dest:x}>;'
        nodes.append(f'''{name} {{ description = "{name}"; data = /incbin/("{name}.bin");
            type = "{kind}"; arch = "riscv"; os = "{osname}"; compression = "none";
            load = <0x{dest:x}>; {entry} hash-1 {{ algo = "sha256"; }}; }};''')
    its = t/'p4.its'
    its.write_text('/dts-v1/; / { description = "ESP32-P4 rev1.3 NOMMU Linux"; #address-cells = <1>; images {\n' +
        '\n'.join(nodes) + '\n}; configurations { default = "p4"; p4 { firmware = "uboot"; kernel = "linux"; fdt = "dtb"; }; }; };\n')
    compact = t/'compact.itb'
    run(a.mkimage,'-f',its,'-E','-p','0x1000',compact)
    original = compact.read_bytes()
    hsize = struct.unpack_from('>I',original,4)[0]
    hdr = t/'manifest.dtb'; hdr.write_bytes(original[:hsize])
    verified = []
    for name,path,kind,osname,off,dest,limit in devices:
        node = '/images/'+name
        position = int(run('fdtget','-t','x',hdr,node,'data-position'),16)
        size = int(run('fdtget','-t','x',hdr,node,'data-size'),16)
        payload = path.read_bytes()
        digest = hashlib.sha256(payload).digest()
        hashed = bytes(int(b,16) for b in run('fdtget','-t','bx',hdr,node+'/hash-1','value').split())
        if hashed != digest or size != len(payload) or original[position:position+size] != payload:
            raise ValueError(f'mkimage data/hash mismatch: {name}')
        run('fdtput','-t','x',hdr,node,'data-position',f'{off-fit_off:x}')
        verified.append((name,off,payload,digest.hex()))
    header = hdr.read_bytes()
    if len(header)>65536:
        raise ValueError('oversized FIT header')
    end = max(off+len(b) for _,off,b,_ in verified)
    fit = bytearray(b'\xff'*(end-fit_off));fit[:len(header)] = header
    flash = bytearray(b'\xff'*capacity);flash[0x2000:0x2000+len(spl)]=spl
    for name,off,b,_ in verified:
        fit[off-fit_off:off-fit_off+len(b)] = b
    flash[fit_off:end] = fit
    (out/'p4-fit-header.dtb').write_bytes(header)
    (out/'p4.itb').write_bytes(fit);(out/'flash.bin').write_bytes(flash)
    (out/'manifest.json').write_text(json.dumps({'fit_flash_offset':fit_off,'header_bytes':len(header),
        'flash_sha256':hashlib.sha256(flash).hexdigest(),'spl_sha256':hashlib.sha256(spl).hexdigest(),
        'images':[dict(name=n,offset=o,bytes=len(b),sha256=h) for n,o,b,h in verified]},indent=2)+'\n')
