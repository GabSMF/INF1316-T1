// Compilar com: gcc -o pra_valer pra_valer.c -lm

/*---- Includes & Definitions----*/
#include <stdio.h>      // Entrada e saída padrão
#include <stdlib.h>     // Funções utilitárias (rand, exit, etc)
#include <unistd.h>     // Funções POSIX (fork, sleep, etc)
#include <signal.h>     // Manipulação de sinais (SIGINT, SIGSTOP, etc)
#include <sys/wait.h>   // Espera por processos filhos
#include <sys/time.h>   // Manipulação de tempo
#include <sys/shm.h>    // Memória compartilhada
#include <string.h>     // Manipulação de strings/memória
#include <sys/select.h> // Multiplexação de I/O
#include <time.h>       // Funções de tempo
#include <math.h>       // Funções matemáticas (sqrt, pow)
#include <sys/sem.h>    // Semáforos
#include <sys/stat.h>   // Permissões de arquivos

// Definições de constantes
#define MAX_PLANES 20    // Número máximo de aviões simultâneos
#define SPEED 0.05      // Velocidade de deslocamento dos aviões
#define CRIT_DIST 0.1   // Distância crítica para risco de colisão

// Definições das pistas
#define RUNWAY_E_03 3
#define RUNWAY_E_06 6
#define RUNWAY_W_18 18
#define RUNWAY_W_27 27

// Estrutura que representa um avião
typedef struct 
{
    pid_t pid;      // PID do processo do avião
    char side;      // Lado de entrada ('E' ou 'W')
    int runway;     // Pista preferida para pouso
    int delay;      // Tempo de atraso inicial (segundos)
    float x, y;     // Coordenadas do avião
    int status;     // 1=voando, 2=pousou, 3=morto, 4=em espera
    time_t time;    // Momento de entrada na fila
} Plane;

// Fila de espera (apenas PIDs)
typedef struct {
    pid_t pids[MAX_PLANES];
    int first;
    int last;
    int size;
} WaitQueue;

// Fila de aviões (com structs Plane)
typedef struct {
    Plane queue[MAX_PLANES];
    int first;
    int last;
    int size;
} PlanesQueue;

// Estrutura de dados compartilhados entre processos
typedef struct {
    PlanesQueue planesQueue; // Fila de aviões ativos
    WaitQueue waitQueue;     // Fila de espera
    time_t initTime;         // Tempo de início da simulação
} SharedData;

/*---- Variáveis globais ----*/
char entrySide[2] = {'E','W'};                  // Lados possíveis de entrada
int RunwaysEast[2] = {RUNWAY_E_03, RUNWAY_E_06}; // Pistas do lado leste
int RunwaysWest[2] = {RUNWAY_W_18, RUNWAY_W_27}; // Pistas do lado oeste
pid_t planes[MAX_PLANES];                       // PIDs dos aviões
int currentPlane = 0;                           // Índice do avião atual
int shmid;                                      // ID da memória compartilhada
int semid;                                      // ID do semáforo
time_t currentTime;                             // Tempo atual
int execTime[MAX_PLANES] = {3, 4, 5, 6, 7 ,8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};   // Tempos de execução (não usado diretamente)
int pIndex;                                     // Índice auxiliar

/*---- Funções auxiliares para criação de aeronaves ----*/

// Sorteia lado de entrada ('E' ou 'W')
char new_entry() {
    char entry = entrySide[rand() % 2];
    return entry;
}

// Sorteia coordenada Y (entre 0 e 1)
float new_coord_Y() {
    double coord = (double)rand() / RAND_MAX;
    return coord;
}

// Define coordenada X inicial conforme lado de entrada
float new_coord_X(char side) {
    if (side == 'E'){
        return 0;   // Leste começa em x=0
    } else {
        return 1;   // Oeste começa em x=1
    }
}

// Sorteia atraso inicial (0 a 2 segundos)
int new_delay_time() {
    int delay = rand() % 3;
    return delay;
}

// Sorteia pista preferida conforme lado de entrada
int new_runway(char side) {
    int runway;
    if(side == 'E'){
        runway = RunwaysEast[rand() % 2];
    } else {
        runway = RunwaysWest[rand() % 2];
    }
    return runway;
}

// Inicializa fila de espera
void initialize_wait_queue(WaitQueue* queue) {
    queue->first = 0;
    queue->last = -1;
    queue->size = 0;
    memset(queue->pids, 0, sizeof(queue->pids));
}

