#include "../src/core/PacketQueue.h"

#include <iostream>
#include <thread>
#include <chrono>

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name, expr) do { \
    if (!(expr)) { \
        std::cerr << "FAIL: " << name << " (" << #expr << ")" << std::endl; \
        testsFailed++; \
    } else { \
        testsPassed++; \
    } \
} while(0)

static AVPacket* makePacket(int size, int64_t duration) {
    AVPacket* p = av_packet_alloc();
    av_new_packet(p, size);
    p->duration = duration;
    return p;
}

void testBasic() {
    PacketQueue q;
    TEST("initially empty", q.nbPackets() == 0);
    TEST("initial serial 0", q.serial() == 0);

    q.put(makePacket(100, 10));
    q.put(makePacket(50, 10));
    TEST("two packets", q.nbPackets() == 2);
    TEST("size accounted", q.sizeBytes() == 150);
    TEST("duration accounted", q.durationTb() == 20);

    PacketItem it;
    TEST("get ok", q.get(it, std::chrono::milliseconds(10)));
    TEST("packet non-null", it.pkt != nullptr);
    TEST("serial matches", it.serial == 0);
    TEST("size reduced", q.sizeBytes() == 50);
    TEST("duration reduced", q.durationTb() == 10);
    av_packet_free(&it.pkt);

    TEST("get second", q.get(it, std::chrono::milliseconds(10)));
    av_packet_free(&it.pkt);
    TEST("empty again", q.nbPackets() == 0 && q.sizeBytes() == 0 && q.durationTb() == 0);
    TEST("get on empty times out", !q.get(it, std::chrono::milliseconds(20)));
}

void testFlushAndSerial() {
    PacketQueue q;
    for (int i = 0; i < 3; ++i) q.put(makePacket(10, 1));
    q.flush(5, 2.5);
    TEST("flush clears", q.nbPackets() == 0 && q.sizeBytes() == 0);
    TEST("flush sets serial", q.serial() == 5);
    TEST("flush sets seek target", q.seekTarget() == 2.5);

    q.put(makePacket(10, 1));
    PacketItem it;
    TEST("get after flush", q.get(it, std::chrono::milliseconds(10)));
    TEST("new packet has new serial", it.serial == 5);
    av_packet_free(&it.pkt);
}

void testSentinel() {
    PacketQueue q;
    q.flush(2, -1.0);
    q.put(nullptr);
    TEST("sentinel counts as a packet", q.nbPackets() == 1);
    TEST("sentinel has no size", q.sizeBytes() == 0);
    PacketItem it;
    TEST("get sentinel", q.get(it, std::chrono::milliseconds(10)));
    TEST("sentinel pkt is null", it.pkt == nullptr);
    TEST("sentinel carries serial", it.serial == 2);
}

void testThreaded() {
    PacketQueue q;
    constexpr int N = 500;
    std::thread producer([&] {
        for (int i = 0; i < N; ++i) q.put(makePacket(8, 1));
    });
    int received = 0;
    std::thread consumer([&] {
        while (received < N) {
            PacketItem it;
            if (q.get(it, std::chrono::milliseconds(100))) {
                if (it.pkt) av_packet_free(&it.pkt);
                received++;
            }
        }
    });
    producer.join();
    consumer.join();
    TEST("all packets received", received == N);
    TEST("queue drained", q.nbPackets() == 0);
}

int main() {
    testBasic();
    testFlushAndSerial();
    testSentinel();
    testThreaded();

    std::cout << "PacketQueue tests: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
