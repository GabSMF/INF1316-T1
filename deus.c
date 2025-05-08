#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <math.h>
#include <time.h>

#define MAX_AVIOES 20
#define VELOCIDADE 0.05
#define TEMPO_ROUND_ROBIN 1
#define DIST_COLISAO 0.1

// Estrutura da aeronave na memoria compartilhada
typedef struct {
    pid_t pid;
    float x, y;
    char origem;
    int pista_pref;
    int ativo;
    int confirm_usr1;
    int confirm_usr2;
} Aviao;

typedef struct {
    Aviao avioes[MAX_AVIOES];
    int n_avioes;
    int escalonamento_ativo;
} EspacoAereo;



int tempo_simulacao = 0; // tempo simulado para controlar eventos
int shmid;
EspacoAereo *espaco;
pid_t filhos[MAX_AVIOES];
int n_processos;
int velocidade_reduzida[MAX_AVIOES] = {0};
int escalonamento_ativo = 0;

void sigusr1_handler(int sig) {
    int idx = -1;
    pid_t pid = getpid();
    for (int i = 0; i < MAX_AVIOES; i++) {
        if (espaco->avioes[i].pid == pid) {
            idx = i;
            break;
        }
    }
    if (idx != -1) {
        velocidade_reduzida[idx] = !velocidade_reduzida[idx];
        espaco->avioes[idx].confirm_usr1 = 1;
        printf("[Aviao %d] Velocidade %s\n", pid, velocidade_reduzida[idx] ? "reduzida" : "normal");
    }
}

void sigusr2_handler(int sig) {
    int idx = -1;
    pid_t pid = getpid();
    for (int i = 0; i < MAX_AVIOES; i++) {
        if (espaco->avioes[i].pid == pid) {
            idx = i;
            break;
        }
    }
    if (idx != -1) {
        int nova_pista;
        if (espaco->avioes[idx].origem == 'W') {
            nova_pista = (espaco->avioes[idx].pista_pref == 3) ? 18 : 3;
        } else {
            nova_pista = (espaco->avioes[idx].pista_pref == 6) ? 27 : 6;
        }
        espaco->avioes[idx].pista_pref = nova_pista;
        espaco->avioes[idx].confirm_usr2 = 1;
        printf("[Aviao %d] Trocou para pista preferencial %d\n", pid, nova_pista);
    }
}



void sigterm_handler(int sig) {
    exit(0);
}

void movimento(int idx) {
    float dx, dy, dist;
    float destino_x = 0.5, destino_y = 0.5;
    while (espaco->avioes[idx].ativo) {
        dx = destino_x - espaco->avioes[idx].x;
        dy = destino_y - espaco->avioes[idx].y;
        dist = sqrt(dx*dx + dy*dy);
        if (dist < VELOCIDADE) {
            espaco->avioes[idx].x = destino_x;
            espaco->avioes[idx].y = destino_y;
            espaco->avioes[idx].ativo = 0;
            printf("[Aviao %d] Pousou em (%.2f, %.2f)\n", getpid(), destino_x, destino_y);
            exit(0);
        } else {
            float v = velocidade_reduzida[idx] ? VELOCIDADE / 2 : VELOCIDADE;
            espaco->avioes[idx].x += v * dx / dist;
            espaco->avioes[idx].y += v * dy / dist;
        }
        sleep(1);
    }
}


void cria_avioes(int n) {
    int pistas_w[2] = {3, 18};
    int pistas_e[2] = {6, 27};

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGUSR1, sigusr1_handler);
            signal(SIGUSR2, sigusr2_handler);
            signal(SIGTERM, sigterm_handler);

            srand(getpid());
            espaco->avioes[i].pid = getpid();
            espaco->avioes[i].origem = (rand() % 2) ? 'W' : 'E';
            espaco->avioes[i].x = (espaco->avioes[i].origem == 'W') ? 0.0 : 1.0;
            espaco->avioes[i].y = (float)(rand() % 100) / 100.0;
            if (espaco->avioes[i].origem == 'W') {
                espaco->avioes[i].pista_pref = pistas_w[rand() % 2];
            } else {
                espaco->avioes[i].pista_pref = pistas_e[rand() % 2];
            }
            espaco->avioes[i].ativo = 1;
            sleep(rand() % 3);
            movimento(i);
            exit(0);
        } else {
            filhos[i] = pid;
        }
    }
    espaco->n_avioes = n;
}

void exibir_status() {
    printf("\n[Status Atual das Aeronaves]\n");
    printf("%-6s %-6s %-6s %-6s %-6s %-6s\n", "PID", "X", "Y", "Origem", "Pista", "Ativo");
    for (int i = 0; i < espaco->n_avioes; i++) {
        printf("%-6d %-6.2f %-6.2f %-6c %-6d %-6s\n",
            espaco->avioes[i].pid,
            espaco->avioes[i].x,
            espaco->avioes[i].y,
            espaco->avioes[i].origem,
            espaco->avioes[i].pista_pref,
            espaco->avioes[i].ativo ? "Sim" : "Nao");
    }
    printf("----------------------------------------\n");
}



