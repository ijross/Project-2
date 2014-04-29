#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#define MAX_BUF_SIZE 2097152
#define BLOCK_SIZE 1024

int main(int argc, char **argv){
   FILE *fd = fopen("iotest", "rb");
   int checksum = 0;
   int index = 0;
   int i = 0;
   unsigned int buf[MAX_BUF_SIZE];
   while(fread(buf,1,1024,fd) > 0){ 
      for(i = 0; i < BLOCK_SIZE; ++i)
         checksum ^= buf[index + i];
      index += 1024;   
   }
   
   printf("Checksum for process %d: %d\n", getpid(),checksum);
   for(i = 0; i < 1024; i = i + 1024){
      fwrite(buf + i, 1, BLOCK_SIZE, fd);
   }
   fclose(fd);
   return 0;
}
