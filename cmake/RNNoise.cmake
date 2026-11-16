include_guard(GLOBAL)

# Original compact RNNoise model, vendored from the immutable v0.1 commit.
# Source and embedded weights are present locally: no download at build/run.
set(sculpt_rnnoise_root "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/RNNoise")
add_library(SculptRNNoise STATIC
  "${sculpt_rnnoise_root}/src/denoise.c"
  "${sculpt_rnnoise_root}/src/rnn.c"
  "${sculpt_rnnoise_root}/src/rnn_data.c"
  "${sculpt_rnnoise_root}/src/pitch.c"
  "${sculpt_rnnoise_root}/src/kiss_fft.c"
  "${sculpt_rnnoise_root}/src/celt_lpc.c")
target_include_directories(SculptRNNoise PUBLIC "${sculpt_rnnoise_root}/include")
target_compile_definitions(SculptRNNoise PRIVATE TRAINING=0 RNNOISE_BUILD=1)
set_target_properties(SculptRNNoise PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS YES
  POSITION_INDEPENDENT_CODE YES C_VISIBILITY_PRESET hidden)
if(UNIX AND NOT APPLE)
  target_link_libraries(SculptRNNoise PUBLIC m)
endif()
