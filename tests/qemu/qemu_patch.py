"""QEMU only: make adc_hw_calibration() return at once (QEMU has no SAR ADC),
then build a flashable .bin from the patched ELF."""
import sys, struct, subprocess, shutil
from elftools.elf.elffile import ELFFile
src, dst_elf, dst_bin = sys.argv[1:4]
shutil.copy(src, dst_elf)
with open(dst_elf, 'r+b') as f:
    elf = ELFFile(f)
    syms = elf.get_section_by_name('.symtab').get_symbol_by_name('adc_hw_calibration')
    addr = syms[0]['st_value'] if syms else None
    for seg in (elf.iter_segments() if addr is not None else []):
        if seg['p_type'] == 'PT_LOAD' and seg['p_vaddr'] <= addr < seg['p_vaddr'] + seg['p_filesz']:
            off = seg['p_offset'] + addr - seg['p_vaddr']
            break
    if addr is None:
        print('no adc_hw_calibration, nothing to patch')
    else:
        f.seek(off); code = f.read(3)
        assert code[0] == 0x36 and code[1] & 0x0f == 1, code.hex()  # entry a1, n
        f.seek(off + 3); f.write(bytes([0x1d, 0xf0]))                    # retw.n
        print('patched adc_hw_calibration at', hex(addr))
subprocess.check_call([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', 'elf2image', '--flash-mode', 'dio',
                       '--flash-size', '8MB', '-o', dst_bin, dst_elf])
