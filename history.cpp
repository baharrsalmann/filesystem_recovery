#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <map>
#include <set>
#include "ext2fs.h"
#include "ext2fs_print.h"
#include <algorithm> //SORUN VAR MI?
using namespace std;

struct GhostEntry {
    uint32_t inode;
    string name;
    uint8_t file_type;
};

struct EntryRecord {
    string full_path;
    string name;
    uint32_t parent_inode;
    bool is_ghost;
    // == operator overload
    bool operator==(const EntryRecord& other) const {
        return full_path == other.full_path &&
               name == other.name &&
               parent_inode == other.parent_inode &&
               is_ghost == other.is_ghost;
    }
};
struct Info {
    int live_count, ghost_count;
    EntryRecord LiveEntry, CreationEntry, DeletionEntry, OtherGhost;
    bool foundCreation, foundDeletion, foundOtherGhost;
};

struct InodeRecord {
    ext2_inode inode_data;
    vector<EntryRecord> entries;
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
    map<uint32_t, InodeRecord> inode_to_info;

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
        traverseDirectory(EXT2_ROOT_INODE, 1, "", "root", false);
    }
    void recovery(){
        printRecoveredActions();
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
    
    uint32_t calculateEntrySize(uint8_t name_length) {   
        uint32_t size = 8 + name_length;
        uint32_t result = (size + 3) & ~3;
        return result; 
    }
    
    // Function to find ghost entries in the unused space after a directory entry
    vector<GhostEntry> findGhostEntries(const std::vector<char>& block_buffer, 
                                           uint32_t start_offset, uint32_t available_space) {
        vector<GhostEntry> ghosts;
        uint32_t offset = start_offset;
        
        while (offset + sizeof(ext2_dir_entry) <= start_offset + available_space) {
            const ext2_dir_entry* potential_entry = 
                reinterpret_cast<const ext2_dir_entry*>(block_buffer.data() + offset);
            
            if (potential_entry->inode == 0 || 
                potential_entry->name_length == 0 || 
                potential_entry->name_length > 255 ||
                potential_entry->length == 0 ||
                offset + potential_entry->name_length + 8 > start_offset + available_space) {
               
                offset += 4; 
                continue;
            }                    
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
        }
        return ghosts;
    }
    
    void traverseDirectory(uint32_t inode_num, int depth, const std::string& current_path, 
                          const std::string& dir_name = "", bool is_ghost = false) {
        ext2_inode inode = readInode(inode_num); 
        
        if (!(inode.mode & EXT2_I_DTYPE)) {
            return;
        }

        if (!dir_name.empty() || depth == 1) {
            std::string indent(depth, '-');
            if (depth == 1) {
                cout << indent << " " << inode_num << ":root/\n";
            } else {
                if (is_ghost) {
                    cout << indent << " (" << inode_num << ":" << dir_name << "/)\n";
                } else {
                    cout << indent << " " << inode_num << ":" << dir_name << "/\n";
                }
            }
        }
        
        // Read directory data blocks
        for (int i = 0; i < EXT2_NUM_DIRECT_BLOCKS && inode.direct_blocks[i] != 0; i++) {
            try {
                auto block_buffer = readBlock(inode.direct_blocks[i]);
                processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path,inode_num, is_ghost);
            } catch (const std::exception& e) {
                //std::cerr << "Error reading directory block: " << e.what() << "\n";
                continue;
            }
        }
        
        if (inode.single_indirect != 0) {
            try {
                auto indirect_block = readBlock(inode.single_indirect);
                uint32_t* block_pointers = reinterpret_cast<uint32_t*>(indirect_block.data());
                uint32_t pointers_per_block = block_size / sizeof(uint32_t);
                
                for (uint32_t i = 0; i < pointers_per_block && block_pointers[i] != 0; i++) {
                    auto block_buffer = readBlock(block_pointers[i]);
                    processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path,inode_num, is_ghost);
                }
            } catch (const std::exception& e) {
                //std::cerr << "Error reading indirect directory block: " << e.what() << "\n";
            }
        }

        if (inode.double_indirect != 0) {
            try {
                auto double_block = readBlock(inode.double_indirect);
                uint32_t* single_indirect_ptrs = reinterpret_cast<uint32_t*>(double_block.data());
                uint32_t pointers_per_block = block_size / sizeof(uint32_t);

                for (uint32_t i = 0; i < pointers_per_block && single_indirect_ptrs[i] != 0; i++) {
                    auto indirect_block = readBlock(single_indirect_ptrs[i]);
                    uint32_t* data_block_ptrs = reinterpret_cast<uint32_t*>(indirect_block.data());

                    for (uint32_t j = 0; j < pointers_per_block && data_block_ptrs[j] != 0; j++) {
                        auto block_buffer = readBlock(data_block_ptrs[j]);
                        processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path,is_ghost);
                    }
                }
            } catch (const std::exception& e) {
                //std::cerr << "Error reading double indirect directory block: " << e.what() << "\n";
            }
        }

        if (inode.triple_indirect != 0) {
            try {
                auto triple_block = readBlock(inode.triple_indirect);
                uint32_t* double_indirect_ptrs = reinterpret_cast<uint32_t*>(triple_block.data());
                uint32_t pointers_per_block = block_size / sizeof(uint32_t);

                for (uint32_t i = 0; i < pointers_per_block && double_indirect_ptrs[i] != 0; i++) {
                    auto double_block = readBlock(double_indirect_ptrs[i]);
                    uint32_t* single_indirect_ptrs = reinterpret_cast<uint32_t*>(double_block.data());

                    for (uint32_t j = 0; j < pointers_per_block && single_indirect_ptrs[j] != 0; j++) {
                        auto indirect_block = readBlock(single_indirect_ptrs[j]);
                        uint32_t* data_block_ptrs = reinterpret_cast<uint32_t*>(indirect_block.data());

                        for (uint32_t k = 0; k < pointers_per_block && data_block_ptrs[k] != 0; k++) {
                            auto block_buffer = readBlock(data_block_ptrs[k]);
                            processDirectoryBlockWithGhosts(block_buffer, depth + 1, current_path,is_ghost);
                        }
                    }
                }
            } catch (const std::exception& e) {
                //std::cerr << "Error reading triple indirect directory block: " << e.what() << "\n";
            }
        }

    }
    
    // Modified processDirectoryBlockWithGhosts to traverse ghost directories
    void processDirectoryBlockWithGhosts(const std::vector<char>& block_buffer, int depth, 
                                        const std::string& current_path,uint32_t dir_inode, bool parent_is_ghost = false) {
        uint32_t offset = 0;
        std::set<uint32_t> active_inodes;
        vector<std::pair<std::string, uint32_t>> active_entries;
        vector<GhostEntry> all_ghosts;
                                          
        while (offset < block_size) {
            const ext2_dir_entry* entry = 
                reinterpret_cast<const ext2_dir_entry*>(block_buffer.data() + offset);
            if (entry->length == 0) break;
            if (entry->inode != 0) {
                string name(entry->name, entry->name_length);
                if (name != "." && name != "..") {
                    active_inodes.insert(entry->inode);
                    ext2_inode inode_data = readInode(entry->inode);
                    string full_path = current_path.empty() ? name : current_path + "/" + name;

                    if (inode_to_info.find(entry->inode) == inode_to_info.end()) {
                        inode_to_info[entry->inode].inode_data = inode_data;
                    }
                    inode_to_info[entry->inode].entries.push_back({"/"+full_path, name, dir_inode, false});

                    if (entry->file_type == EXT2_D_DTYPE) {
                        active_entries.push_back({name + "/", entry->inode});
                    } else {
                        active_entries.push_back({name, entry->inode});
                    }
                }
            }

            uint32_t actual_size = calculateEntrySize(entry->name_length);
            if (entry->length > actual_size) {
                uint32_t unused_space = entry->length - actual_size;
                auto ghosts = findGhostEntries(block_buffer, offset + actual_size, unused_space);
                for (const auto& ghost : ghosts) {
                    if (active_inodes.find(ghost.inode) == active_inodes.end()) {
                        all_ghosts.push_back(ghost);
                        ext2_inode inode_data = readInode(ghost.inode);
                        string full_path = current_path.empty() ? ghost.name : current_path + "/" + ghost.name;

                        if (inode_to_info.find(ghost.inode) == inode_to_info.end()) {
                            inode_to_info[ghost.inode].inode_data = inode_data;
                        }
                        inode_to_info[ghost.inode].entries.push_back({"/"+full_path, ghost.name, dir_inode, true});
                    }
                }
            }
            offset += entry->length;
        }

        for (const auto& [name, inode] : active_entries) {
            string indent(depth, '-');
            if (name.back() == '/') {
                string dir_name = name.substr(0, name.length() - 1);
                string new_path = current_path.empty() ? dir_name : current_path + "/" + dir_name;
                traverseDirectory(inode, depth, new_path, dir_name, parent_is_ghost);
            } else {
                if (parent_is_ghost) {
                    //std::cout << indent << " (" << inode << ":" << name << ")\n";
                } else {
                    std::cout << indent << " " << inode << ":" << name << "\n";
                }
            }
        }

        for (const auto& ghost : all_ghosts) {
            std::string indent(depth, '-');
            if (ghost.file_type == EXT2_D_DTYPE) {
                string new_path = current_path.empty() ? ghost.name : current_path + "/" + ghost.name;
                traverseDirectory(ghost.inode, depth, new_path, ghost.name, true);
            } else {
                if(!parent_is_ghost) cout << indent << " (" << ghost.inode << ":" << ghost.name << ")\n";
            }
        }
    }
    Info getGhostsandLive(InodeRecord inode){
        int live_count = 0, ghost_count = 0;
        EntryRecord LiveEntry, CreationEntry, DeletionEntry, OtherGhost;
        bool foundCreation=false, foundDeletion=false, foundOtherGhost=false;
        for (const auto& e : inode.entries) {
            if (e.is_ghost) ghost_count++;
            else {
                    live_count++;
                    LiveEntry=e;
                }
        }
        //if(live_count==0 && inode.inode_data.deletion_time==0) cout<<"COULDNT FIND LIVE ENTRY!!!"<<endl;
        //Caseler
        if(ghost_count==0 && live_count==1){  //kesin
            CreationEntry=LiveEntry;
            foundCreation=true;
        }
        else if(ghost_count==1 && live_count==1){ //kesin.
            for (const auto& e : inode.entries) {
                if (e.is_ghost) {
                    CreationEntry=e;
                    foundCreation=true;
                    break;
                }
            }   
        }
        else if(ghost_count==2 && live_count==1){ //belki creationentryyi buluruz. gc=2 olunca hem tersten önce otherghostu zorlayıp diğerine creatin entry diyebilirsin.
            //Creation arama
            int potential_flag=0; EntryRecord potential;
            for (const auto& e : inode.entries) {
                if(e.is_ghost && readInode(e.parent_inode).modification_time == inode.inode_data.access_time){foundCreation=true; CreationEntry=e; break;}
                else if (e.is_ghost && readInode(e.parent_inode).access_time < inode.inode_data.access_time){ 
                    potential_flag++; 
                    potential=e;}
            }
            if(potential_flag==1) {CreationEntry=potential; foundCreation=true; }
            //creation found then other ghost is obvious.
            if(foundCreation){
                for(const auto& e : inode.entries) {
                    if(e.is_ghost && !(e==CreationEntry)) {OtherGhost=e; foundOtherGhost=true;}
                }
            }
            else if(!foundCreation){
                for(const auto&e : inode.entries){
                    if((e.is_ghost) && (readInode(e.parent_inode).modification_time==readInode(LiveEntry.parent_inode).modification_time 
                                || readInode(e.parent_inode).modification_time==inode.inode_data.change_time)) {
                        foundOtherGhost=true;
                        OtherGhost=e;
                        break;
                    }
                }
                //otherghost found then creation is obvious.
                if(foundOtherGhost){
                    for(const auto& e : inode.entries) {
                        if(e.is_ghost && !(e==OtherGhost)) {CreationEntry=e; foundCreation=true;}
                    }
                }
            }
        }
        else if(ghost_count>2 && live_count==1){
            //Creation arama
            int potential_flag=0; EntryRecord potential;
            for (const auto& e : inode.entries) {
                if(e.is_ghost && readInode(e.parent_inode).modification_time == inode.inode_data.access_time){foundCreation=true; CreationEntry=e; break;}
                else if (e.is_ghost && readInode(e.parent_inode).access_time < inode.inode_data.access_time){ 
                    potential_flag++; 
                    potential=e;}
            }
            if(potential_flag==1) {CreationEntry=potential; foundCreation=true; }
        }
        else if(ghost_count==1 && live_count==0){ //kesin
            foundCreation=true; foundDeletion=true;
            CreationEntry=inode.entries[0];
            DeletionEntry=inode.entries[0];
        }
        else if(ghost_count==2 && live_count==0){
            //hem creation hem deletion ara.
            int potential_flag=0; EntryRecord potential;
            for (const auto& e : inode.entries) {
                if(readInode(e.parent_inode).modification_time == inode.inode_data.access_time){foundCreation=true; CreationEntry=e; break;}
                else if (readInode(e.parent_inode).access_time < inode.inode_data.access_time){ 
                    potential_flag++; 
                    potential=e;}
            }
            if(potential_flag==1) {CreationEntry=potential; foundCreation=true; }

            //creation found then deletion is obvious.
            if(foundCreation){
                for(const auto& e : inode.entries) {
                    if(e.is_ghost && !(e==CreationEntry)) {DeletionEntry=e; foundDeletion=true;}
                }
            }
            else if(!foundCreation){
                int potential_flag=0; EntryRecord potential;
                for (const auto& e : inode.entries) {
                    if(readInode(e.parent_inode).modification_time == inode.inode_data.deletion_time){foundDeletion=true; DeletionEntry=e; break;}
                    else if (readInode(e.parent_inode).modification_time > inode.inode_data.deletion_time){ 
                        potential_flag++; 
                        potential=e;}
                }
                if(potential_flag==1) {DeletionEntry=potential; foundDeletion=true;}

                //found deletion, creation is obvious
                if(foundDeletion){
                   for(const auto& e : inode.entries) {
                        if(!(e==DeletionEntry)) {CreationEntry=e; foundCreation=true;}
                    }    
                }
            }

        }
        else if(ghost_count>2 &&live_count==0){
            //creation arama
            int potential_flag_c=0; EntryRecord potential_c;
            for (const auto& e : inode.entries) {
                if(e.is_ghost && readInode(e.parent_inode).modification_time == inode.inode_data.access_time){foundCreation=true; CreationEntry=e; break;}
                else if (e.is_ghost && readInode(e.parent_inode).access_time < inode.inode_data.access_time){ 
                    potential_flag_c++; 
                    potential_c=e;}
            }
            if(potential_flag_c==1) {CreationEntry=potential_c; foundCreation=true; }
            //deletion arama
            int potential_flag=0; EntryRecord potential;
            for (const auto& e : inode.entries) {
                if(readInode(e.parent_inode).modification_time == inode.inode_data.deletion_time){foundDeletion=true; DeletionEntry=e; break;}
                else if (readInode(e.parent_inode).modification_time > inode.inode_data.deletion_time){ 
                    potential_flag++; 
                    potential=e;}
                }
            if(potential_flag==1) {DeletionEntry=potential; foundDeletion=true;}

        }
        Info info;
        info={live_count,ghost_count,LiveEntry,CreationEntry,DeletionEntry,OtherGhost,foundCreation,foundDeletion,foundOtherGhost};
        return info;
    }

    void printRecoveredActions() {
        vector<Action> actions;
        Info info;
        for (const auto& [inode, record] : inode_to_info) {
            info=getGhostsandLive(record);
            const auto& inode_data = record.inode_data;
            Action action;
            action.timestamp = inode_data.access_time;
            action.action = (inode_data.mode & EXT2_I_DTYPE) ? "mkdir" : "touch";
            action.affected_inodes = { inode };
            if(info.foundCreation){
                action.args={info.CreationEntry.full_path};
                action.affected_dirs={info.CreationEntry.parent_inode};
            }
            else{
                action.args = {""};
                action.affected_dirs = {0};
            }
            actions.push_back(action);
            //-----------------mkdir/touch yapildi------------------------//

            if(info.ghost_count==0) continue;

            if(inode_data.deletion_time!=0){
                Action action;
                action.timestamp=inode_data.deletion_time;
                action.action=(inode_data.mode & EXT2_I_DTYPE) ? "rmdir" : "rm";
                action.affected_inodes={inode};
                if(info.foundDeletion){
                    action.args={info.DeletionEntry.full_path};
                    action.affected_dirs={info.DeletionEntry.parent_inode};
                }
                else{
                    action.args = {""};
                    action.affected_dirs = {0};
                }
                actions.push_back(action);

            //----------------rm/rmdir yapildi--------------------------//
                             
            Action actmove;
            actmove.action="mv";
            actmove.affected_inodes={inode};
            actmove.timestamp=0;
            if(info.ghost_count==2 && info.foundCreation && info.foundDeletion ){
                actmove.args={info.CreationEntry.full_path, info.DeletionEntry.full_path};
                actmove.affected_dirs={info.CreationEntry.parent_inode, info.DeletionEntry.parent_inode};
                actions.push_back(actmove); 
            }
            else if(info.ghost_count>1 && !info.foundCreation){
                //ghost sayısı kadar dön, sadece nereden cıktıklarının movelarını yazabilirsin, deletion entryi pass geç.
                if(info.foundDeletion){
                    actmove.args={"?",info.DeletionEntry.full_path};
                    actmove.affected_dirs={0 , info.DeletionEntry.parent_inode};
                    actions.push_back(actmove);
                    
                    for (const auto& e : record.entries) {
                        if(e.is_ghost && !(e==info.DeletionEntry)){
                            actmove.args={e.full_path,"?"};
                            actmove.affected_dirs={e.parent_inode,0};
                            actions.push_back(actmove);
                        }
                    }
                }
                else{
                    for (const auto& e : record.entries) { //burada fazladan bir move bastırma olasılığın cok yüksek.
                        if(e.is_ghost && readInode(e.parent_inode).modification_time!=inode_data.deletion_time){
                            actmove.args={e.full_path,"?"};
                            actmove.affected_dirs={e.parent_inode,0};
                            actions.push_back(actmove);
                        }
                    }
                }
            }

            }
 

            else{ //deletion_time==0  // 1ghost-1live, 3 ghost-1live gibi. çünkü sadece live olanlari continueladın. 
                Action actmove;
                actmove.action="mv";
                actmove.affected_inodes={inode};

                if(info.ghost_count==1){
                    if(inode_data.change_time!=inode_data.modification_time) actmove.timestamp=inode_data.change_time;
                    else {actmove.timestamp=0;}  
                    actmove.affected_dirs = { record.entries[0].parent_inode , record.entries[1].parent_inode};
                    if(record.entries[0].is_ghost){ 
                        actmove.args = {record.entries[0].full_path, record.entries[1].full_path};
                        }
                    else{
                        actmove.args = {record.entries[1].full_path, record.entries[0].full_path};
                        }

                    actions.push_back(actmove);
                }
                else if(info.ghost_count==2 && info.foundCreation && info.foundOtherGhost){
                    actmove.affected_dirs={info.CreationEntry.parent_inode, info.OtherGhost.parent_inode};
                    actmove.timestamp=0;
                    actmove.args={info.CreationEntry.full_path,info.OtherGhost.full_path};
                    actions.push_back(actmove);

                    actmove.affected_dirs={info.OtherGhost.parent_inode,info.LiveEntry.parent_inode};
                    actmove.args={info.OtherGhost.full_path,info.LiveEntry.full_path};
                    if(readInode(info.OtherGhost.parent_inode).modification_time==readInode(info.LiveEntry.parent_inode).modification_time 
                            || readInode(info.OtherGhost.parent_inode).modification_time==inode_data.change_time)
                        actmove.timestamp={readInode(info.OtherGhost.parent_inode).modification_time};
                    else if(inode_data.change_time!=inode_data.modification_time) {
                        actmove.timestamp=inode_data.change_time;}
                    actions.push_back(actmove);
                }
                else{
                    bool matchedwithLive=false;
                    for (const auto& e : record.entries) {
                        if(!e.is_ghost) continue;
                        if(readInode(e.parent_inode).modification_time==readInode(info.LiveEntry.parent_inode).modification_time 
                            || readInode(e.parent_inode).modification_time==inode_data.change_time){
                            matchedwithLive=true;
                            actmove.affected_dirs={e.parent_inode,info.LiveEntry.parent_inode};
                            actmove.args={e.full_path,info.LiveEntry.full_path};
                            actmove.timestamp=readInode(e.parent_inode).modification_time;
                            }
                        else{
                        actmove.affected_dirs={e.parent_inode, 0};
                        actmove.args={e.full_path, "?"}; 
                        actmove.timestamp=0;
                        }
                        actions.push_back(actmove);
                    }
                    if(!matchedwithLive){
                        actmove.affected_dirs={0,info.LiveEntry.parent_inode};
                        actmove.args={"?",info.LiveEntry.full_path};
                        if(inode_data.change_time!=inode_data.modification_time) actmove.timestamp=inode_data.change_time;
                        else actmove.timestamp=0;
                        actions.push_back(actmove);
                    }
                    
                }

                
            }
            
        }
    
        std::sort(actions.begin(), actions.end(), [](const Action& a, const Action& b) {
            return a.timestamp < b.timestamp;
        });

        for (const auto& act : actions) {
            printAction(act);
        }
    }

        void printAction(Action action) const{
            if(action.timestamp==0){cout << "? " <<  action.action << " [";}
            else cout << action.timestamp << " " << action.action << " [";
            for (size_t i = 0; i < action.args.size(); ++i) {
                if (i) std::cout << " ";
                if(action.args[i]=="") cout<<"?";
                else cout << action.args[i];
            }
            std::cout << "] [";
            for (size_t i = 0; i < action.affected_dirs.size(); ++i) {
                if (i) std::cout << " ";
                if(action.affected_dirs[i]==0) cout<<"?";
                else std::cout << action.affected_dirs[i];
            }
            std::cout << "] [";
            for (size_t i = 0; i < action.affected_inodes.size(); ++i) {
                if (i) std::cout << " ";
                if(action.affected_inodes[i]==0) cout<<"?";
                else std::cout << action.affected_inodes[i];
            }
            std::cout << "]\n";
    }



};   



int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: ./histext2fs <image> <state_output> <history_output>\n";
        return 1;
    }
    const string image_path = argv[1];
    const string state_output = argv[2];
    const string history_output = argv[3];

    Ext2FileSystem fs(image_path);

    // Redirect state output
    std::ofstream state_out(state_output);
    std::streambuf* coutbuf = std::cout.rdbuf(); // backup
    std::cout.rdbuf(state_out.rdbuf());
    fs.displayDirectoryTree();
    std::cout.rdbuf(coutbuf); // restore

    // Redirect history output
    std::ofstream history_out(history_output);
    std::cout.rdbuf(history_out.rdbuf());
    fs.recovery();
    std::cout.rdbuf(coutbuf); // restore again

    return 0;
}
