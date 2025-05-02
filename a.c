#define _POSIX_C_SOURCE 200809L 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <string.h>
#include <math.h>

#define MAX_PLANES 100
#define QUANTUM_USEC 0.2 // 0.2s
#define X_TARGET 0.5
#define Y_TARGET 0.5
#define CRIT_DIST 0.1

typedef enum {RUNNING, LANDED, KILLED} status_t;

typedef struct {
    pid_t pid;
    double x, y;
    double speed;
    int side;       // 0=W, 1=E
    int runway;     // 18/03 para W; 27/06 para E
    status_t status;
} plane_shm_t;

static plane_shm_t *shm_planes;
static int shm_id;
static int num_planes;

// ponteiro global para o handler
static plane_shm_t *myplane;

void handler_usr1(int sig) {
    if (myplane->speed > 0.04) myplane->speed = 0.025;
    else                      myplane->speed = 0.05;
}

void handler_usr2(int sig) {
    if (myplane->side == 0)
        myplane->runway = (myplane->runway == 18 ? 3 : 18);
    else
        myplane->runway = (myplane->runway == 27 ? 6 : 27);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <num_aeronaves>\n", argv[0]);
        return EXIT_FAILURE;
    }
    num_planes = atoi(argv[1]);
    if (num_planes < 1 || num_planes > MAX_PLANES) {
        fprintf(stderr, "Num aeronaves: 1–%d\n", MAX_PLANES);
        return EXIT_FAILURE;
    }

    shm_id = shmget(IPC_PRIVATE, sizeof(plane_shm_t)*num_planes,
                    IPC_CREAT|0666);
    if (shm_id < 0) { perror("shmget"); exit(1); }
    shm_planes = shmat(shm_id, NULL, 0);
    if (shm_planes == (void*)-1) { perror("shmat"); exit(1); }

    srand(time(NULL));
    for (int i = 0; i < num_planes; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            // --- Filho ---
            plane_shm_t *self = &shm_planes[i];
            myplane = self;               // para os handlers

            signal(SIGUSR1, handler_usr1);
            signal(SIGUSR2, handler_usr2);

            // atraso inicial aleatório 0–2s
            sleep(rand() % 3);

            self->pid = getpid();
            self->side = rand()%2;
            self->y = (double)rand()/RAND_MAX;
            self->x = (self->side==0 ? 0.0 : 1.0);
            self->speed = 0.05;
            self->runway = (self->side==0
                ? (rand()%2 ? 18 : 3)
                : (rand()%2 ? 27 : 6));
            self->status = RUNNING;

            while (1) {
                pause();  // espera SIGCONT do controlador

                double dt = QUANTUM_USEC/1e6 * self->speed/0.05;
                if (self->side==0) self->x += dt;
                else               self->x -= dt;
                shm_planes[i] = *self;

                if (fabs(self->x - X_TARGET)<1e-3 &&
                    fabs(self->y - Y_TARGET)<1e-3) {
                    self->status = LANDED;
                    shm_planes[i] = *self;
                    exit(0);
                }
            }
        }
        // --- Pai registra PID ---
        shm_planes[i].pid = pid;
        shm_planes[i].status = RUNNING;
    }

    // --- Pai (controlador) ---
    int current = 0;
    while (1) {
        int alive = 0;
        for (int i = 0; i < num_planes; i++)
            if (shm_planes[i].status == RUNNING) alive++;
        if (!alive) break;

        pid_t pid = shm_planes[current].pid;
        if (shm_planes[current].status == RUNNING) {
            kill(pid, SIGCONT);
            sleep(QUANTUM_USEC);
            kill(pid, SIGSTOP);
        }

        // checa colisões
        for (int i = 0; i < num_planes; i++) {
            for (int j = i+1; j < num_planes; j++) {
                if (shm_planes[i].status!=RUNNING ||
                    shm_planes[j].status!=RUNNING) continue;
                double dx = fabs(shm_planes[i].x - shm_planes[j].x);
                double dy = fabs(shm_planes[i].y - shm_planes[j].y);
                if (shm_planes[i].side == shm_planes[j].side &&
                    shm_planes[i].runway != shm_planes[j].runway)
                    continue;
                if (dx < CRIT_DIST && dy < CRIT_DIST) {
                    kill(shm_planes[i].pid, SIGUSR1);
                    kill(shm_planes[j].pid, SIGUSR1);
                    kill(shm_planes[j].pid, SIGKILL);
                    shm_planes[j].status = KILLED;
                }
            }
        }

        current = (current + 1) % num_planes;
    }

    while (wait(NULL) > 0);
    shmdt(shm_planes);
    shmctl(shm_id, IPC_RMID, NULL);
    printf("Simulação concluída.\n");
    return 0;
}
