#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <set>
#include "ext2fs.h"
#include "ext2fs_print.h"
using namespace std;

struct GhostEntry {
    uint32_t inode;
    string name;
    uint8_t file_type;
};

struct Action {
    uint32_t timestamp;
    std::string action;
    std::vector<std::string> args;
    std::vector<uint32_t> affected_dirs;
    std::vector<uint32_t> affected_inodes;
};

class Ext2FileSystem {
private:
    std::ifstream fs_file;
    ext2_super_block super_block;
    vector<ext2_block_group_descriptor> bgd_table;
    uint32_t block_size;
    uint32_t num_block_groups;

public:
    explicit Ext2FileSystem(const std::string& filename) {
        fs_file.open(filename, std::ios::binary);
        if (!fs_file.is_open()) {
            throw std::runtime_error("Failed to open filesystem image: " + filename);
        }
        
        readSuperBlock();
        readBGDTable();
    }
    
    ~Ext2FileSystem() {
        if (fs_file.is_open()) {
            fs_file.close();
        }
    }
    
    void displayDirectoryTree() {
        std::cout << "\n=== Current Directory Structure (with Ghost Entries) ===\n";
        traverseDirectory(EXT2_ROOT_INODE, 1, "", "root");
    }

private:
    void readSuperBlock() {
        fs_file.seekg(EXT2_SUPER_BLOCK_POSITION);
        if (!fs_file.read(reinterpret_cast<char*>(&super_block), sizeof(ext2_super_block))) {
            throw std::runtime_error("Failed to read superblock");
        }
        
        if (super_block.magic != EXT2_SUPER_MAGIC) {
            throw std::runtime_error("Invalid ext2 magic number: 0x" + 
                                   std::to_string(super_block.magic));
        }
        
        block_size = EXT2_UNLOG(super_block.log_block_size);
        num_block_groups = (super_block.block_count + super_block.blocks_per_group - 1) / 
                          super_block.blocks_per_group;
        
        std::cout << "Block size: " << block_size << " bytes\n";
        std::cout << "Total blocks: " << super_block.block_count << "\n";
        std::cout << "Block groups: " << num_block_groups << "\n";
        std::cout << "Inodes per group: " << super_block.inodes_per_group << "\n";
        std::cout << "Inode size: " << super_block.inode_size << "\n";
    }
    
    void readBGDTable() {
        uint32_t bgd_table_block = super_block.first_data_block + 1;
        bgd_table.resize(num_block_groups);
        
        fs_file.seekg(bgd_table_block * block_size);
        if (!fs_file.read(reinterpret_cast<char*>(bgd_table.data()), 
                         num_block_groups * sizeof(ext2_block_group_descriptor))) {
            throw std::runtime_error("Failed to read block group descriptor table");
        }
    }
    
    std::vector<char> readBlock(uint32_t block_num) {
        std::vector<char> buffer(block_size);
        fs_file.seekg(block_num * block_size);
        if (!fs_file.read(buffer.data(), block_size)) {
            throw std::runtime_error("Failed to read block " + std::to_string(block_num));
        }
        return buffer;
    }
    
    ext2_inode readInode(uint32_t inode_num) {
        ext2_inode inode;
        std::memset(&inode, 0, sizeof(inode));
        
        if (inode_num == 0) {
            return inode;
        }
        
        // Calculate which block group contains this inode
        uint32_t group = (inode_num - 1) / super_block.inodes_per_group;
        uint32_t index = (inode_num - 1) % super_block.inodes_per_group;
        
        if (group >= num_block_groups) {
            throw std::runtime_error("Invalid inode group: " + std::to_string(group));
        }
        
        // Calculate the block and offset within the inode table
        uint32_t inode_table_block = bgd_table[group].inode_table;
        uint32_t inodes_per_block = block_size / super_block.inode_size;
        uint32_t block_offset = index / inodes_per_block;
        uint32_t inode_offset = (index % inodes_per_block) * super_block.inode_size;
        
        // Read the block containing the inode
        auto block_buffer = readBlock(inode_table_block + block_offset);
        std::memcpy(&inode, block_buffer.data() + inode_offset, sizeof(ext2_inode));
        
        return inode;
    }
    
    // Helper function to calculate the actual size needed for a directory entry
    uint32_t calculateEntrySize(uint8_t name_length) {
        // 8 bytes for the fixed part + name length, aligned to 4-byte boundary
        uint32_t size = 8 + name_length;
        uint32_t result = (size + 3) & ~3;
        return result; // Round up to next 4-byte boundary
    }
    
    // Function to find ghost entries in the unused space after a directory entry
    vector<GhostEntry> findGhostEntries(const std::vector<char>& block_buffer, 
                                           uint32_t start_offset, uint32_t available_space) {
        vector<GhostEntry> ghosts;
        uint32_t offset = start_offset;
        
        while (offset + sizeof(ext2_dir_entry) <= start_offset + available_space) {
            const ext2_dir_entry* potential_entry = 
                reinterpret_cast<const ext2_dir_entry*>(block_buffer.data() + offset);
            
            // Basic sanity checks for a valid directory entry
            if (potential_entry->inode == 0 || 
                potential_entry->name_length == 0 || 
                potential_entry->name_length > 255 ||
                potential_entry->length == 0 ||
                offset + potential_entry->name_length + 8 > start_offset + available_space) {
               
                offset += 4; // Try next 4-byte aligned position
                continue;
            } 
             //std::cout<<"name is:"<<std::string(potential_entry->name, potential_entry->name_length)<<endl;           
            // Additional validation: check if this looks like a real entry
            bool looks_valid = true;
            //NEED OR NOTTTTTT????
            // Check if name contains reasonable characters
            for (int i = 0; i < potential_entry->name_length; i++) {
                char c = potential_entry->name[i];
                if (c == 0 || (c < 32 && c != 0) || c > 126) {
                    //looks_valid = false;
                    break;
                }
            }
            
            if (looks_valid) {
                GhostEntry ghost;
                ghost.inode = potential_entry->inode;
                ghost.name = std::string(potential_entry->name, potential_entry->name_length);
                ghost.file_type = potential_entry->file_type;
                
                // Skip . and .. entries
                if (ghost.name != "." && ghost.name != "..") {
                    ghosts.push_back(ghost);
                }
                
                // Move to next potential entry
                uint32_t entry_size = calculateEntrySize(potential_entry->name_length);
                offset += entry_size;
            } else {
                offset += 4; // Try next 4-byte aligned position
            }
        }
        
        return ghosts;
    }
    
