set(VAD_SOURCES
    "voice_filter.c"
    "vendor/src/fvad.c"
    "vendor/src/signal_processing/division_operations.c"
    "vendor/src/signal_processing/energy.c"
    "vendor/src/signal_processing/get_scaling_square.c"
    "vendor/src/signal_processing/resample_48khz.c"
    "vendor/src/signal_processing/resample_by_2_internal.c"
    "vendor/src/signal_processing/resample_fractional.c"
    "vendor/src/signal_processing/spl_inl.c"
    "vendor/src/vad/vad_core.c"
    "vendor/src/vad/vad_filterbank.c"
    "vendor/src/vad/vad_gmm.c"
    "vendor/src/vad/vad_sp.c")
idf_component_register(SRCS ${VAD_SOURCES} INCLUDE_DIRS ".")
