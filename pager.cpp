#include "vm_pager.h"
#include <vector>
#include <queue>
#include <iostream>
#include <cstring>
#include <array>
#include <map>

#ifdef ASSERTENABLE
#include <cassert>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x)
#endif

using namespace std;

class virtual_page {
public:
    bool valid = 1;
    bool resident = 0;
	bool dirty = 0;
    bool zero_filled = 1;
    bool referenced = 1;
    bool file_backed; // 1 if file_backed, 0 if swap_backed
    int virtual_page_idx = -1;

    int num_sharing = 1; // number of pages sharing same resource

    char *filename;
    int block; // can be used for either file or swap

    int physmem_address = -1;
};

class process {
public:
    pid_t pid;
    int page_table_counter = 0;
    page_table_t* page_table;
    vector<virtual_page*> virtual_pages;
};

pid_t current_process;
map<int, int> process_idx_map;
int process_counter = 0;
int current_process_idx = -1;
vector<process*> p;

int MEMORY_PAGES;
bool* physmem_occupancy;

int SWAP_BLOCKS;
int num_swap_blocks = 0;
bool* swap_occupancy;

queue<virtual_page*> evict_clock;

class file_backed_ppage {
public:
    const char *filename;
    size_t block;
};

vector<file_backed_ppage*> file_backed_ppages;


void vm_init(size_t memory_pages, size_t swap_blocks) {

    MEMORY_PAGES = memory_pages;
    SWAP_BLOCKS = swap_blocks;

    physmem_occupancy = new bool[MEMORY_PAGES];
    physmem_occupancy[MEMORY_PAGES] = {0};

    swap_occupancy = new bool[SWAP_BLOCKS];
    swap_occupancy[SWAP_BLOCKS] = {0};

    memset(vm_physmem, 0, 4096);    //pinned 0 page
    physmem_occupancy[0] = true;
}

int vm_create(pid_t parent_pid, pid_t child_pid) {
    // need to figure out when to return on failure
    process * temp_process = new process;
    temp_process->pid = child_pid;
    process_idx_map[child_pid] = process_counter;
    process_counter++;

    bool is_managed = 0; // if parent process is managed by pager
    for (int i = 0; i < p.size(); i++) {
        if (p[i]->pid == parent_pid) {
            temp_process->page_table_counter = p[i]->page_table_counter;
            temp_process->page_table = new page_table_t(*p[i]->page_table);

            for (int j = 0; j < p[i]->virtual_pages.size(); j++) {
                p[i]->virtual_pages[j]->num_sharing++;
            }
            temp_process->virtual_pages = p[i]->virtual_pages;

            is_managed = 1;
            break;
        }
    }

    if (!is_managed) { // consider arena to be empty if parent isn't managed by pager
        temp_process->page_table_counter = 0;
        page_table_t * temp_table = new page_table_t;
        page_table_entry_t temp_entry;
        temp_entry.ppage = 0;
        temp_entry.read_enable = 0;
        temp_entry.write_enable = 0;
        for (int i = 0; i < VM_ARENA_SIZE/VM_PAGESIZE; ++i) {
            temp_table->ptes[i] = temp_entry;
        }
        temp_process->page_table = temp_table;
    }

    p.push_back(temp_process);
    return 0; // success
}

void vm_switch(pid_t pid) {
    current_process = pid;
    current_process_idx = process_idx_map[current_process];

    page_table_base_register = p[current_process_idx]->page_table;
}

