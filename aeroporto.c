// T1 - Sistemas Operacionais - 3WA

// Gabriel da Silva Marques Ferreira 2210442
// Gustavo Riedel 2210375

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <math.h>
#include <time.h>

#define MAX_AVIOES 60
#define VELOCIDADE 0.05
#define TEMPO_ROUND_ROBIN 1 // quantum de tempo
#define DIST_COLISAO 0.1    // região crítica de colisão
#define DESTINO 0.5f        // posicao do centro [0.5,0.5]

// Estrutura da aeronave na memoria compartilhada
typedef struct
{
    pid_t pid; // pid do processo

    float x, y;     // posicao
    char origem;    // lado de origem (W ou E)
    int pista_pref; // Pistas de preferência provindas do lado
    int ativo;      // se está ativo ou foi morto (1 ou 0)

    // confirmações de recebimento de sinal
    int confirm_usr1;
    int confirm_usr2;

    int parado;            // Se está parado ou não (1 ou 0)
    pid_t ultimo_colidido; // ultimo avião no qual estava próximo demais antes de trocar de pista
    int trocou_quantum;    // flag para evitar múltiplas trocas de pista de uma vez
} Aviao;

// Espaço aéreo compartilhado
typedef struct
{
    Aviao avioes[MAX_AVIOES];
    int n_avioes;
    int escalonamento_ativo; // flag para se existem aviões não pousados para serem escalonados
    int abortados;           // contagem de abortos
    int pousados;            // contagem de pousos
} EspacoAereo;

int shmid;
EspacoAereo *espaco;
pid_t filhos[MAX_AVIOES];
int n_processos;

// Protótipos
int encontra_meu_idx(void);
int algum_ativo(void);
double dist_entre(int i, int j);
int pista_alternativa(Aviao a);
void mata_aviao(int idx);
void resolve_conflito(int i, int j);
void escalonador(void);
void mata_aviao(int idx);
void sigusr1_handler(int sig);
void sigusr2_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);
void sigquit_handler_menu(int sig);
void sigusr1_handler_menu(int sig);
void sigusr2_handler_menu(int sig);
void interface_sinais();
void movimento(int idx);
void cria_avioes(int n);
void resolve_conflito(int i, int j);
void escalonador(void);


int main(int argc, char *argv[])
{
    if (argc != 2) // Parametro de entrada com número de aviões a serem criados
    {
        fprintf(stderr, "Uso: %s <n_avioes>\n", argv[0]);
        return 1;
    }
    n_processos = atoi(argv[1]);

    if (n_processos > MAX_AVIOES) // Só permite até o máximo de aviões
        n_processos = MAX_AVIOES;
    
    shmid = shmget(IPC_PRIVATE, sizeof(EspacoAereo), IPC_CREAT | 0666); // Cria memória compartilhada
    espaco = shmat(shmid, NULL, 0); // Associa à memória do processo

    // inicializa contadores e flags
    espaco->escalonamento_ativo = 0;
    espaco->n_avioes = 0;
    espaco->abortados = 0;
    espaco->pousados = 0;

    cria_avioes(n_processos); // cria os processos que serão os aviões

    sleep(1); // Espera 1 sec pra garantir que todos os filhos foram inicializados


    for (int i = 0; i < n_processos; i++)
        kill(filhos[i], SIGSTOP); // Congela imediatamente os aviões após criá-los

    pid_t pid = fork();
    if (pid == 0) // Cria um novo processo filho para uso único do Escalonador
    {   
        // Ignora chamadas para não travar o escalonador
        signal(SIGINT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGUSR1, SIG_IGN);
        signal(SIGUSR2, SIG_IGN);
        signal(SIGABRT, SIG_IGN);
        escalonador();
        exit(0);
    }
    interface_sinais();
    return 0;
}


// Funções

// Encontra pid do processo sendo executado
int encontra_meu_idx(void)
{
    pid_t pid = getpid();
    for (int i = 0; i < espaco->n_avioes; i++)
    {
        if (espaco->avioes[i].pid == pid)
            return i;
    }
    return -1;
}

// Confirma se existe algum avião ainda voando
int algum_ativo(void)
{
    for (int i = 0; i < espaco->n_avioes; i++)
    {
        if (espaco->avioes[i].ativo)
            return 1;
    }
    return 0;
}

// Distância entre dois aviões a partir de sua posição na lista
double dist_entre(int i, int j)
{
    double dx = espaco->avioes[i].x - espaco->avioes[j].x;
    double dy = espaco->avioes[i].y - espaco->avioes[j].y;
    return sqrt(dx * dx + dy * dy);
}

// Distância de um avião do centro
double dist_centro(Aviao i)
{
    double dx = i.x - DESTINO;
    double dy = i.y - DESTINO;
    return sqrt(dx * dx + dy * dy);
}

