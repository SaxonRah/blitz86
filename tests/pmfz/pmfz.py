#!/usr/bin/env python3
"""Protected-mode (flat CPL0 32-bit) differential fuzzer: blitz86 vs Unicorn.
Each case is one instruction; reports unsupported / mismatching templates."""
import random, struct, subprocess, sys, collections, os
CODE16 = bool(os.environ.get('CODE16'))
from unicorn import UC_MODE_16
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError
from unicorn.x86_const import *
REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | AF | ZF | SF | OF
DST = [0, 1, 2, 5]                      # eax ecx edx ebp: free destinations
def modrm_reg(r, rm): return bytes([0xC0 | (r << 3) | rm])
def modrm_mem(r):                       # [ebx+disp8] / [esi] / [edi+disp8] / sib [ebx+esi*1+d8]
    k = random.randrange(4)
    if k == 0: return bytes([0x40 | (r << 3) | 3, random.randrange(0, 0x60)])
    if k == 1: return bytes([0x00 | (r << 3) | 6])
    if k == 2: return bytes([0x40 | (r << 3) | 7, random.randrange(0, 0x60)])
    return bytes([0x44 | (r << 3), 0x33, random.randrange(0, 0x40)])
def rm_any(r):
    return modrm_reg(r, random.choice(DST)) if random.random() < 0.5 else modrm_mem(r)
def imm(n): return bytes(random.randrange(256) for _ in range(n))
T = []                                  # (name, generator, undefined-flag mask)
def t(name, mask=0):
    def d(f): T.append((name, f, mask)); return f
    return d
for i, nm in enumerate(['add', 'or', 'adc', 'sbb', 'and', 'sub', 'xor', 'cmp']):
    um = AF if nm in ('or', 'and', 'xor') else 0
    T.append((nm + ' r/m32,r32', lambda o=i*8: bytes([o + 1]) + rm_any(random.randrange(8)) if False else bytes([o+1]) + rm_any(random.choice(DST)), um))
    T.append((nm + ' r32,r/m32', lambda o=i*8: bytes([o + 3]) + rm_any(random.choice(DST)), um))
    T.append((nm + ' r/m8,r8', lambda o=i*8: bytes([o]) + rm_any(random.choice([0,1,2,4,5,6])), um))
    T.append((nm + ' eax,imm32', lambda o=i*8: bytes([o + 5]) + imm(4), um))
    T.append((nm + ' r/m32,imm8', lambda r=i: b'\x83' + rm_any(r) + imm(1), um))
    T.append((nm + ' r/m32,imm32', lambda r=i: b'\x81' + rm_any(r) + imm(4), um))
    T.append((nm + ' r/m16,r16 (66)', lambda o=i*8: b'\x66' + bytes([o + 1]) + rm_any(random.choice(DST)), um))
