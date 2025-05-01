#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <time.h>
#include <math.h>
#include <string.h>

#define NUM_AERONAVES 5
#define VELOCIDADE 0.05
#define DISTANCIA_MINIMA 0.1
#define NUM_PISTAS 4
#define EVER ;;

// Estrutura para representar uma aeronave
typedef struct {
    pid_t pid;
    char id;                // Identificador da aeronave (A, B, C, D, E)
    double x, y;            // Posição atual
    char lado_entrada;      // 'E' para leste, 'W' para oeste
    int pista_preferida;    // Pista de pouso preferida (03, 06, 18, 27)
    int pista_atual;        // Pista atual (pode ser alterada pelo controlador)
    int velocidade_reduzida; // Flag para indicar se a velocidade está reduzida
    int em_espera;          // Flag para indicar se está em espera
    int pousou;             // Flag para indicar se já pousou
} Aeronave;

// Estrutura para compartilhar dados entre processos
typedef struct {
    Aeronave aeronaves[NUM_AERONAVES];
    int num_aeronaves_ativas;
    time_t tempo_inicial;
    int processo_atual;     // Controle do round-robin
} DadosCompartilhados;

// Variáveis globais
int shmid;
int semid;
DadosCompartilhados* dados;
pid_t processos[NUM_AERONAVES];

// Função para inicializar semáforo
void inicializar_semaforo() {
    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0666);
    if (semid == -1) {
        perror("Erro ao criar semáforo");
        exit(1);
    }
    semctl(semid, 0, SETVAL, 1);
}

// Função para operar no semáforo
void operar_semaforo(int op) {
    struct sembuf operacao = {0, op, 0};
    semop(semid, &operacao, 1);
}

// Função para calcular distância entre duas aeronaves
double calcular_distancia(Aeronave a1, Aeronave a2) {
    return sqrt(pow(a1.x - a2.x, 2) + pow(a1.y - a2.y, 2));
}

// Função para verificar risco de colisão
int verificar_risco_colisao(Aeronave a1, Aeronave a2) {
    if (a1.pousou || a2.pousou) return 0;
    if (a1.lado_entrada == a2.lado_entrada) return 0;
    if (a1.pista_atual != a2.pista_atual) return 0;
    
    double dist = calcular_distancia(a1, a2);
    return (dist < DISTANCIA_MINIMA);
}

// Função para exibir status das aeronaves
void exibir_status() {
    printf("\n=== Status do Espaço Aéreo ===\n");
    printf("Tempo decorrido: %lds\n", time(NULL) - dados->tempo_inicial);
    for (int i = 0; i < NUM_AERONAVES; i++) {
        if (!dados->aeronaves[i].pousou) {
            printf("Aeronave %c: Posição (%.2f, %.2f) | Pista: %d | %s\n",
                   dados->aeronaves[i].id,
                   dados->aeronaves[i].x,
                   dados->aeronaves[i].y,
                   dados->aeronaves[i].pista_atual,
                   dados->aeronaves[i].velocidade_reduzida ? "Velocidade Reduzida" : "Velocidade Normal");
        }
    }
    printf("==============================\n");
}

// Handler para SIGUSR1 (reduzir velocidade)
void handler_velocidade(int sig) {
    operar_semaforo(-1);
    for (int i = 0; i < NUM_AERONAVES; i++) {
        if (dados->aeronaves[i].pid == getpid()) {
            dados->aeronaves[i].velocidade_reduzida = !dados->aeronaves[i].velocidade_reduzida;
            break;
        }
    }
    operar_semaforo(1);
}

// Handler para SIGUSR2 (mudar pista)
void handler_pista(int sig) {
    operar_semaforo(-1);
    for (int i = 0; i < NUM_AERONAVES; i++) {
        if (dados->aeronaves[i].pid == getpid()) {
            if (dados->aeronaves[i].lado_entrada == 'E') {
                dados->aeronaves[i].pista_atual = 
                    (dados->aeronaves[i].pista_atual == 6) ? 27 : 6;
            } else {
                dados->aeronaves[i].pista_atual = 
                    (dados->aeronaves[i].pista_atual == 3) ? 18 : 3;
            }
            break;
        }
    }
    operar_semaforo(1);
}

// Função para mover aeronave em direção ao aeroporto
void mover_aeronave(int i) {
    double dx = 0.5 - dados->aeronaves[i].x;
    double dy = 0.5 - dados->aeronaves[i].y;
    double dist = sqrt(dx*dx + dy*dy);
    
    if (dist < 0.05) { // Próximo o suficiente para pousar
        dados->aeronaves[i].pousou = 1;
        printf("[Tempo %lds] Aeronave %c pousou na pista %d\n",
               time(NULL) - dados->tempo_inicial,
               dados->aeronaves[i].id,
               dados->aeronaves[i].pista_atual);
        return;
    }
    
    // Normalizar vetor de direção
    dx /= dist;
    dy /= dist;
    
    // Aplicar velocidade
    double velocidade = VELOCIDADE;
    if (dados->aeronaves[i].velocidade_reduzida) {
        velocidade *= 0.5;
    }
    
    dados->aeronaves[i].x += dx * velocidade;
    dados->aeronaves[i].y += dy * velocidade;
}

