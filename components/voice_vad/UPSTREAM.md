# Voice activity detector

Vendored C source from https://github.com/dpirch/libfvad
at commit `532ab666c20d3cfda38bca63abbb0f152706c369`.
WebRTC import recorded by that project: `ebd9abc1a23db8fe9acfd163155e0f9bc544b10e`.
All upstream C/header logic is unmodified. One extra trailing blank line in
`src/signal_processing/resample_fractional.c` was removed for patch hygiene.
Unused upstream build files are omitted. The build and streaming wrapper
outside `vendor/` are SonicSync32 code. No Python package is needed on the board.

Copyright, BSD license, patent grant and author notices are retained in
`vendor/LICENSE`, `vendor/PATENTS` and `vendor/AUTHORS` and in source headers.
Include these notices when redistributing firmware binaries.

This classifies speech-like audio. It does not separate vocals from instruments,
identify a speaker, or guarantee rejection of singing and musical instruments.
The existing voice energy gate and brightness envelope remain additional checks.
