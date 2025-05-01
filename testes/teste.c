#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/shm.h>  
#include <sys/sem.h>  

#define MAX 10
#define NUM_PROCESSOS 3 // Define a qtd maxima de processos
#define EVER ;;
#define MAX_PROCESSOS 6  // Define o tamanho maximo da fila

// Estrutura que representa um no na fila
typedef struct Processo {
    pid_t pid;
    time_t time;
} Processo;

typedef struct FilaProcessos {
    Processo fila[MAX_PROCESSOS];
    int primeiro;
    int ultimo;
    int tamanho;
} FilaProcessos;

// Estrutura para compartilhar dados
typedef struct DadosComp {
    FilaProcessos filaProcessos;
    time_t tempoInicial;
} DadosComp;

void inicializarFila(FilaProcessos* fila) {
    fila->primeiro = 0;
    fila->ultimo = -1;
    fila->tamanho = 0;
}

pid_t processos[NUM_PROCESSOS];  // Vetor para armazenar os pids dos processos
int processoAtual = 0;  // variavel para controlar qual processo esta sendo executado
int shmid;
int semid;
time_t tempoAtual;
int tempo_execucao[MAX_PROCESSOS] = {3, 5, 6}; 
int indice;

void enfileirarProcesso(FilaProcessos* fila, pid_t pid) {
    struct sembuf operacao = {0, -1, 0};
    semop(semid, &operacao, 1);

    if (fila->tamanho < MAX_PROCESSOS) {
        fila->ultimo = (fila->ultimo + 1) % MAX_PROCESSOS;
        fila->fila[fila->ultimo].pid = pid;
        fila->fila[fila->ultimo].time = time(NULL);
        fila->tamanho++;
        printf("[Tempo %lds] Processo %d enfileirado.\n", time(NULL) - ((DadosComp*)shmat(shmid, NULL, 0))->tempoInicial, pid);
    } else {
        printf("Fila cheia, nao foi possivel enfileirar o processo.\n");
    }

    operacao.sem_op = 1;
    semop(semid, &operacao, 1);
    kill(pid, SIGSTOP);
}

int desenfileirarProcesso(FilaProcessos* fila, Processo* processo) {
    if (fila->tamanho == 0) {
        printf("Fila vazia\n");
        return 0; 
    }

    // Copiar os dados do processo para o ponteiro 
    *processo = fila->fila[fila->primeiro];
    fila->primeiro = (fila->primeiro + 1) % MAX_PROCESSOS;
    fila->tamanho--;
    return 1;  
}

int buscaProcesso(FilaProcessos* fila, pid_t pid) {
    for (int i = 0; i < fila->tamanho; i++) {
        int indiceBusca = (fila->primeiro + i) % MAX_PROCESSOS;
        if (fila->fila[indiceBusca].pid == pid) {
            return 1;
        }
    }
    return 0;
}

void exibirFila(FilaProcessos* fila, time_t tempoInicial) {
    printf("===================================\n");
    printf("Fila de processos bloqueados (Tempo atual: %lds):\n", time(NULL) - tempoInicial);
    if (fila->tamanho == 0) {
        printf("Fila vazia\n");
    } else {
        for (int i = 0; i < fila->tamanho; i++) {
            int indiceExibicao = (fila->primeiro + i) % MAX_PROCESSOS;
            printf("Posicao %d: Processo PID: %d | Tempo na fila: %lds\n",
                   indiceExibicao,
                   fila->fila[indiceExibicao].pid,
                   time(NULL) - fila->fila[indiceExibicao].time);
        }
    }
    printf("===================================\n");
}

int obterIndice(pid_t processo) {
    for (int i = 0; i < NUM_PROCESSOS; i++) {
        if (processos[i] == processo) {
            return i;
        }
    }
    return -1;
}

pid_t getNextProcesso(FilaProcessos* fila, pid_t pid) {
    int currentIndex = obterIndice(pid);
    int nextIndex;

    for (int i = 1; i <= NUM_PROCESSOS; i++) {
        nextIndex = (currentIndex + i) % NUM_PROCESSOS;
        pid_t candidatePid = processos[nextIndex];

        // verifica se o processo candidato esta na fila(bloqueado)
        if (!buscaProcesso(fila, candidatePid)) {
            return candidatePid;
        }
    }
    return -1;
}

void tratarIRQ0(FilaProcessos* fila) {
    int bloqueado = buscaProcesso(fila, processos[processoAtual]);
    if (bloqueado) {
        pid_t proximoP = getNextProcesso(fila, processos[processoAtual]);
        if (proximoP == -1) {
            return;
        } else {
            kill(processos[processoAtual], SIGSTOP);
            kill(proximoP, SIGCONT);
            processoAtual = obterIndice(proximoP);
        }
    } else {
        pid_t proximoP = getNextProcesso(fila, processos[processoAtual]);
        if (proximoP == -1 || proximoP == processos[processoAtual]) {
            return;
        } else {
            kill(processos[processoAtual], SIGSTOP);
            kill(proximoP, SIGCONT);
            processoAtual = obterIndice(proximoP);
        }
    }
}

