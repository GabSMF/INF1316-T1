#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

// Compile with: gcc -o simulador doidera.c -lm

#define MAX_PLANES    20
#define HORIZ_SPEED   0.05f
#define COLLIDE_DIST  0.1f

// Macros for min/max of floats
#define MINF(a,b) ((a) < (b) ? (a) : (b))
#define MAXF(a,b) ((a) > (b) ? (a) : (b))

// Plane record in shared memory
typedef struct {
    pid_t    pid;
    int      id;
    char     side;    // 'W' or 'E'
    float    x, y;
    float    vx, vy;
    int      pref_runway;
    int      active;  // 1=voando, 0=chegou
} Plane;

// Shared memory layout
typedef struct {
    int   n_planes;
    int   rr_index;
    Plane planes[MAX_PLANES];
} SharedData;

// Globals for IPC
int shm_id, sem_id;
SharedData *shm = NULL;

// Semaphore ops
struct sembuf P_op = {0, -1, 0};
struct sembuf V_op = {0, +1, 0};

// Prototypes
void init_ipc(int N);
void cleanup_ipc();
bool will_collide(const Plane *p1, const Plane *p2);
void handle_sigusr1(int sig);
void handle_sigusr2(int sig);

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s N_planes\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int N = atoi(argv[1]);
    if (N < 1 || N > MAX_PLANES) {
        fprintf(stderr, "N_planes deve estar entre 1 e %d\n", MAX_PLANES);
        exit(EXIT_FAILURE);
    }

    srand(time(NULL));
    init_ipc(N);

    // Inicializa shared
    shm->n_planes = N;
    shm->rr_index = 0;
    for (int i = 0; i < N; ++i) {
        semop(sem_id, &P_op, 1);
        Plane *p = &shm->planes[i];
        p->id = i;
        p->side = (rand() % 2 ? 'W' : 'E');
        p->x = (p->side == 'W' ? 0.0f : 1.0f);
        p->y = ((float)rand() / RAND_MAX);
        p->vx = (p->side == 'W' ? HORIZ_SPEED : -HORIZ_SPEED);
        p->vy = p->vx * (0.5f - p->y) / (0.5f - p->x);
        p->pref_runway = (p->side == 'W' ? (rand() % 2 ? 18 : 3)
                                         : (rand() % 2 ? 27 : 6));
        p->active = 1;
        semop(sem_id, &V_op, 1);
    }

    // Fork aviões (iniciam em SIGSTOP)
    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }
        if (pid == 0) {
            signal(SIGUSR1, handle_sigusr1);
            signal(SIGUSR2, handle_sigusr2);
            kill(getpid(), SIGSTOP);
            // loop de movimento
            while (1) {
                sleep(1);
                semop(sem_id, &P_op, 1);
                Plane *p = &shm->planes[i];
                if (!p->active) {
                    printf("Plane %d: landed.\n", p->id);
                    semop(sem_id, &V_op, 1);
                    break;
                }
                p->x += p->vx;
                p->y += p->vy;
                if (fabs(p->x - 0.5f) < 1e-3f && fabs(p->y - 0.5f) < 1e-3f) {
                    p->active = 0;
                }
                semop(sem_id, &V_op, 1);
            }
            exit(EXIT_SUCCESS);
        }
        shm->planes[i].pid = pid;
    }

    // Interface de linha de comando
    int running = 0;
    char buffer[128];
    printf("Comandos: start, pause, resume, status, exit\n");
    while (1) {
        printf("> ");
        if (!fgets(buffer, sizeof(buffer), stdin)) break;
        if (strncmp(buffer, "start", 5) == 0) {
            running = 1;
            printf("Simulação iniciada\n");
        } else if (strncmp(buffer, "pause", 5) == 0) {
            running = 0;
            for (int i = 0; i < N; ++i)
                kill(shm->planes[i].pid, SIGSTOP);
            printf("Simulação pausada\n");
        } else if (strncmp(buffer, "resume", 6) == 0) {
            running = 1;
            printf("Simulação retomada\n");
        } else if (strncmp(buffer, "status", 6) == 0) {
            semop(sem_id, &P_op, 1);
            for (int i = 0; i < N; ++i) {
                Plane *p = &shm->planes[i];
                printf("Plane %d: pos=(%.2f,%.2f) dir=(%.2f,%.2f) %s runway=%d\n",
                       p->id, p->x, p->y, p->vx, p->vy,
                       p->active ? "active" : "done", p->pref_runway);
            }
            semop(sem_id, &V_op, 1);
        } else if (strncmp(buffer, "exit", 4) == 0) {
            break;
        }
        // executa um step se em funcionamento
        if (running) {
            semop(sem_id, &P_op, 1);
            int cur = shm->rr_index;
            if (cur > 0) kill(shm->planes[cur-1].pid, SIGSTOP);
            else         kill(shm->planes[N-1].pid, SIGSTOP);
            kill(shm->planes[cur].pid, SIGCONT);

            // detecção de conflitos e colisões
            for (int i = 0; i < N; ++i) {
                for (int j = i + 1; j < N; ++j) {
                    Plane *p1 = &shm->planes[i], *p2 = &shm->planes[j];
                    if (!p1->active || !p2->active) continue;
                    if (p1->side == p2->side && will_collide(p1, p2)) {
                        if (fabs(p1->x - p2->x) < COLLIDE_DIST && fabs(p1->y - p2->y) < COLLIDE_DIST) {
                            printf("Planes %d and %d: collision imminent, killing both.\n", p1->id, p2->id);
                            kill(p1->pid, SIGKILL);
                            kill(p2->pid, SIGKILL);
                        } else {
                            pid_t victim = (p1->id == cur ? p2->pid : p1->pid);
                            int vid = (p1->id == cur ? p2->id : p1->id);
                            printf("Plane %d: reducing speed due to collision forecast.\n", vid);
                            kill(victim, SIGUSR1);
                        }
                    }
                    float cross = p1->vy * p2->vx - p2->vy * p1->vx;
                    if (fabs(cross) < 1e-6f && p1->pref_runway == p2->pref_runway) {
                        printf("Planes %d and %d: same route & runway %d, swapping runway for %d.\n",
                               p1->id, p2->id, p1->pref_runway, p2->id);
                        kill(p2->pid, SIGUSR2);
                    }
                }
            }
            shm->rr_index = (shm->rr_index + 1) % N;
            semop(sem_id, &V_op, 1);
            sleep(1);

            // checa término
            semop(sem_id, &P_op, 1);
            int alive = 0;
            for (int i = 0; i < N; ++i) alive += shm->planes[i].active;
            semop(sem_id, &V_op, 1);
            if (!alive) break;
        }
    }

    // encerramento
    for (int i = 0; i < N; ++i) kill(shm->planes[i].pid, SIGKILL);
    for (int i = 0; i < N; ++i) wait(NULL);
    cleanup_ipc();
    return 0;
}

