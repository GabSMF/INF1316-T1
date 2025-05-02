
/*---- Includes & Definitions----*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <math.h>

#define MAX_PLANES 20
#define SPEED 0.05
#define LANDING 0.5
#define CRIT_DIST 0.1
#define DIAG_SPEED 0.07


/*---- Pistas ----*/
#define RUNWAY_E_03 3
#define RUNWAY_E_06 6
#define RUNWAY_W_18 18
#define RUNWAY_W_27 27

/*---- Struct ----*/
typedef struct 
{
    pid_t pid;
    char side; // Lado de entrada (E ou W)
    int runway; // Pista preferida para pouso
    int delay; // Tempo de atraso em segundos
    float x, y; // Coordenadas X e Y
    int status;
     // 1 = Voando, 2 = Pousou, 3 = morto
} Plane;

typedef struct PlanesQueue {
    Plane queue[MAX_PLANES];
    int first;
    int last;
    int size;
} PlanesQueue;

typedef struct SharedData {
    PlanesQueue planesQueue;
    time_t initTime;
} SharedData;


/*---- Globals ----*/
char entrySide[2] = {'E','W'};
int runwaysEast[2] = {RUNWAY_E_03, RUNWAY_E_06};
int runwaysWest[2] = {RUNWAY_W_18, RUNWAY_W_27};
pid_t planes[MAX_PLANES];
int currentPlane = 0;
int shmid;
int semid;
time_t currentTime;
int execTime[MAX_PLANES] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
int index;


/*ESSA FUNÇÃO É O PASSO INICIAL PARA TRATAR DA COLISÃO DE AVIÕES. NO ENUNCIADO FALA ""*/

float calcule_time_till_landing(float x, float y){
    float dx = x - 0.5;
    float dy = x - 0.5;

    float dist = sqrt(dx*dx + dy*dy);

    return dist/DIAG_SPEED;
}






/*---- Funções de Criação de Aeronave ----*/
char new_entry()
{
    char entry = entrySide[rand() % 2];
    return entry;
}

float new_coord_Y()
{
    double coord = (double)rand() / RAND_MAX;
    return coord;
}

float new_coord_X(char side)
{
    if (side == 'E'){
        return 0;
    }
    else{
        return 1;
    }
}
int new_delay_time()
{
    int delay = rand() % 11;
    return delay;
}

int new_runway(char side)
{
    int runway;
    if(side == 'E'){
        runway = runwaysEast[rand() % 2];
    }
    else{
        runway = runwaysWest[rand() % 2];
    }
    return runway;
}

void initialize_queue(PlanesQueue* queue) {
    queue->first = 0;
    queue->last = -1;
    queue->size = 0;
}

void queue_planes(PlanesQueue* queue, pid_t pid) {
    struct sembuf operacao = {0, -1, 0};
    semop(semid, &operacao, 1);

    if (queue->size == MAX_PLANES) {
        queue->last = (queue->last + 1) % MAX_PLANES;
        queue->queue[queue->last].pid = pid;
        queue->queue[queue->last].time = time(NULL);
        queue->size++;
    }
    else{
        printf("Fila cheia, não foi possível enfileirar o avião %d\n", pid);
    }

    operacao.sem_op = 1;
    semop(semid, &operacao, 1);
    kill(pid, SIGSTOP);
}

int dequeue_planes(PlanesQueue* queue, Plane* plane) {
    if(queue->size == 0){
        printf("Fila vazia, não foi possível desenfileirar o avião\n");
        return 0;
    }
    // Copiar os dados do processo para o ponteiro 
    *plane = queue->queue[queue->first];
    queue->first = (queue->first + 1) % MAX_PLANES;
    queue->size--;
    return 1;
}

int search_plane(PlanesQueue* queue, pid_t pid) {
    for(int i = 0; i < queue->size; i++){
        if(queue->queue[i].pid == pid){
            return i;
        }
    }
    return 0;
}

void show_queue(PlanesQueue* queue, time_t initTime) {
    printf("===================================\n");
    printf("Fila de aviões bloqueados (Tempo atual: %lds):\n", time(NULL) - initTime);
    if (queue->size == 0){
        printf("Fila vazia\n");
    }
    else{
        for(int i = 0; i < queue->size; i++){
            int exibitIndex = (queue->first + i) % MAX_PLANES;
            printf("Posicao %d: Processo PID: %d | Tempo na fila: %lds\n",
            exibitIndex, queue->queue[exibitIndex].pid,
            time(NULL) - queue->queue[exibitIndex].time);
            
        }
    }
    printf("===================================\n");
}

int get_index(pid_t pid) {
    for(int i = 0; i < NUM_PLANES; i++){
        if(planes[i] == pid){
            return i;
        }
    }
    return -1;
}

pid_t get_next_plane(PlanesQueue* queue, pid_t pid) {
    int currentIndex = getIndex(pid);
    int nextIndex;

    for(int i = 1; i<=NUM_PLANES; i++){
        nextIndex = (currentIndex + i) % NUM_PLANES;
        pid_t candidatePid = planes[nextIndex];

        if (!search_plane(queue, candidatePid)){
            return candidatePid;
        }
    }
    return -1;
}

Plane new_plane()
{
    Plane A;
    A.delay = new_delay_time();
    A.side = new_entry();
    A.runway = new_runway(A.side);
    A.Y = new_coord_Y();
    A.x = new_coord_X(A.side);
    A.status = 1;
    return A;
}


int main(void)
{
    shmid = shmget(IPC_PRIVATE, sizeof(SharedData), IPC_CREAT | S_IRUSR | S_IWUSR);
    if (shmid < 0) {
        exit(1);
    }
    SharedData* sharedData = (SharedData*)shmat(shmid, NULL, 0);
    if (sharedData == (SharedData*)-1) {
        exit(1);
    }
}