T += [('mov r/m32,r32', lambda: b'\x89' + rm_any(random.randrange(8)), 0),
      ('mov r32,r/m32', lambda: b'\x8B' + rm_any(random.choice(DST)), 0),
      ('mov r/m8,r8', lambda: b'\x88' + rm_any(random.randrange(8)), 0),
      ('mov r32,imm32', lambda: bytes([0xB8 + random.choice(DST)]) + imm(4), 0),
      ('mov r/m32,imm32', lambda: b'\xC7' + rm_any(0) + imm(4), 0),
      ('mov r16,imm16 (66)', lambda: b'\x66' + bytes([0xB8 + random.choice(DST)]) + imm(2), 0),
      ('lea r32,m', lambda: b'\x8D' + modrm_mem(random.choice(DST)), 0),
      ('test r/m32,r32', lambda: b'\x85' + rm_any(random.randrange(8)), AF),
      ('xchg r/m32,r32', lambda: b'\x87' + rm_any(random.choice(DST)), 0),
      ('inc r32', lambda: bytes([0x40 + random.choice(DST)]), 0),
      ('dec r32', lambda: bytes([0x48 + random.choice(DST)]), 0),
      ('push/pop r32', lambda: bytes([0x50 + random.randrange(8), 0x58 + random.choice(DST)]), 0),
      ('push imm32/pop', lambda: b'\x68' + imm(4) + bytes([0x58 + random.choice(DST)]), 0),
      ('push imm8/pop', lambda: b'\x6A' + imm(1) + bytes([0x58 + random.choice(DST)]), 0),
      ('push r/m32/pop', lambda: b'\xFF' + modrm_mem(6) + bytes([0x58 + random.choice(DST)]), 0),
      ('pushad/popad', lambda: b'\x60\x61', 0),
      ('pushfd/pop', lambda: b'\x9C' + bytes([0x58 + random.choice(DST)]), 0),
      ('cwde', lambda: b'\x98', 0), ('cdq', lambda: b'\x99', 0),
      ('movzx r32,r/m8', lambda: b'\x0F\xB6' + rm_any(random.choice(DST)), 0),
      ('movzx r32,r/m16', lambda: b'\x0F\xB7' + rm_any(random.choice(DST)), 0),
      ('movsx r32,r/m8', lambda: b'\x0F\xBE' + rm_any(random.choice(DST)), 0),
      ('movsx r32,r/m16', lambda: b'\x0F\xBF' + rm_any(random.choice(DST)), 0),
      ('imul r32,r/m32', lambda: b'\x0F\xAF' + rm_any(random.choice(DST)), SF|ZF|AF|PF),
      ('imul r32,r/m32,imm8', lambda: b'\x6B' + rm_any(random.choice(DST)) + imm(1), SF|ZF|AF|PF),
      ('imul r32,r/m32,imm32', lambda: b'\x69' + rm_any(random.choice(DST)) + imm(4), SF|ZF|AF|PF),
      ('mul r/m32', lambda: b'\xF7' + rm_any(4), SF|ZF|AF|PF),
      ('imul r/m32', lambda: b'\xF7' + rm_any(5), SF|ZF|AF|PF),
      ('not r/m32', lambda: b'\xF7' + rm_any(2), 0),
      ('neg r/m32', lambda: b'\xF7' + rm_any(3), 0),
      ('test r/m32,imm32', lambda: b'\xF7' + rm_any(0) + imm(4), AF),
      ('shl/shr/sar r/m32,imm8', lambda: b'\xC1' + rm_any(random.choice([0,1,2,3,4,5,7])) + bytes([random.randrange(1, 32)]), OF|AF),
      ('shift r/m32,1', lambda: b'\xD1' + rm_any(random.choice([0,1,2,3,4,5,7])), AF),
      ('shift r/m32,cl', lambda: b'\xD3' + rm_any(random.choice([0,1,2,3,4,5,7])), OF|AF),
      ('shld r/m32,r32,imm8', lambda: b'\x0F\xA4' + rm_any(random.choice(DST)) + bytes([random.randrange(1, 32)]), OF|AF),
      ('shrd r/m32,r32,cl', lambda: b'\x0F\xAD' + rm_any(random.choice(DST)), OF|AF),
      ('bt r/m32,r32(reg)', lambda: b'\x0F\xA3' + modrm_reg(random.randrange(8), random.choice(DST)), OF|SF|ZF|AF|PF),
      ('bts r/m32,imm8', lambda: b'\x0F\xBA' + rm_any(5) + imm(1), OF|SF|ZF|AF|PF),
      ('bsf r32,r/m32', lambda: b'\x0F\xBC' + rm_any(random.choice(DST)), CF|OF|SF|AF|PF),
      ('setcc r/m8', lambda: bytes([0x0F, 0x90 + random.randrange(16)]) + rm_any(0), 0),
      ('jcc rel8 (fwd 0)', lambda: bytes([0x70 + random.randrange(16), 0]), 0),
      ('jmp rel32 (0)', lambda: b'\xE9\0\0\0\0', 0),
      ('call rel32/pop', lambda: b'\xE8\0\0\0\0' + bytes([0x58 + random.choice(DST)]), 0),
      ('movsd', lambda: b'\xA5', 0), ('stosd', lambda: b'\xAB', 0), ('lodsd', lambda: b'\xAD', 0),
      ('cld/std', lambda: random.choice([b'\xFC', b'\xFD']), 0),
      ('xchg eax,r32', lambda: bytes([0x90 + random.choice([1, 2, 5])]), 0),
      ('cmc/clc/stc', lambda: random.choice([b'\xF5', b'\xF8', b'\xF9']), 0),
      ('sahf/lahf', lambda: random.choice([b'\x9E', b'\x9F']), 0),

      ('mov eax,[moffs32]', lambda: b'\xA1' + struct.pack('<I', 0x20000 + random.randrange(0, 0x800)), 0),
      ('daa/aaa (BCD)', lambda: random.choice([b'\x27', b'\x37']), OF|SF|ZF|PF),
     ]