    void traverseDirectory(uint32_t inode_num, int depth, const std::string& current_path, const std::string& dir_name = "") {
        ext2_inode inode = readInode(inode_num); 
        // Check if this is a directory
        if (!(inode.mode & EXT2_I_DTYPE)) {return;}

        // Print current directory witsh proper indentation
        if (!dir_name.empty() || depth == 1) {
            std::string indent(depth, '-');
            if (depth == 1) {
                cout << indent << " " << inode_num << ":root/\n";
            } else {
                cout << indent << " " << inode_num << ":" << dir_name << "/\n";
            }
        }
        
        // Read directory data blocks
        for (int i = 0; i < EXT2_NUM_DIRECT_BLOCKS && inode.direct_blocks[i] != 0; i++) {
            try {
                auto block_buffer = readBlock(inode.direct_blocks[i]);
                processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path);
            } catch (const std::exception& e) {
                std::cerr << "Error reading directory block: " << e.what() << "\n";
                continue;
            }
        }
        //BURAYA BAK HARD LİNKKLEERRR??
        // Handle single indirect block if present
        if (inode.single_indirect != 0) {
            try {
                cout<<"single ind"<<endl;
                auto indirect_block = readBlock(inode.single_indirect);
                uint32_t* block_pointers = reinterpret_cast<uint32_t*>(indirect_block.data());
                uint32_t pointers_per_block = block_size / sizeof(uint32_t);
                
                for (uint32_t i = 0; i < pointers_per_block && block_pointers[i] != 0; i++) {
                    auto block_buffer = readBlock(block_pointers[i]);
                    processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path);
                }
            } catch (const std::exception& e) {
                std::cerr << "Error reading indirect directory block: " << e.what() << "\n";
            }
        }
        if(inode.double_indirect != 0){
            cout<<"double ind"<<endl;
        }
        if(inode.triple_indirect != 0){
            cout<<"triple ind"<<endl;
        }
    }
    
    void processDirectoryBlockWithGhosts(const std::vector<char>& block_buffer, int depth, const std::string& current_path) {
        uint32_t offset = 0;
        std::set<uint32_t> active_inodes; // Track active inodes to avoid duplicate ghosts
        
        // First pass: process active entries and collect ghost entries
        vector<std::pair<std::string, uint32_t>> active_entries;
        vector<GhostEntry> all_ghosts;
        
        while (offset < block_size) {
            const ext2_dir_entry* entry = 
                reinterpret_cast<const ext2_dir_entry*>(block_buffer.data() + offset);
            if (entry->length == 0) {
                break;
            }
            
            if (entry->inode != 0) {
                // Extract and null-terminate the name
                string name(entry->name, entry->name_length);
                //cout<<"the name is:" <<name<<endl;

                // Skip . and .. entries for active processing
                if (name != "." && name != ".." ) {
                    active_inodes.insert(entry->inode);
                    
                    if (entry->file_type == EXT2_D_DTYPE) {
                        active_entries.push_back({name + "/", entry->inode});
                        //live_paths.push_back()
                    } else {
                        active_entries.push_back({name, entry->inode});
                    }
                }
            }
            
            // Check for ghost entries in the unused space of this entry
            uint32_t actual_size = calculateEntrySize(entry->name_length);
            if (entry->length > actual_size) {
                uint32_t unused_space = entry->length - actual_size;
                auto ghosts = findGhostEntries(block_buffer, offset + actual_size, unused_space);
                string name(entry->name, entry->name_length);
                // Filter out ghosts that are actually active entries  //NEDEENNN AYNI DİR İCİNDEYİZ DİYE Mİ?
                for (const auto& ghost : ghosts) {
                    if (active_inodes.find(ghost.inode) == active_inodes.end()) {
                        all_ghosts.push_back(ghost);
                    }
                }
            
            }
        offset += entry->length;
        }
        // Print active entries
        for (const auto& [name, inode] : active_entries) {
            string indent(depth, '-');
            
            if ((name.back() == '/')) {
                // Directory entry
                string dir_name = name.substr(0, name.length() - 1);
                string new_path = current_path.empty() ? dir_name : current_path + "/" + dir_name;
                traverseDirectory(inode, depth, new_path, dir_name);
            } else {
                // Regular file entry
                std::cout << indent << " " << inode << ":" << name << "\n";
            }
        }
        
        // Print ghost entries (in parentheses)
        for (const auto& ghost : all_ghosts) {
            std::string indent(depth, '-');
            if (ghost.file_type == EXT2_D_DTYPE) {
                // Ghost directory - don't traverse its contents as per requirement
                std::cout << indent << " (" << ghost.inode << ":" << ghost.name << "/)\n";
            } else {
                // Ghost file
                std::cout << indent << " (" << ghost.inode << ":" << ghost.name << ")\n";
            }
        }
}
                       

};   

int main(int argc, char* argv[]) {
    Ext2FileSystem fs(argv[1]);
    fs.displayDirectoryTree();
    return 0;
}