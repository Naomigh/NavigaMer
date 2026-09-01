#include "navigamer/builder.hpp"
#include "navigamer/edit_distance.hpp"
#include "navigamer/index.hpp"
#include "navigamer/query.hpp"
#include "navigamer/sequence_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
using namespace navigamer;

class Arguments {
 public:
  Arguments(int argc, char** argv, int begin) {
    for (int i = begin; i < argc; ++i) {
      std::string key = argv[i];
      if (!key.starts_with("--")) {
        throw std::invalid_argument("unexpected positional argument: " + key);
      }
      if (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--")) {
        values_[key] = argv[++i];
      } else {
        flags_.insert(key);
      }
    }
  }
  [[nodiscard]] std::string require(std::string_view key) const {
    if (const auto found = values_.find(std::string(key)); found != values_.end()) {
      return found->second;
    }
    throw std::invalid_argument("missing required option " + std::string(key));
  }
  [[nodiscard]] std::string get(std::string_view key,
                                std::string default_value = {}) const {
    if (const auto found = values_.find(std::string(key)); found != values_.end()) {
      return found->second;
    }
    return default_value;
  }
  [[nodiscard]] std::uint64_t number(std::string_view key,
                                     std::uint64_t default_value) const {
    const auto value = get(key);
    return value.empty() ? default_value : std::stoull(value);
  }
  [[nodiscard]] bool flag(std::string_view key) const {
    return flags_.contains(std::string(key));
  }

 private:
  std::unordered_map<std::string, std::string> values_;
  std::unordered_set<std::string> flags_;
};

std::vector<std::uint32_t> parse_radii(std::string_view value) {
  std::vector<std::uint32_t> radii;
  std::stringstream input{std::string(value)};
  std::string token;
  while (std::getline(input, token, ',')) {
    radii.push_back(static_cast<std::uint32_t>(std::stoul(token)));
  }
  if (radii.empty()) throw std::invalid_argument("empty radius schedule");
  return radii;
}

BuildMode parse_build_mode(std::string_view value) {
  if (value == "nested" || value == "nested-balls") {
    return BuildMode::kNestedBalls;
  }
  if (value == "owner" || value == "nearest-owner") {
    return BuildMode::kNearestOwner;
  }
  throw std::invalid_argument("--build-mode must be nested or owner");
}

void add_stats(QueryStats& total, const QueryStats& value) {
  total.edit_distance_calls += value.edit_distance_calls;
  total.top_center_edlib_calls += value.top_center_edlib_calls;
  total.middle_center_edlib_calls += value.middle_center_edlib_calls;
  total.leaf_center_edlib_calls += value.leaf_center_edlib_calls;
  total.beacon_edlib_calls += value.beacon_edlib_calls;
  total.path_pivot_query_edlib_calls += value.path_pivot_query_edlib_calls;
  total.path_pivot_row_edlib_calls += value.path_pivot_row_edlib_calls;
  total.query_anchor_edlib_calls += value.query_anchor_edlib_calls;
  total.leaf_verification_edlib_calls +=
      value.leaf_verification_edlib_calls;
  total.uncategorized_edlib_calls += value.uncategorized_edlib_calls;
  total.worlds_considered += value.worlds_considered;
  total.worlds_mbb_pruned += value.worlds_mbb_pruned;
  total.world_center_distances += value.world_center_distances;
  total.leaf_members_considered += value.leaf_members_considered;
  total.leaf_members_mbb_pruned += value.leaf_members_mbb_pruned;
  total.exact_verifications += value.exact_verifications;
  total.cache_fast_paths += value.cache_fast_paths;
  total.greedy_single_steps += value.greedy_single_steps;
  total.boundary_steps += value.boundary_steps;
}

std::uint32_t thread_count(const Arguments& args) {
  const auto requested = static_cast<std::uint32_t>(args.number("--threads", 0));
  return requested == 0 ? std::max(1U, std::thread::hardware_concurrency())
                        : requested;
}

