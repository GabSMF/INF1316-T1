
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
#define CRIT_DIST 0.1 


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
    float posX, posY; // Coordenadas X e Y
    int status // 1 = Voando, 2 = Pousou, 3 = morto
} Plane;

typedef struct
{
    int n;
    Plane planes[MAX_PLANES];
}


/*---- Globals ----*/
char entrySide[2] = {'E','W'};
int RunwaysEast[2] = {RUNWAY_E_03, RUNWAY_E_06};
int RunwaysWest[2] = {RUNWAY_W_18, RUNWAY_W_27};


/*---- Funções de Criação de Aeronave ----*/
char new_entry()
{
    char entry = entrySide[rand() % 2];
    return entry;
}

float new_coord_y()
{
    double coordenadaY = (double)rand() / RAND_MAX;
    return coordenadaY;
}

int new_delay_time()
{
    int atraso = rand() % 11;
    return atraso;
}

int new_runway(char entrada)
{
    int pistaPreferida;
    if(entrada == 'E'){
        pistaPreferida = pistasEast[rand() % 2];
    }
    else{
        pistaPreferida = pistasWest[rand() % 2];
    }
    return pistaPreferida;
}

Plane new_plane()
{
    Plane A;
    A.delay = new_delay_time();
    A.side = new_entry();
    A.runway = new_runway(A.side);
    A.posY = new_coord_y();
    return A;
}


void create_n_children(int n)
{
    for(int i = 0; i < n; i++){
        if(fork() == 0){
            // criar um avião usando new_plane() e inserir numa lista que conterá todos os dados de todos os filhos na memória compartilhada
        }
    }
    return 0;
}


int main(void)
{
    srand(time(NULL));

    Plane plane1 = new_plane();
    printf("Avião 1: %c Pista-%d %dsecs %.2f\n", plane1.side, plane1.runway, plane1.delay, plane1.posY);

    Plane plane2 = new_plane();
    printf("Avião 2: %c runway-%d %dsecs %.2f\n", plane2.side, plane2.runway, plane2.delay, plane2.posY);

    Plane plane3 = new_plane();
    printf("Avião 3: %c runway-%d %dsecs %.2f\n", plane3.side, plane3.runway, plane3.delay, plane3.posY);
}


