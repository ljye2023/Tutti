#include "metadata_journal.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>

namespace tutti {
namespace block_storage {

MetadataJournal::MetadataJournal()
    : initialized_(false) {
}

MetadataJournal::~MetadataJournal() {
    close();
}

bool MetadataJournal::initialize(const std::string& root_directory) {
    root_directory_ = root_directory;
    journal_path_ = root_directory + "/.journal";
    checkpoint_path_ = root_directory + "/.checkpoint";

    // Create root directory if it doesn't exist
    try {
        std::filesystem::create_directories(root_directory);
    } catch (...) {
        return false;
    }

    // Open journal file for append
    journal_file_.open(journal_path_, std::ios::binary | std::ios::app);
    if (!journal_file_.is_open()) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool MetadataJournal::log_create(const GpuFile& file) {
    if (!initialized_) {
        return false;
    }

    JournalEntry entry;
    entry.op_type = JournalOpType::CREATE;
    entry.file_id = file.file_id;
    entry.name = file.name;
    entry.logical_size = file.logical_size;
    entry.stripe_size = file.stripe_size;
    entry.shards = file.shards;
    entry.timestamp = get_timestamp();

    return write_entry(entry);
}

bool MetadataJournal::log_delete(FileId file_id, const std::string& name) {
    if (!initialized_) {
        return false;
    }

    JournalEntry entry;
    entry.op_type = JournalOpType::DELETE;
    entry.file_id = file_id;
    entry.name = name;
    entry.timestamp = get_timestamp();

    return write_entry(entry);
}

bool MetadataJournal::log_resize(FileId file_id, uint64_t new_size) {
    if (!initialized_) {
        return false;
    }

    JournalEntry entry;
    entry.op_type = JournalOpType::RESIZE;
    entry.file_id = file_id;
    entry.logical_size = new_size;
    entry.timestamp = get_timestamp();

    return write_entry(entry);
}

bool MetadataJournal::checkpoint(const std::vector<GpuFile>& all_files) {
    if (!initialized_) {
        return false;
    }

    // Write checkpoint to temporary file
    std::string temp_path = checkpoint_path_ + ".tmp";
    std::ofstream checkpoint_file(temp_path, std::ios::binary);
    if (!checkpoint_file.is_open()) {
        return false;
    }

    // Write checkpoint header
    uint64_t file_count = all_files.size();
    checkpoint_file.write(reinterpret_cast<const char*>(&file_count), sizeof(file_count));

    // Write each file
    for (const auto& file : all_files) {
        // Write file_id
        checkpoint_file.write(reinterpret_cast<const char*>(&file.file_id), sizeof(file.file_id));

        // Write name length and name
        uint32_t name_len = static_cast<uint32_t>(file.name.size());
        checkpoint_file.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
        checkpoint_file.write(file.name.c_str(), name_len);

        // Write logical_size and stripe_size
        checkpoint_file.write(reinterpret_cast<const char*>(&file.logical_size), sizeof(file.logical_size));
        checkpoint_file.write(reinterpret_cast<const char*>(&file.stripe_size), sizeof(file.stripe_size));

        // Write shard count and shards
        uint32_t shard_count = static_cast<uint32_t>(file.shards.size());
        checkpoint_file.write(reinterpret_cast<const char*>(&shard_count), sizeof(shard_count));
        for (const auto& shard : file.shards) {
            checkpoint_file.write(reinterpret_cast<const char*>(&shard.device_id), sizeof(shard.device_id));
            checkpoint_file.write(reinterpret_cast<const char*>(&shard.namespace_id), sizeof(shard.namespace_id));
            checkpoint_file.write(reinterpret_cast<const char*>(&shard.start_lba), sizeof(shard.start_lba));
            checkpoint_file.write(reinterpret_cast<const char*>(&shard.length_blocks), sizeof(shard.length_blocks));
        }

        // Write creation_time as timestamp
        auto time_since_epoch = file.creation_time.time_since_epoch();
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch).count();
        checkpoint_file.write(reinterpret_cast<const char*>(&timestamp), sizeof(timestamp));
    }

    checkpoint_file.close();

    // Atomically replace checkpoint file
    try {
        std::filesystem::rename(temp_path, checkpoint_path_);
    } catch (...) {
        return false;
    }

    // Truncate journal
    journal_file_.close();
    journal_file_.open(journal_path_, std::ios::binary | std::ios::trunc);
    if (!journal_file_.is_open()) {
        return false;
    }

    return true;
}

std::vector<JournalEntry> MetadataJournal::recover() {
    std::vector<JournalEntry> entries;

    if (!initialized_) {
        return entries;
    }

    // First, try to load checkpoint state
    std::vector<GpuFile> checkpoint_files;
    bool checkpoint_exists = read_checkpoint(checkpoint_files);

    // Convert checkpoint files to CREATE journal entries
    if (checkpoint_exists) {
        for (const auto& file : checkpoint_files) {
            JournalEntry entry;
            entry.op_type = JournalOpType::CREATE;
            entry.file_id = file.file_id;
            entry.name = file.name;
            entry.logical_size = file.logical_size;
            entry.stripe_size = file.stripe_size;
            entry.shards = file.shards;

            // Use creation_time as timestamp
            auto time_since_epoch = file.creation_time.time_since_epoch();
            entry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(time_since_epoch).count();

            entries.push_back(entry);
        }
    }

    // Then replay journal entries on top of checkpoint state
    std::vector<JournalEntry> journal_entries;
    read_journal(journal_entries);

    // Append journal entries after checkpoint entries
    entries.insert(entries.end(), journal_entries.begin(), journal_entries.end());

    return entries;
}

void MetadataJournal::close() {
    if (journal_file_.is_open()) {
        journal_file_.close();
    }
    initialized_ = false;
}

bool MetadataJournal::write_entry(const JournalEntry& entry) {
    if (!journal_file_.is_open()) {
        return false;
    }

    // Write operation type
    uint8_t op_type = static_cast<uint8_t>(entry.op_type);
    journal_file_.write(reinterpret_cast<const char*>(&op_type), sizeof(op_type));

    // Write file_id
    journal_file_.write(reinterpret_cast<const char*>(&entry.file_id), sizeof(entry.file_id));

    // Write name length and name
    uint32_t name_len = static_cast<uint32_t>(entry.name.size());
    journal_file_.write(reinterpret_cast<const char*>(&name_len), sizeof(name_len));
    if (name_len > 0) {
        journal_file_.write(entry.name.c_str(), name_len);
    }

    // Write logical_size and stripe_size
    journal_file_.write(reinterpret_cast<const char*>(&entry.logical_size), sizeof(entry.logical_size));
    journal_file_.write(reinterpret_cast<const char*>(&entry.stripe_size), sizeof(entry.stripe_size));

    // Write shard count and shards
    uint32_t shard_count = static_cast<uint32_t>(entry.shards.size());
    journal_file_.write(reinterpret_cast<const char*>(&shard_count), sizeof(shard_count));
    for (const auto& shard : entry.shards) {
        journal_file_.write(reinterpret_cast<const char*>(&shard.device_id), sizeof(shard.device_id));
        journal_file_.write(reinterpret_cast<const char*>(&shard.namespace_id), sizeof(shard.namespace_id));
        journal_file_.write(reinterpret_cast<const char*>(&shard.start_lba), sizeof(shard.start_lba));
        journal_file_.write(reinterpret_cast<const char*>(&shard.length_blocks), sizeof(shard.length_blocks));
    }

    // Write timestamp
    journal_file_.write(reinterpret_cast<const char*>(&entry.timestamp), sizeof(entry.timestamp));

    // Flush to ensure durability
    journal_file_.flush();

    return true;
}

bool MetadataJournal::read_checkpoint(std::vector<GpuFile>& files) {
    std::ifstream checkpoint_file(checkpoint_path_, std::ios::binary);
    if (!checkpoint_file.is_open()) {
        return false;
    }

    // Read file count
    uint64_t file_count;
    checkpoint_file.read(reinterpret_cast<char*>(&file_count), sizeof(file_count));
    if (checkpoint_file.fail()) {
        return false;
    }

    files.reserve(file_count);

    // Read each file
    for (uint64_t i = 0; i < file_count; ++i) {
        GpuFile file;

        // Read file_id
        checkpoint_file.read(reinterpret_cast<char*>(&file.file_id), sizeof(file.file_id));

        // Read name
        uint32_t name_len;
        checkpoint_file.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
        file.name.resize(name_len);
        checkpoint_file.read(&file.name[0], name_len);

        // Read logical_size and stripe_size
        checkpoint_file.read(reinterpret_cast<char*>(&file.logical_size), sizeof(file.logical_size));
        checkpoint_file.read(reinterpret_cast<char*>(&file.stripe_size), sizeof(file.stripe_size));

        // Read shards
        uint32_t shard_count;
        checkpoint_file.read(reinterpret_cast<char*>(&shard_count), sizeof(shard_count));
        file.shards.resize(shard_count);
        for (auto& shard : file.shards) {
            checkpoint_file.read(reinterpret_cast<char*>(&shard.device_id), sizeof(shard.device_id));
            checkpoint_file.read(reinterpret_cast<char*>(&shard.namespace_id), sizeof(shard.namespace_id));
            checkpoint_file.read(reinterpret_cast<char*>(&shard.start_lba), sizeof(shard.start_lba));
            checkpoint_file.read(reinterpret_cast<char*>(&shard.length_blocks), sizeof(shard.length_blocks));
        }

        // Read creation_time
        uint64_t timestamp;
        checkpoint_file.read(reinterpret_cast<char*>(&timestamp), sizeof(timestamp));
        file.creation_time = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestamp));

