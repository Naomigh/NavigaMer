#include "navigamer/builder.hpp"
#include "navigamer/query.hpp"
#ifdef NAVIGAMER_HAVE_ARCHIVE
#include "navigamer/archive.hpp"
#include <csignal>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
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

template <typename T>
std::vector<T> values_list(const std::string& input) {
  std::vector<T> result;
  std::size_t begin = 0;
  for (;;) {
    const auto comma = input.find(',', begin);
    const auto value = input.substr(begin, comma - begin);
    if (value.empty()) throw std::invalid_argument("empty tuple value: " + input);
    std::size_t used = 0;
    if constexpr (std::is_integral_v<T>) {
      if (value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("expected unsigned integer: " + value);
      }
      const auto parsed = std::stoull(value, &used);
      if (parsed > std::numeric_limits<T>::max()) {
        throw std::out_of_range("tuple value is too large: " + value);
      }
      result.push_back(static_cast<T>(parsed));
    } else {
      result.push_back(static_cast<T>(std::stod(value, &used)));
    }
    if (used != value.size()) {
      throw std::invalid_argument("invalid tuple value: " + value);
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return result;
}

std::uint32_t worker_count(const Arguments& args, std::uint32_t default_value) {
  const auto value = args.integer("--threads", default_value);
  if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("--threads must be a positive 32-bit integer");
  }
  return static_cast<std::uint32_t>(value);
}

std::uint32_t integer32(const Arguments& args, const std::string& key,
                        std::uint32_t default_value) {
  const auto value = args.integer(key, default_value);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::out_of_range(key + " exceeds the 32-bit range");
  }
  return static_cast<std::uint32_t>(value);
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
  config.tolerance = integer32(args, "--tolerance", index.max_tolerance);
  if (config.tolerance > index.max_tolerance) {
    throw std::invalid_argument("query tolerance exceeds the index build tolerance");
  }
  const auto route = args.get("--route", "multilateration");
  if (route == "multilateration") {
    config.route_mode = navigamer::RouteMode::kMultilateration;
  } else if (route == "scan") {
    config.route_mode = navigamer::RouteMode::kScan;
  } else {
    throw std::invalid_argument("--route must be multilateration or scan");
  }
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
    std::uint64_t min_queries, std::uint64_t repeat, bool retain_hits) {
  if (records.empty()) throw std::invalid_argument("query collection must not be empty");
  if (min_queries == 0 || repeat == 0) {
    throw std::invalid_argument("minimum queries per thread and repeat count must be positive");
  }
  const auto total = records.size() * repeat;
  BatchResult result;
  if (retain_hits) result.hits.resize(total);
  result.latency_ns.resize(total);
  std::vector<std::string_view> sequences;
  sequences.reserve(records.size());
  for (const auto& record : records) sequences.push_back(record.sequence);
  navigamer::QueryEngine engine(index, reference);
  navigamer::PathCache cache;
  const auto start = Clock::now();
  for (std::uint64_t run = 0; run < repeat; ++run) {
    navigamer::QueryStats stats;
    const auto query_start = Clock::now();
    auto hits = engine.query_batch(sequences, config, &cache, &stats,
                                   {threads, min_queries, retain_hits});
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - query_start).count();
    result.stats += stats;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto position = run * records.size() + i;
      result.latency_ns[position] = elapsed;
      if (retain_hits) result.hits[position] = std::move(hits[i]);
    }
  }
  result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
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
      << " latency_scope=collection_completion"
      << " mean_latency_us=" << static_cast<double>(sum / queries / 1000)
      << " p50_us=" << percentile(result.latency_ns, 0.50)
      << " p95_us=" << percentile(result.latency_ns, 0.95)
      << " p99_us=" << percentile(result.latency_ns, 0.99)
      << " peak_rss_bytes=" << peak_rss_bytes() << '\n'
      << "avg_route_edits=" << static_cast<double>(stats.routing_edit_calls()) / queries
      << " avg_exact_verifications=" << static_cast<double>(stats.exact_verifications) / queries
      << " avg_leaf_members=" << static_cast<double>(stats.leaf_members) / queries
      << " avg_cache_candidates=" << static_cast<double>(stats.cache_candidates) / queries << '\n'
      << "route_worlds=";
  for (std::size_t layer = 0; layer < stats.route_worlds.size(); ++layer) {
    if (layer != 0) std::cout << ',';
    std::cout << stats.route_worlds[layer];
  }
  std::cout << '\n';
}