// Troca de pistas de mesmo lado
int pista_alternativa(Aviao a)
{
    if (a.origem == 'W')
        return (a.pista_pref == 3) ? 18 : 3;
    else
        return (a.pista_pref == 6) ? 27 : 6;
}

// da SIGKILL em um avião
void mata_aviao(int idx)
{
    if (!espaco->avioes[idx].ativo)
        return;          // Evita que um processo passe aqui 2 vezes
    espaco->abortados++; // incrementa contador de abortados

    pid_t pid = espaco->avioes[idx].pid;
    kill(pid, SIGKILL); // mata o processo

    espaco->avioes[idx].ativo = 0; // declara como inativo
    printf("[Controlador] Aviao %d abortado\n", pid);
}

// Handlers

// Sinal de Flip-Flop do estado do avião entre Parado e Voando
void sigusr1_handler(int sig)
{
    int idx = encontra_meu_idx();

    espaco->avioes[idx].parado = !espaco->avioes[idx].parado; // troca por parado ou voando 0
    espaco->avioes[idx].confirm_usr1 = 1;                     // confirmação que filho recebeu a flag

    printf("[Aviao %d] %s\n", getpid(), espaco->avioes[idx].parado ? "Parado" : "Retomando");
}

// Sinal de Flip Flop da pista de preferência
void sigusr2_handler(int sig)
{
    int idx = encontra_meu_idx();

    espaco->avioes[idx].pista_pref = pista_alternativa(espaco->avioes[idx]); // troca a pista
    espaco->avioes[idx].confirm_usr2 = 1;                                    // confirmação que o filho recebeu a flag

    printf("[Aviao %d] Pista -> %d\n", getpid(), espaco->avioes[idx].pista_pref);
}

void sigterm_handler(int sig)
{
    exit(0);
}

// Movimento
void movimento(int idx)
{
    while (espaco->avioes[idx].ativo)
    { // enquanto aquele aviao estiver voando
        if (!espaco->avioes[idx].parado)
        { // se ele não estiver parado

            // calcula distancia do processo ao centro
            float dx = DESTINO - espaco->avioes[idx].x;
            float dy = DESTINO - espaco->avioes[idx].y;
            float dist = sqrt(dx * dx + dy * dy); // calculo da hipotenusa (distancia na diagonal do avião ao centro)
            if (dist < VELOCIDADE)
            {
                espaco->pousados++;            // incrementa n° de pousados
                espaco->avioes[idx].ativo = 0; // desativa o avião
                espaco->avioes[idx].x = DESTINO;
                espaco->avioes[idx].y = DESTINO;
                printf("[Aviao %d] Pousou em (%.2f,%.2f)\n", getpid(), DESTINO, DESTINO);
                exit(0);
            }

            // incrementa/decrementa a posicao do aviao em X e Y
            // além de multiplicar pelo vetor unitário para o avião ficar na direção correta
            espaco->avioes[idx].x += VELOCIDADE * dx / dist;
            espaco->avioes[idx].y += VELOCIDADE * dy / dist;
        }
        sleep(1);
    }
}

// Criação
void cria_avioes(int n)
{
    // pistas possíveis para cada lado
    int pistas_w[2] = {3, 18};
    int pistas_e[2] = {6, 27};

    for (int i = 0; i < n; i++)
    { // cria n processos filhos
        pid_t pid = fork();
        if (pid == 0)
        {
            // Processa os sinais
            signal(SIGUSR1, sigusr1_handler);
            signal(SIGUSR2, sigusr2_handler);
            signal(SIGTERM, sigterm_handler);
            signal(SIGINT, SIG_IGN);
            srand(getpid()); // ativa para cada filho poder gerar valores aleatórios

            // Escolha aleatória dos dados do filho
            espaco->avioes[i].pid = getpid();
            espaco->avioes[i].origem = (rand() % 2) ? 'W' : 'E';
            espaco->avioes[i].x = (espaco->avioes[i].origem == 'W') ? 0.0f : 1.0f;
            espaco->avioes[i].y = (rand() % 100) / 100.0f;
            espaco->avioes[i].pista_pref = (espaco->avioes[i].origem == 'W') ? pistas_w[rand() % 2] : pistas_e[rand() % 2];
            espaco->avioes[i].ativo = 1;
            sleep(rand() % 3); // delay de 0 até 2 segundos
            movimento(i);      // ativa o movimento
            exit(0);
        }
        else
        {
            filhos[i] = pid; // armazena o pid de cada filho na lista de filhos que vai para a memCompartilhada.
        }
    }
    espaco->n_avioes = n; // insere número de aviões criados na memCompartilhada
}

// Resolve conflito de colisão (Região crítica [0.1,0.1])

