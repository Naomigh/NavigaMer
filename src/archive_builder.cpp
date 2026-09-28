#include "navigamer/archive.hpp"
#include "navigamer/center_selector.hpp"
#include "navigamer/member_classifier.hpp"
#include "archive_storage.hpp"
#include "build_parallel.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace navigamer {
namespace {
using namespace archive_detail;

enum class Phase : std::uint64_t { kImport, kSelect, kClassify, kComplete };
struct Job {
  ArchiveMetadata metadata;
  std::uint64_t binding{0}, import_next{0}, import_factor{1}, parent{0};
  std::uint64_t selected_members{0}, classified_members{0}, child_first{0}, page_capacity{128};
  Phase phase{Phase::kImport};
  ArchiveNode parent_before;
  std::vector<SequenceId> centers;
  std::vector<MemberCursor> cursors;
};

class OutputLock {
 public:
  explicit OutputLock(const std::filesystem::path& directory) {
    fd_ = ::open((directory / ".archive.lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) throw std::runtime_error("cannot lock archive output");
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd_); fd_ = -1;
      throw std::runtime_error("another builder owns the archive output");
    }
  }
  ~OutputLock() { if (fd_ >= 0) ::close(fd_); }
 private:
  int fd_{-1};
};

std::uint64_t file_hash(const std::filesystem::path& path, std::uint64_t bytes) {
  std::ifstream in(path, std::ios::binary);
  std::array<char, 65536> buffer;
  std::uint64_t result = 14695981039346656037ULL;
  while (bytes) {
    const auto count = std::min<std::uint64_t>(bytes, buffer.size());
    in.read(buffer.data(), count);
    if (!in) throw std::runtime_error("truncated archive checkpoint");
    for (std::size_t i = 0; i < count; ++i) result = (result ^ static_cast<unsigned char>(buffer[i])) * 1099511628211ULL;
    bytes -= count;
  }
  return result;
}
void write_job(std::ostream& out, const Job& job) {
  write(out, std::uint64_t{0x31424f4a4d564eULL}); write(out, std::uint64_t{1});
  write_metadata(out, job.metadata);
  for (const auto value : {job.binding, std::uint64_t(job.phase), job.import_next, job.import_factor,
      job.parent, job.selected_members, job.classified_members, job.child_first, job.page_capacity}) write(out, value);
  write_node(out, job.parent_before);
  write(out, std::uint64_t(job.centers.size()));
  for (const auto center : job.centers) write(out, center);
  write(out, std::uint64_t(job.cursors.size()));
  for (const auto& cursor : job.cursors) {
    write_members(out, cursor.members); write(out, cursor.last);
    write(out, std::uint64_t(cursor.pending.size()));
    for (const auto& entry : cursor.pending) {
      for (const auto value : {entry.offset, entry.bytes, entry.count, entry.first, entry.last}) write(out, value);
    }
  }
}
Job read_job(const std::filesystem::path& output, std::uint64_t budget) {
  const auto path = output / "job.bin";
  const auto size = std::filesystem::file_size(path);
  if (size < 8 || size > budget / 2) throw std::runtime_error("archive checkpoint exceeds working-memory budget");
  std::ifstream in(path, std::ios::binary);
  in.seekg(size - 8); const auto checksum = read<std::uint64_t>(in);
  if (file_hash(path, size - 8) != checksum) throw std::runtime_error("archive checkpoint checksum mismatch");
  in.seekg(0);
  if (read<std::uint64_t>(in) != 0x31424f4a4d564eULL || read<std::uint64_t>(in) != 1) throw std::runtime_error("invalid archive checkpoint format");
  Job job; job.metadata = read_metadata(in);
  job.binding = read<std::uint64_t>(in); const auto phase = read<std::uint64_t>(in);
  if (phase > 3) throw std::runtime_error("invalid archive checkpoint phase");
  job.phase = Phase(phase);
  job.import_next = read<std::uint64_t>(in); job.import_factor = read<std::uint64_t>(in);
  job.parent = read<std::uint64_t>(in); job.selected_members = read<std::uint64_t>(in);
  job.classified_members = read<std::uint64_t>(in); job.child_first = read<std::uint64_t>(in);
  job.page_capacity = read<std::uint64_t>(in); job.parent_before = read_node(in);
  const auto centers = read<std::uint64_t>(in);
  if (centers > budget / 32) throw std::runtime_error("too many checkpoint centers for memory budget");
  for (std::uint64_t i = 0; i < centers; ++i) job.centers.push_back(read<std::uint64_t>(in));
  const auto cursors = read<std::uint64_t>(in);
  if (cursors > budget / (4 * sizeof(MemberCursor))) throw std::runtime_error("too many checkpoint cursors");
  std::uint64_t pending_bytes = 0;
  for (std::uint64_t i = 0; i < cursors; ++i) {
    MemberCursor cursor; cursor.members = read_members(in); cursor.last = read<std::uint64_t>(in);
    const auto count = read<std::uint64_t>(in);
    if (count > 128 || (pending_bytes += count * sizeof(BlockEntry)) > budget / 2) throw std::runtime_error("invalid checkpoint index buffers");
    for (std::uint64_t j = 0; j < count; ++j) {
      cursor.pending.push_back({read<std::uint64_t>(in), read<std::uint64_t>(in), read<std::uint64_t>(in), read<std::uint64_t>(in), read<std::uint64_t>(in)});
    }
    job.cursors.push_back(std::move(cursor));
  }
  if (std::uint64_t(in.tellg()) != size - 8 || job.page_capacity == 0 || job.page_capacity > 128 ||
      job.import_factor == 0 || job.import_factor > 16 || job.parent > job.metadata.nodes ||
      job.metadata.sources.back().name != "members.nma") throw std::runtime_error("invalid archive checkpoint state");
  return job;
}

void checkpoint(const std::filesystem::path& output, Job& job, MemberWriter& writer, NodeTable& nodes) {
  writer.flush(); nodes.flush();
  sync_file(output / "members.nma"); sync_file(output / "nodes.bin");
  job.metadata.sources.back().bytes = writer.position();
  const auto temporary = output / "job.tmp";
  { std::ofstream out(temporary, std::ios::binary); write_job(out, job); out.close();
    if (!out) throw std::runtime_error("archive checkpoint write failed"); }
  const auto checksum = file_hash(temporary, std::filesystem::file_size(temporary));
  { std::ofstream out(temporary, std::ios::binary | std::ios::app); write(out, checksum); }
  publish(temporary, output / "job.bin");
  save_metadata(output, job.metadata);
  const auto progress = output / "progress.tmp";
  const char* phase = job.phase == Phase::kImport ? "import" : job.phase == Phase::kSelect ? "select"
      : job.phase == Phase::kClassify ? "classify" : "complete";
  { std::ofstream out(progress);
    out << "{\"phase\":\"" << phase << "\",\"imported_windows\":" << job.import_next
        << ",\"parent\":" << job.parent << ",\"nodes\":" << job.metadata.nodes
        << ",\"selected_members\":" << job.selected_members << ",\"classified_members\":" << job.classified_members
        << ",\"centers\":" << job.centers.size() << ",\"complete\":" << (job.metadata.complete ? "true" : "false") << "}\n";
    out.close(); if (!out) throw std::runtime_error("archive progress write failed"); }
  publish(progress, output / "progress.json");
}

std::uint64_t input_binding(const std::filesystem::path& input, const ArchiveBuildConfig& config,
                           const LegacyCenters* legacy, const ArchiveIndex* archive) {
  std::ostringstream bytes(std::ios::binary);
  write_string(bytes, input.string());
  write(bytes, config.world.value_or(kNoNode));
  write(bytes, std::uint64_t(config.radii.size()));
  for (const auto radius : config.radii) write(bytes, radius);
  if (legacy) {
    for (const auto value : {legacy->fingerprint, legacy->windows, legacy->center_hash}) write(bytes, value);
  } else write_metadata(bytes, archive->metadata());
  const auto value = bytes.str();
  return hash({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

std::uint64_t import_factor(const LegacyCenters& legacy, const ArchiveBuildConfig& config, std::uint64_t budget) {
  const auto count = config.world ? 1 : legacy.ids.size();
  const auto fixed = legacy.ids.size() * (2 * 8192 + 8) + count * (sizeof(MemberCursor) + 128 * 40) + 8 * 1024 * 1024;
  if (fixed + count * 8192 > budget) throw std::runtime_error("working-memory budget is too small to decode and repack one R75 chunk");
  std::uint64_t factor = 1;
  while (factor < 16 && fixed + count * 8192 * factor * 2 <= budget) factor *= 2;
  return factor;
}

void inherit_sources(const ArchiveIndex& input, const std::filesystem::path& output,
                     ArchiveMetadata& metadata, bool allow_copy) {
  for (std::size_t i = 0; i < input.metadata().sources.size(); ++i) {
    auto source = input.metadata().sources[i];
    const auto original = input.directory() / source.name;
    if (std::filesystem::is_symlink(original)) throw std::runtime_error("archive data sources must not be symbolic links");
    source.name = "source-" + std::to_string(i) + ".nma";
    const auto target = output / source.name;
    std::error_code error;
    std::filesystem::create_hard_link(original, target, error);
    if (error) {
      if (!allow_copy) throw std::runtime_error("cannot hard-link immutable input data; use --copy-input for a cross-filesystem copy");
      if (std::filesystem::space(output).available < source.bytes + 1024 * 1024) throw std::runtime_error("insufficient disk space to copy input archive");
      std::filesystem::copy_file(original, target);
    }
    metadata.sources.push_back(std::move(source));
  }
}

bool stopped(const ArchiveBuildConfig& config) { return config.stop_requested && config.stop_requested(); }

bool repack(const SequenceStore& reference, const std::filesystem::path& input,
            const std::filesystem::path& output, const LegacyCenters& legacy,
            const ArchiveBuildConfig& config, std::uint64_t budget,
            Job& job, MemberWriter& writer, NodeTable& nodes) {
  (void)reference;
  const auto count = job.cursors.size();
  std::uint64_t since_checkpoint = 0;
  while (job.import_next < legacy.windows) {
    const auto first = job.import_next, windows = std::min(65536 * job.import_factor, legacy.windows - first);
    const auto words = (windows + 63) / 64;
    const auto buffer_bytes = count * words * 8;
    const auto index_bytes = count * (sizeof(MemberCursor) + 128 * sizeof(BlockEntry));
    if (buffer_bytes + index_bytes + 8 * 1024 * 1024 >= budget) throw std::runtime_error("resume requires more repacking buffer memory");
    std::vector<std::uint64_t> buffer(count * words, 0);
    for (std::uint64_t offset = 0; offset < windows; offset += 65536) {
      const auto chunk = read_legacy_chunk(input, legacy, first + offset, budget - buffer_bytes - index_bytes - 8 * 1024 * 1024);
      const auto chunk_words = (chunk.count + 63) / 64;
      for (std::size_t c = 0; c < count; ++c) {
        const auto original = config.world ? *config.world : c;
        std::copy_n(chunk.bits.begin() + original * chunk_words, chunk_words, buffer.begin() + c * words + offset / 64);
      }
    }
    for (std::size_t c = 0; c < count; ++c) writer.bitmap(job.cursors[c], first, windows, {buffer.data() + c * words, words});
    job.import_next += windows; since_checkpoint += windows;
    const bool stop = stopped(config);
    if (since_checkpoint >= config.checkpoint_members || stop) {
      checkpoint(output, job, writer, nodes); since_checkpoint = 0;
    }
    if (stop) return false;
  }
  for (auto& cursor : job.cursors) {
    writer.finish(cursor);
    auto node = nodes.get(cursor.members.owner);
    node.members = cursor.members;
    if (node.members.count == 0) throw std::runtime_error("R75 world has no members");
    nodes.put(cursor.members.owner, node);
  }
  job.cursors.clear(); job.phase = Phase::kSelect;
  checkpoint(output, job, writer, nodes);
  return true;
}

bool select_parent(const SequenceStore& reference, const std::filesystem::path& output,
                   const ArchiveBuildConfig& config, std::uint64_t budget,
                   Job& job, MemberWriter& writer, NodeTable& nodes) {
  const auto archive = ArchiveIndex::open(output, false);
  const auto parent = archive.node(job.parent);
  job.parent_before = parent;
  ArchiveMemberReader reader(archive, job.parent);
  reader.skip(job.selected_members);
  const auto threads = parent.members.count < 4096 ? 1u : static_cast<std::uint32_t>(
      std::min<std::uint64_t>({config.threads, 1024, budget / (4 * 65536)}));
  CenterSelector selector(reference, job.metadata.radii.at(parent.depth), threads, job.centers, budget / 2);
  std::uint64_t since_checkpoint = 0;
  for (;;) {
    const auto members = reader.next(65536);
    if (members.empty()) break;
    selector.consume(members);
    job.selected_members += members.size(); since_checkpoint += members.size();
    const bool stop = stopped(config);
    if (since_checkpoint >= config.checkpoint_members || stop) {
      job.centers = selector.centers(); checkpoint(output, job, writer, nodes); since_checkpoint = 0;
    }
    if (stop) return false;
  }
  if (job.selected_members != parent.members.count) throw std::runtime_error("selection member count mismatch");
  job.centers = selector.centers();
  if (job.centers.empty()) throw std::runtime_error("cannot select centers for an empty parent");
  job.child_first = job.metadata.nodes;
  job.cursors.clear();
  for (const auto center : job.centers) {
    const auto id = job.metadata.nodes++;
    ArchiveNode child; child.center = center; child.parent = job.parent; child.depth = parent.depth + 1;
    child.members.source = job.metadata.sources.size() - 1; child.members.owner = id;
    nodes.put(id, child);
    job.cursors.push_back({child.members, kNoSequence, {}});
  }
  job.phase = Phase::kClassify;
  job.page_capacity = std::min<std::uint64_t>(128, std::max<std::uint64_t>(1, budget / (16 * job.centers.size() * 40)));
  writer.page_capacity(job.page_capacity);
  checkpoint(output, job, writer, nodes);
  return true;
}

bool classify_parent(const SequenceStore& reference, const std::filesystem::path& output,
                     const ArchiveBuildConfig& config, std::uint64_t budget,
                     Job& job, MemberWriter& writer, NodeTable& nodes) {
  const auto archive = ArchiveIndex::open(output, false);
  const auto parent = archive.node(job.parent);
  ArchiveMemberReader reader(archive, job.parent); reader.skip(job.classified_members);
  BatchDistance profiles(reference.window_length());
  for (const auto center : job.centers) {
    profiles.append(reference.sequence(center));
    if (profiles.bytes() > budget / 2) throw std::runtime_error("center profiles exceed working-memory budget");
  }
  const auto centers = job.centers.size();
  const auto fixed = profiles.bytes() + centers * (8 + sizeof(MemberCursor) + 2 * job.page_capacity * sizeof(BlockEntry)) + 8 * 1024 * 1024;
  if (fixed >= budget) throw std::runtime_error("classification index buffers exceed working-memory budget");
  const auto per_worker = centers * 14 + 65536;
  const auto worker_count = std::min<std::uint64_t>({config.threads, 1024, (budget - fixed) / (2 * per_worker)});
  if (worker_count == 0) throw std::runtime_error("working-memory budget cannot fit one classifier");
  const auto max_words = std::min<std::uint64_t>(1024, (budget - fixed - worker_count * per_worker) / (centers * 8));
  if (max_words == 0) throw std::runtime_error("working-memory budget cannot fit a classification bitmap");
  std::vector<std::unique_ptr<AdjacentClassifier>> classifiers;
  for (std::size_t i = 0; i < worker_count; ++i) classifiers.push_back(std::make_unique<AdjacentClassifier>(reference, job.centers, profiles, job.metadata.tolerance));
  detail::WorkerPool pool(worker_count);
  std::uint64_t since_checkpoint = 0;
  std::vector<std::uint64_t> bits;
  for (;;) {
    const auto members = reader.next(max_words * 64);
    if (members.empty()) break;
    const auto words = (members.size() + 63) / 64;
    bits.assign(centers * words, 0);
    pool.run([&](std::size_t worker, std::size_t workers) {
      const auto begin = words * worker / workers * 64;
      const auto end = std::min(members.size(), words * (worker + 1) / workers * 64);
      for (auto q = begin; q < end; ++q) {
        const auto membership = classifiers[worker]->classify(members[q]);
        for (std::size_t c = 0; c < centers; ++c) if (membership[c]) bits[c * words + q / 64] |= std::uint64_t{1} << (q % 64);
      }
    });
    for (std::size_t c = 0; c < centers; ++c) writer.selected(job.cursors[c], members, {bits.data() + c * words, words});
    job.classified_members += members.size(); since_checkpoint += members.size();
    const bool stop = stopped(config);
    if (since_checkpoint >= config.checkpoint_members || stop) {
      checkpoint(output, job, writer, nodes); since_checkpoint = 0;
    }
    if (stop) return false;
  }
  if (job.classified_members != parent.members.count) throw std::runtime_error("classification member count mismatch");
  for (auto& cursor : job.cursors) {
    writer.finish(cursor);
    if (cursor.members.count == 0) throw std::runtime_error("classified child is empty");
    auto child = nodes.get(cursor.members.owner); child.members = cursor.members;
    nodes.put(cursor.members.owner, child);
  }
  auto completed = job.parent_before; completed.first_child = job.child_first; completed.child_count = centers;
  nodes.put(job.parent, completed);
  ++job.parent; job.centers.clear(); job.cursors.clear();
  job.selected_members = job.classified_members = 0; job.phase = Phase::kSelect;
  checkpoint(output, job, writer, nodes);
  return true;
}

}  // namespace

ArchiveMetadata ArchiveBuilder::input_metadata(const std::filesystem::path& input) {
  if (std::filesystem::exists(input / "archive.bin")) return ArchiveIndex::open(input).metadata();
  const LegacyCenters legacy(input);
  ArchiveMetadata metadata;
  metadata.window = 150; metadata.stride = 1; metadata.tolerance = 5;
  metadata.reference_windows = legacy.windows; metadata.reference_fingerprint = legacy.fingerprint;
  metadata.nodes = legacy.ids.size() + 1; metadata.radii = {75};
  return metadata;
}

bool ArchiveBuilder::extend(const SequenceStore& reference, const std::filesystem::path& input_path,
    const std::filesystem::path& output_path, const ArchiveBuildConfig& config) {
  if (config.radii.empty() || config.radii.size() > 255 || config.threads == 0 || config.threads > 4096 ||
      config.working_memory_mib < 16 || config.working_memory_mib > UINT64_MAX / (1024 * 1024) || config.checkpoint_members == 0) {
    throw std::invalid_argument("invalid archive radii, threads, memory budget or checkpoint interval");
  }
  const auto budget = config.working_memory_mib * 1024 * 1024;
  const auto input = std::filesystem::canonical(input_path), output = std::filesystem::weakly_canonical(std::filesystem::absolute(output_path));
  if (input == output || output.string().starts_with(input.string() + "/")) throw std::invalid_argument("archive output must be separate from its read-only input");
  std::unique_ptr<LegacyCenters> legacy;
  std::optional<ArchiveIndex> prior;
  if (std::filesystem::exists(input / "archive.bin")) {
    prior = ArchiveIndex::open(input); prior->check_reference(reference);
    if (config.world) (void)prior->node(*config.world);
  } else {
    legacy = std::make_unique<LegacyCenters>(input, budget / 4);
    if (reference.window_length() != 150 || reference.stride() != 1 || reference.size() != legacy->windows || reference.fingerprint() != legacy->fingerprint) {
      throw std::invalid_argument("reference does not match R75 archive");
    }
    if (config.world && *config.world >= legacy->ids.size()) throw std::out_of_range("R75 world ordinal out of range");
    (void)import_factor(*legacy, config, budget);
  }
  const auto prefix_depth = config.world ? 0 : legacy ? 1 : prior->metadata().radii.size();
  if (prefix_depth + config.radii.size() > 255) throw std::invalid_argument("archive total depth exceeds 255");
  const auto binding = input_binding(input, config, legacy.get(), prior ? &*prior : nullptr);
  if (!config.resume) {
    if (std::filesystem::exists(output)) throw std::invalid_argument("archive output already exists; use a new directory or --resume");
    std::filesystem::create_directories(output.parent_path());
    if (!std::filesystem::create_directory(output)) throw std::runtime_error("cannot reserve archive output directory");
  } else if (!std::filesystem::is_directory(output)) throw std::runtime_error("no archive output to resume");
  OutputLock lock(output);
  Job job;
  if (config.resume) {
    job = read_job(output, budget);
    if (job.binding != binding || job.metadata.reference_fingerprint != reference.fingerprint()) throw std::invalid_argument("archive resume input/scope/radii do not match checkpoint");
    if (job.phase == Phase::kComplete) { save_metadata(output, job.metadata); (void)ArchiveIndex::open(output); return true; }
    // Validate ownership before trimming only uncommitted tails of this job's
    // own files. Inherited, immutable source stores are never opened for writing.
    for (const auto name : {"members.nma", "nodes.bin"}) {
      const auto path = output / name;
      if (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) ||
          std::filesystem::hard_link_count(path) != 1) {
        throw std::runtime_error("mutable archive output must be an exclusively owned regular file");
      }
    }
    { MemberWriter verify(output / "members.nma", job.metadata, job.page_capacity, false); }
    { NodeTable verify(output / "nodes.bin", reference.fingerprint(), false); }
    const auto data_bytes = job.metadata.sources.back().bytes;
    if (std::filesystem::file_size(output / "members.nma") < data_bytes ||
        std::filesystem::file_size(output / "nodes.bin") < kNodeHeaderBytes + job.metadata.nodes * kNodeBytes) {
      throw std::runtime_error("archive output is shorter than its checkpoint");
    }
    std::filesystem::resize_file(output / "members.nma", data_bytes);
    std::filesystem::resize_file(output / "nodes.bin", kNodeHeaderBytes + job.metadata.nodes * kNodeBytes);
    NodeTable nodes(output / "nodes.bin", reference.fingerprint(), false, true);
    if (job.phase == Phase::kClassify) nodes.put(job.parent, job.parent_before);
    nodes.flush(); save_metadata(output, job.metadata);
  } else {
    job.binding = binding;
    job.metadata.nonce = std::chrono::steady_clock::now().time_since_epoch().count() ^ std::uint64_t(::getpid());
    job.metadata.window = reference.window_length(); job.metadata.stride = reference.stride();
    job.metadata.reference_windows = reference.size(); job.metadata.reference_fingerprint = reference.fingerprint();
    NodeTable nodes(output / "nodes.bin", reference.fingerprint(), true);
    ArchiveNode root;
    if (legacy) {
      job.metadata.tolerance = 5; job.import_factor = import_factor(*legacy, config, budget);
      if (config.world) {
        root.center = legacy->ids[*config.world]; root.members.source = 0;
        job.metadata.root_radius = 75; job.metadata.nodes = 1;
        job.cursors.push_back({root.members, kNoSequence, {}});
      } else {
        root.members.count = reference.size(); root.child_count = legacy->ids.size(); root.first_child = 1;
        job.metadata.radii = {75}; job.metadata.nodes = legacy->ids.size() + 1;
        for (std::size_t c = 0; c < legacy->ids.size(); ++c) {
          ArchiveNode node; node.center = legacy->ids[c]; node.parent = 0; node.depth = 1;
          node.members.source = 0; node.members.owner = c + 1;
          nodes.put(c + 1, node); job.cursors.push_back({node.members, kNoSequence, {}});
        }
      }
      nodes.put(0, root);
    } else {
      inherit_sources(*prior, output, job.metadata, config.copy_input);
      job.metadata.tolerance = prior->metadata().tolerance; job.phase = Phase::kSelect;
      if (config.world) {
        root = prior->node(*config.world); root.parent = kNoNode; root.first_child = root.child_count = root.depth = 0;
        job.metadata.root_radius = *config.world == 0 ? prior->metadata().root_radius
            : prior->metadata().radii.at(prior->node(*config.world).depth - 1);
        job.metadata.nodes = 1; nodes.put(0, root);
      } else {
        job.metadata.radii = prior->metadata().radii; job.metadata.root_radius = prior->metadata().root_radius;
        job.metadata.nodes = prior->metadata().nodes;
        NodeTable previous(input / "nodes.bin", reference.fingerprint(), false);
        for (NodeId id = 0; id < job.metadata.nodes; ++id) nodes.put(id, previous.get(id));
      }
    }
    job.metadata.radii.insert(job.metadata.radii.end(), config.radii.begin(), config.radii.end());
    job.metadata.sources.push_back({"members.nma", job.metadata.nonce, 64});
    MemberWriter writer(output / "members.nma", job.metadata, job.page_capacity, true);
    checkpoint(output, job, writer, nodes);
  }

  NodeTable nodes(output / "nodes.bin", reference.fingerprint(), false, true);
  MemberWriter writer(output / "members.nma", job.metadata, job.page_capacity, false);
  if (job.phase == Phase::kImport && !repack(reference, input, output, *legacy, config, budget, job, writer, nodes)) return false;
  while (job.parent < job.metadata.nodes) {
    if (job.phase == Phase::kSelect) {
      const auto parent = nodes.get(job.parent);
      if (parent.child_count || parent.depth >= job.metadata.radii.size()) { ++job.parent; continue; }
      if (!select_parent(reference, output, config, budget, job, writer, nodes)) return false;
    }
    if (job.phase == Phase::kClassify && !classify_parent(reference, output, config, budget, job, writer, nodes)) return false;
  }
  job.metadata.complete = true; job.phase = Phase::kComplete;
  checkpoint(output, job, writer, nodes);
  return true;
}

}  // namespace navigamer
