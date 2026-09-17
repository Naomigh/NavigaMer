#include "navigamer/builder.hpp"
#include "navigamer/query.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

using navigamer::NavigaMerIndex;
using Clock = std::chrono::steady_clock;

class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
      const std::string key = argv[i];
      if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
        values_[key] = argv[++i];
      } else {
        values_[key] = "";
      }
    }
  }
  bool has(const std::string& key) const { return values_.contains(key); }
  std::string get(const std::string& key, const std::string& default_value = "") const {
    const auto it = values_.find(key);
    return it == values_.end() ? default_value : it->second;
  }
  std::uint64_t integer(const std::string& key, std::uint64_t default_value) const {
    return has(key) ? std::stoull(get(key)) : default_value;
  }

 private:
  std::map<std::string, std::string> values_;
};

template <typename T, std::size_t N>
std::array<T, N> tuple(const std::string& input) {
  std::array<T, N> result;
  std::size_t begin = 0;
  for (std::size_t i = 0; i < N; ++i) {
    const auto comma = input.find(',', begin);
    const auto value = input.substr(begin, comma - begin);
    if constexpr (std::is_integral_v<T>) result[i] = std::stoul(value);
    else result[i] = std::stod(value);
    begin = comma + 1;
  }
  return result;
}

std::uint32_t worker_count(const Arguments& args, std::uint32_t default_value) {
  return args.integer("--threads", default_value);
}

navigamer::SequenceStore load_reference(const Arguments& args, const NavigaMerIndex& index) {
  return navigamer::SequenceStore::from_fasta(args.get("--reference"),
      index.window_length, index.stride, index.reference_window_count);
}

std::uint64_t peak_rss_bytes() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
}

navigamer::QueryConfig query_config(const Arguments& args, const NavigaMerIndex& index) {
  navigamer::QueryConfig config;
  config.tolerance = args.integer("--tolerance", index.max_tolerance);
  config.route_mode = args.get("--route", "multilateration") == "scan"
      ? navigamer::RouteMode::kScan : navigamer::RouteMode::kMultilateration;
  config.use_cache = !args.has("--no-cache");
  config.both_strands = args.has("--both-strands");
  config.enable_prefetch = args.has("--prefetch");
  return config;
}

struct BatchResult {
  std::vector<std::vector<navigamer::QueryHit>> hits;
  std::vector<std::uint64_t> latency_ns;
  navigamer::QueryStats stats;
  double seconds;
};

BatchResult run_batch(const NavigaMerIndex& index,
    const navigamer::SequenceStore& reference,
    const std::vector<navigamer::QueryRecord>& records,
    const navigamer::QueryConfig& config, std::uint32_t threads,
    std::uint64_t block_size, std::uint64_t repeat, bool retain_hits) {
  const auto total = records.size() * repeat;
  BatchResult result;
  if (retain_hits) result.hits.resize(total);
  result.latency_ns.resize(total);
  std::vector<navigamer::QueryStats> thread_stats(threads);
  std::atomic<std::uint64_t> next{0};
  const auto start = Clock::now();
  auto worker = [&](std::uint32_t worker_id) {
    navigamer::QueryEngine engine(index, reference);
    navigamer::PathCache cache;
    while (true) {
      const auto begin = next.fetch_add(block_size);
      if (begin >= total) break;
      const auto end = std::min<std::uint64_t>(total, begin + block_size);
      for (auto i = begin; i < end; ++i) {
        navigamer::QueryStats stats;
        const auto query_start = Clock::now();
        auto hits = engine.query(records[i % records.size()].sequence, config, &cache, &stats);
        result.latency_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - query_start).count();
        thread_stats[worker_id] += stats;
        if (retain_hits) result.hits[i] = std::move(hits);
      }
    }
  };
  std::vector<std::thread> workers;
  for (std::uint32_t i = 1; i < threads; ++i) workers.emplace_back(worker, i);
  worker(0);
  for (auto& thread : workers) thread.join();
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  for (const auto& stats : thread_stats) result.stats += stats;
  return result;
}

double percentile(std::vector<std::uint64_t> values, double fraction) {
  const auto ordinal = static_cast<std::size_t>(fraction * (values.size() - 1));
  std::nth_element(values.begin(), values.begin() + ordinal, values.end());
  return values[ordinal] / 1000.0;
}

void print_batch_stats(const BatchResult& result, std::uint64_t queries, std::uint32_t threads) {
  const long double sum = std::accumulate(result.latency_ns.begin(), result.latency_ns.end(),
                                         static_cast<long double>(0));
  const auto& stats = result.stats;
  std::cout << std::fixed << std::setprecision(3)
      << "queries=" << queries << " threads=" << threads
      << " wall_seconds=" << result.seconds << " throughput_qps=" << queries / result.seconds
      << " mean_latency_us=" << static_cast<double>(sum / queries / 1000)
      << " p50_us=" << percentile(result.latency_ns, 0.50)
      << " p95_us=" << percentile(result.latency_ns, 0.95)
      << " p99_us=" << percentile(result.latency_ns, 0.99)
      << " peak_rss_bytes=" << peak_rss_bytes() << '\n'
      << "avg_route_edits=" << static_cast<double>(stats.routing_edit_calls()) / queries
      << " avg_exact_verifications=" << static_cast<double>(stats.exact_verifications) / queries
      << " avg_leaf_members=" << static_cast<double>(stats.leaf_members) / queries
      << " avg_cache_candidates=" << static_cast<double>(stats.cache_candidates) / queries << '\n';
}

