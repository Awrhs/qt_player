# ── 单元测试 ──────────────────────────────────────────────────────────────

# BoundedQueue 测试
add_executable(test_bounded_queue test_bounded_queue.cpp)
target_link_libraries(test_bounded_queue PRIVATE qt_player_core)
add_test(NAME BoundedQueue COMMAND test_bounded_queue)

# FFT 测试
add_executable(test_fft test_fft.cpp)
target_link_libraries(test_fft PRIVATE qt_player_core)
add_test(NAME FFT COMMAND test_fft)

# Equalizer 测试
add_executable(test_equalizer test_equalizer.cpp)
target_link_libraries(test_equalizer PRIVATE qt_player_core)
add_test(NAME Equalizer COMMAND test_equalizer)

# AudioClock 测试
add_executable(test_audio_clock test_audio_clock.cpp)
target_link_libraries(test_audio_clock PRIVATE qt_player_core)
add_test(NAME AudioClock COMMAND test_audio_clock)