T += [('rol/ror/rcl/rcr r/m32,imm8', lambda: b'\xC1' + rm_any(random.randrange(4)) + bytes([random.randrange(1, 32)]), OF|AF),
      ('rotate r/m8,1', lambda: b'\xD0' + rm_any(random.randrange(4)), AF),
      ('shift r/m16,cl (66)', lambda: b'\x66\xD3' + rm_any(random.choice([0,1,4,5,7])), OF|AF),
      ('div r/m32 (safe)', lambda: b'\x31\xD2' + b'\xB9' + struct.pack('<I', random.randrange(1, 1<<31)) + b'\xF7\xF1', ALL),
      ('idiv r/m32 (safe)', lambda: b'\x99' + b'\xB9' + struct.pack('<I', random.randrange(1, 1<<31)) + b'\xF7\xF9', ALL),
      ('div r/m8 (safe)', lambda: b'\x66\x25\xFF\x00' + b'\xB1' + bytes([random.randrange(1,256)]) + b'\xF6\xF1', ALL),
      ('mul r/m8', lambda: b'\xF6' + rm_any(4), SF|ZF|AF|PF),
      ('neg r/m8', lambda: b'\xF6' + rm_any(3), 0),
      ('not r/m16 (66)', lambda: b'\x66\xF7' + rm_any(2), 0),
      ('inc/dec r/m32', lambda: b'\xFF' + rm_any(random.randrange(2)), 0),
      ('inc/dec r/m8', lambda: b'\xFE' + rm_any(random.randrange(2)), 0),
      ('inc r16 (66)', lambda: b'\x66' + bytes([0x40 + random.choice(DST)]), 0),
      ('cbw/cwd (66)', lambda: random.choice([b'\x66\x98', b'\x66\x99']), 0),
      ('xlat', lambda: b'\xD7', 0),
      ('lodsb/scasb/cmpsb', lambda: random.choice([b'\xAC', b'\xAE', b'\xA6']), 0),
      ('repe cmpsb (short)', lambda: b'\xB9' + struct.pack('<I', random.randrange(0, 20)) + b'\xF3\xA6', 0),
      ('repne scasb (short)', lambda: b'\xB9' + struct.pack('<I', random.randrange(0, 20)) + b'\xF2\xAE', 0),
      ('rep movsd (short)', lambda: b'\xB9' + struct.pack('<I', random.randrange(0, 20)) + b'\xF3\xA5', 0),
      ('call/ret', lambda: b'\xE8\x01\x00\x00\x00' + b'\xEB\x01' + b'\xC3', 0) if False else ('call/ret', lambda: b'\xE8\x02\x00\x00\x00\xEB\x01\xC3', 0),
      ('ret imm16', lambda: b'\xE8\x02\x00\x00\x00\xEB\x03\xC2\x00\x00', 0),
      ('loop', lambda: b'\xB9' + struct.pack('<I', random.randrange(1, 10)) + b'\xE2\xFE', 0),
      ('jecxz', lambda: b'\xE3\x00', 0),
      ('enter/leave', lambda: b'\xC8' + bytes([random.randrange(0,64)*4, 0, 0]) + b'\xC9', 0),
      ('push/pop r16 (66)', lambda: b'\x66' + bytes([0x50 + random.randrange(8)]) + b'\x66' + bytes([0x58 + random.choice(DST)]), 0),
      ('jmp r/m32', lambda: b'\xE8\x00\x00\x00\x00' + bytes([0x58 + 2]) + b'\x83\xC2\x08' + b'\xFF\xE2', 0),
      ('call r/m32 / pop', lambda: b'\xE8\x00\x00\x00\x00\x5A\x83\xC2\x09\xFF\xD2\x5D', 0),
      ('mov r/m16,imm16 (66)', lambda: b'\x66\xC7' + rm_any(0) + imm(2), 0),
      ('movsx r16,r/m8 (66)', lambda: b'\x66\x0F\xBE' + rm_any(random.choice(DST)), 0),
      ('sar r/m8,cl', lambda: b'\xD2' + rm_any(7), OF|AF),
      ('aam/aad', lambda: random.choice([b'\xD4\x0A', b'\xD5\x0A']), OF|AF|CF),
      ('bswap-free nop/xchg', lambda: b'\x90', 0),
      ('lea 16-bit addr (67)', lambda: b'\x67\x8D\x40' + imm(1), 0),
     ]

