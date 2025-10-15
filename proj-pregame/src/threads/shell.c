#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/shell.h"
#include "lib/kernel/console.h"
#include "lib/string.h"
#include "devices/input.h"
#include "devices/shutdown.h"



#define SHELL_PROMPT "Pintos $ "
#define MAX_LINE 256

void reset_cmd(char *cmd);
void read_input(char * buf);
bool run_command(char* cmd);

void reset_cmd(char *cmd){
    memset(cmd,0, MAX_LINE * sizeof(char));
    cmd[0] = '\0';
    return;
}

void read_input(char * buf){
    int index = 0;
    int cursor_pos = 0; // 当前光标位置（字符数）
    
    while(true){
        uint8_t c = input_getc();
        
        switch(c){
            case '\r': // 回车键
            case '\n':
                printf("\n");
                buf[index] = '\0'; // 确保字符串以null结尾
                return;

            case '\b': // 退格键
            case 127:  // Delete键
                if (cursor_pos > 0) {
                    // 移动后面的字符向前
                    for (int i = cursor_pos; i < index; i++) {
                        buf[i-1] = buf[i];
                    }
                    index--;
                    cursor_pos--;
                    buf[index] = '\0';
                    
                    // 重新显示从删除位置开始的内容
                    printf("\b"); // 先退格
                    for (int i = cursor_pos; i < index; i++) {
                        printf("%c", buf[i]);
                    }
                    printf(" "); // 清除最后一个字符
                    // 将光标移回正确位置
                    for (int i = index; i >= cursor_pos; i--) {
                        printf("\b");
                    }
                }
                break;

            case 27: // ESC序列（方向键）
                {
                    uint8_t c2 = input_getc();
                    if (c2 == '[') {
                        uint8_t c3 = input_getc();
                        switch(c3){
                            case 'A': // 上键 - 忽略
                            case 'B': // 下键 - 忽略
                                break;
                            case 'C': // 右键
                                if (cursor_pos < index) {
                                    cursor_pos++;
                                    printf("%c", buf[cursor_pos-1]); // 显示下一个字符
                                }
                                break;
                            case 'D': // 左键
                                if (cursor_pos > 0) {
                                    cursor_pos--;
                                    printf("\b"); // 光标左移
                                }
                                break;
                        }
                    }
                }
                break;

            default:
                /* 只接受可打印字符 */
                if (c >= 0x20 && c <= 0x7E && index < MAX_LINE - 1) {
                    // 在光标位置插入字符
                    if (cursor_pos < index) {
                        // 移动后面的字符
                        for (int i = index; i > cursor_pos; i--) {
                            buf[i] = buf[i-1];
                        }
                    }
                    buf[cursor_pos] = c;
                    index++;
                    cursor_pos++;
                    buf[index] = '\0';
                    
                    // 显示新输入的字符和后面的所有字符
                    printf("%c", c);
                    if (cursor_pos < index) {
                        for (int i = cursor_pos; i < index; i++) {
                            printf("%c", buf[i]);
                        }
                        // 将光标移回正确位置
                        for (int i = index; i > cursor_pos; i--) {
                            printf("\b");
                        }
                    }
                }
                break;
        }
    }
}

bool run_command(char* cmd){

    if(!strcmp(cmd,"")){}
    else if(!strcmp(cmd,"whoami"))
        printf("Wang\n");
    else if(!strcmp(cmd,"exit")) {
        return false;
    }else{
        printf("invalid command\n");
    }

    return true;
}


void run_shell(void){
    char* cmd = (char*)malloc(MAX_LINE * sizeof(char));
    reset_cmd(cmd);

    while(true){
        printf(SHELL_PROMPT);
        read_input(cmd);   
        if(!run_command(cmd))
            return;
        reset_cmd(cmd);
    }
    return ;
}