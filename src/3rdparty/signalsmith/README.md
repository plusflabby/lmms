# Signalsmith Stretch

LMMS vendors the upstream header-only Signalsmith Stretch implementation for bounded offline vocal timing work.

- Stretch source: `signalsmith-stretch.h` from `Signalsmith-Audio/signalsmith-stretch` commit `88061fd4eda56a2353c5d65c74aa3f4ccf76f9da` (upstream version constant 1.3.2).
- FFT helpers: `Signalsmith-Audio/linear` tag `0.6.6`, commit `7bb1c8f36794d6980080ce817c91f8da8fe56a66`.
- Both components are MIT licensed. The unmodified license texts are next to their retained headers.
- The build uses only the checked-in headers. It does not fetch code at configure time or load a runtime backend DLL.

Upstream references:

- https://github.com/Signalsmith-Audio/signalsmith-stretch
- https://github.com/Signalsmith-Audio/linear
