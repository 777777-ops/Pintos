#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "stdbool.h"
#include <stdio.h>
#include "filesys/file.h"

/* 辅助页的类型不同的类型有不同的页替换机制，
    SWAP写回磁盘中的交换分区 
    FILE由于是只读文件，数据本就在文件中，直接清零即可，无需写回 
    ZERO本身就不存在数据，无需写回 
    MMAP指明了需要写回的文件标识符，偏移量等，需要写回到文件中   */

/*     进程执行中最频繁触发的是ZERO，由于无论是栈的分配、还是可执行
    代码的访存，都实现了按需分页————即不直接在内存中分配物理页帧，而
    是在辅助页中保存信息，交给CPU中MMU单元以及页错误处理流程处理。
    页错误处理会根据相关辅助页中的保存的信息，选择对应的加载机制，这里
    不详细展开，在page_fault()中会详细说明。
    */
enum swap_type{
    SWAP,
    FILE,
    ZERO,
    MMAP
};

/* 辅助页中flags字段中的位标识 
    uint8_t flags = 0 0 0 0 0 0 0 READ WIRTE  */
enum page_flags{
    WRITE = 0x1,
    READ = 0x2,
};

/* 辅助页：
        区别于Linux中的管理连续区域的VMA(vm_area_struct)，例如code段只用
    一个VMA记录该段的所有信息。本微型os选择颗粒化管理每个虚拟页(4KB)，只要
    进程需要一段空间(例如栈拓展、堆拓展、代码加载)，就要新建空间大小/PGSIZE
    个struct page管理各个虚拟页*/

struct page{
    enum swap_type type;

    struct file *file;
    off_t pos;              /*如果是SWAP页，pos代表分区槽位，
                              如果是FILE文件，pos代表文件偏移量*/
    size_t read_bytes;       /*需要读取的字节数*/

    uint8_t flags;
    void* uaddr;
};

/* 辅助页表，交给pcb实现 */
struct ptable{
    struct page* pages;    /*辅助页数组*/
    size_t len;            /*辅助页个数*/
    size_t pages_size;     /*辅助页数组长度，初始化一页（4KB）*/
};

void pages_reg_mmap(struct process*, struct file* file, off_t pos, size_t read_bytes, void* uaddr, bool lazy);
void pages_reg_exec(struct process*, struct file* file, off_t pos, size_t read_bytes, bool write, void* uaddr, bool lazy);
void pages_reg(struct process* pcb, void* uaddr, enum swap_type type, bool lazy);

struct page *pages_get(struct ptable *pt, size_t avl, void* uaddr);
#endif