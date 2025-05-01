#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/shm.h>
#include <string.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>
#include <math.h>
#include <stdbool.h>
#include <termios.h>

#define MAX_PLANES    20       // Máximo de aeronaves
#define SPEED_NORMAL 0.05      // Velocidade normal (unidades/s)
#define SPEED_SLOW   0.025     // Velocidade reduzida via SIGUSR1
#define QUANTUM_USEC 200000    // Quantum de 0.2s para round-robin
#define COLL_DIST    0.10      // Distância mínima segura em x e y

// Runways: para entrada W (0): 18 <-> 03; para E (1): 06 <-> 27
const int runways[2][2] = {{18,3}, {6,27}};

// Estruturas de dados
typedef struct {
    pid_t    pid;
    double   x, y;
    double   speed;
    int      side;
    int      runway;
    int      status;  // 1=voando,2=pousou,3=morto
    bool     colidiu; // flag para colisão
    bool     velocidade_reduzida; // flag para velocidade reduzida
    bool     pista_trocada; // flag para troca de pista
} PlaneInfo;

typedef struct {
    int        n;
    PlaneInfo  planes[MAX_PLANES];
} SharedData;

// Globals
static int shm_id;
static SharedData *shm;
static int curr_idx = 0;
static int N_planes = 5;
static struct termios orig_termios;
static time_t start_time; // tempo de início da simulação

// Handler vazio para SIGCONT (faz pause() retornar)
void cont_handler(int sig) {
    // nada
}

// Modo raw para CLI
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

// Round-Robin handler
void rr_handler(int sig) {
    if (shm->planes[curr_idx].status == 1)
        kill(shm->planes[curr_idx].pid, SIGSTOP);
    int next = (curr_idx + 1) % shm->n;
    for (int i = 0; i < shm->n; i++) {
        int idx = (curr_idx + 1 + i) % shm->n;
        if (shm->planes[idx].status == 1) { next = idx; break; }
    }
    curr_idx = next;
    if (shm->planes[curr_idx].status == 1)
        kill(shm->planes[curr_idx].pid, SIGCONT);
}

// Filhos terminados
void chld_handler(int sig) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < shm->n; i++) {
            if (shm->planes[i].pid == pid) {
                if (WIFEXITED(status)) {
                    shm->planes[i].status = 2;  // pousou
                } else if (WIFSIGNALED(status)) {
                    int tsig = WTERMSIG(status);
                    if (tsig == SIGKILL)
                        printf("[!] Avião PID %d foi morto (SIGKILL)\n", pid);
                    shm->planes[i].status = 3;  // morto
                }
                break;
            }
        }
    }
}

// SIGUSR1: toggle speed
void plane_usr1(int sig) {
    for (int i = 0; i < shm->n; i++) {
        if (shm->planes[i].pid == getpid()) {
            shm->planes[i].speed = (fabs(shm->planes[i].speed - SPEED_NORMAL) < 1e-6 ? SPEED_SLOW : SPEED_NORMAL);
            shm->planes[i].velocidade_reduzida = (shm->planes[i].speed == SPEED_SLOW);
            break;
        }
    }
}