navigamer::BuildConfig build_config(const Arguments& args, std::uint32_t tolerance = 5) {
  navigamer::BuildConfig config;
  config.center_radii = values_list<std::uint32_t>(args.get("--radii", "60,35,15"));
  const auto layer_count = config.center_radii.size();
  if (layer_count == 0 || layer_count > navigamer::kRootLayer) {
    throw std::invalid_argument("layer count must be between 1 and 255");
  }
  if (args.has("--layers")) {
    const auto layers = values_list<std::uint32_t>(args.get("--layers"));
    if (layers.size() != 1 || layers[0] != layer_count) {
      throw std::invalid_argument("--layers must match the number of --radii values (1..255)");
    }
  }
  if (args.has("--beacon-ratios")) {
    config.beacon_ratios = values_list<double>(args.get("--beacon-ratios"));
    if (config.beacon_ratios.size() != layer_count) {
      throw std::invalid_argument("--beacon-ratios requires one value per layer");
    }
  }
  if (args.has("--beacon-cover-radii")) {
    if (args.has("--beacon-ratios")) {
      throw std::invalid_argument("choose --beacon-cover-radii or --beacon-ratios, not both");
    }
    config.beacon_cover_radii = values_list<std::uint32_t>(args.get("--beacon-cover-radii"));
    if (config.beacon_cover_radii->size() != layer_count) {
      throw std::invalid_argument("--beacon-cover-radii requires one value per layer");
    }
  }
  config.max_tolerance = integer32(args, "--T", tolerance);
  config.threads = worker_count(
      args, std::max(1u, std::thread::hardware_concurrency()));
  const auto classifier = args.get("--classifier", "beacon");
  if (classifier == "adjacent") config.classification_mode = navigamer::ClassificationMode::kAdjacent;
  else if (classifier != "beacon") throw std::invalid_argument("--classifier must be beacon or adjacent");
  return config;
}

void print_build_stats(const NavigaMerIndex& index, const navigamer::BuildStats& stats,
                       Clock::time_point start, Clock::time_point built,
                       std::size_t layer_offset = 0) {
  std::cout << std::fixed << std::setprecision(6) << index.summary();
  constexpr std::array<std::string_view, 3> names{"large", "middle", "small"};
  for (std::size_t layer = 0; layer < stats.layers.size(); ++layer) {
    const auto& value = stats.layers[layer];
    const auto beacon_calls = value.beacon_table_edit_calls +
                              value.member_beacon_edit_calls;
    const double pruning_ratio = value.classification_center_pairs == 0 ? 0.0 :
        static_cast<double>(value.pruned_center_pairs) /
        static_cast<double>(value.classification_center_pairs);
    const auto layer_index = layer + layer_offset;
    std::cout << "build_layer=" << (index.layers.size() == 3 ? std::string(names[layer_index]) : std::to_string(layer_index))
        << " layer_index=" << layer_index
        << " center_selection_seconds=" << value.center_selection_seconds
        << " beacon_table_seconds=" << value.beacon_table_seconds
        << " classification_seconds=" << value.classification_seconds
        << " center_edit_calls=" << value.center_edit_calls
        << " center_bound_accepts=" << value.center_bound_accepts
        << " center_hint_hits=" << value.center_hint_hits
        << " center_batch_groups=" << value.center_batch_groups
        << " beacon_selection_edit_calls=" << value.beacon_selection_edit_calls
        << " selected_beacons=" << value.selected_beacons
        << " beacon_distance_calls=" << beacon_calls
        << " beacon_table_distance_calls=" << value.beacon_table_edit_calls
        << " member_beacon_distance_calls=" << value.member_beacon_edit_calls
        << " candidate_distance_calls=" << value.candidate_edit_calls
        << " candidate_batch_groups=" << value.candidate_batch_groups
        << " classification_center_pairs=" << value.classification_center_pairs
        << " pruned_center_pairs=" << value.pruned_center_pairs
        << " pruning_ratio=" << pruning_ratio
        << " classified_members=" << value.classified_members
        << " classification_repeated=" << value.classification_repeated << '\n';
  }
  std::cout
      << "build_seconds=" << std::chrono::duration<double>(built - start).count()
      << " save_seconds=" << std::chrono::duration<double>(Clock::now() - built).count()
      << " peak_rss_bytes=" << peak_rss_bytes() << '\n';
}

