#include "../src/core/PcmQueue.h"

#include <iostream>
#include <cmath>
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

static PcmChunk makeChunk(size_t frames, double pts, double tempo, float value = 0.5f) {
    PcmChunk c;
    c.data.assign(frames * 2, value);
    c.pts = pts;
    c.tempo = tempo;
    return c;
}

static bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

void testPushPullAccounting() {
    PcmQueue q(2, 1000, 100);   // 立体声，1000 Hz，容量 100 帧
    uint64_t gen = q.generation();

    TEST("push first", q.push(makeChunk(50, 1.0, 1.0), gen, std::chrono::milliseconds(10)));
    TEST("push second (total 50 < 100)", q.push(makeChunk(60, 1.05, 1.0), gen, std::chrono::milliseconds(10)));
    TEST("queued frames", q.queuedFrames() == 110);
    TEST("push blocks when full", !q.push(makeChunk(10, 1.11, 1.0), gen, std::chrono::milliseconds(20)));

    std::vector<float> buf(200);
    auto r = q.pull(buf.data(), 30);
    TEST("pull 30 frames", r.frames == 30 && r.any);
    TEST("first pts of started chunk", near(r.firstPts, 1.0));
    TEST("floor is first chunk of generation", near(r.floorPts, 1.0));
    TEST("generation reported", r.generation == gen);
    TEST("end position after 30 frames", near(r.endPos, 1.0 + 30.0 / 1000.0));
    TEST("data copied", buf[0] == 0.5f && buf[59] == 0.5f);
    TEST("not empty", !q.empty());

    r = q.pull(buf.data(), 40);   // 20 from chunk1 + 20 from chunk2
    TEST("pull across chunks", r.frames == 40);
    TEST("firstPts is the newly started chunk", near(r.firstPts, 1.05));
    TEST("floor stays at generation start", near(r.floorPts, 1.0));
    TEST("end position in second chunk", near(r.endPos, 1.05 + 20.0 / 1000.0));

    // 消费后空间释放，之前被拒的块现在可以入队
    TEST("push after space freed", q.push(makeChunk(10, 1.11, 1.0), gen, std::chrono::milliseconds(10)));

    r = q.pull(buf.data(), 100);   // 剩余 40 + 10 = 50
    TEST("pull drains remaining", r.frames == 50);
    TEST("end position at last chunk end", near(r.endPos, 1.11 + 10.0 / 1000.0));
    TEST("empty after drain", q.empty());

    r = q.pull(buf.data(), 10);
    TEST("pull on empty returns nothing", r.frames == 0 && !r.any);
}

void testTempoPosition() {
    PcmQueue q(2, 1000, 1000);
    q.push(makeChunk(100, 0.0, 2.0), q.generation(), std::chrono::milliseconds(10));
    std::vector<float> buf(200);
    auto r = q.pull(buf.data(), 50);
    TEST("tempo scales media time", near(r.endPos, 50 * 2.0 / 1000.0));
    TEST("tempo reported", near(r.tempo, 2.0));
}

void testFlushGeneration() {
    PcmQueue q(2, 1000, 1000);
    uint64_t gen = q.generation();
    q.push(makeChunk(10, 0.0, 1.0), gen, std::chrono::milliseconds(10));
    std::vector<float> buf(200);
    q.pull(buf.data(), 5);   // 部分消费
    q.flush();
    TEST("flush drops queued and partial", q.empty());
    TEST("generation incremented", q.generation() == gen + 1);
    {
        auto r = q.pull(buf.data(), 5);
        TEST("floor reset by flush", r.floorPts < 0.0 && !r.any);
        TEST("pull reports new generation", r.generation == gen + 1);
    }
    q.push(makeChunk(10, 7.5, 1.0), q.generation(), std::chrono::milliseconds(10));
    {
        auto r = q.pull(buf.data(), 5);
        TEST("floor re-established from first new chunk", near(r.floorPts, 7.5));
    }
    q.flush();
    TEST("stale generation push rejected", !q.push(makeChunk(10, 0.0, 1.0), gen, std::chrono::milliseconds(10)));
    TEST("fresh generation push accepted", q.push(makeChunk(10, 0.0, 1.0), q.generation(), std::chrono::milliseconds(10)));
}

void testFlushWakesBlockedProducer() {
    PcmQueue q(2, 1000, 50);
    uint64_t gen = q.generation();
    q.push(makeChunk(60, 0.0, 1.0), gen, std::chrono::milliseconds(10));   // 已满

    bool result = true;
    auto start = std::chrono::steady_clock::now();
    std::thread producer([&] {
        result = q.push(makeChunk(10, 0.06, 1.0), gen, std::chrono::milliseconds(2000));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    q.flush();
    producer.join();
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    TEST("blocked push rejected after flush", !result);
    TEST("blocked push woke promptly", elapsed < 1.0);
}

void testSetCapacity() {
    PcmQueue q(2, 1000, 10);
    uint64_t gen = q.generation();
    TEST("first chunk always accepted", q.push(makeChunk(100, 0.0, 1.0), gen, std::chrono::milliseconds(10)));
    TEST("second rejected (over capacity)", !q.push(makeChunk(1, 0.1, 1.0), gen, std::chrono::milliseconds(10)));
    q.setCapacityFrames(1000);
    TEST("accepted after capacity raised", q.push(makeChunk(1, 0.1, 1.0), gen, std::chrono::milliseconds(10)));
}

int main() {
    testPushPullAccounting();
    testTempoPosition();
    testFlushGeneration();
    testFlushWakesBlockedProducer();
    testSetCapacity();

    std::cout << "PcmQueue tests: " << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;
    return testsFailed > 0 ? 1 : 0;
}
