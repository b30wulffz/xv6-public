#include "types.h"
#include "user.h"
#include "stat.h"
#include "fs.h"

int main(int argc, char *argv[]){
    if(argc < 3){
        printf(1, "Unsufficient arguments supplied\n");
        exit();
    }
    else{
        int new_priority = atoi(argv[1]);
        int pid = atoi(argv[2]);
        if(argv[2][0] == '-'){
            printf(1, "Error, Process id should be positive.\n");
            printf(1, "Priority is not updated.\n");
        }
        else{
            if(new_priority >=0 && new_priority<=100 && argv[1][0] != '-'){
                // printf(1, "--> %d %d\n", new_priority, pid);
                int old_priority = set_priority(new_priority, pid);
                if(old_priority != -1){
                    printf(1, "Priority of pid %d updated.\n", pid);
                    printf(1, "Old priority: %d\n", old_priority);
                }
                else{
                    printf(1, "Error, Process with pid %d does not exist.\n", pid);
                    printf(1, "Priority is not updated.\n");
                }
            }
            else{
                printf(1, "Error, Priority should be a value in the range [0,100].\n");
                printf(1, "Priority is not updated.\n");
            }
        }
        exit();
    }
}