// SIGUSR2: toggle runway
void plane_usr2(int sig) {
    for (int i = 0; i < shm->n; i++) {
        if (shm->planes[i].pid == getpid()) {
            int si = shm->planes[i].side;
            shm->planes[i].runway = (shm->planes[i].runway == runways[si][0] ? runways[si][1] : runways[si][0]);
            shm->planes[i].pista_trocada = true;
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    // Número de aviões por argumento
    if (argc > 1) N_planes = atoi(argv[1]);
    if (N_planes < 1 || N_planes > MAX_PLANES) N_planes = 5;

    // Inicializa tempo de início
    start_time = time(NULL);

    // Configura SHM
    shm_id = shmget(IPC_PRIVATE, sizeof(SharedData), IPC_CREAT | 0600);
    shm = (SharedData*)shmat(shm_id, NULL, 0);
    shm->n = N_planes;
    memset(shm->planes, 0, sizeof(shm->planes));

    // Instala handlers no pai
    struct sigaction sa;
    sa.sa_handler = rr_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = chld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    // Timer de round-robin (0.2s)
    struct itimerval tv = {{0, QUANTUM_USEC}, {0, QUANTUM_USEC}};
    setitimer(ITIMER_REAL, &tv, NULL);

    // Cria processos (aeronaves)
    for (int i = 0; i < N_planes; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        else if (pid == 0) {
            // Código do filho
            srand(getpid());
            int side     = rand() % 2;
            double y     = (double)rand() / RAND_MAX;
            double x     = (side == 0 ? 0.0 : 1.0);
            int delay    = rand() % 3;
            double speed = SPEED_NORMAL;
            int runway   = runways[side][rand() % 2];

            // Atraso de chegada
            sleep(delay);

            // Instala handlers do filho
            signal(SIGUSR1, plane_usr1);
            signal(SIGUSR2, plane_usr2);
            signal(SIGCONT, cont_handler);

            // Registra em SHM
            for (int j = 0; j < shm->n; j++) {
                if (shm->planes[j].pid == 0) {
                    shm->planes[j] = (PlaneInfo){getpid(), x, y, speed, side, runway, 1, false, false, false};
                    break;
                }
            }

            // Espera primeiro SIGCONT
            pause();

            // Loop de voo
            while (1) {
                double dt = (double)QUANTUM_USEC / 1e6;
                x += (side == 0 ? speed * dt : -speed * dt);

                // Atualiza posição em SHM
                for (int j = 0; j < shm->n; j++) {
                    if (shm->planes[j].pid == getpid()) {
                        shm->planes[j].x     = x;
                        shm->planes[j].speed = speed;
                        break;
                    }
                }

                // Pouso em x=0.5
                if ((side == 0 && x >= 0.5) || (side == 1 && x <= 0.5)) {
                    printf("[P] PID %d pousou na pista %d\n", getpid(), runway);
                    exit(0);
                }

                // Aguarda próximo SIGCONT
                pause();
            }
        }
    }

    // CLI: modo raw + stdin não-bloqueante
    enable_raw_mode();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    bool started = false;
    printf("Comandos: s(start) v(view) p(pause) r(resume) q(quit)\n");

    // Loop do pai
    while (1) {
        int any = 0;
        for (int i = 0; i < shm->n; i++) if (shm->planes[i].status == 1) any++;
        if (!any) break;

        // Detecção de colisão
        for (int i = 0; i < shm->n; i++) {
            for (int j = i + 1; j < shm->n; j++) {
                if (shm->planes[i].status == 1 && shm->planes[j].status == 1) {
                    double dx = fabs(shm->planes[i].x - shm->planes[j].x);
                    double dy = fabs(shm->planes[i].y - shm->planes[j].y);
                    if (dx < COLL_DIST && dy < COLL_DIST) {
                        shm->planes[i].colidiu = true;
                        shm->planes[j].colidiu = true;
                        kill(shm->planes[i].pid, SIGUSR1);
                        kill(shm->planes[j].pid, SIGUSR1);
                    }
                }
            }
        }

        // Leitura de comando
        char cmd;
        if (read(STDIN_FILENO, &cmd, 1) == 1) {
            switch (cmd) {
                case 's': // inicia simulação
                    if (!started) {
                        kill(shm->planes[0].pid, SIGCONT);
                        started = true;
                    }
                    break;
                case 'v': // view
                    printf("=== Status das Aeronaves (Tempo decorrido: %lds) ===\n", time(NULL) - start_time);
                    for (int i = 0; i < shm->n; i++) {
                        PlaneInfo *p = &shm->planes[i];
                        const char *dir = (p->side == 0 ? "W→E" : "E→W");
                        printf("PID %d: x=%.2f y=%.2f dir=%s speed=%.3f st=%d rw=%d", 
                               p->pid, p->x, p->y, dir, p->speed, p->status, p->runway);
                        
                        // Mostra eventos
                        if (p->colidiu) printf(" [COLIDIU]");
                        if (p->velocidade_reduzida) printf(" [VELOCIDADE REDUZIDA]");
                        if (p->pista_trocada) printf(" [PISTA TROCADA]");
                        printf("\n");
                    }
                    break;
                case 'p': // pause all
                    for (int i = 0; i < shm->n; i++)
                        if (shm->planes[i].status == 1)
                            kill(shm->planes[i].pid, SIGSTOP);
                    break;
                case 'r': // resume round-robin
                    for (int i = 0; i < shm->n; i++)
                        if (shm->planes[i].status == 1)
                            kill(shm->planes[i].pid, SIGSTOP);
                    if (started)
                        kill(shm->planes[curr_idx].pid, SIGCONT);
                    break;
                case 'q': // quit
                    for (int i = 0; i < shm->n; i++)
                        if (shm->planes[i].status == 1)
                            kill(shm->planes[i].pid, SIGKILL);
                    goto end;
            }
        }
        usleep(100000);
    }

end:
    disable_raw_mode();
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    printf("Simulação finalizada.\n");
    return 0;
}
