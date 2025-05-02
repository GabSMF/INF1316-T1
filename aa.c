/*
 * air_traffic_control_simulation.c
 * Simulação de Controle de Tráfego Aéreo com múltiplos processos (aeronaves)
 * Cada aeronave atualiza sua posição em memória compartilhada e responde a sinais
 * O pai atua como controlador, fazendo escalonamento round-robin, detecção de colisão
 * e interface de linha de comando (CLI).
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <signal.h>
 #include <sys/wait.h>
 #include <sys/time.h>
 #include <time.h>
 #include <sys/shm.h>
 #include <sys/sem.h>
 #include <sys/select.h>
 #include <string.h>
 #include <math.h>
 #include <fcntl.h>
 
 /* Parâmetros da simulação */
 #define MAX_PLANES      100
 #define DEFAULT_SPEED   0.05      // unidades por segundo
 #define REDUCED_SPEED   0.02      // quando SIGUSR1 recebido
 #define COLLISION_DIST  0.1       // distância crítica para colisão
 #define QUANTUM_USEC    200000
 #define LANDING_DIST  0.1 
 /* Estruturas compartilhadas */
 typedef struct {
     pid_t    pid;
     double   x, y;
     double   speed;
     double   vy;       // velocidade vertical
     int      side;     // 0=W, 1=E
     int      runway;   // 18/03 para W ou 27/06 para E
     int      sigusr1;  // toggle velocidade reduzida
     int      sigusr2;  // toggle pista alternativa
     int      status;   // 0=voando,1=pousou,2=abortado
     time_t   last_update;
 } Plane;
 
 typedef struct {
     int    count;
     Plane  planes[MAX_PLANES];
 } SharedData;
 
 /* IDs de IPC */
 static int shmid;
 static int semid;
 
 /* Controlador mantém lista de PIDs e índice atual */
 static pid_t pids[MAX_PLANES];
 static int   current = 0;
 static int   plane_count;
 static int auto_status = 0;
 
 /* Protótipos */
 void setup_shared(int n);
 void spawn_planes(int n);
 void controller_loop();
 void schedule_next();
 void check_collisions();
 void cli_loop();
 
 /* Semáforo P/V */
 void sem_lock() { struct sembuf op = {0, -1, 0}; semop(semid, &op, 1); }
 void sem_unlock() { struct sembuf op = {0, 1, 0}; semop(semid, &op, 1); }
 
 /* Handler de SIGUSR1 (reduz/retorna velocidade) */
 void handle_sigusr1(int sig) {
    SharedData *sh = shmat(shmid, NULL, 0);
    for (int i = 0; i < sh->count; i++) {
        if (sh->planes[i].pid == getpid()) {
            sh->planes[i].sigusr1 ^= 1;
            sh->planes[i].speed = 
                sh->planes[i].sigusr1 ? REDUCED_SPEED : DEFAULT_SPEED;
            printf("[Plane %d] Velocidade %s: %.2f u/s\n",
                   getpid(),
                   sh->planes[i].sigusr1 ? "reduzida" : "normal",
                   sh->planes[i].speed);
            fflush(stdout);
            break;
        }
    }
    shmdt(sh);
}
 
 /* Handler de SIGUSR2 (troca de pista) */
 void handle_sigusr2(int sig) {
    SharedData *sh = shmat(shmid, NULL, 0);
    for (int i = 0; i < sh->count; i++) {
        if (sh->planes[i].pid == getpid()) {
            sh->planes[i].sigusr2 ^= 1;
            int side = sh->planes[i].side;
            int alt  = (side == 0 ? 3 : 6);
            int pref = (side == 0 ? 18 : 27);
            sh->planes[i].runway = 
                sh->planes[i].sigusr2 ? alt : pref;
            printf("[Plane %d] Trocou para pista %02d\n",
                   getpid(),
                   sh->planes[i].runway);
            fflush(stdout);
            break;
        }
    }
    shmdt(sh);
}
 
 /* Loop de voo de cada avião */
 void plane_loop(int idx) {
    SharedData *sh = shmat(shmid, NULL, 0);
    Plane *me = &sh->planes[idx];
    /* quantum em segundos */
    const double dt = (double)QUANTUM_USEC / 1e6;

    while (me->status == 0) {
        /* cada iteração só roda após SIGCONT, 
           e ao final faz STOP de si mesmo */
        raise(SIGSTOP);

        /* passo único de movimento */
        me->x += (me->side == 0 ? +1 : -1) * me->speed * dt;
        me->y += me->vy * dt;

        /* detecta pouso */
        double d_land = hypot(me->x - 0.5, me->y - 0.5);
        if (d_land <= LANDING_DIST) {
            me->status = 1;
            printf("[Plane %d] Pousou em (%.2f,%.2f)\n", getpid(), me->x, me->y);
            fflush(stdout);
            break;
        }
    }

    shmdt(sh);
    exit(0);
}
 
 int main(int argc, char *argv[]) {
     if(argc!=2){fprintf(stderr,"Uso: %s <num_planes>\n",argv[0]);exit(1);}    
     int n=atoi(argv[1]); if(n<1||n>MAX_PLANES){fprintf(stderr,"Número de aeronaves inválido\n");exit(1);}    
     plane_count=n;
     setup_shared(n);
     spawn_planes(n);
     controller_loop();
     return 0;
 }
 
 void setup_shared(int n) {
     shmid=shmget(IPC_PRIVATE,sizeof(SharedData),IPC_CREAT|0666);
     if(shmid<0){perror("shmget");exit(1);} 
     SharedData *sh=shmat(shmid,NULL,0);
     sh->count=n;
     semid=semget(IPC_PRIVATE,1,IPC_CREAT|0666);
     if(semid<0){perror("semget");exit(1);} semctl(semid,0,SETVAL,1);
     srand(time(NULL));
     for(int i=0;i<n;i++){
         Plane *p=&sh->planes[i]; p->pid=0; p->side=rand()%2;
         p->y=(double)rand()/RAND_MAX;
         p->x=(p->side==0?0.0:1.0);
         p->speed=DEFAULT_SPEED;
         double dx=fabs(0.5-p->x);
         p->vy=(0.5-p->y)/(dx/p->speed);
         p->sigusr1=0; p->sigusr2=0; p->status=0;
         int alt=(p->side==0?3:6), pref=(p->side==0?18:27);
         p->runway=(rand()%2?alt:pref);
         p->last_update=time(NULL);
     }
     shmdt(sh);
 }
 
 void spawn_planes(int n) {
    SharedData *sh = shmat(shmid, NULL, 0);
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            signal(SIGUSR1, handle_sigusr1);
            signal(SIGUSR2, handle_sigusr2);

            /* delay aleatório antes de iniciar */
            srand(time(NULL) ^ getpid());
            int delay = rand() % 3;
            printf("[Plane %d] Delay %d s antes de iniciar\n",
                   getpid(), delay);
            fflush(stdout);
            sleep(delay);

            /* aqui o delay acabou: avião pronto para voar */
            printf("[Plane %d] Iniciando voo\n", getpid());
            fflush(stdout);

            /* só voa após SIGCONT do controlador */
            raise(SIGSTOP);

            plane_loop(i);
        }
        pids[i] = pid;
        sh->planes[i].pid = pid;
    }
    shmdt(sh);

    /* espera cada filho parar com raise(SIGSTOP) */
    for (int i = 0; i < n; i++) {
        int status;
        waitpid(pids[i], &status, WUNTRACED);
    }
}
 
