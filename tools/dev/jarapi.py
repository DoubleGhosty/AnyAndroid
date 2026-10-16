#!/usr/bin/env python3
"""API of the classes in a .jar/.apk (classes*.dex) or a .dex: what the real
framework declares, to check stubs and native signatures against it.

  jarapi.py <file> classes [pattern]     classes whose name contains pattern
  jarapi.py <file> methods <class>       access flags, name and descriptor
  jarapi.py <file> natives [pattern]     native methods (of matching classes)
  jarapi.py <file> fields <class>        fields, with static final values
  jarapi.py <file> dump                  every class and method, one per line:
                                         C <class> <flags> <super> <interfaces>
                                         M <class> <flags> <name> <descriptor>

<class> is a Java name (android.graphics.Matrix, android.graphics.Matrix$ExtraNatives)
or a descriptor (Landroid/graphics/Matrix;). Run with python3 -I on inputs from an
Android image (they are untrusted data). Handles DEX 035 to 041 (041: a container
of several DEX sections, offsets relative to the container).
"""
import struct
import sys
import zipfile

ACC_NATIVE = 0x100


def uleb(b, o):
    r = s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if x < 0x80:
            return r, o


def mutf8(b, o):
    _, o = uleb(b, o)
    return b[o:b.index(0, o)].decode('utf-8', 'replace')


class Section:
    """One DEX section (a whole pre-041 file, or one header of a 041 container)."""

    def __init__(self, b, base):
        h = struct.unpack_from('<8sI20s' + 'I' * 20, b, base)
        self.magic = h[0]
        self.file_size = h[3]
        (n_str, o_str, n_type, o_type, n_proto, o_proto, n_field, o_field, n_meth, o_meth, n_cls,
         o_cls) = h[9:21]
        off = 0 if self.magic[4:7] == b'041' else base
        self.b, self.off = b, off
        self.strings = [mutf8(b, off + struct.unpack_from('<I', b, off + o_str + 4 * i)[0]) for i in range(n_str)]
        self.types = [self.strings[struct.unpack_from('<I', b, off + o_type + 4 * i)[0]] for i in range(n_type)]
        self.protos = []
        for i in range(n_proto):
            _, ret, params = struct.unpack_from('<III', b, off + o_proto + 12 * i)
            ps = []
            if params:
                (n,) = struct.unpack_from('<I', b, off + params)
                ps = [self.types[struct.unpack_from('<H', b, off + params + 4 + 2 * k)[0]] for k in range(n)]
            self.protos.append('(' + ''.join(ps) + ')' + self.types[ret])
        self.fields = []
        for i in range(n_field):
            c, t, n = struct.unpack_from('<HHI', b, off + o_field + 8 * i)
            self.fields.append((self.types[c], self.strings[n], self.types[t]))
        self.methods = []
        for i in range(n_meth):
            c, p, n = struct.unpack_from('<HHI', b, off + o_meth + 8 * i)
            self.methods.append((self.types[c], self.strings[n], self.protos[p]))
        self.class_defs = [struct.unpack_from('<8I', b, off + o_cls + 32 * i) for i in range(n_cls)]

    def encoded_value(self, o):
        """(value, next offset) of an encoded_value."""
        b = self.b
        head = b[o]
        o += 1
        kind, arg = head & 0x1F, head >> 5
        if kind == 0x1E:
            return None, o
        if kind == 0x1F:
            return bool(arg), o
        if kind == 0x1C:  # array
            n, o = uleb(b, o)
            out = []
            for _ in range(n):
                v, o = self.encoded_value(o)
                out.append(v)
            return out, o
        if kind == 0x1D:  # annotation
            _, o = uleb(b, o)
            n, o = uleb(b, o)
            for _ in range(n):
                _, o = uleb(b, o)
                _, o = self.encoded_value(o)
            return '<annotation>', o
        raw = b[o:o + arg + 1]
        o += arg + 1
        if kind == 0x10:  # float: right zero-extended
            return struct.unpack('<f', bytes(4 - len(raw)) + raw)[0], o
        if kind == 0x11:
            return struct.unpack('<d', bytes(8 - len(raw)) + raw)[0], o
        value = int.from_bytes(raw, 'little')
        if kind in (0x00, 0x02, 0x04, 0x06) and raw[-1] & 0x80:  # signed
            value -= 1 << (8 * len(raw))
        if kind == 0x17:
            return self.strings[value], o
        if kind == 0x18:
            return self.types[value], o
        if kind in (0x19, 0x1B):
            return '%s.%s' % self.fields[value][:2], o
        if kind == 0x1A:
            return '%s.%s%s' % self.methods[value], o
        return value, o

    def classes(self):
        b, off = self.b, self.off
        for cls, acc, sup, ifs, _src, _ann, data, static_values in self.class_defs:
            ifaces = []
            if ifs:
                (n,) = struct.unpack_from('<I', b, off + ifs)
                ifaces = [self.types[struct.unpack_from('<H', b, off + ifs + 4 + 2 * k)[0]] for k in range(n)]
            fields, methods = [], []
            if data:
                o = off + data
                sf, o = uleb(b, o)
                inf, o = uleb(b, o)
                dm, o = uleb(b, o)
                vm, o = uleb(b, o)
                for count in (sf, inf):
                    idx = 0
                    for _ in range(count):
                        d, o = uleb(b, o)
                        a, o = uleb(b, o)
                        idx += d
                        fields.append([self.fields[idx], a, None])
                for count in (dm, vm):
                    idx = 0
                    for _ in range(count):
                        d, o = uleb(b, o)
                        a, o = uleb(b, o)
                        _, o = uleb(b, o)
                        idx += d
                        methods.append((self.methods[idx], a))
                if static_values:
                    o = off + static_values
                    n, o = uleb(b, o)
                    for k in range(min(n, sf)):
                        fields[k][2], o = self.encoded_value(o)
            yield (self.types[cls], acc, self.types[sup] if sup != 0xFFFFFFFF else None, ifaces, fields, methods)


