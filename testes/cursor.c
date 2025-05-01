#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <time.h>

#define MAX_AERONAVES 20

// Estrutura para status de cada aeronave na memória compartilhada
typedef struct {
    pid_t pid;
    float x, y;
    float velocidade;
    int pista_preferida;
    int pista_atual;
    int status; // 0=voando, 1=pousando, 2=aguardando, 3=remetendo
} aeronave_t;

// Estrutura para o espaço aéreo compartilhado
typedef struct {
    aeronave_t aeronaves[MAX_AERONAVES];
    int n_aeronaves;
} espaco_aereo_t;

void controlador(int n_aeronaves);
void aeronave(int idx, int shmid);

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Uso: %s <numero_de_aeronaves>\n", argv[0]);
        exit(1);
    }
    int n = atoi(argv[1]);
    if (n < 1 || n > MAX_AERONAVES) {
        printf("Número de aeronaves deve ser entre 1 e %d\n", MAX_AERONAVES);
        exit(1);
    }
    controlador(n);
    return 0;
}