void evict_and_add(int addr_idx, bool file_backed) {
    //cout << "EVICT AND ADD\n";
    // trying to find empty space in clock
    int empty_idx = -1;
    for(int i = 0; i < MEMORY_PAGES; ++i){
        if(!physmem_occupancy[i]){
            empty_idx = i;
            break;
        }
    }
    if(empty_idx == -1) { // need to evict some page
        bool found = 0;
        while(!found) { // go thru LRU clock algorithm
            if(evict_clock.front()->referenced) {
                p[current_process_idx]->page_table->ptes[evict_clock.front()->virtual_page_idx].read_enable = 0;
                p[current_process_idx]->page_table->ptes[evict_clock.front()->virtual_page_idx].write_enable = 0;

                evict_clock.front()->referenced = 0;
                evict_clock.push(evict_clock.front());
                evict_clock.pop();
            } else { // referenced == 0, can evict this page
                empty_idx = evict_clock.front()->physmem_address;
                //cout << "empty_idx: " << empty_idx << endl;
                if(evict_clock.front()->dirty){
                    for(int i = 0; i < SWAP_BLOCKS; ++i){
                        // where in swap file we can write the page to
                        if(!swap_occupancy[i]){
                            swap_occupancy[i] = 1;
                            evict_clock.front()->block = i;
                            break;
                        }
                    }
                    char *filename = nullptr;
                    if(file_backed) {
                        filename = evict_clock.front()->filename;
                    }
                    char * buf = (char *)(vm_physmem + 4096*empty_idx);
                    file_write(filename, evict_clock.front()->block, buf);
                }
                evict_clock.front()->resident = 0;
                evict_clock.front()->dirty = 0;
                evict_clock.pop();
                evict_clock.push(p[current_process_idx]->virtual_pages[addr_idx]);
                found = 1;
            }
        }
    } else { // empty space in clock, can just add page to clock
        evict_clock.push(p[current_process_idx]->virtual_pages[addr_idx]);
    }

    p[current_process_idx]->virtual_pages[addr_idx]->physmem_address = empty_idx;
    p[current_process_idx]->virtual_pages[addr_idx]->resident = 1;
    p[current_process_idx]->page_table->ptes[addr_idx].ppage = empty_idx;
    physmem_occupancy[empty_idx] = 1;

   if(p[current_process_idx]->virtual_pages[addr_idx]->zero_filled) {
       memset(vm_physmem + empty_idx*4096, 0, 4096);
   } else {
       char *buf =(char*)(vm_physmem + empty_idx*4096);
       int block = p[current_process_idx]->virtual_pages[addr_idx]->block;

       char *filename2 = nullptr;
       if(file_backed) {
           filename2 = p[current_process_idx]->virtual_pages[addr_idx]->filename;
           int addr_idx = ((intptr_t) filename2 - (intptr_t)vm_physmem)/4096;

           //cout << "filename2: " << (intptr_t)filename2 << endl;
           //cout << "addr_idx: " << addr_idx << endl;
           //cout << "block: " << block << endl;
       }
       file_read(filename2, block, buf);
   }
}

int vm_fault(const void *addr, bool write_flag) {
    // debugging output
    // for (int i = 0; i < evict_clock.size(); i++) {
    //     cout << "virtual_page_idx: " << evict_clock.front()->virtual_page_idx << '\n';
    //     cout << "r/w = ("
    //          << p[current_process_idx]->page_table->ptes[evict_clock.front()->virtual_page_idx].read_enable
    //          << "," << p[current_process_idx]->page_table->ptes[evict_clock.front()->virtual_page_idx].write_enable << ")\n"
    //          << "referenced = " << evict_clock.front()->referenced << '\n'
    //          << "dirty = " << evict_clock.front()->dirty << "\n\n";
    //     evict_clock.push(evict_clock.front());
    //     evict_clock.pop();
    // }

    for (int i = 0; i < p[current_process_idx]->page_table_counter; i++) {
        virtual_page * temp_print = p[current_process_idx]->virtual_pages[i];

        //cout << "virtual_page_idx: " << temp_print->virtual_page_idx << '\n';
        //cout << "resident = " << temp_print->resident << '\n';
        //cout << "r/w = ("
             //<< p[current_process_idx]->page_table->ptes[temp_print->virtual_page_idx].read_enable
             //<< "," << p[current_process_idx]->page_table->ptes[temp_print->virtual_page_idx].write_enable << ")\n"
             //<< "referenced = " << temp_print->referenced << '\n'
             //<< "dirty = " << temp_print->dirty << "\n\n";
    }
    // end debugging output

    int addr_idx = ((intptr_t) addr - (intptr_t)VM_ARENA_BASEADDR)/4096;

    virtual_page * temp = p[current_process_idx]->virtual_pages[addr_idx];
    page_table_entry_t * temp_pte = &p[current_process_idx]->page_table->ptes[addr_idx];

    if(write_flag) {
      //  cout << "WRITE FLAG\n";
        if (temp->num_sharing > 1) { // copy on write
            // decrement virtual page num_sharing
            temp->num_sharing--;
            // allocate make new deep copy of virtual page
            temp = new virtual_page(*temp); // now the virtual page points to deep copy
            // set num_sharing for new separate virtual page = 1
            temp->num_sharing = 1;
            // allocate new mem for physical page
            // point page table to new virtual page
            evict_and_add(addr_idx, temp->file_backed);
        }

        if(temp->zero_filled || !temp->resident) evict_and_add(addr_idx, temp->file_backed);
        temp_pte->write_enable = 1;
        temp->dirty = 1;
        temp->zero_filled = 0;
    } else if (addr_idx < p[current_process_idx]->virtual_pages.size()) {
        //  cout << "READ FAULT\n";
        if (!temp->resident) evict_and_add(addr_idx, temp->file_backed);
        if (temp->dirty) temp_pte->write_enable = 1;
        temp->resident = 1;
    } else {
        //  cout << "this shouldn't happen\n";
        return -1;
    }

    temp_pte->read_enable = 1;
    temp->referenced = 1;
    return 0;
}

