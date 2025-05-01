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
#define SPEED_NORMAL 0.05
#define SPEED_SLOW 0.025
#define QUANTUM_USEC 200000 // 0.2 s
#define COLL_DIST 0.1       // distância crítica

typedef struct
{
    pid_t pid;
    double x, y;  // posição
    double speed; // velocidade atual
    int side;     // 0=W→E, 1=E→W
    int runway;   // pista atual
    int status;   // 0=aguardando,1=voando,2=pousou,3=kill
    int colisao;  // contador de colisões
    int mudancas_velocidade; // contador de mudanças de velocidade
    int mudancas_pista;     // contador de mudanças de pista
} PlaneInfo;

typedef struct
{
    int n;
    PlaneInfo planes[MAX_PLANES];
    time_t start_time; // tempo de início da simulação
} SharedData;

static SharedData *shm;
static int shm_id;
static int curr_idx = 0;

// ——————————————————————————
// ROUND-ROBIN: SIGALRM
// ——————————————————————————
void rr_handler(int sig)
{
    if (shm->planes[curr_idx].status == 1)
        kill(shm->planes[curr_idx].pid, SIGSTOP);

    // próximo voando
    int next = (curr_idx + 1) % shm->n;
    for (int i = 0; i < shm->n; i++)
    {
        int idx = (curr_idx + 1 + i) % shm->n;
        if (shm->planes[idx].status == 1)
        {
            next = idx;
            break;
        }
    }
    curr_idx = next;

    if (shm->planes[curr_idx].status == 1)
        kill(shm->planes[curr_idx].pid, SIGCONT);
}

// ——————————————————————————
// CHILD EXIT: SIGCHLD
// ——————————————————————————
void chld_handler(int sig)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        for (int i = 0; i < shm->n; i++)
        {
            if (shm->planes[i].pid == pid)
            {
                if (WIFEXITED(status))
                {
                    shm->planes[i].status = 2;
                    printf("[C] PID %d pousou normalmente\n", pid);
                }
                else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)
                {
                    shm->planes[i].status = 3;
                    printf("[C] PID %d foi morto (SIGKILL)\n", pid);
                }
            }
        }
    }
}

// ——————————————————————————
// CHILD: SIGUSR1 toggle speed
// ——————————————————————————
void plane_usr1(int sig)
{
    for (int i = 0; i < shm->n; i++)
    {
        if (shm->planes[i].pid == getpid())
        {
            shm->planes[i].speed =
                (fabs(shm->planes[i].speed - SPEED_NORMAL) < 1e-6
                     ? SPEED_SLOW
                     : SPEED_NORMAL);
            shm->planes[i].mudancas_velocidade++;
            printf("[T] PID %d mudou velocidade para %.3f\n", getpid(), shm->planes[i].speed);
            break;
        }
    }
}