// Função para criar uma nova aeronave
void criar_aeronave(int i) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("Erro ao criar processo");
        exit(1);
    } else if (pid == 0) {
        // Código do processo filho (aeronave)
        srand(time(NULL) ^ getpid());
        
        // Inicializar aeronave
        dados->aeronaves[i].pid = getpid();
        dados->aeronaves[i].id = 'A' + i;
        dados->aeronaves[i].lado_entrada = (rand() % 2) ? 'E' : 'W';
        dados->aeronaves[i].y = (double)rand() / RAND_MAX; // Posição Y aleatória
        dados->aeronaves[i].x = (dados->aeronaves[i].lado_entrada == 'E') ? 1.0 : 0.0;
        dados->aeronaves[i].velocidade_reduzida = 0;
        dados->aeronaves[i].em_espera = 0;
        dados->aeronaves[i].pousou = 0;
        
        // Definir pista preferida baseada no lado de entrada
        if (dados->aeronaves[i].lado_entrada == 'E') {
            dados->aeronaves[i].pista_preferida = (rand() % 2) ? 6 : 27;
        } else {
            dados->aeronaves[i].pista_preferida = (rand() % 2) ? 3 : 18;
        }
        dados->aeronaves[i].pista_atual = dados->aeronaves[i].pista_preferida;
        
        // Configurar handlers de sinais
        signal(SIGUSR1, handler_velocidade);
        signal(SIGUSR2, handler_pista);
        
        // Simular voo
        while (!dados->aeronaves[i].pousou) {
            // Aguardar sua vez no round-robin
            while (dados->processo_atual != i) {
                usleep(100000); // 100ms
            }
            
            operar_semaforo(-1);
            mover_aeronave(i);
            operar_semaforo(1);
            
            // Passar para o próximo processo
            dados->processo_atual = (dados->processo_atual + 1) % NUM_AERONAVES;
            
            usleep(100000); // 100ms
        }
        exit(0);
    } else {
        processos[i] = pid;
    }
}

// Função principal do controlador
int main() {
    // Criar memória compartilhada
    shmid = shmget(IPC_PRIVATE, sizeof(DadosCompartilhados), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("Erro ao criar memória compartilhada");
        exit(1);
    }
    
    dados = (DadosCompartilhados*)shmat(shmid, NULL, 0);
    if (dados == (DadosCompartilhados*)-1) {
        perror("Erro ao anexar memória compartilhada");
        exit(1);
    }
    
    // Inicializar dados compartilhados
    dados->num_aeronaves_ativas = NUM_AERONAVES;
    dados->tempo_inicial = time(NULL);
    dados->processo_atual = 0;
    
    // Inicializar semáforo
    inicializar_semaforo();
    
    // Criar aeronaves
    for (int i = 0; i < NUM_AERONAVES; i++) {
        criar_aeronave(i);
        sleep(rand() % 3); // Atraso aleatório entre 0 e 2 segundos
    }
    
    // Loop principal do controlador
    while (1) {
        sleep(1);
        operar_semaforo(-1);
        
        // Verificar colisões e controlar tráfego
        for (int i = 0; i < NUM_AERONAVES; i++) {
            if (dados->aeronaves[i].pousou) continue;
            
            for (int j = i + 1; j < NUM_AERONAVES; j++) {
                if (dados->aeronaves[j].pousou) continue;
                
                if (verificar_risco_colisao(dados->aeronaves[i], dados->aeronaves[j])) {
                    // Tentar reduzir velocidade da aeronave mais próxima do aeroporto
                    double dist_i = sqrt(pow(0.5 - dados->aeronaves[i].x, 2) + 
                                       pow(0.5 - dados->aeronaves[i].y, 2));
                    double dist_j = sqrt(pow(0.5 - dados->aeronaves[j].x, 2) + 
                                       pow(0.5 - dados->aeronaves[j].y, 2));
                    
                    if (dist_i < dist_j) {
                        kill(dados->aeronaves[i].pid, SIGUSR1);
                    } else {
                        kill(dados->aeronaves[j].pid, SIGUSR1);
                    }
                    
                    // Se ainda houver risco de colisão, mudar pista
                    if (verificar_risco_colisao(dados->aeronaves[i], dados->aeronaves[j])) {
                        kill(dados->aeronaves[i].pid, SIGUSR2);
                    }
                    
                    // Se ainda houver risco, abortar pouso
                    if (verificar_risco_colisao(dados->aeronaves[i], dados->aeronaves[j])) {
                        printf("[Tempo %lds] COLISÃO IMINENTE! Abortando pouso da aeronave %c\n",
                               time(NULL) - dados->tempo_inicial,
                               dados->aeronaves[i].id);
                        kill(dados->aeronaves[i].pid, SIGKILL);
                        dados->aeronaves[i].pousou = 1;
                    }
                }
            }
        }
        
        // Exibir status
        exibir_status();
        
        // Verificar se todas as aeronaves pousaram
        int todas_pousaram = 1;
        for (int i = 0; i < NUM_AERONAVES; i++) {
            if (!dados->aeronaves[i].pousou) {
                todas_pousaram = 0;
                break;
            }
        }
        
        if (todas_pousaram) {
            printf("Todas as aeronaves pousaram com sucesso!\n");
            break;
        }
        
        operar_semaforo(1);
    }
    
    // Limpar recursos
    shmdt(dados);
    shmctl(shmid, IPC_RMID, NULL);
    semctl(semid, 0, IPC_RMID);
    
    return 0;
} 