// Enfileira PID na fila de espera
void enqueue_wait(WaitQueue* queue, pid_t pid) {
    if (queue->size < MAX_PLANES) {
        queue->last = (queue->last + 1) % MAX_PLANES;
        queue->pids[queue->last] = pid;
        queue->size++;
    }
}

// Retira PID da fila de espera
pid_t dequeue_wait(WaitQueue* queue) {
    if (queue->size == 0) return -1;
    pid_t pid = queue->pids[queue->first];
    queue->first = (queue->first + 1) % MAX_PLANES;
    queue->size--;
    return pid;
}

// Verifica se PID está na fila de espera
int is_in_wait_queue(WaitQueue* queue, pid_t pid) {
    for (int i = 0; i < queue->size; i++) {
        int idx = (queue->first + i) % MAX_PLANES;
        if (queue->pids[idx] == pid) return 1;
    }
    return 0;
}

// Inicializa fila de aviões
void initialize_queue(PlanesQueue* queue) {
    queue->first = 0;
    queue->last = -1;
    queue->size = 0;
    memset(queue->queue, 0, sizeof(queue->queue));
}

// Enfileira avião na fila de aviões (com proteção de semáforo)
void queue_planes(PlanesQueue* queue, pid_t pid, Plane plane) {
    struct sembuf operacao = {0, -1, 0};
    semop(semid, &operacao, 1); // Bloqueia o semáforo
    if (queue->size < MAX_PLANES) {
        queue->last = (queue->last + 1) % MAX_PLANES;
        plane.time = time(NULL); // Marca o tempo de entrada
        queue->queue[queue->last] = plane;
        queue->size++;
    } else {
        printf("Fila cheia, não foi possível enfileirar o avião %d\n", plane.pid);
    }
    operacao.sem_op = 1;
    semop(semid, &operacao, 1); // Libera o semáforo
}

// Remove avião da fila de aviões e retorna por referência
int dequeue_planes(PlanesQueue* queue, Plane* plane) {
    if(queue->size == 0){
        printf("Fila vazia, não foi possível desenfileirar o avião\n");
        return 0;
    }
    *plane = queue->queue[queue->first];
    queue->first = (queue->first + 1) % MAX_PLANES;
    queue->size--;
    return 1;
}

// Busca índice do avião pelo PID na fila de aviões
int search_plane(PlanesQueue* queue, pid_t pid) {
    for(int i = 0; i < queue->size; i++){
        int pIndex = (queue->first + i) % MAX_PLANES;
        if(queue->queue[pIndex].pid == pid){
            return pIndex;
        }
    }
    return -1; // Não encontrado
}

// Mostra a fila de aviões na tela
void show_queue(PlanesQueue* queue, time_t initTime) {
    printf("===================================\n");
    printf("Fila de aviões (Tempo atual: %lds):\n", time(NULL) - initTime);
    if (queue->size == 0) {
        printf("Fila vazia\n");
        exit(0);
    } else {
        for (int i = 0; i < queue->size; i++) {
            int exibitIndex = (queue->first + i) % MAX_PLANES;
            printf("Avião %d: Processo PID: %d | Tempo na fila: %lds - Posição (%.2f, %.2f)\n",
                   exibitIndex, queue->queue[exibitIndex].pid,
                   time(NULL) - queue->queue[exibitIndex].time, queue->queue[exibitIndex].x , queue->queue[exibitIndex].y);
        }
    }
    printf("===================================\n");
}

// Busca índice do avião pelo PID no vetor global planes[]
int get_index(pid_t pid) {
    for(int i = 0; i < MAX_PLANES; i++){
        if(planes[i] == pid){
            return i;
        }
    }
    return -1;
}

// Retorna o próximo avião (PID) na lista circular, que não está na fila de aviões
pid_t get_next_plane(PlanesQueue* queue, pid_t pid) {
    int currentIndex = get_index(pid);
    int nextIndex;
    for(int i = 1; i<=MAX_PLANES; i++){
        nextIndex = (currentIndex + i) % MAX_PLANES;
        pid_t candidatePid = planes[nextIndex];
        if (!search_plane(queue, candidatePid)){
            return candidatePid;
        }
    }
    return -1;
}

// Cria um novo avião com dados aleatórios
Plane new_plane() {
    Plane A;
    A.delay = new_delay_time();
    A.side = new_entry();
    A.runway = new_runway(A.side);
    A.y = new_coord_Y();
    A.x = new_coord_X(A.side);
    A.status = 1; // Começa voando
    return A;
}