void command_build(const Arguments& args) {
  const auto config = build_config(args);
  auto reference = navigamer::SequenceStore::from_fasta(args.get("--reference"),
      integer32(args, "--window", 150), integer32(args, "--stride", 1),
      args.integer("--limit", 0));
  navigamer::BuildStats stats;
  const auto start = Clock::now();
  auto index = navigamer::IndexBuilder::build(reference, config, &stats);
  const auto built = Clock::now();
  index.save(args.get("--index"));
  print_build_stats(index, stats, start, built);
}

void command_continue(const Arguments& args, bool rebuild) {
  if (args.get("--input").empty() || args.get("--index").empty()) {
    throw std::invalid_argument("--input and a new --index output are required");
  }
  if (std::filesystem::exists(args.get("--index"))) {
    throw std::invalid_argument("output index already exists; use a new path to preserve previous results");
  }
  if (!args.has("--radii")) throw std::invalid_argument("new child --radii must be explicit");
  if (args.has("--window") || args.has("--stride") || args.has("--limit")) {
    throw std::invalid_argument("window, stride and reference limit are inherited from --input");
  }
  if (rebuild != args.has("--world")) {
    throw std::invalid_argument("--world is required only for rebuild-world; extend appends to all leaves");
  }
  const auto selected = rebuild ? values_list<navigamer::NodeId>(args.get("--world"))
                                : std::vector<navigamer::NodeId>{0};
  if (selected.size() != 1) throw std::invalid_argument("--world requires one node ID");
  const auto input = NavigaMerIndex::load(args.get("--input"));
  const auto reference = load_reference(args, input);
  const auto config = build_config(args, input.max_tolerance);
  navigamer::BuildStats stats;
  const auto start = Clock::now();
  auto index = rebuild
      ? navigamer::IndexBuilder::rebuild_world(reference, input,
          selected[0], config, &stats)
      : navigamer::IndexBuilder::extend(reference, input, config, &stats);
  const auto built = Clock::now();
  index.save(args.get("--index"));
  print_build_stats(index, stats, start, built, rebuild ? 0 : input.layers.size());
}

void command_inspect(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.get("--index"));
  std::cout << index.summary();
  if (!args.has("--worlds")) return;
  std::vector<navigamer::NodeId> parents(index.nodes.size(), navigamer::kNoNode);
  for (navigamer::NodeId id = 0; id < index.nodes.size(); ++id) {
    const auto& node = index.nodes[id];
    for (std::uint64_t c = 0; c < node.child_count; ++c) {
      parents[index.children[node.first_child + c]] = id;
    }
  }
  std::cout << "node_id\tparent_id\tlayer\tcenter_sequence_id\tmembers\tchildren\n";
  for (navigamer::NodeId id = 0; id < index.nodes.size(); ++id) {
    const auto& node = index.nodes[id];
    std::cout << id << '\t';
    if (parents[id] == navigamer::kNoNode) std::cout << '-';
    else std::cout << parents[id];
    std::cout << '\t';
    if (node.layer == navigamer::kRootLayer) std::cout << "root";
    else std::cout << static_cast<unsigned>(node.layer);
    std::cout << '\t';
    if (node.center_sequence_id == navigamer::kNoSequence) std::cout << '-';
    else std::cout << node.center_sequence_id;
    std::cout << '\t' << node.member_count << '\t' << node.child_count << '\n';
  }
}

