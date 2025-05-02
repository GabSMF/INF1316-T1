#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <math.h>

#define MAX_AVIOES 20
#define EVER ;;

typedef struct {
    pid_t pid;
    float x, y;
    float velocidade;
    int pista_preferida;
    int pista_alternativa;
    int reduz_velocidade;
    int em_voo;
    char direcao;
} Aviao;

typedef struct {
    Aviao avioes[MAX_AVIOES];
    int total_avioes;
} EspacoAereo;

int shmid;
EspacoAereo *espaco;
int num_avioes;
pid_t controlador_pid;

void print_status() {
    printf("\nSTATUS DAS AERONAVES:\n");
    printf("%-6s %-5s %-5s %-5s %-4s %-6s %-6s %-4s\n", "PID", "X", "Y", "VEL", "DIR", "PISTA", "ALT", "VOO");
    for (int i = 0; i < num_avioes; i++) {
        Aviao *a = &espaco->avioes[i];
        printf("%-6d %-5.2f %-5.2f %-5.2f %-4c %-6d %-6d %-4d\n",
               a->pid, a->x, a->y, a->velocidade * (a->reduz_velocidade ? 0.5 : 1.0),
               a->direcao, a->pista_preferida, a->pista_alternativa, a->em_voo);
    }
    printf("\n");
}

void atualizar_posicao(int i) {
    if (!espaco->avioes[i].em_voo) return;
    float v = espaco->avioes[i].velocidade * (espaco->avioes[i].reduz_velocidade ? 0.5 : 1.0);
    if (espaco->avioes[i].direcao == 'E') {
        espaco->avioes[i].x -= v;
    } else {
        espaco->avioes[i].x += v;
    }
    if (fabs(espaco->avioes[i].x - 0.5) < 0.01 && fabs(espaco->avioes[i].y - 0.5) < 0.01) {
        printf("[POUSO] Aeronave %d pousou com sucesso.\n", espaco->avioes[i].pid);
        espaco->avioes[i].em_voo = 0;
        exit(0);
    }
}

void sigusr1_handler(int signum) {
    for (int i = 0; i < num_avioes; i++) {
        if (getpid() == espaco->avioes[i].pid) {
            espaco->avioes[i].reduz_velocidade ^= 1;
            printf("[VEL] Aeronave %d mudou velocidade para %s.\n", espaco->avioes[i].pid,
                   espaco->avioes[i].reduz_velocidade ? "reduzida" : "normal");
        }
    }
}

void sigusr2_handler(int signum) {
    for (int i = 0; i < num_avioes; i++) {
        if (getpid() == espaco->avioes[i].pid) {
            int tmp = espaco->avioes[i].pista_preferida;
            espaco->avioes[i].pista_preferida = espaco->avioes[i].pista_alternativa;
            espaco->avioes[i].pista_alternativa = tmp;
            printf("[PISTA] Aeronave %d trocou pista para %d.\n", espaco->avioes[i].pid, espaco->avioes[i].pista_preferida);
        }
    }
}

void criar_aviao(int i) {
    srand(getpid());
    sleep(rand() % 3);
    espaco->avioes[i].pid = getpid();
    espaco->avioes[i].direcao = (rand() % 2) ? 'E' : 'W';
    espaco->avioes[i].x = (espaco->avioes[i].direcao == 'E') ? 1.0 : 0.0;
    espaco->avioes[i].y = ((rand() % 10) / 10.0);
    espaco->avioes[i].velocidade = 0.05;
    espaco->avioes[i].pista_preferida = (espaco->avioes[i].direcao == 'E') ? 6 : 18;
    espaco->avioes[i].pista_alternativa = (espaco->avioes[i].direcao == 'E') ? 27 : 3;
    espaco->avioes[i].reduz_velocidade = 0;
    espaco->avioes[i].em_voo = 1;

    signal(SIGUSR1, sigusr1_handler);
    signal(SIGUSR2, sigusr2_handler);

    while (1) {
        pause();
        atualizar_posicao(i);
        sleep(1);
        kill(controlador_pid, SIGUSR1);
    }
}

void controlador() {
    char buffer[64];
    while (1) {
        printf("Digite 'status' para ver tabela ou 'q' para sair: ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin)) {
            if (strncmp(buffer, "status", 6) == 0) {
                print_status();
            } else if (buffer[0] == 'q') {
                break;
            }
        }

        for (int i = 0; i < num_avioes; i++) {
            if (!espaco->avioes[i].em_voo) continue;

            kill(espaco->avioes[i].pid, SIGCONT);
            sleep(1);
            kill(espaco->avioes[i].pid, SIGSTOP);

            for (int j = 0; j < num_avioes; j++) {
                if (i != j && espaco->avioes[i].em_voo && espaco->avioes[j].em_voo) {
                    float dx = fabs(espaco->avioes[i].x - espaco->avioes[j].x);
                    float dy = fabs(espaco->avioes[i].y - espaco->avioes[j].y);
                    if (dx <= 0.1 && dy <= 0.1) {
                        printf("[COLISAO IMINENTE] %d e %d\n", espaco->avioes[i].pid, espaco->avioes[j].pid);
                        if (!espaco->avioes[i].reduz_velocidade)
                            kill(espaco->avioes[i].pid, SIGUSR1);
                        else {
                            kill(espaco->avioes[i].pid, SIGKILL);
                            espaco->avioes[i].em_voo = 0;
                            printf("[REMOVIDO] Aeronave %d destruida por colisao.\n", espaco->avioes[i].pid);
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Uso: %s <num_avioes (max 20)>\n", argv[0]);
        return 1;
    }
    num_avioes = atoi(argv[1]);
    if (num_avioes > MAX_AVIOES) num_avioes = MAX_AVIOES;

    shmid = shmget(IPC_PRIVATE, sizeof(EspacoAereo), IPC_CREAT | 0666);
    espaco = (EspacoAereo *)shmat(shmid, NULL, 0);
    espaco->total_avioes = num_avioes;

    controlador_pid = getpid();

    for (int i = 0; i < num_avioes; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            criar_aviao(i);
            exit(0);
        } else {
            espaco->avioes[i].pid = pid;
        }
    }

    controlador();

    for (int i = 0; i < num_avioes; i++) wait(NULL);

    shmdt(espaco);
    shmctl(shmid, IPC_RMID, NULL);
    return 0;
}
