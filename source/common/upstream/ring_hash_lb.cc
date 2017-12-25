#include "common/upstream/ring_hash_lb.h"

#include <cstdint>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/upstream/load_balancer_impl.h"

#include "absl/strings/string_view.h"

namespace Envoy {
namespace Upstream {

RingHashLoadBalancer::RingHashLoadBalancer(
    PrioritySet& priority_set, ClusterStats& stats, Runtime::Loader& runtime,
    Runtime::RandomGenerator& random,
    const Optional<envoy::api::v2::Cluster::RingHashLbConfig>& config)
    : LoadBalancerBase(priority_set, stats, runtime, random), config_(config),
      factory_(new LoadBalancerFactoryImpl(stats, random)) {
  // Make sure we correctly return nullptr for any early chooseHost() calls.
  factory_->current_ring_ = std::make_shared<Ring>(config_, std::vector<HostSharedPtr>{});
}

void RingHashLoadBalancer::initialize() {
  priority_set_.addMemberUpdateCb([this](uint32_t, const std::vector<HostSharedPtr>&,
                                         const std::vector<HostSharedPtr>&) -> void { refresh(); });

  refresh();
}

HostConstSharedPtr
RingHashLoadBalancer::LoadBalancerImpl::chooseHost(LoadBalancerContext* context) {
  if (global_panic_) {
    stats_.lb_healthy_panic_.inc();
  }
  return ring_->chooseHost(context, random_);
}

LoadBalancerPtr RingHashLoadBalancer::LoadBalancerFactoryImpl::create() {
  // We must protect current_ring_ via a RW lock since it is accessed and written to by multiple
  // threads. All complex processing happens outside of locking however.
  RingConstSharedPtr ring_to_use;
  {
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    ring_to_use = current_ring_;
  }

  return std::make_unique<LoadBalancerImpl>(stats_, random_, ring_to_use, global_panic_);
}

HostConstSharedPtr RingHashLoadBalancer::Ring::chooseHost(LoadBalancerContext* context,
                                                          Runtime::RandomGenerator& random) const {
  if (ring_.empty()) {
    return nullptr;
  }

  // If there is no hash in the context, just choose a random value (this effectively becomes
  // the random LB but it won't crash if someone configures it this way).
  // computeHashKey() may be computed on demand, so get it only once.
  Optional<uint64_t> hash;
  if (context) {
    hash = context->computeHashKey();
  }
  const uint64_t h = hash.valid() ? hash.value() : random.random();

  // Ported from https://github.com/RJ/ketama/blob/master/libketama/ketama.c (ketama_get_server)
  // I've generally kept the variable names to make the code easier to compare.
  // NOTE: The algorithm depends on using signed integers for lowp, midp, and highp. Do not
  //       change them!
  int64_t lowp = 0;
  int64_t highp = ring_.size();
  while (true) {
    int64_t midp = (lowp + highp) / 2;

    if (midp == static_cast<int64_t>(ring_.size())) {
      return ring_[0].host_;
    }

    uint64_t midval = ring_[midp].hash_;
    uint64_t midval1 = midp == 0 ? 0 : ring_[midp - 1].hash_;

    if (h <= midval && h > midval1) {
      return ring_[midp].host_;
    }

    if (midval < h) {
      lowp = midp + 1;
    } else {
      highp = midp - 1;
    }

    if (lowp > highp) {
      return ring_[0].host_;
    }
  }
}

RingHashLoadBalancer::Ring::Ring(const Optional<envoy::api::v2::Cluster::RingHashLbConfig>& config,
                                 const std::vector<HostSharedPtr>& hosts) {
  ENVOY_LOG(trace, "ring hash: building ring");
  if (hosts.empty()) {
    return;
  }

  // Currently we specify the minimum size of the ring, and determine the replication factor
  // based on the number of hosts. It's possible we might want to support more sophisticated
  // configuration in the future.
  // NOTE: Currently we keep a ring for healthy hosts and unhealthy hosts, and this is done per
  //       thread. This is the simplest implementation, but it's expensive from a memory
  //       standpoint and duplicates the regeneration computation. In the future we might want
  //       to generate the rings centrally and then just RCU them out to each thread. This is
  //       sufficient for getting started.
  const uint64_t min_ring_size =
      config.valid() ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.value(), minimum_ring_size, 1024)
                     : 1024;