void checa_colisoes() {
    if (tempo_simulacao < 5) return; // adia verificação de colisão para permitir deslocamento inicial

    for (int i = 0; i < espaco->n_avioes; i++) {
        if (!espaco->avioes[i].ativo) continue;
        for (int j = i + 1; j < espaco->n_avioes; j++) {
            if (!espaco->avioes[j].ativo) continue;
            if (espaco->avioes[i].origem != espaco->avioes[j].origem) continue;

            float dx = fabs(espaco->avioes[i].x - espaco->avioes[j].x);
            float dy = fabs(espaco->avioes[i].y - espaco->avioes[j].y);

            if (dx < 0.1 && dy < 0.1) {
                int escolhido = (rand() % 2 == 0) ? i : j;
                printf("[Controlador] Colisão iminente entre %d e %d. Abortando %d...\n",
                    espaco->avioes[i].pid, espaco->avioes[j].pid, espaco->avioes[escolhido].pid);
                kill(espaco->avioes[escolhido].pid, SIGKILL);
                espaco->avioes[escolhido].ativo = 0;
                continue;
            }

            if (dx < 0.15 && dy < 0.15) {
                int alvo = (rand() % 2 == 0) ? i : j;
                kill(espaco->avioes[alvo].pid, SIGCONT);
                usleep(100000);
                espaco->avioes[alvo].confirm_usr1 = 0;
                kill(espaco->avioes[alvo].pid, SIGUSR1);
                sleep(1);
                if (espaco->avioes[alvo].confirm_usr1) {
                    printf("[Controlador] Aviao %d confirmou redução de velocidade.\n", espaco->avioes[alvo].pid);
                } else {
                    printf("[Controlador] Aviao %d não respondeu a SIGUSR1.\n", espaco->avioes[alvo].pid);
                }
                continue;
            }

            if (espaco->avioes[i].pista_pref == espaco->avioes[j].pista_pref) {
                kill(espaco->avioes[i].pid, SIGCONT);
                usleep(100000);
                espaco->avioes[i].confirm_usr2 = 0;
                kill(espaco->avioes[i].pid, SIGUSR2);
                sleep(1);
                if (espaco->avioes[i].confirm_usr2) {
                    printf("[Controlador] Aviao %d confirmou troca de pista.\n", espaco->avioes[i].pid);
                } else {
                    printf("[Controlador] Aviao %d não respondeu a SIGUSR2.\n", espaco->avioes[i].pid);
                }
            }
        }
    }
}

void menu_interativo() {
    char opcao;
    int pid;
    printf("\n[Menu de Controle - Comandos Disponíveis]\n");
    printf("i: Iniciar todos os aviões\n");
    printf("p: Pausar todos os aviões\n");
    printf("r: Retomar todos os aviões\n");
    printf("f: Finalizar simulação\n");
    printf("u: Reduzir velocidade de um avião\n");
    printf("t: Trocar pista de um avião\n");
    printf("s: Exibir status das aeronaves\n");
    printf("Digite uma opção: ");

    while (scanf(" %c", &opcao)) {
        switch (opcao) {
            case 'i':
                espaco->escalonamento_ativo = 1;
                break;
            case 'p':
                espaco->escalonamento_ativo = 0;
                for (int i = 0; i < espaco->n_avioes; i++) {
                    if (espaco->avioes[i].ativo) kill(espaco->avioes[i].pid, SIGSTOP);
                }
                break;
            case 'r':
                espaco->escalonamento_ativo = 1;
                break;
            case 'f':
                printf("Finalizando simulação...\n");
                for (int i = 0; i < espaco->n_avioes; i++) {
                    if (espaco->avioes[i].ativo) kill(espaco->avioes[i].pid, SIGKILL);
                }
                exit(0);
            case 'u':
                printf("Digite PID do avião para SIGUSR1: ");
                scanf("%d", &pid);
                kill(pid, SIGUSR1);
                break;
            case 't':
                printf("Digite PID do avião para SIGUSR2: ");
                scanf("%d", &pid);
                kill(pid, SIGUSR2);
                break;
            case 's':
                exibir_status();
                break;
            default:
                printf("Opção inválida.\n");
        }
        printf("\nDigite uma opção: ");
    }
}




void escalonador() {
    int atual = 0;
    while (1) {
        if (!espaco->escalonamento_ativo) {
            sleep(1);
            continue;
        }

        tempo_simulacao++;
        checa_colisoes();
        exibir_status();

        int ativos = 0;
        for (int i = 0; i < espaco->n_avioes; i++) {
            if (espaco->avioes[i].ativo) ativos++;
        }
        if (ativos == 0) {
            printf("\n[Controlador] Todos os aviões pousaram ou foram abortados. Encerrando simulação.\n");
            break;
        }

        if (espaco->avioes[atual].ativo) {
            kill(espaco->avioes[atual].pid, SIGCONT);
            sleep(TEMPO_ROUND_ROBIN);
            kill(espaco->avioes[atual].pid, SIGSTOP);
        }
        atual = (atual + 1) % espaco->n_avioes;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <numero_de_avioes>\n", argv[0]);
        exit(1);
    }

    n_processos = atoi(argv[1]);
    if (n_processos > MAX_AVIOES) n_processos = MAX_AVIOES;

    shmid = shmget(IPC_PRIVATE, sizeof(EspacoAereo), IPC_CREAT | 0666);
    espaco = (EspacoAereo *)shmat(shmid, NULL, 0);

    cria_avioes(n_processos);
    sleep(1);
    for (int i = 0; i < n_processos; i++) kill(filhos[i], SIGSTOP);

    // Executa escalonador em paralelo com menu (sem threads)
    if (fork() == 0) {
        escalonador();
        exit(0);
    }

    menu_interativo();
    return 0;
}
