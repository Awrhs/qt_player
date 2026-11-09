# ── 单元 / 集成测试 ──────────────────────────────────────────────────────
# 全部可在无显示器、无声卡的 CI 环境运行（QT_QPA_PLATFORM=offscreen）。

set(TEST_ENV "QT_QPA_PLATFORM=offscreen")

function(add_player_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE qt_player_core qt_player_flags)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "${TEST_ENV}"
        TIMEOUT 300)
endfunction()

add_player_test(test_bounded_queue  test_bounded_queue.cpp)
add_player_test(test_fft            test_fft.cpp)
add_player_test(test_equalizer      test_equalizer.cpp)
add_player_test(test_media_clock    test_media_clock.cpp)
add_player_test(test_packet_queue   test_packet_queue.cpp)
add_player_test(test_pcm_queue      test_pcm_queue.cpp)
add_player_test(test_audio_pipeline test_audio_pipeline.cpp)
add_player_test(test_audio_output   test_audio_output.cpp)

# 端到端：用 ffmpeg 命令行生成测试媒体，驱动 PlayerController 全流程
# （打开/播放/seek/暂停 seek/逐帧/倍速/结束/循环/停止）。没有 ffmpeg 时跳过。
add_player_test(test_player_headless test_player_headless.cpp)
set_tests_properties(test_player_headless PROPERTIES SKIP_RETURN_CODE 77)