  uint64_t hashes_per_host = 1;
  if (hosts.size() < min_ring_size) {
    hashes_per_host = min_ring_size / hosts.size();
    if ((min_ring_size % hosts.size()) != 0) {
      hashes_per_host++;
    }
  }

  ENVOY_LOG(info, "ring hash: min_ring_size={} hashes_per_host={}", min_ring_size, hashes_per_host);
  ring_.reserve(hosts.size() * hashes_per_host);

  const bool use_std_hash =
      config.valid()
          ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(config.value().deprecated_v1(), use_std_hash, true)
          : true;

  std::unique_ptr<char[]> hash_key_buffer;
  uint64_t last_hash_key_size = 0;
  for (const auto& host : hosts) {
    const std::string& address_string = host->address()->asString();
    uint64_t offset_start = address_string.size();

    // Although in almost all cases the buffer could be stack allocated to 128 bytes or so, we
    // don't explicitly know what is in the address string (e.g., UDS path). We allocate a raw
    // buffer on the heap and use it for all calculations. The needed size is the sizse of the
    // address, plus '_', plus 32 bytes for the index. All of this is done to avoid string
    // allocations in the fast path.
    const uint64_t needed_size = std::min(128UL, address_string.size() + 1 + 32);
    if (hash_key_buffer == nullptr || last_hash_key_size < needed_size) {
      hash_key_buffer.reset(new char[needed_size]);
      last_hash_key_size = needed_size;
    }

    memcpy(hash_key_buffer.get(), address_string.c_str(), offset_start);
    hash_key_buffer[offset_start++] = '_';
    for (uint64_t i = 0; i < hashes_per_host; i++) {
      const uint64_t total_hash_key_len =
          offset_start + StringUtil::itoa(&hash_key_buffer.get()[offset_start], 32, i);
      absl::string_view hash_key(hash_key_buffer.get(), total_hash_key_len);

      // Sadly std::hash provides no mechanism for hashing arbitrary bytes so we must copy here.
      // xxHash is done wihout copies.
      const uint64_t hash = use_std_hash ? std::hash<std::string>()(std::string(hash_key))
                                         : HashUtil::xxHash64(hash_key);
      ENVOY_LOG(trace, "ring hash: hash_key={} hash={}", hash_key.data(), hash);
      ring_.push_back({hash, host});
    }
  }

  std::sort(ring_.begin(), ring_.end(), [](const RingEntry& lhs, const RingEntry& rhs) -> bool {
    return lhs.hash_ < rhs.hash_;
  });
#ifndef NVLOG
  for (auto entry : ring_) {
    ENVOY_LOG(trace, "ring hash: host={} hash={}", entry.host_->address()->asString(), entry.hash_);
  }
#endif
}

void RingHashLoadBalancer::refresh() {
  // Note that we only compute global panic on host set refresh. Given that the runtime setting will
  // rarely changer, this is a reasonable compromise to avoid creating multiple rings when we only
  // need to create one for LB.
  RingConstSharedPtr new_ring;
  const auto& host_set = chooseHostSet();
  if (isGlobalPanic(host_set, runtime_)) {
    new_ring = std::make_shared<Ring>(config_, host_set.hosts());
    factory_->global_panic_ = true;
  } else {
    new_ring = std::make_shared<Ring>(config_, host_set.healthyHosts());
    factory_->global_panic_ = false;
  }

  std::unique_lock<std::shared_timed_mutex> lock(factory_->mutex_);
  factory_->current_ring_ = new_ring;
}

} // namespace Upstream
} // namespace Envoy
