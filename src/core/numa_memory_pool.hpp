#pragma once

#include <memory>
#include <thread>
#include <vector>

#include "memory_pool.hpp"

// NUMA support is Linux-specific and requires libnuma
#ifdef __linux__
#ifdef HAVE_NUMA
#include <numa.h>
#endif
#include <sched.h>
#endif

namespace crypto_lob::core {

// NUMA-aware memory pool wrapper that maintains per-socket pools
// This significantly reduces memory access latency on multi-socket servers
template <Allocatable T>
class NumaMemoryPool {
  private:
    struct NumaNode {
        int node_id;
        std::unique_ptr<MemoryPool<T>> pool;

        NumaNode(int id, size_t capacity, PoolDepletionPolicy policy, const CacheConfig& config)
            : node_id(id), pool(std::make_unique<MemoryPool<T>>(capacity, policy, config)) {}
    };

    std::vector<NumaNode> numa_pools_;
    bool numa_available_;
    size_t objects_per_node_;
    PoolDepletionPolicy depletion_policy_;
    CacheConfig cache_config_;

    // Get the NUMA node for the current CPU
    [[nodiscard]] int get_current_numa_node() const noexcept {
#if defined(__linux__) && defined(HAVE_NUMA)
        if (numa_available_) {
            int cpu = sched_getcpu();
            if (cpu >= 0) {
                return numa_node_of_cpu(cpu);
            }
        }
#endif
        return 0;  // Default to node 0 if NUMA not available
    }

  public:
    explicit NumaMemoryPool(size_t total_capacity,
                            PoolDepletionPolicy policy = PoolDepletionPolicy::TERMINATE_PROCESS,
                            const CacheConfig& config = CacheConfig{})
        : numa_available_(false), objects_per_node_(total_capacity), depletion_policy_(policy), cache_config_(config) {
#if defined(__linux__) && defined(HAVE_NUMA)
        // Check if NUMA is available
        if (numa_available() >= 0) {
            numa_available_ = true;
            int num_nodes = numa_num_configured_nodes();

            if (num_nodes > 1) {
                // Distribute capacity across NUMA nodes
                objects_per_node_ = total_capacity / num_nodes;

                // Create per-node pools
                for (int node = 0; node < num_nodes; ++node) {
                    // Set memory allocation policy for this node
                    numa_set_preferred(node);

                    // Create pool on this node
                    numa_pools_.emplace_back(node, objects_per_node_, policy, config);

                    // Reset to default policy
                    numa_set_preferred(-1);
                }

                return;
            }
        }
#endif

        // Fallback: Create single pool if NUMA not available
        numa_pools_.emplace_back(0, total_capacity, policy, config);
    }

    // Allocate from the pool on the current NUMA node
    [[clang::always_inline]] [[nodiscard]] T* allocate() {
        int node = get_current_numa_node();

        // Clamp to valid range
        if (node < 0 || node >= static_cast<int>(numa_pools_.size())) {
            node = 0;
        }

        return numa_pools_[node].pool->allocate();
    }

    // Deallocate to the appropriate NUMA node pool
    [[clang::always_inline]] void deallocate(T* ptr) {
        if (!ptr) [[unlikely]]
            return;

        // Find which pool owns this pointer
        for (auto& numa_node : numa_pools_) {
            if (numa_node.pool->owns(ptr)) {
                numa_node.pool->deallocate(ptr);
                return;
            }
        }

        // If not found in any pool, it's an error
        // In production, we might want to handle this more gracefully
        std::terminate();
    }

    // Construct object on the current NUMA node
    template <typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        int node = get_current_numa_node();

        // Clamp to valid range
        if (node < 0 || node >= static_cast<int>(numa_pools_.size())) {
            node = 0;
        }

        return numa_pools_[node].pool->construct(std::forward<Args>(args)...);
    }