void command_build(const Arguments& args) {
  auto reference = navigamer::SequenceStore::from_fasta(args.get("--reference"),
      args.integer("--window", 150), args.integer("--stride", 1), args.integer("--limit", 0));
  navigamer::BuildConfig config;
  config.center_radii = tuple<std::uint32_t, 3>(args.get("--radii", "60,35,15"));
  config.beacon_ratios = tuple<double, 2>(args.get("--beacon-ratios", "0.1,0.1"));
  config.max_tolerance = args.integer("--T", 5);
  config.threads = worker_count(args, std::thread::hardware_concurrency());
  const auto start = Clock::now();
  auto index = navigamer::IndexBuilder::build(reference, config);
  const auto built = Clock::now();
  index.save(args.get("--index"));
  std::cout << index.summary()
      << "build_seconds=" << std::chrono::duration<double>(built - start).count()
      << " save_seconds=" << std::chrono::duration<double>(Clock::now() - built).count()
      << " peak_rss_bytes=" << peak_rss_bytes() << '\n';
}

void command_query(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.get("--index"));
  const auto reference = load_reference(args, index);
  const auto records = navigamer::read_queries(args.get("--queries"), args.integer("--limit", 0));
  const auto threads = worker_count(args, 1);
  const auto result = run_batch(index, reference, records, query_config(args, index), threads,
      args.integer("--block-size", 64), 1, !args.has("--no-output"));
  if (!args.has("--no-output")) {
    std::ofstream file;
    std::ostream* out = &std::cout;
    if (args.has("--output")) {
      file.open(args.get("--output"));
      out = &file;
    }
    *out << "query\tcontig\tstart\tdistance\tstrand\tsequence_id\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
      for (const auto& hit : result.hits[i]) {
        const auto [contig, position] = reference.location(hit.occurrence_id);
        *out << records[i].name << '\t' << contig << '\t' << position << '\t'
             << hit.distance << '\t' << (hit.reverse ? '-' : '+') << '\t'
             << hit.occurrence_id << '\n';
      }
    }
  }
  print_batch_stats(result, records.size(), threads);
}

void command_benchmark(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.get("--index"));
  const auto reference = load_reference(args, index);
  const auto records = navigamer::read_queries(args.get("--queries"), args.integer("--limit", 0));
  const auto config = query_config(args, index);
  const auto threads = worker_count(args, std::thread::hardware_concurrency());
  const auto block = args.integer("--block-size", 64);
  const auto repeat = args.integer("--repeat", 1);
  const auto warmup = args.integer("--warmup", 0);
  if (warmup != 0) {
    const auto count = std::min<std::uint64_t>(warmup, records.size());
    const std::vector<navigamer::QueryRecord> warm(records.begin(), records.begin() + count);
    run_batch(index, reference, warm, config, threads, block, 1, false);
  }
  const auto result = run_batch(index, reference, records, config, threads, block, repeat, false);
  print_batch_stats(result, records.size() * repeat, threads);
}

int command_verify(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.get("--index"));
  const auto reference = load_reference(args, index);
  const auto records = navigamer::read_queries(args.get("--queries"), args.integer("--limit", 100));
  const auto config = query_config(args, index);
  navigamer::QueryEngine engine(index, reference);
  navigamer::PathCache cache;
  std::uint64_t false_negatives = 0, false_positives = 0, expected_total = 0, observed_total = 0;
  auto key = [](const auto& hit) {
    return std::tuple(hit.occurrence_id, hit.reverse, hit.distance, hit.unique_id);
  };
  for (const auto& record : records) {
    auto expected = navigamer::brute_force_query(index, reference, record.sequence, config.tolerance);
    if (config.both_strands) {
      auto reverse = navigamer::brute_force_query(index, reference,
          navigamer::reverse_complement(record.sequence), config.tolerance, true);
      expected.insert(expected.end(), reverse.begin(), reverse.end());
    }
    std::sort(expected.begin(), expected.end(), [&](const auto& lhs, const auto& rhs) {
      return key(lhs) < key(rhs);
    });
    const auto observed = engine.query(record.sequence, config, &cache);
    expected_total += expected.size();
    observed_total += observed.size();
    std::size_t i = 0, j = 0;
    while (i < expected.size() || j < observed.size()) {
      if (j == observed.size() || (i < expected.size() && key(expected[i]) < key(observed[j]))) {
        ++false_negatives; ++i;
      } else if (i == expected.size() || key(observed[j]) < key(expected[i])) {
        ++false_positives; ++j;
      } else {
        ++i; ++j;
      }
    }
  }
  std::cout << "queries=" << records.size() << " expected=" << expected_total
      << " observed=" << observed_total << " false_negatives=" << false_negatives
      << " false_positives=" << false_positives << '\n';
  return false_negatives != 0 || false_positives != 0;
}

void usage() {
  std::cout << "NavigaMer\n"
      "  build --reference ref.fa --index out.nvm [--window 150] [--stride 1]"
      " [--T 5] [--radii 60,35,15] [--beacon-ratios 0.1,0.1] [--threads N]\n"
      "  query --reference ref.fa --index out.nvm --queries reads.fa"
      " [--tolerance T] [--route multilateration|scan] [--threads N] [--output hits.tsv]\n"
      "  inspect --index out.nvm\n"
      "  verify --reference ref.fa --index out.nvm --queries reads.fa [--limit 100]\n"
      "  benchmark --reference ref.fa --index out.nvm --queries reads.fa"
      " [--repeat N] [--warmup N] [--threads N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string command = argc == 1 ? "help" : argv[1];
  const Arguments args(argc, argv);
  if (command == "build") command_build(args);
  else if (command == "query") command_query(args);
  else if (command == "inspect") std::cout << NavigaMerIndex::load(args.get("--index")).summary();
  else if (command == "verify") return command_verify(args);
  else if (command == "benchmark") command_benchmark(args);
  else usage();
  return 0;
}
