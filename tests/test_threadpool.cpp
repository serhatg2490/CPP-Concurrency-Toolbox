#include <gtest/gtest.h>
#include <ThreadPool.h>
#include <vector>
#include <string>
#include <chrono>
#include <numeric>
#include <atomic>

// --- Helper Functions and Classes ---

int multiply(int a, int b) {
    return a * b;
}

class MathOps {
public:
    int add(int a, int b) {
        return a + b;
    }
};

// 1. Lambda and rvalue arguments
TEST(ThreadPoolTest, LambdaAndRvalueArguments) {
    ThreadPool pool(2);

    auto future1 = pool.enqueue([](int x, int y) { return x + y; }, 5, 10);
    EXPECT_EQ(future1.get(), 15);

    // Testing with unique_ptr (rvalue only)
    auto future2 = pool.enqueue([](std::unique_ptr<int> ptr) {
        return *ptr * 2;
    }, std::make_unique<int>(20));

    EXPECT_EQ(future2.get(), 40);
}

// 2. Normal function and arguments
TEST(ThreadPoolTest, NormalFunctionAndArguments) {
    ThreadPool pool(2);

    auto future = pool.enqueue(multiply, 6, 7);
    EXPECT_EQ(future.get(), 42);
}

// 3. Class member function
TEST(ThreadPoolTest, ClassMemberFunction) {
    ThreadPool pool(2);
    MathOps math;

    // Using lambda to wrap member function
    auto future1 = pool.enqueue([&math](int a, int b) { return math.add(a, b); }, 10, 20);
    EXPECT_EQ(future1.get(), 30);

    // Direct binding using pointer to member function via std::bind or direct invocation in wrapper
    // Since our threadpool uses std::invoke under the hood, we can just pass the member function pointer,
    // the object pointer/reference, and arguments.
    auto future2 = pool.enqueue(&MathOps::add, &math, 50, 50);
    EXPECT_EQ(future2.get(), 100);
}

// 4. String/vector container arguments
TEST(ThreadPoolTest, ContainerArguments) {
    ThreadPool pool(2);

    std::string test_str = "Hello";
    std::vector<int> test_vec = {1, 2, 3, 4, 5};

    auto future1 = pool.enqueue([](std::string s) {
        return s + " World!";
    }, test_str); // Passed by copy

    auto future2 = pool.enqueue([](std::vector<int>& v) {
        return std::accumulate(v.begin(), v.end(), 0);
    }, std::ref(test_vec)); // Passed by reference using std::ref

    std::vector<int> test_vec2 = {1, 2, 3, 4, 5};
    auto future3 = pool.enqueue([](std::vector<int> v) {
        v.push_back(6);
        return v.size();
    }, std::move(test_vec2)); // Passed by move

    EXPECT_EQ(future1.get(), "Hello World!");
    EXPECT_EQ(future2.get(), 15);
    EXPECT_EQ(future3.get(), 6);
}

// 5. Performance tests, time measurements
TEST(ThreadPoolTest, PerformanceTest) {
    const int num_tasks = 1000;
    std::atomic<int> completed_tasks{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    {
        ThreadPool pool(std::thread::hardware_concurrency());

        for (int i = 0; i < num_tasks; ++i) {
            pool.enqueue([&completed_tasks]() {
                // Simulate some work
                int result = 0;
                for (int j = 0; j < 10000; ++j) {
                    result += j;
                }
                completed_tasks++;
            });
        }
    } // Pool is destroyed here, waiting for all tasks (stop requests, jthread joins)
      // Actually jthread destruction stops threads if stop is requested, but wait, 
      // the ThreadPool destructor currently sends request_stop to all workers and waits. 
      // Wait, let's verify if all pending tasks are executed.
      // In the implementation, wait finishes if stop_requested AND tasks are empty.
      // So all tasks are processed before the threads exit.

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    EXPECT_EQ(completed_tasks.load(), num_tasks);
    std::cout << "Executed " << num_tasks << " tasks in " << duration.count() << " ms.\n";
}

TEST(ThreadPoolTest, HardwareConcurrencyTest) {
    ThreadPool pool; // Uses std::thread::hardware_concurrency()
    
    std::vector<std::future<int>> results;
    for (int i = 0; i < 100; ++i) {
        results.push_back(pool.enqueue([](int x) { return x * x; }, i));
    }
    
    int sum = 0;
    for (auto& res : results) {
        sum += res.get();
    }
    
    // Sum of squares from 0 to 99
    int expected_sum = 99 * 100 * 199 / 6;
    EXPECT_EQ(sum, expected_sum);
}

// Performance test: measures task execution time including the overhead of waiting on std::future objects
TEST(ThreadPoolTest, PerformanceTest_WithFutures) {
    const int num_tasks = 1000;
    std::atomic<int> completed_tasks{0};
    
    // Create the pool outside of the timed section
    ThreadPool pool(std::thread::hardware_concurrency());

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.enqueue([&completed_tasks]() {
            // Simulate some work exactly like PerformanceTest
            int result = 0;
            for (int j = 0; j < 10000; ++j) {
                result += j;
            }
            completed_tasks++;
        }));
    }

    // Wait for all tasks to complete
    for (auto& f : futures) {
        f.get();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end_time - start_time;

    EXPECT_EQ(completed_tasks.load(), num_tasks);
    std::cout << "Executed " << num_tasks << " tasks in " << duration.count() << " ms (includes std::future::get() overhead).\n";
}

// Concurrency test: multiple workers incrementing a shared atomic variable
TEST(ThreadPoolTest, AtomicIncrementConcurrency) {
    ThreadPool pool(std::thread::hardware_concurrency());
    
    std::atomic<int> shared_counter{0};
    const int increments_per_task = 10000;
    const int num_tasks = 100;

    std::vector<std::future<void>> futures;
    futures.reserve(num_tasks);

    for (int i = 0; i < num_tasks; ++i) {
        futures.push_back(pool.enqueue([&shared_counter, increments_per_task]() {
            for (int j = 0; j < increments_per_task; ++j) {
                // Testing thread safety under heavy contention
                shared_counter.fetch_add(1, std::memory_order_relaxed);
            }
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    int expected_total = num_tasks * increments_per_task;
    EXPECT_EQ(shared_counter.load(), expected_total);
}
