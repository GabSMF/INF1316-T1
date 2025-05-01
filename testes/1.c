#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>


#define MAX_PLANES    20     // máximo de aeronaves suportadas


// Estrutura de dados para cada aeronave
typedef struct {
    pid_t pid;      // PID do processo-avião
    char side;      // 'E' ou 'W'
    int runway;     // número da pista
    int dist;       // distância percorrida
    int status;       // 1 se chegou ao TARGET_DIST
} Plane;

// Memória compartilhada
typedef struct {
    int n;                  // quantas aeronaves foram lançadas
    Plane planes[MAX_PLANES];
} SharedMem;