void vm_destroy() {

}

void *vm_map(const char *filename, size_t block){
    // return nullptr if arena is full
    current_process_idx = process_idx_map[current_process];
    int valid_virtual_pages = p[current_process_idx]->virtual_pages.size();
    intptr_t return_address_int = valid_virtual_pages * VM_PAGESIZE + (intptr_t)VM_ARENA_BASEADDR;
    char *return_address_char = (char*)return_address_int;

    if (valid_virtual_pages * VM_PAGESIZE >= VM_ARENA_SIZE) return nullptr; // is full

    if(filename == nullptr) {    //swap backed
        if (num_swap_blocks >= SWAP_BLOCKS) return nullptr;
        virtual_page *temp_vpage = new virtual_page;
        temp_vpage->resident = 1;
        temp_vpage->zero_filled = 1;
        temp_vpage->referenced = 1;
        temp_vpage->file_backed = 0;
        temp_vpage->virtual_page_idx = p[current_process_idx]->page_table_counter;
        temp_vpage->physmem_address = 0;
        p[current_process_idx]->virtual_pages.push_back(temp_vpage);

        page_table_entry_t temp_entry;
        temp_entry.ppage = 0;   //pinned 0 page
        temp_entry.read_enable = 1;
        temp_entry.write_enable = 0;
        p[current_process_idx]->page_table->ptes[p[current_process_idx]->page_table_counter] = temp_entry;
        p[current_process_idx]->page_table_counter++;
    } else {   //file backed
        intptr_t virtual_page_idx = ((intptr_t)filename - (intptr_t)VM_ARENA_BASEADDR)/4096;
        virtual_page * filename_vpage = p[current_process_idx]->virtual_pages[virtual_page_idx];

        page_table_entry_t temp_pte = p[current_process_idx]->page_table->ptes[virtual_page_idx];

        char * real_filename = new char [4096];
        if(!temp_pte.read_enable) vm_fault(filename, 0);

        int filename_ppage = temp_pte.ppage;
        real_filename = ((char*)vm_physmem + 4096 * filename_ppage);

        virtual_page *temp_vpage = new virtual_page;
        temp_vpage->resident = 0;
        temp_vpage->zero_filled = 0;
        temp_vpage->referenced = 0;
        temp_vpage->file_backed = 1;
        temp_vpage->filename = real_filename;
        temp_vpage->block = block;
        temp_vpage->virtual_page_idx = p[current_process_idx]->page_table_counter;
        p[current_process_idx]->virtual_pages.push_back(temp_vpage);

        page_table_entry_t temp_entry;
        temp_entry.read_enable = 0;
        temp_entry.write_enable = 0;
        p[current_process_idx]->page_table->ptes[p[current_process_idx]->page_table_counter] = temp_entry;
        p[current_process_idx]->page_table_counter++;
    }
    return return_address_char;
}