void build_command(const Arguments& args) {
  const auto reference_path = args.require("--reference");
  const auto output_path = args.require("--output");
  const auto window = static_cast<std::uint32_t>(args.number("--window", 150));
  const auto stride = static_cast<std::uint32_t>(args.number("--stride", 1));
  const auto limit = args.number("--limit", 0);
  auto reference =
      SequenceStore::from_fasta(reference_path, window, stride, limit);
  BuildConfig config;
  config.radii = parse_radii(args.get("--radii", "65,35,15"));
  config.max_beacons =
      static_cast<std::uint32_t>(args.number("--beacons", 4));
  config.hot_cache_size =
      static_cast<std::uint32_t>(args.number("--hot-cache", 16));
  config.delayed_centers = !args.flag("--no-delayed-centers");
  config.mode = parse_build_mode(args.get("--build-mode", "nested"));
  config.exact_global_reuse = !args.flag("--local-creation");
  config.threads = thread_count(args);
  BuildStats stats;
  IndexBuilder builder(reference);
  auto index = builder.build(config, &stats);
  index.save(output_path);
  std::ostringstream report;
  report << std::setprecision(12) << index.summary()
         << "edit_distance_calls\t" << stats.edit_distance_calls << '\n'
         << "center_seconds\t" << stats.center_seconds << '\n'
         << "owner_seconds\t" << stats.owner_seconds << '\n'
         << "packing_seconds\t" << stats.packing_seconds << '\n'
         << "topology_seconds\t" << stats.topology_seconds << '\n'
         << "mbb_seconds\t" << stats.mbb_seconds << '\n'
         << "world_edges\t" << stats.world_edges << '\n'
         << "terminal_memberships\t" << stats.terminal_memberships << '\n';
  std::cout << report.str();
  if (const auto stats_path = args.get("--stats-output"); !stats_path.empty()) {
    std::ofstream stats_file(stats_path);
    if (!stats_file) {
      throw std::runtime_error("cannot create build stats: " + stats_path);
    }
    stats_file << report.str();
  }
}

struct StrandResult {
  std::vector<QueryHit> forward;
  std::vector<QueryHit> reverse;
  QueryStats stats;
  double query_seconds{0.0};
};

