#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# AURORA in the browser: firmware/src/aurora.c on GR!MM's fake FM-1 (firmware/src/fm1_web.c), compiled to WebAssembly
# with plain clang + wasm-ld (no Emscripten needed), then Binaryen's Asyncify so the app's main loop can pause between
# audio blocks (as GR!MM's web/build_web.sh), and wasm2js for browsers / frames that refuse WebAssembly.
#   GRIMM   a checkout of github.com/GRiMMBIENT/grimm (default ../src/grimm)
#   BIN     where wasm-opt and wasm2js are (default ../tools/node_modules/.bin: npm install binaryen)
set -e
cd "$(dirname "$0")"
GRIMM="${GRIMM:-../src/grimm}"; BIN="${BIN:-../tools/node_modules/.bin}"
mkdir -p build web/dist
CF="--target=wasm32 -O2 -ffreestanding -fno-builtin -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable"
clang $CF -DGRIMM_WEB -DAPP_VER='"1.0"' -Ishim -Ifirmware/src -I"$GRIMM/firmware/src" -I"$GRIMM/firmware/gen" -c firmware/src/aurora.c -o build/aurora.o
clang $CF -c shim/libc_web.c -o build/libc.o
wasm-ld --no-entry --export-dynamic -z stack-size=262144 --initial-memory=4194304 --import-undefined build/aurora.o build/libc.o -o build/aurora.raw.wasm
"$BIN/wasm-opt" build/aurora.raw.wasm --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext \
    --enable-mutable-globals --asyncify --pass-arg=asyncify-imports@env.web_yield -O2 -o web/dist/aurora.wasm
echo "web: $(wc -c < web/dist/aurora.wasm) B wasm"
# the same module as plain JavaScript (wasm2js), for where WebAssembly is refused: it defines globalThis.AURORA_ASM
"$BIN/wasm2js" web/dist/aurora.wasm -O2 --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext --enable-mutable-globals -o build/aurora.asm.js 2>/dev/null
python3 - <<'PY'
s = open("build/aurora.asm.js").read()
assert s.startswith("import * as env from 'env';")
s = s[len("import * as env from 'env';"):]
s = s[:s.index("var retasmFunc = asmFunc(")]
open("web/dist/aurora-js.js", "w").write("// AURORA's firmware as plain JavaScript (Binaryen wasm2js of aurora.wasm). GPL-3.0-only.\n(function () {\n" + s + "\nglobalThis.AURORA_ASM = asmFunc;\n})();\n")
PY
# the page: the wasm and the worklet's source inside it (no fetch needed), a still of the screen until it is switched on
cp web/worklet.js web/dist/
python3 - <<'PY'
import base64, json
s = open("web/index.html").read()
s = s.replace("__WASM_B64__", base64.b64encode(open("web/dist/aurora.wasm", "rb").read()).decode())
s = s.replace("__STILL_PNG__", "data:image/png;base64," + base64.b64encode(open("web/still.png", "rb").read()).decode())
s = s.replace("__WORKLET_SRC__", json.dumps(open("web/worklet.js").read()).replace("</", "<\\/"))
open("web/dist/index.html", "w").write(s)
PY
echo "web: $(wc -c < web/dist/aurora-js.js) B js fallback"