void resolve_conflito(int i, int j)
{
    Aviao *Ai = &espaco->avioes[i]; // Avião sendo executado
    Aviao *Aj = &espaco->avioes[j];


    // Se estão em pistas diferentes não podem colidir
    if (Ai->pista_pref != Aj->pista_pref)
    return;

    // Calcula a distância entre os 2 aviões
    float dx = fabs(Ai->x - Aj->x);
    float dy = fabs(Ai->y - Aj->y);
    if (!(dx < DIST_COLISAO && dy < DIST_COLISAO)) // verifica se estão em Região Crítica
        return;

    // Se estão em região crítica

    // Se um estiver Parado e outro Voando, para o que está voando para evitar engarrafamentos
    if (Ai->parado && Ai->ultimo_colidido !=Aj->pid && !Aj->parado)
    {
        mata_aviao(i);
        printf("[Controlador] %d estava parado e %d se aproximou demais \n", Ai->pid, Aj->pid);
        return;
    }
    if (Aj->parado && Aj->ultimo_colidido !=Ai->pid && !Ai->parado)
    {
        mata_aviao(j);
        printf("[Controlador] %d estava parado e %d se aproximou demais \n", Aj->pid, Ai->pid);
        return;
    }

    pid_t pj = Aj->pid;
    pid_t pi = Ai->pid;

    // Seleciona o avião que está mais distante do centro para trocar de pista ou ser parado
    double di = dist_centro(*Ai);
    double dj = dist_centro(*Aj);

    int swap_idx = (di > dj) ? i : j;
    Aviao *A = &espaco->avioes[swap_idx];

    int encontro1 = (Ai->ultimo_colidido != pj && Aj->ultimo_colidido != pi); // Verifica se os aviões estão se encontrando pela primeira vez na Região Crítica
    if (encontro1 && !Ai->trocou_quantum)
    { // Evita que o mesmo avião tente trocar de pista várias vezes dentro do mesmo quantum.

        A->confirm_usr2 = 0; // Controlador zera a confirmação de sinal para verificar se logo após do envio do sinal ele muda

        // Se o avião não estiver sendo executado, temporariamente coloca ele em execução para receber o sinal
        kill(A->pid, SIGCONT);
        kill(A->pid, SIGUSR2); // envia o sinal
        sleep(1);
        kill(A->pid, SIGSTOP); // para o sinal novamente

        // Verifica se o avião recebeu o sinal
        if (A->confirm_usr2)
        {
            A->ultimo_colidido = (swap_idx == i) ? pj : pi; // guarda o avião no qual ele entrou em Região Crítica
            A->trocou_quantum = 1;
        }
        else
        { // Se não recebeu o sinal mata o processo
            mata_aviao(swap_idx);
            printf("[Controlador] %d Não respondeu ao SIGUSR2 enviado", A->pid);
        }
        return;
    }
    // Se por acaso dois aviões que já se encontraram, se encontram novamente na mesma pista, troca a estratégia e pare o que está mais atrás
    int stop_idx = (Ai->ultimo_colidido == pj) ? i : j;
    Aviao *Astop = &espaco->avioes[stop_idx];
    Astop->confirm_usr1 = 0; // Controlador zera a confirmação de sinal para verificar se logo após do envio do sinal ele muda

    // Se o avião não estiver sendo executado, temporariamente coloca ele em execução para receber o sinal
    kill(Astop->pid, SIGCONT);
    kill(Astop->pid, SIGUSR1); // envia o sinal
    sleep(1);
    kill(Astop->pid, SIGSTOP); // para o sinal novamente

    if (!Astop->confirm_usr1)
    { // Se não recebeu o sinal mata o processo
        printf("[Controlador] %d Não respondeu ao SIGUSR1 enviado", Astop->pid);
        mata_aviao(stop_idx);
    }
}

// Escalonador - Round Robin