    // Destroy object
    void destroy(T* ptr) {
        if (!ptr) [[unlikely]]
            return;

        // Find which pool owns this pointer
        for (auto& numa_node : numa_pools_) {
            if (numa_node.pool->owns(ptr)) {
                numa_node.pool->destroy(ptr);
                return;
            }
        }

        // If not found in any pool, it's an error
        std::terminate();
    }

    // Batch allocation on current NUMA node
    [[nodiscard]] std::vector<T*> allocate_batch(size_t count) {
        int node = get_current_numa_node();

        // Clamp to valid range
        if (node < 0 || node >= static_cast<int>(numa_pools_.size())) {
            node = 0;
        }

        return numa_pools_[node].pool->allocate_batch(count);
    }

    // Batch deallocation
    void deallocate_batch(const std::vector<T*>& ptrs) {
        if (ptrs.empty()) [[unlikely]]
            return;

        // Group by owning pool for efficiency
        for (auto& numa_node : numa_pools_) {
            std::vector<T*> node_ptrs;

            for (T* ptr : ptrs) {
                if (ptr && numa_node.pool->owns(ptr)) {
                    node_ptrs.push_back(ptr);
                }
            }

            if (!node_ptrs.empty()) {
                numa_node.pool->deallocate_batch(node_ptrs);
            }
        }
    }

    // Get total allocated objects across all nodes
    [[nodiscard]] size_t allocated_objects() const noexcept {
        size_t total = 0;
        for (const auto& numa_node : numa_pools_) {
            total += numa_node.pool->allocated_objects();
        }
        return total;
    }

    // Get total capacity across all nodes
    [[nodiscard]] size_t object_capacity() const noexcept {
        size_t total = 0;
        for (const auto& numa_node : numa_pools_) {
            total += numa_node.pool->object_capacity();
        }
        return total;
    }

    // Get utilization across all nodes
    [[nodiscard]] double utilization() const noexcept {
        return static_cast<double>(allocated_objects()) / object_capacity();
    }

    // Flush all thread-local caches
    void flush_thread_cache() {
        for (auto& numa_node : numa_pools_) {
            numa_node.pool->flush_thread_cache();
        }
    }

    // Get number of NUMA nodes
    [[nodiscard]] size_t num_nodes() const noexcept {
        return numa_pools_.size();
    }

    // Check if NUMA is available and being used
    [[nodiscard]] bool is_numa_aware() const noexcept {
        return numa_available_ && numa_pools_.size() > 1;
    }

    // Get per-node statistics
    struct NodeStats {
        int node_id;
        size_t allocated;
        size_t capacity;
        double utilization;
        bool uses_huge_pages;
    };

    [[nodiscard]] std::vector<NodeStats> get_node_stats() const {
        std::vector<NodeStats> stats;

        for (const auto& numa_node : numa_pools_) {
            stats.push_back({numa_node.node_id,
                             numa_node.pool->allocated_objects(),
                             numa_node.pool->object_capacity(),
                             numa_node.pool->utilization(),
                             numa_node.pool->uses_huge_pages()});
        }

        return stats;
    }

    // Pin thread to specific NUMA node (utility function)
    static void pin_thread_to_node(int node) {
#if defined(__linux__) && defined(HAVE_NUMA)
        if (numa_available() >= 0) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);

            // Get CPUs for this NUMA node
            struct bitmask* cpus = numa_allocate_cpumask();
            if (numa_node_to_cpus(node, cpus) == 0) {
                // Set CPU affinity to CPUs on this node
                for (int cpu = 0; cpu < numa_num_configured_cpus(); ++cpu) {
                    if (numa_bitmask_isbitset(cpus, cpu)) {
                        CPU_SET(cpu, &cpuset);
                    }
                }

                pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
            }
            numa_free_cpumask(cpus);

            // Also set memory policy
            numa_set_preferred(node);
        }
#else
        (void)node;  // Suppress unused parameter warning
#endif
    }
};

}  // namespace crypto_lob::core