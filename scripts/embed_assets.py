#!/usr/bin/env python3
# Gera um .cpp com as imagens e fontes de assets/ embutidas (build do PS5: o payload é um .elf só).
# Usa .incbin: assets/app traz o próprio app (eboot.elf, dezenas de MB), grande demais para um array em C.
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
        files.append((os.path.relpath(full, src).replace(os.sep, "/"), os.path.abspath(full)))
files.sort()

with open(out, "w", encoding="utf-8") as f:
    f.write("// Gerado por scripts/embed_assets.py - não editar\n")
    f.write("struct EmbeddedAsset { const char* path; const unsigned char* data; unsigned int size; };\n")
    f.write("__asm__(\n    \".section .rodata\\n\"\n")
    for i, (_, full) in enumerate(files):
        path = full.replace("\\", "/")
        f.write('    ".balign 16\\n"\n    "wp_asset_%d:\\n"\n    ".incbin \\"%s\\"\\n"\n    ".byte 0\\n"\n'
                % (i, path))
    f.write("    \".text\\n\");\n")
    f.write('extern "C" {\n')
    for i in range(len(files)):
        f.write("extern const unsigned char wp_asset_%d[] __asm__(\"wp_asset_%d\");\n" % (i, i))
    f.write("}\n")
    f.write("extern const EmbeddedAsset g_embeddedAssets[] = {\n")
    for i, (rel, full) in enumerate(files):
        f.write('    {"%s", wp_asset_%d, %du},\n' % (rel, i, os.path.getsize(full)))
    f.write("};\n")
    f.write("extern const unsigned int g_embeddedAssetCount = %du;\n" % len(files))
print("%d arquivos embutidos em %s" % (len(files), out))