void escalonador(void)
{
    int atual = 0; // avião atual
    while (1)
    { // loop infinito
        if (espaco->escalonamento_ativo)
        {
            Aviao *A = &espaco->avioes[atual];
            if (A->ativo)
            { // Se o avião atual está ativo

                // Se A está parado encontra o id do avião que o parou
                if (A->parado && A->ultimo_colidido)
                { //
                    int k = -1;
                    for (int m = 0; m < espaco->n_avioes; m++)
                    {
                        if (espaco->avioes[m].pid == A->ultimo_colidido)
                        {
                            k = m;
                            break;
                        }
                    }
                    // Se o avião que parou A já pousou ou não está mais na Região Critica com A. Retorna o movimento de A
                    if (k < 0 || espaco->avioes[k].ativo == 0 || !(fabs(A->x - espaco->avioes[k].x) < DIST_COLISAO && fabs(A->y - espaco->avioes[k].y) < DIST_COLISAO))
                    {
                        A->confirm_usr1 = 0;
                        kill(A->pid, SIGCONT); // Se o avião não estiver sendo executado, temporariamente coloca ele em execução para receber o sinal
                        kill(A->pid, SIGUSR1); // envia o sinal
                        sleep(1);
                        kill(A->pid, SIGSTOP); // para novamente o sinal
                        if (A->confirm_usr1)
                        { // Se recebeu o sinal avisa
                            printf("[Controlador] %d Recebeu sinal de Retomada com sucesso\n", A->pid);
                            A->ultimo_colidido = 0; // Reseta a flag
                        }
                        else
                        {
                            printf("[Controlador] %d Não respondeu ao SIGUSR1 enviado", A->pid);
                            mata_aviao(A->pid); // mata o avião
                        }
                    }
                }

                // reset troca
                A->trocou_quantum = 0;

                kill(A->pid, SIGCONT);    // Retoma o Avião atual
                sleep(TEMPO_ROUND_ROBIN); // dá quantum de tempo ao processo

                for (int j = 0; j < espaco->n_avioes; j++) // Compara o avião atual a todos os outros
                {
                    if (j != atual && espaco->avioes[j].ativo)
                        resolve_conflito(atual, j); // Confere e trata se estiver em Região Crítica
                }
                kill(A->pid, SIGSTOP); // Para o processo atual para dar vez ao próximo
            }
        }
        else
        {
            sleep(1);
        }
        if (!algum_ativo()) // Se não tiver nenhum ativo, encerra a simulação
        {
            printf("[Controlador] Simulacao encerrada. Abortados: %d, Pousados: %d\n",
                   espaco->abortados, espaco->pousados);
            printf("[Controlador] Fim da simulacao.\n");
            break;
        }
        atual = (atual + 1) % espaco->n_avioes; // Vai para o próximo na lista de aviões
    }
}

// Sinais para iniciar, pausar, retomar, exibir dados dos aviões e finalizar a simulacao
void sigint_handler(int sig) {
    // Iniciar todos os avioes
    if (espaco->escalonamento_ativo == 0) {
        espaco->escalonamento_ativo = 1;
        printf("[SINAL] Simulacao iniciada\n");
    } else {
        espaco->escalonamento_ativo = 0;
        printf("[SINAL] Simulacao pausada\n");
    }
}

void sigtstp_handler(int sig) {
    //Exibir status das aeronaves (usando SIGTSTP)
    printf("[Status]\n");
    printf("PID     X      Y   Origem Pista Ativo Parado\n");
    for (int i = 0; i < espaco->n_avioes; i++) {
        Aviao *a = &espaco->avioes[i];
        printf("%6d %6.2f %6.2f   %c     %2d    %s     %s\n",
               a->pid, a->x, a->y, a->origem, a->pista_pref,
               a->ativo ? "Sim" : "Nao", a->parado ? "Sim" : "Nao");
    }
}

void sigquit_handler_menu(int sig) {
    // Finalizar simulacao (SIGQUIT)
    printf("[SINAL] Finalizando...\n");
    for (int i = 0; i < espaco->n_avioes; i++)
        if (espaco->avioes[i].ativo)
            kill(espaco->avioes[i].pid, SIGKILL);
    exit(0);
}

void sigusr1_handler_menu(int sig) {
    // Parar todos os avioes
    printf("[SINAL] Parando todos os avioes (SIGUSR1)\n");
    for (int i = 0; i < espaco->n_avioes; i++)
        if (espaco->avioes[i].ativo)
            kill(espaco->avioes[i].pid, SIGUSR1);
}

void sigusr2_handler_menu(int sig) {
    // Trocar pista de todos os avioes
    printf("[SINAL] Trocando pista de todos os avioes (SIGUSR2)\n");
    for (int i = 0; i < espaco->n_avioes; i++)
        if (espaco->avioes[i].ativo)
            kill(espaco->avioes[i].pid, SIGUSR2);
}

void interface_sinais() {
    signal(SIGINT, sigint_handler);         // Ctrl+C
    signal(SIGTSTP, sigtstp_handler);       // Ctrl+Z
    signal(SIGABRT, sigquit_handler_menu);  // Ctrl+/
    signal(SIGUSR1, sigusr1_handler_menu);
    signal(SIGUSR2, sigusr2_handler_menu);

    printf("[Modo Sinal] Use os sinais para controlar a simulacao:\n");
    printf("  SIGINT  (Ctrl+C): Iniciar/Pausar\n");
    printf("  SIGTSTP (Ctrl+Z): Status\n");
    printf("  SIGQUIT (Ctrl+\\): Finalizar\n");
    printf("  SIGUSR1 (kill -10): Parar avioes\n");
    printf("  SIGUSR2 (kill -12): Trocar pistas\n");

    while (1) pause();
}