def dex_blobs(path):
    if path.endswith('.dex'):
        with open(path, 'rb') as f:
            yield f.read()
        return
    with zipfile.ZipFile(path) as z:
        for name in sorted(z.namelist()):
            if name.startswith('classes') and name.endswith('.dex'):
                yield z.read(name)


def all_classes(path):
    for b in dex_blobs(path):
        base = 0
        while base + 0x70 <= len(b) and b[base:base + 4] == b'dex\n':
            section = Section(b, base)
            yield from section.classes()
            if section.magic[4:7] != b'041' or section.file_size == 0:
                break
            base += section.file_size


def descriptor(name):
    if name.startswith('L') and name.endswith(';'):
        return name
    return 'L' + name.replace('.', '/') + ';'


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip())
        return 2
    path, mode = argv[1], argv[2]
    arg = argv[3] if len(argv) > 3 else ''
    wanted = descriptor(arg) if mode in ('methods', 'fields') else None
    pattern = arg.replace('.', '/')
    for cls, acc, sup, ifaces, fields, methods in all_classes(path):
        if mode == 'classes' and pattern in cls:
            print(cls, hex(acc), sup or '-', ' '.join(ifaces))
        elif mode == 'methods' and cls == wanted:
            for (_, name, proto), a in methods:
                print(hex(a), name, proto)
        elif mode == 'fields' and cls == wanted:
            for (_, name, ftype), a, value in fields:
                print(hex(a), name, ftype, '' if value is None else '= %r' % (value,))
        elif mode == 'natives' and pattern in cls:
            for (_, name, proto), a in methods:
                if a & ACC_NATIVE:
                    print(cls, hex(a), name, proto)
        elif mode == 'dump':
            print('C', cls, hex(acc), sup or '-', ' '.join(ifaces))
            for (_, name, proto), a in methods:
                print('M', cls, hex(a), name, proto)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