void init_ipc(int N) {
    key_t key = ftok("/tmp", 'A');
    shm_id = shmget(key, sizeof(SharedData), IPC_CREAT | 0666);
    shm = (SharedData*)shmat(shm_id, NULL, 0);
    sem_id = semget(key, 1, IPC_CREAT | 0666);
    SemUn arg; arg.val = 1; semctl(sem_id, 0, SETVAL, arg);
}

void cleanup_ipc() {
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
}

bool will_collide(const Plane *p1, const Plane *p2) {
    float A = p2->x - p1->x;
    float B = p2->vx - p1->vx;
    float C = p2->y - p1->y;
    float D = p2->vy - p1->vy;
    float t0x, t1x, t0y, t1y;
    if (fabs(B) < 1e-6f) {
        if (fabs(A) >= COLLIDE_DIST) return false;
        t0x = 0; t1x = INFINITY;
    } else {
        float ta = (COLLIDE_DIST - A) / B;
        float tb = (-COLLIDE_DIST - A) / B;
        t0x = MINF(ta, tb);
        t1x = MAXF(ta, tb);
    }
    if (fabs(D) < 1e-6f) {
        if (fabs(C) >= COLLIDE_DIST) return false;
        t0y = 0; t1y = INFINITY;
    } else {
        float ta = (COLLIDE_DIST - C) / D;
        float tb = (-COLLIDE_DIST - C) / D;
        t0y = MINF(ta, tb);
        t1y = MAXF(ta, tb);
    }
    float t_start = MAXF(t0x, t0y);
    float t_end   = MINF(t1x, t1y);
    return (t_end >= 0 && t_start <= t_end);
}

void handle_sigusr1(int sig) {
    semop(sem_id, &P_op, 1);
    for (int i = 0; i < shm->n_planes; ++i) {
        if (shm->planes[i].pid == getpid()) {
            shm->planes[i].vy *= 0.5f;
            printf("Plane %d: speed reduced.\n", shm->planes[i].id);
            break;
        }
    }
    semop(sem_id, &V_op, 1);
}

void handle_sigusr2(int sig) {
    semop(sem_id, &P_op, 1);
    for (int i = 0; i < shm->n_planes; ++i) {
        if (shm->planes[i].pid == getpid()) {
            int old = shm->planes[i].pref_runway;
            if (old == 18) shm->planes[i].pref_runway = 3;
            else if (old == 3) shm->planes[i].pref_runway = 18;
            else if (old == 27) shm->planes[i].pref_runway = 6;
            else if (old == 6) shm->planes[i].pref_runway = 27;
            printf("Plane %d: runway changed from %d to %d.\n",
                   shm->planes[i].id, old, shm->planes[i].pref_runway);
            break;
        }
    }
    semop(sem_id, &V_op, 1);
}
