#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <sys/wait.h>
#include <string.h>

// Constantes do sistema
#define SPACE_SIZE 1.0
#define MIN_DISTANCE 0.1
#define SPEED 0.05
#define SHM_KEY 1234
#define UPDATE_INTERVAL 1  // segundos
#define MAX_AIRCRAFT 20

// Estrutura para representar uma aeronave
typedef struct {
    pid_t pid;          // PID do processo
    float x;            // Posição x
    float y;            // Posição y
    char direction;     // 'W' ou 'E'
    int runway;         // Pista de pouso (18, 03, 27, 06)
    int active;         // 1 se ativa, 0 se inativa
    int slowed;         // 1 se reduzida velocidade, 0 se normal
    char status[50];    // Status atual
} Aircraft;

// Estrutura para a memória compartilhada
typedef struct {
    int num_aircraft;   // Número total de aeronaves
    int paused;         // 1 se simulado pausado
    Aircraft aircrafts[MAX_AIRCRAFT]; // Array de aeronaves
} SharedMemory;

// Variáveis globais
static SharedMemory *shm = NULL;
static int shmid = -1;
static int should_exit = 0;
static int current_aircraft_id = -1;

// Protótipos de funções
void control_tower(int num_aircraft);
void aircraft_process(int id, char direction, int runway);
void update_aircraft_status(int id);
void handle_aircraft_signals(int sig);
void handle_control_signals(int sig);
void print_help();
void display_status();

// Função para exibir ajuda
void print_help() {
    printf("\n=== Sistema de Controle de Tráfego Aéreo ===\n");
    printf("Uso: ./air_traffic <número_de_aeronaves>\n");
    printf("Onde <número_de_aeronaves> deve ser entre 1 e %d\n", MAX_AIRCRAFT);
    printf("\nComandos durante a execução:\n");
    printf("  p - Pausar/Retomar simulação\n");
    printf("  q - Sair do programa\n");
    printf("  h - Mostrar esta ajuda\n");
    printf("\nO programa simula o controle de tráfego aéreo com:\n");
    printf("- Aeronaves entrando por W (oeste) ou E (leste)\n");
    printf("- 4 pistas de pouso (18, 03, 27, 06)\n");
    printf("- Detecção e prevenção de colisões\n");
    printf("- Controle de velocidade e mudança de pista\n");
}

// Função para exibir o status
void display_status() {
    system("clear"); // Limpa a tela
    printf("=== Sistema de Controle de Tráfego Aéreo ===\n\n");
    
    // Exibe status de cada aeronave
    for (int i = 0; i < shm->num_aircraft; i++) {
        if (shm->aircrafts[i].active) {
            printf("Aeronave %d: [%.2f, %.2f] %c -> Pista %d | %s\n",
                   i,
                   shm->aircrafts[i].x,
                   shm->aircrafts[i].y,
                   shm->aircrafts[i].direction,
                   shm->aircrafts[i].runway,
                   shm->aircrafts[i].status);
        }
    }
    
    printf("\nComandos: p-pausar, q-sair, h-ajuda\n");
    fflush(stdout);
}

// Função principal
int main(int argc, char *argv[]) {
    if (argc != 2) {
        print_help();
        return 1;
    }

    int num_aircraft = atoi(argv[1]);
    if (num_aircraft <= 0 || num_aircraft > MAX_AIRCRAFT) {
        printf("Número de aeronaves deve estar entre 1 e %d\n", MAX_AIRCRAFT);
        return 1;
    }

    // Configura handlers de sinais
    signal(SIGINT, handle_control_signals);
    signal(SIGTERM, handle_control_signals);

    control_tower(num_aircraft);
    return 0;
}

void control_tower(int num_aircraft) {
    // Inicializa gerador de números aleatórios
    srand(time(NULL));

    // Cria segmento de memória compartilhada
    shmid = shmget(SHM_KEY, sizeof(SharedMemory), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("Erro ao criar memória compartilhada");
        exit(1);
    }

    // Anexa memória compartilhada
    shm = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) {
        perror("Erro ao anexar memória compartilhada");
        exit(1);
    }

    // Inicializa memória compartilhada
    memset(shm, 0, sizeof(SharedMemory));
    shm->num_aircraft = num_aircraft;
    shm->paused = 0;

    // Cria processos filhos (aeronaves)
    for (int i = 0; i < num_aircraft; i++) {
        pid_t pid = fork();
        
        if (pid == 0) { // Processo filho (aeronave)
            // Sorteia posição inicial aleatória dentro do quadrado
            float x = (float)rand() / RAND_MAX; // Entre 0 e 1
            float y = (float)rand() / RAND_MAX; // Entre 0 e 1
            
            // Determina direção baseada na posição inicial
            char direction = (x < 0.5) ? 'W' : 'E';
            int runway = (direction == 'W') ? 
                        ((rand() % 2 == 0) ? 18 : 03) : 
                        ((rand() % 2 == 0) ? 27 : 06);
            
            // Inicializa aeronave
            shm->aircrafts[i].pid = getpid();
            shm->aircrafts[i].x = x;
            shm->aircrafts[i].y = y;
            shm->aircrafts[i].direction = direction;
            shm->aircrafts[i].runway = runway;
            shm->aircrafts[i].active = 1;
            shm->aircrafts[i].slowed = 0;
            strcpy(shm->aircrafts[i].status, "Voo normal");
            
            aircraft_process(i, direction, runway);
            exit(0);
        }
        else if (pid < 0) {
            perror("Erro ao criar processo filho");
            exit(1);
        }
    }

    // Loop principal do controlador
    while (!should_exit) {
        if (!shm->paused) {
            // Implementa escalonamento com prioridades
            for (int i = 0; i < num_aircraft; i++) {
                if (shm->aircrafts[i].active) {
                    // Para a aeronave atual
                    kill(shm->aircrafts[i].pid, SIGSTOP);
                    
                    // Verifica colisões e atualiza prioridades
                    update_aircraft_status(i);
                    
                    // Continua a aeronave
                    kill(shm->aircrafts[i].pid, SIGCONT);
                    usleep(100000); // 100ms por aeronave
                }
            }
        }
        
        // Exibe status
        display_status();
        
        // Verifica se todas as aeronaves pousaram
        int all_landed = 1;
        for (int i = 0; i < num_aircraft; i++) {
            if (shm->aircrafts[i].active) {
                all_landed = 0;
                break;
            }
        }
        if (all_landed) break;
        
        usleep(100000); // 100ms entre ciclos
    }

    // Limpa recursos
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
}

