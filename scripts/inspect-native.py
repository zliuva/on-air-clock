"""Check that the distribution EXE is native and imports only Windows DLLs."""
import json
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()
pe = struct.unpack_from('<I', data, 0x3C)[0]
assert data[pe:pe + 4] == b'PE\0\0', 'Not a PE executable'
machine, count = struct.unpack_from('<HH', data, pe + 4)
optional_size = struct.unpack_from('<H', data, pe + 20)[0]
optional = pe + 24
assert machine == 0x8664 and struct.unpack_from('<H', data, optional)[0] == 0x20B, 'Expected Windows x64'
assert struct.unpack_from('<H', data, optional + 68)[0] == 2, 'Expected Windows GUI subsystem'
directories = optional + 112
assert struct.unpack_from('<II', data, directories + 14 * 8) == (0, 0), 'EXE contains a CLR dependency'
sections = []
for i in range(count):
    start = optional + optional_size + i * 40
    virtual_size, address, raw_size, raw_start = struct.unpack_from('<IIII', data, start + 8)
    sections.append((address, max(virtual_size, raw_size), raw_start))

def offset(rva):
    for address, size, raw in sections:
        if address <= rva < address + size:
            return raw + rva - address
    raise AssertionError(f'Unmapped RVA {rva:x}')

imports = []
import_rva = struct.unpack_from('<I', data, directories + 8)[0]
pos = offset(import_rva)
while any(struct.unpack_from('<IIIII', data, pos)):
    name = offset(struct.unpack_from('<I', data, pos + 12)[0])
    imports.append(data[name:data.index(0, name)].decode('ascii').lower())
    pos += 20
system = {'advapi32.dll', 'bcrypt.dll', 'comctl32.dll', 'crypt32.dll', 'gdi32.dll',
          'kernel32.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'psapi.dll',
          'shell32.dll', 'user32.dll', 'userenv.dll', 'ucrtbase.dll', 'msvcrt.dll',
          'winhttp.dll', 'ws2_32.dll', 'shlwapi.dll'}
assert all(name in system or name.startswith('api-ms-win-') for name in imports), imports
print(json.dumps({'file': str(path.resolve()), 'bytes': len(data), 'architecture': 'x64',
                  'gui': True, 'clrDirectory': False, 'imports': sorted(imports)}, indent=2))
