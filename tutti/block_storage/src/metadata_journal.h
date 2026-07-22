#pragma once

#include "block_storage_types.h"

#include <string>
#include <fstream>
#include <vector>

namespace tutti {
namespace block_storage {

enum class JournalOpType {
    CREATE,
    DELETE,
    RESIZE
};

struct JournalEntry {
    JournalOpType op_type;
    FileId file_id;
    std::string name;
    uint64_t logical_size;
    uint64_t stripe_size;
    std::vector<FileShard> shards;
    uint64_t timestamp;

    JournalEntry()
        : op_type(JournalOpType::CREATE), file_id(INVALID_FILE_ID),
          logical_size(0), stripe_size(0), timestamp(0) {}
};

class MetadataJournal {
public:
    MetadataJournal();
    ~MetadataJournal();

    // Initialize journal with root directory
    bool initialize(const std::string& root_directory);

    // Log operations
    bool log_create(const GpuFile& file);
    bool log_delete(FileId file_id, const std::string& name);
    bool log_resize(FileId file_id, uint64_t new_size);

    // Checkpoint operations
    bool checkpoint(const std::vector<GpuFile>& all_files);

    // Recovery
    std::vector<JournalEntry> recover();

    // Close journal
    void close();

private:
    bool write_entry(const JournalEntry& entry);
    bool read_checkpoint(std::vector<GpuFile>& files);
    bool read_journal(std::vector<JournalEntry>& entries);
    uint64_t get_timestamp();

    std::string root_directory_;
    std::string journal_path_;
    std::string checkpoint_path_;
    std::ofstream journal_file_;
    bool initialized_;
};

}  // namespace block_storage
}  // namespace tutti