// ——————————————————————————
// CHILD: SIGUSR2 toggle runway
// ——————————————————————————
void plane_usr2(int sig)
{
    const int runW[2] = {18, 3}, runE[2] = {6, 27};
    for (int i = 0; i < shm->n; i++)
    {
        if (shm->planes[i].pid == getpid())
        {
            int side = shm->planes[i].side;
            int curr = shm->planes[i].runway;
            if (side == 0)
                shm->planes[i].runway = (curr == runW[0] ? runW[1] : runW[0]);
            else
                shm->planes[i].runway = (curr == runE[0] ? runE[1] : runE[0]);
            shm->planes[i].mudancas_pista++;
            printf("[T] PID %d mudou para pista %d\n", getpid(), shm->planes[i].runway);
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    // —————————————————————
    // Quantidade de aeronaves
    // —————————————————————
    int N = 5;
    if (argc > 1)
        N = atoi(argv[1]);
    if (N < 1 || N > MAX_PLANES)
        N = 5;

    // —————————————————————
    // Cria e anexa SHM
    // —————————————————————
    shm_id = shmget(IPC_PRIVATE, sizeof(SharedData),
                    IPC_CREAT | IPC_EXCL | 0600);
    shm = shmat(shm_id, NULL, 0);
    shm->n = N;
    memset(shm->planes, 0, sizeof(shm->planes));

    // Inicializa o tempo de início
    shm->start_time = time(NULL);

    // —————————————————————
    // Instala handlers no pai
    // —————————————————————
    struct sigaction sa;

    // Zera o mask e os flags separadamente:
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Agora atribui o handler
    sa.sa_handler = rr_handler;
    sigaction(SIGALRM, &sa, NULL);

    // E para o segundo handler:
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = chld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    // —————————————————————
    // Timer para round-robin
    // —————————————————————
    struct itimerval tv = {
        .it_interval = {0, QUANTUM_USEC},
        .it_value = {0, QUANTUM_USEC}};
    setitimer(ITIMER_REAL, &tv, NULL);

    // —————————————————————
    // Fork das aeronaves (filhos)
    // —————————————————————
    for (int i = 0; i < N; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            exit(1);
        }
        if (pid == 0)
        {
            // ————— CHILD —————
            srand(getpid());
            int side = rand() % 2;                // W/E
            double x = (side == 0 ? 0.0 : 1.0);   // ponto de partida
            double y = (double)rand() / RAND_MAX; // coord. y
            int delay = rand() % 3;               // 0–2 s delay

            sleep(delay);

            // instala handlers de USR1/USR2
            signal(SIGUSR1, plane_usr1);
            signal(SIGUSR2, plane_usr2);

            // escolhe pista inicial
            double speed = SPEED_NORMAL;
            int runway = (side == 0 ? (rand() % 2 ? 18 : 3)
                                    : (rand() % 2 ? 6 : 27));

            // registra estado em SHM
            SharedData *cshm = shmat(shm_id, NULL, 0);
            for (int j = 0; j < N; j++)
            {
                if (cshm->planes[j].pid == 0)
                {
                    cshm->planes[j].pid = getpid();
                    cshm->planes[j].x = x;
                    cshm->planes[j].y = y;
                    cshm->planes[j].speed = speed;
                    cshm->planes[j].side = side;
                    cshm->planes[j].runway = runway;
                    cshm->planes[j].status = 1; // voando
                    break;
                }
            }

            // aguarda SIGCONT do pai para iniciar
            pause();

            // loop de voo em direção a [0.5,0.5]
            while (1)
            {
                double dt = (double)QUANTUM_USEC / 1e6;
                double dx = 0.5 - x, dy = 0.5 - y;
                double dist = hypot(dx, dy);

                // pousou?
                if (dist < speed * dt)
                {
                    printf("[P] PID %d pousou na pista %d\n",
                           getpid(), runway);
                    exit(0);
                }

                // move em direção
                x += speed * dt * (dx / dist);
                y += speed * dt * (dy / dist);

                // atualiza SHM
                SharedData *ushm = shmat(shm_id, NULL, 0);
                for (int j = 0; j < N; j++)
                {
                    if (ushm->planes[j].pid == getpid())
                    {
                        ushm->planes[j].x = x;
                        ushm->planes[j].y = y;
                        ushm->planes[j].speed = speed;
                        break;
                    }
                }

                // espera próximo RR
                pause();
            }
        }
    }

    // —————————————————————
    // LOOP DO PAI: CLI + colisões
    // —————————————————————
    printf("Comandos: s(start) v(view) p(pause) r(resume) "
           "t(toggle speed) c(toggle runway) q(quit)\n");
    int started = 0;

    while (1)
    {
        // saí quando nenhuma aeronave voa
        int any = 0;
        for (int i = 0; i < N; i++)
            if (shm->planes[i].status == 1)
            {
                any = 1;
                break;
            }
        if (!any)
            break;

        // leitura não-bloqueante de comando
        fd_set rf;
        struct timeval tvsel = {0, 100000}; // 0.1 s
        FD_ZERO(&rf);
        FD_SET(STDIN_FILENO, &rf);
        if (select(STDIN_FILENO + 1, &rf, NULL, NULL, &tvsel) > 0)
        {
            char cmd = getchar();
            PlaneInfo *p = &shm->planes[curr_idx];

            if (cmd == 's' && !started)
            {
                // dispara primeiro filho
                kill(shm->planes[0].pid, SIGCONT);
                started = 1;
            }
            else if (cmd == 'v')
            {
                // view status
                time_t current_time = time(NULL);
                double elapsed_time = difftime(current_time, shm->start_time);
                printf("\nTempo decorrido: %.1f segundos\n", elapsed_time);
                for (int i = 0; i < N; i++)
                {
                    PlaneInfo *q = &shm->planes[i];
                    const char *dir = (q->side == 0 ? "W->E" : "E->W");
                    printf("PID %d: x=%.2f y=%.2f dir=%s sp=%.3f st=%d rw=%d\n",
                           q->pid, q->x, q->y, dir, q->speed,
                           q->status, q->runway);
                    printf("  Colisões: %d, Mudanças de velocidade: %d, Mudanças de pista: %d\n",
                           q->colisao, q->mudancas_velocidade, q->mudancas_pista);
                }
            }
            else if (cmd == 'p')
            {
                // pausa todas
                for (int i = 0; i < N; i++)
                    if (shm->planes[i].status == 1)
                        kill(shm->planes[i].pid, SIGSTOP);
            }
            else if (cmd == 'r')
            {
                // retoma o round-robin
                for (int i = 0; i < N; i++)
                    if (shm->planes[i].status == 1)
                        kill(shm->planes[i].pid, SIGSTOP);
                if (started)
                    kill(p->pid, SIGCONT);
            }
            else if (cmd == 't' && started)
            {
                // toggle speed (SIGUSR1) e verifica
                double old = p->speed;
                kill(p->pid, SIGUSR1);
                usleep(10000);
                printf("[>] SIGUSR1 em %d: speed %.3f→%.3f\n",
                       p->pid, old, p->speed);
            }
            else if (cmd == 'c' && started)
            {
                // toggle runway (SIGUSR2) e verifica
                int oldr = p->runway;
                kill(p->pid, SIGUSR2);
                usleep(10000);
                printf("[>] SIGUSR2 em %d: runway %d→%d\n",
                       p->pid, oldr, p->runway);
            }
            else if (cmd == 'q')
            {
                // encerra tudo
                for (int i = 0; i < N; i++)
                    if (shm->planes[i].status == 1)
                        kill(shm->planes[i].pid, SIGKILL);
                break;
            }
        }

        // ——————————————————————————
        // Detecção de colisão
        // ——————————————————————————
        for (int i = 0; i < N; i++)
        {
            for (int j = i + 1; j < N; j++)
            {
                PlaneInfo *a = &shm->planes[i];
                PlaneInfo *b = &shm->planes[j];
                if (a->status == 1 && b->status == 1)
                {
                    double dx = fabs(a->x - b->x);
                    double dy = fabs(a->y - b->y);
                    if (dx < COLL_DIST && dy < COLL_DIST)
                    {
                        a->colisao++;
                        b->colisao++;
                        // já em velocidade mínima?
                        if (a->speed <= SPEED_SLOW + 1e-6 &&
                            b->speed <= SPEED_SLOW + 1e-6)
                        {
                            // inevitável → mata um
                            pid_t victim = (a->pid > b->pid ? a->pid : b->pid);
                            kill(victim, SIGKILL);
                            printf("[!] Colisão inevitável: matando %d\n",
                                   victim);
                        }
                        else
                        {
                            // reduz velocidade
                            printf("[!] Risco colisão %d ↔ %d: reduzindo\n",
                                   a->pid, b->pid);
                            kill(a->pid, SIGUSR1);
                            kill(b->pid, SIGUSR1);
                            usleep(10000);
                            printf("[>] speeds: %d→%.3f, %d→%.3f\n",
                                   a->pid, a->speed, b->pid, b->speed);
                        }
                    }
                }
            }
        }
    }

    // —————————————————————
    // Cleanup
    // —————————————————————
    shmdt(shm);
    shmctl(shm_id, IPC_RMID, NULL);
    printf("Simulação encerrada.\n");
    return 0;
}
