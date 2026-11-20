#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Negative tests against an emulator flash COPY. Never open a serial port."""
import argparse, json, hashlib, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser()
for name in ('flash','emu','rom','efuse','evidence'):
    p.add_argument(name,type=Path)
a=p.parse_args();a.evidence.mkdir(parents=True,exist_ok=True)
original=a.flash.read_bytes()
if len(original)!=16<<20:raise ValueError('expected 16MiB')
cases=[('uboot',0x100000+128,'P4 FIT: uboot SHA256 mismatch; halted'),
       ('linux',0x200000+128,'P4 FIT: linux SHA256 mismatch; halted'),
       ('dtb',0x900000+128,'P4 FIT: dtb SHA256 mismatch; halted'),
       ('manifest',0xf0000,'P4 FIT: invalid manifest/image; halted')]
summary=[]
with tempfile.TemporaryDirectory(dir=a.evidence) as td:
    t=Path(td)
    for label,idx,stop in cases:
        image=bytearray(original);image[idx]^=1
        file=t/'bad.bin';file.write_bytes(image)
        ef=t/'identity.efuse';ef.write_bytes(a.efuse.read_bytes())
        cmd=[a.emu,'--chip','esp32p4','--firmware',file,'--rom',a.rom,'--efuse',ef,
             '--psram-size','32M','--net','user,restrict=on','--console','uart0',
             '--timeout','100s','--exit-on',stop]
        r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=120)
        log=r.stdout; (a.evidence/(label+'.log')).write_bytes(log)
        uart=log.split(b"ESP-ROM:",1)[-1].split(b"[INFO  cli::run] Exit-on",1)[0]
        ok=(r.returncode==0 and b'entry 0x4ff40000' in uart and stop.encode() in uart and
            b'jump: entry' not in uart and b'esp32p4>' not in uart and b'login:' not in uart)
        summary.append(dict(case=label,flipped_flash_offset=hex(idx),observed=stop,
                            flash_sha256=hashlib.sha256(image).hexdigest(),passed=ok))
        if not ok:raise RuntimeError(f'{label}: {log[-450:]}')
        print(label,stop,flush=True)
(a.evidence/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