def rand_state():
    r = [random.getrandbits(32) for _ in range(8)]
    if CODE16:
        r[3] = random.randrange(0, 0x400); r[4] = 0xC00; r[6] = 0x400 + random.randrange(0x200); r[7] = 0x800 + random.randrange(0x200)
        r[1] = random.choice([r[1], random.randrange(0, 40)])
        return r, random.getrandbits(12) & 0x8D5 | 0x2
    r[3] = 0x20000 + random.randrange(0, 0x400)   # ebx
    r[4] = 0x20C00                                # esp
    r[6] = 0x20400 + random.randrange(0, 0x200)   # esi
    r[7] = 0x20800 + random.randrange(0, 0x200)   # edi
    r[1] = random.choice([r[1], random.randrange(0, 40)])
    fl = random.getrandbits(12) & 0x8D5 | 0x2
    return r, fl
def run_ref(code, r, fl, data):
    mu = Uc(UC_ARCH_X86, UC_MODE_16 if CODE16 else UC_MODE_32)
    mu.mem_map(0, 0x110000)
    if CODE16:
        for sr in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS): mu.reg_write(sr, 0x2000)
        mu.reg_write(UC_X86_REG_CS, 0x1000)
    mu.mem_write(0x10000, code); mu.mem_write(0x20000, data)
    for i in range(8): mu.reg_write(REGS[i], r[i] if i != 1 or True else 0)
    mu.reg_write(UC_X86_REG_EFLAGS, fl)
    try: mu.emu_start(0x10000, 0x10000 + len(code), count=200)
    except UcError as e: return None
    out = [mu.reg_read(x) for x in REGS] + [mu.reg_read(UC_X86_REG_EFLAGS)]
    return out, bytes(mu.mem_read(0x20000, 0x1000))
def run_b86(code, r, fl, data):
    blob = struct.pack('<9I', *r, fl) + struct.pack('<I', len(code)) + code + data
    p = subprocess.run(['./pm_run'], input=blob, capture_output=True)
    o = p.stdout
    v = struct.unpack('<11I', o[:44])
    return v[0], list(v[1:9]) + [v[9]], v[10], o[44:]
def main():
    random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
    per = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    stats = collections.OrderedDict()
    for name, gen, um in T:
        s = stats.setdefault(name, [0, 0, 0, ''])
        for _ in range(per):
            code = gen(); r, fl = rand_state(); data = bytes(random.getrandbits(8) for _ in range(0x1000))
            ref = run_ref(code, r, fl, data)
            if ref is None: continue
            rc, regs, eip, mem = run_b86(code, r, fl, data)
            if rc != 0 or eip != 0x10000 + len(code):
                s[1] += 1; s[3] = s[3] or f'rc={rc} eip={eip:X} code={code.hex()}'; continue
            mask = ALL & ~um
            bad = [i for i in range(8) if regs[i] != ref[0][i]]
            if (regs[8] ^ ref[0][8]) & mask: bad.append('flags')
            if mem != ref[1]: bad.append('mem')
            if bad:
                s[2] += 1
                s[3] = s[3] or f'{bad} code={code.hex()} b86={[hex(x) for x in regs]} ref={[hex(x) for x in ref[0]]}'
            else: s[0] += 1
    tot = [0, 0, 0]
    for k, (ok, un, mm, ex) in stats.items():
        tot[0] += ok; tot[1] += un; tot[2] += mm
        flag = 'OK ' if un == 0 and mm == 0 else ('UNS' if un else 'MIS')
        print(f'{flag} {k:28s} ok={ok:3d} unsupported={un:3d} mismatch={mm:3d}  {ex[:150]}')
    print(f'TOTAL ok={tot[0]} unsupported={tot[1]} mismatch={tot[2]}')
main()