// Calcula a distância entre dois aviões
float calcula_dist(Plane a, Plane b) {
    return sqrt(pow(a.x - b.x, 2) + pow(a.y - b.y, 2));
}

// Libera recursos e encerra o programa
void encerra_prog(SharedData* sharedData){
    shmdt(sharedData); // Desanexa memória compartilhada
    shmctl(shmid, IPC_RMID, NULL); // Remove memória compartilhada
    semctl(semid, 0, IPC_RMID);    // Remove semáforo
    exit(1);
}

// Handler para SIGINT (Ctrl+C)
void signal_handler(int sig){
    encerra_prog(NULL);
}

// Decide qual avião deve ser travado (o de maior PID)
int quem_trava(Plane a, Plane b) {
    return (a.pid > b.pid) ? a.pid : b.pid;
}

// Remove avião da fila de aviões pelo PID
void remove_plane_by_pid(PlanesQueue *queue, pid_t pid) {
    int found = -1;
    for (int i = 0; i < queue->size; i++) {
        int idx = (queue->first + i) % MAX_PLANES;
        if (queue->queue[idx].pid == pid) {
            found = idx;
            break;
        }
    }
    if (found != -1) {
        // Desloca elementos para preencher o buraco
        for (int i = found; i != queue->last; i = (i + 1) % MAX_PLANES) {
            int next = (i + 1) % MAX_PLANES;
            queue->queue[i] = queue->queue[next];
        }
        queue->last = (queue->last - 1 + MAX_PLANES) % MAX_PLANES;
        queue->size--;
    }
}