#ifdef NAVIGAMER_HAVE_ARCHIVE
volatile std::sig_atomic_t archive_stop = 0;
void stop_archive(int) { archive_stop = 1; }

int command_archive_build(const Arguments& args) {
  if (args.get("--input").empty() || args.get("--output").empty() || !args.has("--radii")) {
    throw std::invalid_argument("extend-archive requires --input, --output and explicit --radii");
  }
  if (args.has("--window") || args.has("--stride") || args.has("--limit") || args.has("--layers") ||
      args.has("--beacon-ratios") || args.has("--beacon-cover-radii")) {
    throw std::invalid_argument("archive reference parameters are inherited; archive routing does not use beacon tables");
  }
  navigamer::ArchiveBuildConfig config;
  config.radii = values_list<std::uint32_t>(args.get("--radii"));
  auto unsigned_option = [&](const std::string& key, std::uint64_t fallback) {
    if (!args.has(key)) return fallback;
    const auto values = values_list<std::uint64_t>(args.get(key));
    if (values.size() != 1) throw std::invalid_argument(key + " requires one unsigned integer");
    return values[0];
  };
  const auto threads = unsigned_option("--threads", std::max(1u, std::thread::hardware_concurrency()));
  if (threads == 0 || threads > 4096) throw std::invalid_argument("--threads must be between 1 and 4096");
  config.threads = threads;
  config.working_memory_mib = unsigned_option("--memory-mib", 1024);
  config.checkpoint_members = unsigned_option("--checkpoint-members", 1000000);
  if (args.has("--T")) {
    const auto tolerances = values_list<std::uint32_t>(args.get("--T"));
    if (tolerances.size() != 1) throw std::invalid_argument("--T requires one unsigned integer");
    config.tolerance = tolerances[0];
  }
  config.resume = args.has("--resume"); config.copy_input = args.has("--copy-input");
  if (args.has("--world")) {
    const auto ids = values_list<navigamer::NodeId>(args.get("--world"));
    if (ids.size() != 1) throw std::invalid_argument("--world requires one archive node/legacy ordinal");
    config.world = ids[0];
  }
  const auto parameters = navigamer::ArchiveBuilder::input_metadata(args.get("--input"));
  const auto reference = navigamer::SequenceStore::from_fasta(args.get("--reference"),
      parameters.window, parameters.stride, parameters.reference_windows);
  std::signal(SIGINT, stop_archive); std::signal(SIGTERM, stop_archive);
  config.stop_requested = [] { return archive_stop != 0; };
  const auto start = Clock::now();
  const bool complete = navigamer::ArchiveBuilder::extend(reference, args.get("--input"), args.get("--output"), config);
  if (complete) std::cout << navigamer::ArchiveIndex::open(args.get("--output")).summary();
  else std::cout << "complete=false checkpoint_saved=true resume_with=--resume\n";
  std::cout << "archive_build_seconds=" << std::chrono::duration<double>(Clock::now() - start).count()
            << " peak_rss_bytes=" << peak_rss_bytes() << '\n';
  return complete ? 0 : 130;
}

void command_archive_inspect(const Arguments& args) {
  const auto archive = navigamer::ArchiveIndex::open(args.get("--input"));
  std::cout << archive.summary();
  if (!args.has("--worlds")) return;
  std::cout << "node_id\tparent_id\tdepth\tcenter_sequence_id\tmembers\tchildren\n";
  for (navigamer::NodeId id = 0; id < archive.metadata().nodes; ++id) {
    const auto node = archive.node(id);
    std::cout << id << '\t';
    if (node.parent == navigamer::kNoNode) std::cout << '-'; else std::cout << node.parent;
    std::cout << '\t' << node.depth << '\t';
    if (node.center == navigamer::kNoSequence) std::cout << '-'; else std::cout << node.center;
    std::cout << '\t' << node.members.count << '\t' << node.child_count << '\n';
  }
}
#endif

std::uint64_t min_queries_per_thread(const Arguments& args) {
  if (args.has("--min-queries-per-thread") && args.has("--block-size")) {
    throw std::invalid_argument("use --min-queries-per-thread or its --block-size alias, not both");
  }
  return args.integer("--min-queries-per-thread", args.integer("--block-size", 1));
}