void trataSinal(int signal) {
    DadosComp* dadosComp = (DadosComp*)shmat(shmid, NULL, 0);
    if (dadosComp == (DadosComp*)-1) {
        exit(1);
        return;
    }

    tratarIRQ0(&dadosComp->filaProcessos);
    shmdt(dadosComp);
}

void IRQ1(int signal) {
    DadosComp* dadosComp = (DadosComp*)shmat(shmid, NULL, 0); 
    if (dadosComp == (DadosComp*)-1) {
        exit(1);
        return;
    }
    tempoAtual = time(NULL);
    Processo processo;
    int res = desenfileirarProcesso(&dadosComp->filaProcessos, &processo);
    if (res) {
        printf("[Tempo %lds] -- Processo %d passou 3 segundos na fila. Retomando...\n", tempoAtual - dadosComp->tempoInicial, processo.pid);
        kill(processos[processoAtual], SIGSTOP);
        kill(processo.pid, SIGCONT);
        processoAtual = obterIndice(processo.pid);
    }
    shmdt(dadosComp); 
}

int main() {
    shmid = shmget(IPC_PRIVATE, sizeof(DadosComp), IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shmid < 0) {
        exit(1);
    }
    DadosComp* dadosComp = (DadosComp*)shmat(shmid, NULL, 0);
    if (dadosComp == (DadosComp*)-1) {
        exit(1);
    }

    inicializarFila(&dadosComp->filaProcessos);
    dadosComp->tempoInicial = time(NULL);

    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        exit(1);
    }
    semctl(semid, 0, SETVAL, 1);  

    // Processo Kernel
    pid_t kernel;
    if ((kernel = fork()) == 0) {
        dadosComp = (DadosComp*)shmat(shmid, NULL, 0);  
        if (dadosComp == (DadosComp*)-1) {
            exit(1);
        }

        // Criando N processos
        for (int i = 0; i < NUM_PROCESSOS; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                exit(1);
            } else if (pid == 0) {
                int pc = 0;
                while (pc < tempo_execucao[i]) {
                    printf("[Tempo %lds] Processo %d - pc: %d\n", time(NULL) - dadosComp->tempoInicial, getpid(), pc);
                    sleep(1);
                    if (pc % 3 == 0 && pc != 0) {
                        enfileirarProcesso(&dadosComp->filaProcessos, getpid());
                    }
                    pc++;
                }
                printf("[Tempo %lds] [FIM] PROCESSO %d TERMINADO., PC= %d\n", time(NULL) - dadosComp->tempoInicial, getpid(), pc);
                exit(0);
            } else {  
                processos[i] = pid;  
                kill(pid, SIGSTOP);  // Para os processos
            }
        }

        // Iniciar o primeiro processo (primeiro processo criado)
        processoAtual = 0;
        kill(processos[processoAtual], SIGCONT);

        signal(SIGALRM, trataSinal);
        signal(SIGUSR1, IRQ1);

        for (EVER);
    }

    // Processo do InterControler
    pid_t interControler;
    if ((interControler = fork()) == 0) {
        dadosComp = (DadosComp*)shmat(shmid, NULL, 0);  
        if (dadosComp == (DadosComp*)-1) {
            exit(1);
        }
        sleep(1);
        while (1) {
            kill(kernel, SIGALRM);
            struct sembuf operacao = {0, -1, 0};
            semop(semid, &operacao, 1);  

            // Verificar se ha um processo na fila e se ja se passaram 3 segundos desde que foi enfileirado
            if (dadosComp->filaProcessos.tamanho > 0) {
                indice = dadosComp->filaProcessos.primeiro;
                tempoAtual = time(NULL);
                double diff = difftime(tempoAtual, dadosComp->filaProcessos.fila[indice].time);
                exibirFila(&dadosComp->filaProcessos, dadosComp->tempoInicial);
                if (diff >= 3.0) {  // Se ja se passaram 3 segundos (tempo no i/o)
                    kill(kernel,SIGUSR1);
                }
            }
            operacao.sem_op = 1;
            semop(semid, &operacao, 1);  
            sleep(1);
        }
    }

    wait(NULL); 
    wait(NULL);

    shmdt(dadosComp);
    shmctl(shmid, IPC_RMID, NULL);

    semctl(semid, 0, IPC_RMID);

    return 0;
}