void query_command(const Arguments& args) {
  const auto index_path = args.require("--index");
  const auto reference_path = args.require("--reference");
  const auto query_path = args.require("--queries");
  auto index = NavigaMerIndex::load(index_path);
  auto reference = SequenceStore::from_fasta(reference_path, index.window_length,
                                             index.stride, index.reference_count);
  index.validate(&reference);
  auto queries = read_queries(query_path);
  const auto query_limit = args.number("--limit", 0);
  if (query_limit != 0 && queries.size() > query_limit) {
    queries.resize(query_limit);
  }
  QueryConfig config;
  config.tolerance =
      static_cast<std::uint32_t>(args.number("--tolerance", 5));
  config.cache_similarity =
      static_cast<std::uint32_t>(args.number("--cache-similarity", 8));
  config.anchor_refresh_interval =
      static_cast<std::uint32_t>(args.number("--anchor-refresh", 8));
  config.enable_path_cache = !args.flag("--no-cache");
  const bool both_strands = args.flag("--both-strands");
  auto threads = std::min<std::uint32_t>(thread_count(args),
      std::max<std::size_t>(1, queries.size()));
  const auto block_size = std::max<std::uint64_t>(1,
      args.number("--block-size", 64));
  std::vector<StrandResult> results(queries.size());
  std::vector<std::future<void>> futures;
  std::atomic<std::uint64_t> next_block{0};
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    futures.push_back(std::async(std::launch::async, [&] {
      QueryEngine engine(index, reference);
      while (true) {
        const auto begin = next_block.fetch_add(block_size);
        if (begin >= queries.size()) break;
        const auto end = std::min<std::uint64_t>(queries.size(),
                                                 begin + block_size);
        PathCache forward_cache, reverse_cache;
        for (std::size_t i = begin; i < end; ++i) {
          const auto query_start = std::chrono::steady_clock::now();
          QueryStats forward_stats;
          results[i].forward = engine.query(queries[i].sequence, config,
                                            &forward_stats, &forward_cache);
          add_stats(results[i].stats, forward_stats);
          if (both_strands) {
            QueryStats reverse_stats;
            const auto rc = reverse_complement(queries[i].sequence);
            results[i].reverse = engine.query(rc, config, &reverse_stats,
                                              &reverse_cache);
            add_stats(results[i].stats, reverse_stats);
          }
          results[i].query_seconds = std::chrono::duration<double>(
              std::chrono::steady_clock::now() - query_start).count();
        }
      }
    }));
  }
  for (auto& future : futures) future.get();
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();

  std::ofstream file;
  std::ostream* output = &std::cout;
  if (const auto output_path = args.get("--output"); !output_path.empty()) {
    file.open(output_path);
    if (!file) throw std::runtime_error("cannot create output: " + output_path);
    output = &file;
  }
  *output << "query\tstrand\tsequence_id\tcontig\tposition\tdistance\n";
  QueryStats total;
  double worker_query_seconds = 0.0;
  for (std::size_t i = 0; i < queries.size(); ++i) {
    auto emit = [&](const std::vector<QueryHit>& hits, char strand) {
      for (const auto& hit : hits) {
        const auto [contig, position] = reference.location(hit.sequence_id);
        *output << queries[i].name << '\t' << strand << '\t' << hit.sequence_id
                << '\t' << contig << '\t' << position << '\t' << hit.distance
                << '\n';
      }
    };
    emit(results[i].forward, '+');
    emit(results[i].reverse, '-');
    add_stats(total, results[i].stats);
    worker_query_seconds += results[i].query_seconds;
  }
  if (const auto timings_path = args.get("--timings"); !timings_path.empty()) {
    std::ofstream timings(timings_path);
    if (!timings) {
      throw std::runtime_error("cannot create timings: " + timings_path);
    }
    timings << "query_index\tquery\tworker_query_us\thits"
               "\tedit_distance_calls\ttop_center_edlib_calls"
               "\tmiddle_center_edlib_calls\tleaf_center_edlib_calls"
               "\tbeacon_edlib_calls\tpath_pivot_query_edlib_calls"
               "\tpath_pivot_row_edlib_calls\tquery_anchor_edlib_calls"
               "\tleaf_verification_edlib_calls"
               "\tuncategorized_edlib_calls"
               "\tworlds_considered\tworld_center_distances"
               "\tleaf_members_considered\texact_verifications"
               "\tgreedy_single_steps\tboundary_steps\n";
    timings << std::setprecision(12);
    for (std::size_t i = 0; i < queries.size(); ++i) {
      timings << i << '\t' << queries[i].name << '\t'
              << results[i].query_seconds * 1e6 << '\t'
              << results[i].forward.size() + results[i].reverse.size() << '\t'
              << results[i].stats.edit_distance_calls << '\t'
              << results[i].stats.top_center_edlib_calls << '\t'
              << results[i].stats.middle_center_edlib_calls << '\t'
              << results[i].stats.leaf_center_edlib_calls << '\t'
              << results[i].stats.beacon_edlib_calls << '\t'
              << results[i].stats.path_pivot_query_edlib_calls << '\t'
              << results[i].stats.path_pivot_row_edlib_calls << '\t'
              << results[i].stats.query_anchor_edlib_calls << '\t'
              << results[i].stats.leaf_verification_edlib_calls << '\t'
              << results[i].stats.uncategorized_edlib_calls << '\t'
              << results[i].stats.worlds_considered << '\t'
              << results[i].stats.world_center_distances << '\t'
              << results[i].stats.leaf_members_considered << '\t'
              << results[i].stats.exact_verifications << '\t'
              << results[i].stats.greedy_single_steps << '\t'
              << results[i].stats.boundary_steps << '\n';
    }
  }
  const double query_count = static_cast<double>(queries.size());
  std::ostringstream report;
  report << std::setprecision(12)
         << "queries\t" << queries.size() << "\nseconds\t" << elapsed
         << "\nqueries_per_second\t"
         << (elapsed == 0 ? 0 : query_count / elapsed)
         << "\naverage_wall_microseconds_per_query\t"
         << (queries.empty() ? 0 : elapsed * 1e6 / query_count)
         << "\nmean_worker_microseconds_per_query\t"
         << (queries.empty() ? 0 : worker_query_seconds * 1e6 / query_count)
         << "\nedit_distance_calls\t" << total.edit_distance_calls
         << "\ntop_center_edlib_calls\t" << total.top_center_edlib_calls
         << "\nmiddle_center_edlib_calls\t"
         << total.middle_center_edlib_calls
         << "\nleaf_center_edlib_calls\t" << total.leaf_center_edlib_calls
         << "\nbeacon_edlib_calls\t" << total.beacon_edlib_calls
         << "\npath_pivot_query_edlib_calls\t"
         << total.path_pivot_query_edlib_calls
         << "\npath_pivot_row_edlib_calls\t"
         << total.path_pivot_row_edlib_calls
         << "\nquery_anchor_edlib_calls\t" << total.query_anchor_edlib_calls
         << "\nleaf_verification_edlib_calls\t"
         << total.leaf_verification_edlib_calls
         << "\nuncategorized_edlib_calls\t"
         << total.uncategorized_edlib_calls
         << "\nworlds_considered\t" << total.worlds_considered
         << "\nworlds_mbb_pruned\t" << total.worlds_mbb_pruned
         << "\nworld_center_distances\t" << total.world_center_distances
         << "\nleaf_members_considered\t" << total.leaf_members_considered
         << "\nleaf_members_mbb_pruned\t" << total.leaf_members_mbb_pruned
         << "\nexact_verifications\t" << total.exact_verifications
         << "\ncache_fast_paths\t" << total.cache_fast_paths
         << "\ngreedy_single_steps\t" << total.greedy_single_steps
         << "\nboundary_steps\t" << total.boundary_steps << '\n';
  std::cerr << report.str();
  if (const auto stats_path = args.get("--stats-output"); !stats_path.empty()) {
    std::ofstream stats_file(stats_path);
    if (!stats_file) {
      throw std::runtime_error("cannot create query stats: " + stats_path);
    }
    stats_file << report.str();
  }
}