void controller_loop() {
    kill(pids[0], SIGCONT);
    while (1) {
        usleep(QUANTUM_USEC);
        schedule_next();
        check_collisions();
        if (auto_status) {
            /* limpa a tela e reposiciona o cursor no topo */
            printf("\033[H\033[J");
            print_status();
        }
        cli_loop();
    }
}

 
 void schedule_next() {
     SharedData *sh=shmat(shmid,NULL,0);
     int next=current;
     for(int i=1;i<=plane_count;i++){
         int idx=(current+i)%plane_count;
         if(sh->planes[idx].status==0){next=idx;break;}
     }
     if(next!=current){kill(pids[current],SIGSTOP);kill(pids[next],SIGCONT);current=next;}
     shmdt(sh);
 }
 
 /*
  * Estratégia 1 (Pré-reação + desvio + recuperação):
  * 1) Se dist<2*COLLISION_DIST, dispara SIGUSR1 em ambos.
  * 2) Após novo check (dist<COLLISION_DIST e mesma pista), dispara SIGUSR2 em um.
  * 3) Recupera velocidade com duplo SIGUSR1.
  */
 void check_collisions() {
     SharedData *sh=shmat(shmid,NULL,0);
     for(int i=0;i<plane_count;i++){
         for(int j=i+1;j<plane_count;j++){
             Plane *a=&sh->planes[i], *b=&sh->planes[j];
             if(a->status||b->status) continue;
             if(a->side!=b->side) continue;
             double dx=fabs(a->x-b->x), dy=fabs(a->y-b->y);
             double d=hypot(dx,dy);
             // 1) Pré-redução
             if(d<2*COLLISION_DIST && a->runway==b->runway) {
                 kill(a->pid,SIGUSR2);
             }
             // 2) Se ainda muito perto e mesma pista, troca pista de b
             else if(d< 1.5*COLLISION_DIST && a->runway==b->runway) {
                 kill(b->pid,SIGUSR1);
             }

             if(d< COLLISION_DIST && a->runway==b->runway){
                kill(a->pid,SIGKILL);
             }
         }
     }
     shmdt(sh);
 }
 
 void cli_loop() {
    fd_set rfds; struct timeval tv = {0,0};
    FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds);
    if (select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv) > 0) {
        char buf[128];
        if (fgets(buf, sizeof(buf), stdin)) {
            if (strncmp(buf, "status", 6) == 0) {
                auto_status = 1;
                print_status();
            }
            else if (strncmp(buf, "stop", 4) == 0) {
                auto_status = 0;
            }
            else if (strncmp(buf, "quit", 4) == 0) {
                for (int i = 0; i < plane_count; i++)
                    kill(pids[i], SIGKILL);
                shmctl(shmid, IPC_RMID, NULL);
                semctl(semid, 0, IPC_RMID);
                exit(0);
            }
        }
    }
}

void print_status() {
    SharedData *sh = shmat(shmid, NULL, 0);
    printf("PID\tX\tY\tSpeed\tRunway\tStatus\n");
    for (int i = 0; i < plane_count; i++) {
        Plane *p = &sh->planes[i];
        printf("%d\t%.2f\t%.2f\t%.2f\t%02d\t%d\n",
               p->pid, p->x, p->y, p->speed, p->runway, p->status);
    }
    shmdt(sh);
}