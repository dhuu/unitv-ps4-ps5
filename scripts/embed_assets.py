#!/usr/bin/env python3
# Gera um .cpp com as imagens e fontes de assets/ embutidas (build do PS5: o payload é um .elf só).
# Uso: embed_assets.py <pasta assets> <saida.cpp>
import os
import sys

src, out = sys.argv[1], sys.argv[2]
files = []
for root, _, names in os.walk(src):
    for n in sorted(names):
        if n.lower().endswith(".txt"):
            continue
        full = os.path.join(root, n)
        files.append((os.path.relpath(full, src).replace(os.sep, "/"), full))
files.sort()

with open(out, "w", encoding="utf-8") as f:
    f.write("// Gerado por scripts/embed_assets.py - não editar\n")
    f.write("struct EmbeddedAsset { const char* path; const unsigned char* data; unsigned int size; };\n")
    for i, (_, full) in enumerate(files):
        data = open(full, "rb").read()
        f.write("static const unsigned char a%d[] = {" % i)
        for j in range(0, len(data), 32):
            f.write("\n" + ",".join(str(b) for b in data[j:j + 32]) + ",")
        f.write("0};\n")
    f.write("extern const EmbeddedAsset g_embeddedAssets[] = {\n")
    for i, (rel, full) in enumerate(files):
        f.write('    {"%s", a%d, %du},\n' % (rel, i, os.path.getsize(full)))
    f.write("};\n")
    f.write("extern const unsigned int g_embeddedAssetCount = %du;\n" % len(files))
print("%d arquivos embutidos em %s" % (len(files), out))