void inspect_command(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.require("--index"));
  std::cout << index.summary();
}

void verify_command(const Arguments& args) {
  const auto index_path = args.require("--index");
  const auto reference_path = args.require("--reference");
  const auto query_path = args.require("--queries");
  auto index = NavigaMerIndex::load(index_path);
  auto reference = SequenceStore::from_fasta(reference_path, index.window_length,
                                             index.stride, index.reference_count);
  index.validate(&reference);
  auto queries = read_queries(query_path);
  const auto limit = args.number("--limit", 0);
  if (limit != 0 && queries.size() > limit) queries.resize(limit);
  QueryConfig config;
  config.tolerance =
      static_cast<std::uint32_t>(args.number("--tolerance", 5));
  config.enable_path_cache = args.flag("--with-cache");
  config.anchor_refresh_interval =
      static_cast<std::uint32_t>(args.number("--anchor-refresh", 8));
  const auto threads = std::min<std::uint32_t>(thread_count(args),
      std::max<std::size_t>(1, queries.size()));
  struct Difference {
    std::uint64_t false_negatives{0};
    std::uint64_t false_positives{0};
    std::uint64_t expected_hits{0};
    std::uint64_t observed_hits{0};
  };
  std::vector<Difference> differences(threads);
  std::vector<std::future<void>> futures;
  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t thread = 0; thread < threads; ++thread) {
    const auto begin = queries.size() * thread / threads;
    const auto end = queries.size() * (thread + 1) / threads;
    futures.push_back(std::async(std::launch::async, [&, thread, begin, end] {
      QueryEngine engine(index, reference);
      EditDistance distance;
      PathCache path_cache;
      for (std::size_t i = begin; i < end; ++i) {
        const auto observed_hits = engine.query(
            queries[i].sequence, config, nullptr,
            config.enable_path_cache ? &path_cache : nullptr);
        std::vector<SequenceId> observed;
        observed.reserve(observed_hits.size());
        for (const auto& hit : observed_hits) observed.push_back(hit.sequence_id);
        std::vector<SequenceId> expected;
        for (SequenceId id = 0; id < reference.size(); ++id) {
          if (distance(queries[i].sequence, reference.sequence(id),
                       config.tolerance) >= 0) {
            expected.push_back(id);
          }
        }
        std::vector<SequenceId> missing, extra;
        std::set_difference(expected.begin(), expected.end(), observed.begin(),
                            observed.end(), std::back_inserter(missing));
        std::set_difference(observed.begin(), observed.end(), expected.begin(),
                            expected.end(), std::back_inserter(extra));
        differences[thread].false_negatives += missing.size();
        differences[thread].false_positives += extra.size();
        differences[thread].expected_hits += expected.size();
        differences[thread].observed_hits += observed.size();
      }
    }));
  }
  for (auto& future : futures) future.get();
  Difference total;
  for (const auto& difference : differences) {
    total.false_negatives += difference.false_negatives;
    total.false_positives += difference.false_positives;
    total.expected_hits += difference.expected_hits;
    total.observed_hits += difference.observed_hits;
  }
  const auto elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  std::ostringstream report;
  report << std::setprecision(12)
         << "verified_queries\t" << queries.size()
         << "\nreference_sequences\t" << reference.size()
         << "\nfalse_negatives\t" << total.false_negatives
         << "\nfalse_positives\t" << total.false_positives
         << "\nexpected_hits\t" << total.expected_hits
         << "\nobserved_hits\t" << total.observed_hits
         << "\nfalse_negative_rate\t"
         << (total.expected_hits == 0 ? 0.0
             : static_cast<double>(total.false_negatives) /
                   static_cast<double>(total.expected_hits))
         << "\nfalse_positive_rate\t"
         << (total.observed_hits == 0 ? 0.0
             : static_cast<double>(total.false_positives) /
                   static_cast<double>(total.observed_hits))
         << "\nseconds\t" << elapsed << '\n';
  std::cout << report.str();
  if (const auto output_path = args.get("--output"); !output_path.empty()) {
    std::ofstream output(output_path);
    if (!output) {
      throw std::runtime_error("cannot create verification report: " + output_path);
    }
    output << report.str();
  }
  if (total.false_negatives != 0 || total.false_positives != 0) {
    throw std::runtime_error("hierarchy differs from brute-force edit distance");
  }
}

