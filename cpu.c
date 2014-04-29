#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

int i = 0;
double x = 1;
int main (int argc, char **argv){
   int time = 350000;
   if(argc == 2) time = atoi(argv[1]) * 35000;

   while (i < time){
      x = sin(((sin(x) * cos(x) + 1)/tan(x))*(3.14/atan(x)));
      if((i % 35000) == 0){
         printf("Running process: %d\n", getpid());
         printf("Total percentage done: %0.2d/%0.2d\n\n", i,time);
      }
      i++;
   }
   return 0;
}
