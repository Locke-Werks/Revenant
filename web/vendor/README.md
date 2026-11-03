# Vendored files

Third-party files served by the app, copied unmodified from published packages. No npm dependency is added for
them; update by fetching the tarball again and replacing the file.

## web/static/vendor/opus-decoder.min.js

- Package: `opus-decoder` 0.7.12 (wasm-audio-decoders, Ethan Halsall)
- Source: https://registry.npmjs.org/opus-decoder/-/opus-decoder-0.7.12.tgz,
  file `package/dist/opus-decoder.min.js`
- Tarball SHA-256: `da533b53ee80a0c98c58f2ea9e37221c21025145f0b0037def38ae1a31c3d5dc`
- File SHA-256: `68a08546a23df78bc837808af82af5a7550899ed5e17e281826dc2b3320dccdf`
- Licence: MIT, with libopus (BSD-3-Clause) and puff (Zlib) compiled into the
  WebAssembly it carries. Texts: `licenses/notices/opus-decoder-0.7.12-NOTICE.txt`.

A UMD bundle with the WebAssembly embedded, so it is one request and needs no
`.wasm` MIME type from the gateway. Loaded as a classic script it defines
`globalThis["opus-decoder"]`. The source map it names is not vendored.