        if (checkpoint_file.fail()) {
            return false;
        }

        files.push_back(file);
    }

    return true;
}

bool MetadataJournal::read_journal(std::vector<JournalEntry>& entries) {
    std::ifstream journal_file(journal_path_, std::ios::binary);
    if (!journal_file.is_open()) {
        return false;
    }

    while (journal_file.good() && journal_file.peek() != EOF) {
        JournalEntry entry;

        // Read operation type
        uint8_t op_type;
        journal_file.read(reinterpret_cast<char*>(&op_type), sizeof(op_type));
        if (journal_file.fail()) break;
        entry.op_type = static_cast<JournalOpType>(op_type);

        // Read file_id
        journal_file.read(reinterpret_cast<char*>(&entry.file_id), sizeof(entry.file_id));

        // Read name
        uint32_t name_len;
        journal_file.read(reinterpret_cast<char*>(&name_len), sizeof(name_len));
        if (name_len > 0) {
            entry.name.resize(name_len);
            journal_file.read(&entry.name[0], name_len);
        }

        // Read logical_size and stripe_size
        journal_file.read(reinterpret_cast<char*>(&entry.logical_size), sizeof(entry.logical_size));
        journal_file.read(reinterpret_cast<char*>(&entry.stripe_size), sizeof(entry.stripe_size));

        // Read shards
        uint32_t shard_count;
        journal_file.read(reinterpret_cast<char*>(&shard_count), sizeof(shard_count));
        entry.shards.resize(shard_count);
        for (auto& shard : entry.shards) {
            journal_file.read(reinterpret_cast<char*>(&shard.device_id), sizeof(shard.device_id));
            journal_file.read(reinterpret_cast<char*>(&shard.namespace_id), sizeof(shard.namespace_id));
            journal_file.read(reinterpret_cast<char*>(&shard.start_lba), sizeof(shard.start_lba));
            journal_file.read(reinterpret_cast<char*>(&shard.length_blocks), sizeof(shard.length_blocks));
        }

        // Read timestamp
        journal_file.read(reinterpret_cast<char*>(&entry.timestamp), sizeof(entry.timestamp));

        if (journal_file.fail()) break;

        entries.push_back(entry);
    }

    return true;
}

uint64_t MetadataJournal::get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

}  // namespace block_storage
}  // namespace tutti