// Atualiza status e prioridades de uma aeronave
void update_aircraft_status(int id) {
    Aircraft *ac = &shm->aircrafts[id];
    int collision_risk = 0;
    int runway_conflict = 0;
    
    // Verifica colisões e conflitos
    for (int j = 0; j < shm->num_aircraft; j++) {
        if (id != j && shm->aircrafts[j].active) {
            Aircraft *other = &shm->aircrafts[j];
            
            // Verifica distância
            float dx = fabs(ac->x - other->x);
            float dy = fabs(ac->y - other->y);
            
            if (dx < MIN_DISTANCE && dy < MIN_DISTANCE) {
                collision_risk = 1;
                
                // Ajusta status baseado no risco
                if (dx < MIN_DISTANCE/2 && dy < MIN_DISTANCE/2) {
                    strcpy(ac->status, "RISCO DE COLISÃO!");
                    
                    // Tenta evitar colisão
                    if (!ac->slowed) {
                        kill(ac->pid, SIGUSR1); // Reduz velocidade
                    }
                    
                    // Se ainda houver risco crítico de colisão, mata uma das aeronaves
                    if (dx < MIN_DISTANCE/3 && dy < MIN_DISTANCE/3) {
                        // Decide qual aeronave abortar (a que está mais longe do aeroporto)
                        if (fabs(ac->x - 0.5) > fabs(other->x - 0.5)) {
                            printf("Colisão inevitável! Abortando aeronave %d\n", id);
                            kill(ac->pid, SIGKILL);
                            ac->active = 0;
                        } else {
                            printf("Colisão inevitável! Abortando aeronave %d\n", j);
                            kill(other->pid, SIGKILL);
                            other->active = 0;
                        }
                    }
                }
            }
            
            // Verifica conflito de pista
            if (ac->direction == other->direction && ac->runway == other->runway) {
                runway_conflict = 1;
                strcpy(ac->status, "Conflito de pista!");
            }
        }
    }
    
    // Atualiza status se não houver problemas
    if (!collision_risk && !runway_conflict) {
        if (ac->slowed) {
            strcpy(ac->status, "Velocidade reduzida");
        } else {
            strcpy(ac->status, "Voo normal");
        }
    }
}

// Handler de sinais para o controlador
void handle_control_signals(int sig) {
    switch(sig) {
        case SIGINT:
        case SIGTERM:
            should_exit = 1;
            break;
    }
}

// Handler de sinais para as aeronaves
void handle_aircraft_signals(int sig) {
    if (current_aircraft_id == -1 || shm == NULL) return;
    Aircraft *ac = &shm->aircrafts[current_aircraft_id];

    switch(sig) {
        case SIGUSR1:
            // Toggle de redução de velocidade
            ac->slowed = !ac->slowed;
            strcpy(ac->status, ac->slowed ? "Velocidade reduzida" : "Velocidade normal");
            break;
            
        case SIGUSR2:
            // Toggle de pista alternativa
            if (ac->direction == 'W') {
                ac->runway = (ac->runway == 18) ? 03 : 18;
            } else {
                ac->runway = (ac->runway == 27) ? 06 : 27;
            }
            sprintf(ac->status, "Mudou para pista %d", ac->runway);
            break;
    }
}

void aircraft_process(int id, char direction, int runway) {
    current_aircraft_id = id;
    
    // Configura handlers de sinais
    signal(SIGUSR1, handle_aircraft_signals);
    signal(SIGUSR2, handle_aircraft_signals);
    
    // Anexa memória compartilhada
    shmid = shmget(SHM_KEY, 0, 0);
    if (shmid == -1) {
        perror("Erro ao obter memória compartilhada");
        exit(1);
    }
    
    shm = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm == (void *)-1) {
        perror("Erro ao anexar memória compartilhada");
        exit(1);
    }
    
    Aircraft *ac = &shm->aircrafts[id];
    
    while (1) {
        if (!shm->paused) {
            // Calcula vetor de direção para o centro
            float dx = 0.5 - ac->x;
            float dy = 0.5 - ac->y;
            
            // Normaliza o vetor
            float length = sqrt(dx*dx + dy*dy);
            if (length > 0) {
                dx /= length;
                dy /= length;
            }
            
            // Move a aeronave na direção do centro
            ac->x += dx * SPEED * (ac->slowed ? 0.5 : 1.0);
            ac->y += dy * SPEED * (ac->slowed ? 0.5 : 1.0);
            
            // Verifica se chegou ao aeroporto
            if (fabs(ac->x - 0.5) < 0.05 && fabs(ac->y - 0.5) < 0.05) {
                sprintf(ac->status, "Pousando na pista %d", ac->runway);
                ac->active = 0;
                shmdt(shm);
                exit(0);
            }
        }
        usleep(100000); // 100ms
    }
}

void check_collisions(SharedMemory *shm) {
    // TODO: Implementar verificação de colisões
} 