void command_query(const Arguments& args) {
  const auto index = NavigaMerIndex::load(args.get("--index"));
  const auto reference = load_reference(args, index);
  const auto records = navigamer::read_queries(args.get("--queries"), args.integer("--limit", 0));
  const auto threads = worker_count(args, 1);
  const auto result = run_batch(index, reference, records, query_config(args, index), threads,
      min_queries_per_thread(args), 1, !args.has("--no-output"));
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
  const auto threads = worker_count(
      args, std::max(1u, std::thread::hardware_concurrency()));
  const auto min_queries = min_queries_per_thread(args);
  const auto repeat = args.integer("--repeat", 1);
  const auto warmup = args.integer("--warmup", 0);
  if (warmup != 0) {
    const auto count = std::min<std::uint64_t>(warmup, records.size());
    const std::vector<navigamer::QueryRecord> warm(records.begin(), records.begin() + count);
    run_batch(index, reference, warm, config, threads, min_queries, 1, false);
  }
  const auto result = run_batch(index, reference, records, config, threads, min_queries, repeat, false);
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
      " [--T 5] [--layers N] [--radii R1,...,RN]"
      " [--beacon-ratios B1,...,BN | --beacon-cover-radii C1,...,CN] [--threads N]"
      " [--classifier beacon|adjacent]\n"
      "    Defaults: 3 layers, radii 60,35,15, beacon ratio 0.1 per layer.\n"
      "  extend --reference ref.fa --input old.nvm --index new.nvm --radii R1,...,RN"
      " [--threads N] [--classifier beacon|adjacent]"
      " [--beacon-ratios B1,...,BN | --beacon-cover-radii C1,...,CN]\n"
      "    Preserve the hierarchy and append below all current leaves.\n"
      "  rebuild-world --reference ref.fa --input old.nvm --world NODE --index new.nvm"
      " --radii R1,...,RN [--threads N] [--classifier beacon|adjacent]\n"
      "    Keep this world's center/members; replace its descendants in a new index.\n"
      "  extend-archive --reference ref.fa --input R75_DIRECTORY --output NEW_DIRECTORY"
      " --radii R1,...,RN [--world ID] [--T NEW_T] [--threads N] [--memory-mib 1024]"
      " [--checkpoint-members 1000000] [--resume] [--copy-input]\n"
      "    --T NEW_T on a full legacy input reuses first-layer centers and rebuilds memberships/descendants.\n"
      "    Legacy R75/R90 world IDs are center ordinals; new archive world IDs are node IDs.\n"
      "  inspect-archive --input ARCHIVE_DIRECTORY [--worlds]\n"
      "  query --reference ref.fa --index out.nvm --queries reads.fa"
      " [--tolerance T] [--route multilateration|scan] [--threads N]"
      " [--min-queries-per-thread N] [--output hits.tsv]\n"
      "  inspect --index out.nvm [--worlds]\n"
      "  verify --reference ref.fa --index out.nvm --queries reads.fa [--limit 100]\n"
      "  benchmark --reference ref.fa --index out.nvm --queries reads.fa"
      " [--repeat N] [--warmup N] [--threads N] [--min-queries-per-thread N]\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string command = argc == 1 ? "help" : argv[1];
    const Arguments args(argc, argv);
    if (command == "build") command_build(args);
    else if (command == "extend") command_continue(args, false);
    else if (command == "rebuild-world") command_continue(args, true);
#ifdef NAVIGAMER_HAVE_ARCHIVE
    else if (command == "extend-archive") return command_archive_build(args);
    else if (command == "inspect-archive") command_archive_inspect(args);
#else
    else if (command == "extend-archive" || command == "inspect-archive") {
      throw std::runtime_error("archive support requires a build with Zstd headers and library");
    }
#endif
    else if (command == "query") command_query(args);
    else if (command == "inspect") command_inspect(args);
    else if (command == "verify") return command_verify(args);
    else if (command == "benchmark") command_benchmark(args);
    else usage();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
