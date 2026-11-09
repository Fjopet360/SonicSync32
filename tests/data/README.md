# Speech fixture

`upstream-speech-16k.wav` is an unchanged copy of
`tests/data/audio_tiny16.wav` from libfvad commit
`532ab666c20d3cfda38bca63abbb0f152706c369`:
https://github.com/dpirch/libfvad/blob/532ab666c20d3cfda38bca63abbb0f152706c369/tests/data/audio_tiny16.wav

The upstream regression suite treats it as voice and checks detected-frame
counts at different VAD modes. The upstream BSD license and notices are
retained under `components/voice_vad/vendor/`. This is a software fixture,
not a recording from Jopet's microphone or evidence of music rejection.