void usage(std::ostream& out) {
  out << "NavigaMer: exact multilateration hierarchy (never seed-and-extend)\n\n"
      << "Build:\n  navigamer build --reference ref.fa --output ref.nvm "
         "[--window 150] [--stride 1] [--radii 65,35,15] "
         "[--build-mode nested|owner] "
         "[--beacons 4] [--threads N] [--no-delayed-centers] [--limit N] "
         "[--local-creation] "
         "[--stats-output build.tsv]\n\n"
      << "Query:\n  navigamer query --index ref.nvm --reference ref.fa "
         "--queries reads.fq [--tolerance 5] [--threads N] [--both-strands] "
         "[--no-cache] [--output hits.tsv] [--timings timings.tsv] "
         "[--stats-output query.tsv] [--anchor-refresh 8] [--block-size 64] "
         "[--limit N]\n\n"
      << "Inspect:\n  navigamer inspect --index ref.nvm\n";
  out << "\nVerify exactness against brute force:\n  navigamer verify "
         "--index ref.nvm --reference ref.fa --queries queries.fa "
         "[--tolerance 5] [--limit N] [--threads N] [--with-cache] "
         "[--anchor-refresh 8] [--output verify.tsv]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h") {
      usage(std::cout);
      return argc < 2 ? 1 : 0;
    }
    const std::string command = argv[1];
    const Arguments args(argc, argv, 2);
    if (command == "build") {
      build_command(args);
    } else if (command == "query") {
      query_command(args);
    } else if (command == "inspect") {
      inspect_command(args);
    } else if (command == "verify") {
      verify_command(args);
    } else {
      throw std::invalid_argument("unknown command: " + command);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
