# Runtime files (not in this repository)

This project drives NVIDIA's DLSS Neural Rendering runtime; it does not contain
it. None of the files below are redistributable under this repository's MIT
licence, so you have to supply them yourself.

## Required layout

```text
runtime/host/dxgi.dll                <- ReShade with full add-on support, renamed
runtime/host/renodx-dlss5.addon64    <- RenoDX DLSS 5 add-on
runtime/host/nvngx_dlssnr.dll        <- NVIDIA DLSSNR (DLSS 5 Neural Rendering)
runtime/host/nvngx.dll               <- the DLSS 5 feeder worker executable
runtime/dlss/nvngx_dlss.dll          <- NVIDIA DLSS Super Resolution
```

`runtime/host/nvngx.dll` is an executable despite the extension: the worker
checks its own image name, so renaming it breaks the signed-snippet check.

## Where each file comes from

- **ReShade** with full add-on support, from https://reshade.me/. The standard
  build refuses to load unsigned `.addon64` files. Rename the 64-bit DLL to
  `dxgi.dll`.
- **RenoDX DLSS 5 add-on**, from the RenoDX releases. The RenoDX framework is
  MIT; that licence covers the framework, not any particular prebuilt binary.
- **NVIDIA DLSS runtimes** (`nvngx_dlssnr.dll`, `nvngx_dlss.dll`) from genuine
  NVIDIA SDK, driver or game distributions only. They are governed by the
  NVIDIA RTX SDK Licence: no standalone redistribution, NVIDIA GPUs only, and
  no implying NVIDIA sponsorship.
- **The worker** (`nvngx.dll`) is the DLSS 5 feeder built by the
  `dlss5-visual-enhancer` project. This overlay speaks its `--video` protocol:
  a setup handshake, then one RGBA frame in and one RGBA frame out per call.
  Point `worker_path` at whichever build you have.

## Checking the wiring before running the overlay

The worker writes its diagnostics to stderr, which the overlay forwards into
its own log. If setup fails, the first useful line is usually the NGX result
code; `0xC0000005` during evaluation means the neural runtime crashed rather
than refused, which on RTX 30 is the known experimental path.
