#include "../src/core/BoundedQueue.h"

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

// 简单的测试框架
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

void testBasicOperations() {
    BoundedQueue<int> q(5);

    TEST("Initially empty", q.empty());
    TEST("Size is 0", q.size() == 0);

    q.push(1, std::chrono::milliseconds(100));
    TEST("Not empty after push", !q.empty());
    TEST("Size is 1", q.size() == 1);

    q.push(2, std::chrono::milliseconds(100));
    TEST("Size is 2", q.size() == 2);

    int val;
    TEST("Pop returns true", q.pop(val, std::chrono::milliseconds(100)));
    TEST("First value is 1", val == 1);
    TEST("Size is 1", q.size() == 1);

    TEST("Pop second", q.pop(val, std::chrono::milliseconds(100)));
    TEST("Second value is 2", val == 2);
    TEST("Empty after pops", q.empty());
}

void testTimeout() {
    BoundedQueue<int> q(1);
    q.push(1, std::chrono::milliseconds(100));

    // 队列满，push 应超时
    bool ok = q.push(2, std::chrono::milliseconds(100));
    TEST("Push on full queue times out", !ok);

    int val;
    q.pop(val, std::chrono::milliseconds(100));
    TEST("Pop works after timeout", val == 1);

    // 空队列 pop 应超时
    ok = q.pop(val, std::chrono::milliseconds(100));
    TEST("Pop on empty queue times out", !ok);
}

void testClear() {
    BoundedQueue<int> q(5);
    for (int i = 0; i < 5; ++i) {
        q.push(i, std::chrono::milliseconds(100));
    }
    TEST("Full queue", q.size() == 5);

    q.clear();
    TEST("Empty after clear", q.empty());
}

void testMultiThread() {
    BoundedQueue<int> q(100);
    constexpr int NUM_ITEMS = 1000;

    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!q.push(i, std::chrono::milliseconds(100))) {}
        }
    });

    std::vector<int> received;
    std::thread consumer([&]() {
        int count = 0;
        while (count < NUM_ITEMS) {
            int val;
            if (q.pop(val, std::chrono::milliseconds(100))) {
                received.push_back(val);
                count++;
            }
        }
    });

    producer.join();
    consumer.join();

    TEST("All items received", received.size() == NUM_ITEMS);
}

int main() {
    testBasicOperations();
    testTimeout();
    testClear();
    testMultiThread();

    std::cout << "BoundedQueue tests: "
              << testsPassed << " passed, "
              << testsFailed << " failed" << std::endl;

    return testsFailed > 0 ? 1 : 0;
}