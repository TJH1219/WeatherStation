from SCons.Script import Import  # provided by PlatformIO at build time
Import("env")  # now 'env' is defined

import os, gzip

SRC_HTML = os.path.join("WebPage", "index.html")
SRC_GZ   = os.path.join("WebPage", "index.html.gz")
DST_H    = os.path.join("include", "index_html_gz.h")
SYMBOL   = "index_html_gz"

def ensure_gzip(src_html, dst_gz):
    if not os.path.exists(src_html):
        print(f"[embed_web] Missing {src_html}")
        return False
    # If gz exists and is newer than html, keep it
    if os.path.exists(dst_gz) and os.path.getmtime(dst_gz) >= os.path.getmtime(src_html):
        return True
    # Create/overwrite gz
    with open(src_html, "rb") as fin, gzip.open(dst_gz, "wb", compresslevel=9) as fout:
        fout.write(fin.read())
    print(f"[embed_web] Compressed -> {dst_gz} ({os.path.getsize(dst_gz)} bytes)")
    return True

def write_header(src_gz, dst_h, symbol):
    data = open(src_gz, "rb").read()
    os.makedirs(os.path.dirname(dst_h), exist_ok=True)
    with open(dst_h, "w", encoding="ascii") as f:
        f.write("#ifndef INDEX_HTML_GZ_H\n#define INDEX_HTML_GZ_H\n#include <stdint.h>\n")
        f.write(f"const unsigned int {symbol}_len = {len(data)};\n")
        f.write(f"const uint8_t {symbol}[{len(data)}] = {{\n")
        for i in range(0, len(data), 16):
            line = ", ".join(f"0x{b:02X}" for b in data[i:i+16])
            f.write(f"  {line},\n")
        f.write("};\n#endif\n")
    print(f"[embed_web] Wrote {dst_h}")

def generate_header(*args, **kwargs):
    if not ensure_gzip(SRC_HTML, SRC_GZ):
        return
    write_header(SRC_GZ, DST_H, SYMBOL)

# Run before the generic 'buildprog' target (more reliable across frameworks)
env.AddPreAction("buildprog", generate_header)