/*------------------- Função principal -------------------*/
int main(void)
{
    signal(SIGINT, signal_handler); // Captura Ctrl+C para liberar recursos


    // Cria memória compartilhada
    shmid = shmget(IPC_PRIVATE, sizeof(SharedData), IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shmid < 0) {
        exit(1);
    }
    SharedData* sharedData = (SharedData*)shmat(shmid, NULL, 0);
    if (sharedData == (SharedData*)-1) {
        encerra_prog(sharedData);
    }

    // Cria semáforo para sincronização
    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("Erro ao criar semáforo");
        exit(1);
    }
    // Inicializa semáforo com valor 1 (mutex)
    if (semctl(semid, 0, SETVAL, 1) < 0) {
        perror("Erro ao inicializar semáforo");
        exit(1);
    }

    // Inicializa filas na memória compartilhada
    initialize_queue(&sharedData->planesQueue);
    initialize_wait_queue(&sharedData->waitQueue);
    sharedData->initTime = time(NULL);

    // Cria processos filhos (um para cada avião)
    for (int i = 0; i < MAX_PLANES; i++) {
        pid_t pid = fork();
        if (pid < 0){
            perror("Erro fork");
            exit(1);
        }

        if (pid == 0) {
            // Processo filho (avião)
            srand(time(NULL) ^ getpid()); //Gera a aleatoriedade
            Plane plane = new_plane();      // Cria avião com dados aleatórios
            plane.pid = getpid();           // Define PID do processo
            plane.time = time(NULL);        // Marca tempo de entrada

            queue_planes(&sharedData->planesQueue, plane.pid, plane); // Enfileira avião

            // Loop principal do avião
            while (1) {
                sleep(1); // Espera 1 segundo (simula movimento)

                // Atualiza status do avião a partir da memória compartilhada
                struct sembuf operacao = {0, -1, 0};
                semop(semid, &operacao, 1);
                int pIndex = search_plane(&sharedData->planesQueue, plane.pid);
                if (pIndex >= 0) {
                    plane.status = sharedData->planesQueue.queue[pIndex].status;
                }
                operacao.sem_op = 1;
                semop(semid, &operacao, 1);

                if (plane.status != 1) continue; // Só voa se status==1

                // Atualiza coordenada X conforme lado de entrada
                if (plane.side == 'E') {
                    plane.x += SPEED;
                } else {
                    plane.x -= SPEED;
                }

                // Verifica se chegou ao destino (pousou)
                if ((plane.side == 'E' && plane.x >= 1.0) || (plane.side == 'W' && plane.x <= 0.0)) {
                    plane.status = 2; // pousou
                    printf("Avião %d pousou na pista %d\n", plane.pid, plane.runway);

                    struct sembuf operacao = {0, -1, 0};
                    semop(semid, &operacao, 1);
                    remove_plane_by_pid(&sharedData->planesQueue, plane.pid); // Remove da fila
                    pid_t proximo_pid = dequeue_wait(&sharedData->waitQueue); // Libera próximo da fila de espera
                    if (proximo_pid != -1) {
                        kill(proximo_pid, SIGCONT); // Continua próximo avião
                    }
                    operacao.sem_op = 1;
                    semop(semid, &operacao, 1);
                    break; // Sai do loop (avião terminou)
                }

                // Atualiza dados do avião na memória compartilhada
                operacao.sem_op = -1;
                semop(semid, &operacao, 1);
                pIndex = search_plane(&sharedData->planesQueue, plane.pid);
                if (pIndex >= 0) {
                    sharedData->planesQueue.queue[pIndex] = plane;
                }
                operacao.sem_op = 1;
                semop(semid, &operacao, 1);

                // Verifica colisão com outros aviões
                for (int j = 0; j < sharedData->planesQueue.size; j++) {
                    int idx = (sharedData->planesQueue.first + j) % MAX_PLANES;
                    Plane outro = sharedData->planesQueue.queue[idx];
                    if (outro.pid != plane.pid && outro.status == 1) {
                        float dist = calcula_dist(plane, outro);
                        if (dist < CRIT_DIST) {
                            int trava_pid = quem_trava(plane, outro);
                            if (plane.pid == trava_pid && plane.status != 4) {
                                printf("Avião %d travado por risco de colisão com %d\n", plane.pid, outro.pid);
                                plane.status = 4; // em espera

                                struct sembuf operacao = {0, -1, 0};
                                semop(semid, &operacao, 1);
                                int pIndex = search_plane(&sharedData->planesQueue, plane.pid);
                                if (pIndex >= 0) {
                                    sharedData->planesQueue.queue[pIndex] = plane;
                                }
                                // Enfileira na fila de espera se ainda não estiver lá
                                if (!is_in_wait_queue(&sharedData->waitQueue, plane.pid)) {
                                    enqueue_wait(&sharedData->waitQueue, plane.pid);
                                }
                                operacao.sem_op = 1;
                                semop(semid, &operacao, 1);

                                kill(plane.pid, SIGSTOP); // Para o processo até ser liberado
                            }
                        }
                    }
                }
            }
            exit(0); // Avião terminou
        } else {
            // Processo pai: armazena PID do filho
            printf("Pai - Filho Criado PID = %d (i = %d)\n", pid, i);
            planes[i] = pid;
        }
    }

    // Libera todos os aviões para começarem a voar
    for (int i = 0; i < MAX_PLANES; i++) {
        if (planes[i] > 0) {
            kill(planes[i], SIGCONT);
        }
    }

    // Loop principal do processo pai: monitora e gerencia aviões
    while (1) {
        struct sembuf operacao = {0, -1, 0};
        semop(semid, &operacao, 1);

        // Percorre todos os aviões na fila
        for (int i = 0; i < sharedData->planesQueue.size; i++) {
            int idx = (sharedData->planesQueue.first + i) % MAX_PLANES;
            Plane *p1 = &sharedData->planesQueue.queue[idx];
            if (p1->status == 4) { // Avião em espera
                int pode_liberar = 1;
                // Verifica se ainda há risco de colisão
                for (int j = 0; j < sharedData->planesQueue.size; j++) {
                    if (i == j) continue;
                    int idx2 = (sharedData->planesQueue.first + j) % MAX_PLANES;
                    Plane *p2 = &sharedData->planesQueue.queue[idx2];
                    if (p2->status == 1) { // Avião voando
                        float dist = calcula_dist(*p1, *p2);
                        if (dist < CRIT_DIST) {
                            pode_liberar = 0;
                            break;
                        }
                    }
                }
                if (pode_liberar) {
                    printf("Liberando avião %d\n", p1->pid);
                    kill(p1->pid, SIGCONT); // Libera avião
                    p1->status = 1; // Atualiza status para voando
                }
            }
        }

        operacao.sem_op = 1;
        semop(semid, &operacao, 1);
        show_queue(&sharedData->planesQueue, sharedData->initTime); // Mostra fila

        sleep(5); // Espera 5 segundos antes de repetir
    }

    // Finaliza programa (nunca chega aqui, pois o loop é infinito)
    encerra_prog(sharedData);
}